#include "dppd/slot_index.h"

#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

struct dppd_slot_key {
    /** 联合查找键，先比较 id 再比较 generation；规则账本使用 (规则 ID, 0) */
    uint64_t id, generation;
    /** 槽位空闲时指向下一个空闲槽位，DPPD_SLOT_NONE 表示链尾；已用槽位不读取此字段 */
    uint32_t next_free;
};

static bool less(const struct dppd_slot_key *key, uint64_t id, uint64_t generation)
{
    return key->id < id || (key->id == id && key->generation < generation);
}

/**
 * 在有序槽位编号数组中二分，返回首个键不小于 (id, generation) 的位置，或 count
 * 返回的是 order 的位置，不是对象槽位：position → order[position] → slot → keys[slot]
 * 插入时在此位置腾出空位，查询时还须检查这里的键是否相等
 */
static uint32_t lower_bound(const struct dppd_slot_index *index, uint64_t id, uint64_t generation)
{
    uint32_t first = 0, last = index->count;
    while (first < last) {
        uint32_t middle = first + (last - first) / 2U;
        if (less(&index->keys[index->order[middle]], id, generation))
            first = middle + 1U;
        else
            last = middle;
    }
    return first;
}

void dppd_slot_index_clear(struct dppd_slot_index *index)
{
    if (index == NULL || index->storage == NULL)
        return;
    memset(index->keys, 0, (size_t)index->capacity * sizeof(*index->keys));
    for (uint32_t i = 0; i < index->capacity; ++i)
        index->keys[i].next_free = i + 1U;
    index->keys[index->capacity - 1U].next_free = DPPD_SLOT_NONE;
    index->count = 0;
    index->free_head = 0;
}

int dppd_slot_index_init(struct dppd_slot_index *index, uint32_t capacity)
{
    if (index == NULL || capacity == 0)
        return -EINVAL;
    size_t width = sizeof(struct dppd_slot_key) + sizeof(uint32_t);
    if ((uint64_t)capacity > SIZE_MAX / width)
        return -EOVERFLOW;
    void *storage = calloc(capacity, width);
    if (storage == NULL)
        return -ENOMEM;
    struct dppd_slot_index next = {.storage = storage,
        .keys = storage,
        .order = (uint32_t *)((unsigned char *)storage + (size_t)capacity * sizeof(struct dppd_slot_key)),
        .capacity = capacity};
    dppd_slot_index_clear(&next);
    *index = next;
    return 0;
}

void dppd_slot_index_fini(struct dppd_slot_index *index)
{
    if (index == NULL)
        return;
    free(index->storage);
    memset(index, 0, sizeof(*index));
}

uint32_t dppd_slot_index_find(const struct dppd_slot_index *index, uint64_t id, uint64_t generation)
{
    if (index == NULL || index->storage == NULL || id == 0)
        return DPPD_SLOT_NONE;
    uint32_t position = lower_bound(index, id, generation);
    if (position == index->count)
        return DPPD_SLOT_NONE;
    uint32_t slot = index->order[position];
    return index->keys[slot].id == id && index->keys[slot].generation == generation ? slot : DPPD_SLOT_NONE;
}

uint32_t dppd_slot_index_upper_bound(const struct dppd_slot_index *index, uint64_t id, uint64_t generation)
{
    if (index == NULL || index->storage == NULL)
        return 0;
    uint32_t position = lower_bound(index, id, generation);
    if (position < index->count) {
        const struct dppd_slot_key *key = &index->keys[index->order[position]];
        if (key->id == id && key->generation == generation)
            ++position;
    }
    return position;
}

uint32_t dppd_slot_index_latest(const struct dppd_slot_index *index, uint64_t id)
{
    if (index == NULL || index->storage == NULL || id == 0)
        return DPPD_SLOT_NONE;
    uint32_t position = dppd_slot_index_upper_bound(index, id, UINT64_MAX);
    if (position == 0)
        return DPPD_SLOT_NONE;
    uint32_t slot = index->order[position - 1U];
    return index->keys[slot].id == id ? slot : DPPD_SLOT_NONE;
}

uint32_t dppd_slot_index_at(const struct dppd_slot_index *index, uint32_t position)
{
    return index != NULL && index->storage != NULL && position < index->count ? index->order[position] : DPPD_SLOT_NONE;
}

