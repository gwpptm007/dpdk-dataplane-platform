#include "dppd/slot_index.h"

#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

struct dppd_slot_key {
    uint64_t id, generation;
    uint32_t next_free;
};

static bool less(const struct dppd_slot_key *key, uint64_t id, uint64_t generation)
{
    return key->id < id || (key->id == id && key->generation < generation);
}

/** 只在紧凑的编号数组中二分，不反复扫描包含完整规则或驱动句柄的大对象数组 */
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
    uint32_t slot = index->free_head;
    index->free_head = index->keys[slot].next_free;
    index->keys[slot] = (struct dppd_slot_key){.id = id, .generation = generation};
    memmove(&index->order[position + 1U], &index->order[position],
        (size_t)(index->count - position) * sizeof(*index->order));
    index->order[position] = slot;
    ++index->count;
    *output = slot;
    return 0;
}

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
    memmove(&index->order[position], &index->order[position + 1U],
        (size_t)(index->count - position - 1U) * sizeof(*index->order));
    index->keys[slot] = (struct dppd_slot_key){.next_free = index->free_head};
    index->free_head = slot;
    --index->count;
    return 0;
}
