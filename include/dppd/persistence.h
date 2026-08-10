#ifndef DPPD_PERSISTENCE_H
#define DPPD_PERSISTENCE_H

#include <stdint.h>
#include "dppd/rule_repository.h"

/*
 * 从磁盘加载后的纯数据快照。它不代表规则已经重新安装到硬件：
 * 调用方必须完成 topology resolve、plan、validate/create 和 repository publish，
 * 才能把恢复过程视为成功。
 */
struct dppd_persisted_snapshot {
    struct dppd_rule *rules;
    uint32_t count;
    uint64_t repository_generation;
};

/*
 * 保存使用同目录临时文件、fsync、rename 和目录 fsync。
 * 成功返回时，新文件已经原子替换旧快照。rename 前失败时旧文件保持不变；
 * rename 后目录 fsync 失败时新文件已可见，但掉电持久性不能保证。
 */
int dppd_persistence_save(const char *path,
                          const struct dppd_rule_repository *repository);

/*
 * load 只负责格式、校验和及 rule IR 语义校验，不执行任何硬件操作。
 * 文件不存在返回 -ENOENT；格式/校验和损坏返回 -EBADMSG。
 */
int dppd_persistence_load(const char *path,
                          struct dppd_persisted_snapshot *snapshot);
void dppd_persisted_snapshot_destroy(struct dppd_persisted_snapshot *snapshot);

#endif