/**
 * 为 (id, generation) 分配空闲槽位并加入有序索引，成功返回 0，通过 output 返回槽位
 * 参数无效返回 -EINVAL，键已存在返回 -EEXIST，无空闲槽位返回 -ENOSPC，失败不修改索引或 output
 * 内存已在 init 分配，本函数只维护索引；完整规则由调用方写入 records[slot]
 *
 * 以规则账本为例，以下键的 generation 均为 0
 * 插入前：order = [0, 2, 3] → 对应 ID [10, 30, 40]，count = 3
 *         keys[0].id = 10，keys[2].id = 30，keys[3].id = 40
 *         free_head → 槽位 4 → 槽位 1 → NONE，假设这些槽位当前空闲
 * 插入 ID 20：二分得到 position = 1，空闲链提供 slot = 4
 *   → free_head 改为 1，keys[4] 写入 (20, 0)
 *   → order 中原位置 1、2 的编号右移到位置 2、3，在位置 1 写入编号 4
 * 插入后：order = [0, 4, 2, 3] → 对应 ID [10, 20, 30, 40]，count = 4
 *   → *output = 4 → 调用方把完整规则写入 records[4]
 * 已有完整规则始终留在原槽位，排序只移动 order 中的整数编号
 */
int dppd_slot_index_insert(struct dppd_slot_index *index, uint64_t id, uint64_t generation, uint32_t *output)
{
    if (index == NULL || index->storage == NULL || output == NULL || id == 0)
        return -EINVAL;
    uint32_t position = lower_bound(index, id, generation);
    if (position < index->count) {
        const struct dppd_slot_key *found = &index->keys[index->order[position]];
        if (found->id == id && found->generation == generation)
            return -EEXIST;
    }
    if (index->free_head == DPPD_SLOT_NONE)
        return -ENOSPC;
    /** 从空闲链头取出槽位，先保存其后继为新链头，再覆盖该槽位的键 */
    uint32_t slot = index->free_head;
    index->free_head = index->keys[slot].next_free;
    index->keys[slot] = (struct dppd_slot_key){.id = id, .generation = generation};
    /** 插入位置之后的编号整体右移一格，源和目标区域可能重叠，因此使用 memmove */
    memmove(&index->order[position + 1U], &index->order[position],
        (size_t)(index->count - position) * sizeof(*index->order));
    index->order[position] = slot;
    ++index->count;
    *output = slot;
    return 0;
}

/**
 * 删除 (id, generation) 的索引并归还槽位，成功返回 0，参数无效返回 -EINVAL，键不存在返回 -ENOENT
 * 这里只处理 keys、order 和空闲链，规则账本调用方还须清空对应 records[slot]
 *
 * 接续 insert 中的例子：order = [0, 4, 2, 3]，对应 ID [10, 20, 30, 40]
 * 删除 ID 20 → position = 1 → slot = order[1] = 4，原空闲链为 1 → NONE
 *   → 将位置 2、3 的编号左移到位置 1、2 → 有效 order = [0, 2, 3]
 *   → 清空 keys[4] 的键，令 keys[4].next_free = 1
 *   → free_head = 4，空闲链变成 4 → 1 → NONE，count 从 4 减为 3
 * 数组末尾可能留有旧编号，但只有 order[0..count) 有效，查询不会读取尾部
 *
 * 随后新增 ID 25 → insert 先取空闲槽位 4 → order = [0, 4, 2, 3]
 * 调用方将 ID 25 的完整规则写入 records[4]，ID 10、30、40 的完整记录始终留在原槽位
 * 槽位复用不代表规则身份相同，外部仍按 ID 和版本识别规则
 */
int dppd_slot_index_erase(struct dppd_slot_index *index, uint64_t id, uint64_t generation)
{
    if (index == NULL || index->storage == NULL || id == 0)
        return -EINVAL;
    uint32_t position = lower_bound(index, id, generation);
    if (position == index->count)
        return -ENOENT;
    uint32_t slot = index->order[position];
    if (index->keys[slot].id != id || index->keys[slot].generation != generation)
        return -ENOENT;
    /** 左移删除位置之后的有效编号填补空位，重叠区域使用 memmove，完整对象不随之移动 */
    memmove(&index->order[position], &index->order[position + 1U],
        (size_t)(index->count - position - 1U) * sizeof(*index->order));
    /** 先接住原空闲链，再把刚释放的槽位设为链头，下一次插入可直接复用 */
    index->keys[slot] = (struct dppd_slot_key){.next_free = index->free_head};
    index->free_head = slot;
    --index->count;
    return 0;
}
