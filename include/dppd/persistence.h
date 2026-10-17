#ifndef DPPD_PERSISTENCE_H
#define DPPD_PERSISTENCE_H

#include <stdint.h>
#include "dppd/rule_repository.h"
#include "dppd/shared_count.h"

/*
 * 从磁盘加载后的纯数据快照。它不代表规则已经重新安装到硬件：
 * 调用方必须完成 topology resolve、plan、validate/create 和 repository publish
 * 才能把恢复过程视为成功
 */
struct dppd_persisted_snapshot {
    struct dppd_rule *rules;
    uint32_t count;
    uint64_t repository_generation;
    struct dppd_shared_count_spec *shared_counts;
    uint32_t shared_count;
    uint64_t shared_revision;
};

/*
 * 保存使用同目录临时文件、fsync、rename 和目录 fsync
 * 成功返回时，新文件已经原子替换旧快照。rename 前失败时旧文件保持不变；
 * rename 后目录 fsync 失败时新文件已可见，但掉电持久性不能保证
 *
 * 包含 AGE 的规则使用 v4，仅保存配置的超时秒数，不保存已过空闲时间或驱动 context
 * 读取后仍须由正式创建流程重建对象，因此重启会重新开始空闲计时
 * 无 AGE 的快照继续写原有 v2/v3，旧 v1 仍只允许通过显式迁移入口读取
 */
int dppd_persistence_save(const char *path, const struct dppd_rule_repository *repository);

/**
 * 规则和共享计数策略写入同一份 v5 快照，空表仍保存非零修订号以阻止删除重建后的旧引用
 * 对象只保存 ID、代次、安装端口和作用域，恢复时重建句柄且计数从零开始
 * 未使用共享能力且修订号为零时仍采用 v2/v3/v4 格式
 */
int dppd_persistence_save_with_shared(const char *path, const struct dppd_rule_repository *repository,
    const struct dppd_shared_count_spec *specs, uint32_t count, uint64_t shared_revision);

/*
 * load 只负责格式、校验和及 rule IR 语义校验，不执行任何硬件操作
 * 文件不存在返回 -ENOENT；格式/校验和损坏返回 -EBADMSG
 */
int dppd_persistence_load(const char *path, struct dppd_persisted_snapshot *snapshot);

/*
 * 仅供离线迁移工具读取 v1 快照。v1 不保存 install_port_id，因此成功返回的
 * 所有规则该字段均为 0；调用方必须显式补全端口，不能把结果直接用于启动恢复
 * 与普通加载一样，此接口会严格验证文件大小、CRC、记录布局和 rule IR 语义
 */
int dppd_persistence_load_v1_for_migration(const char *path, struct dppd_persisted_snapshot *snapshot);
void dppd_persisted_snapshot_destroy(struct dppd_persisted_snapshot *snapshot);

#endif
