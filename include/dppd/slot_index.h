#ifndef DPPD_SLOT_INDEX_H
#define DPPD_SLOT_INDEX_H

#include <stdint.h>

#define DPPD_SLOT_NONE UINT32_MAX

struct dppd_slot_key;

/** 单控制线程维护编号到固定槽位的有序索引，移动编号时不移动事务仍在使用的对象 */
struct dppd_slot_index {
    void *storage;
    struct dppd_slot_key *keys;
    uint32_t *order;
    uint32_t capacity, count, free_head;
};

int dppd_slot_index_init(struct dppd_slot_index *index, uint32_t capacity);
void dppd_slot_index_fini(struct dppd_slot_index *index);
/** 只清理本地索引，上层必须先确认实际对象已删除或按明确的复位契约失效 */
void dppd_slot_index_clear(struct dppd_slot_index *index);
uint32_t dppd_slot_index_find(const struct dppd_slot_index *index, uint64_t id, uint64_t generation);
uint32_t dppd_slot_index_latest(const struct dppd_slot_index *index, uint64_t id);
/** 返回严格大于此编号/版本的位置，可用于稳定分页，不把内部槽位当作外部游标 */
uint32_t dppd_slot_index_upper_bound(const struct dppd_slot_index *index, uint64_t id, uint64_t generation);
uint32_t dppd_slot_index_at(const struct dppd_slot_index *index, uint32_t position);
/** 固定存储在初始化时分配，后续插入/删除不分配内存，失败保持输出槽位和索引不变 */
int dppd_slot_index_insert(struct dppd_slot_index *index, uint64_t id, uint64_t generation, uint32_t *slot);
int dppd_slot_index_erase(struct dppd_slot_index *index, uint64_t id, uint64_t generation);

#endif
