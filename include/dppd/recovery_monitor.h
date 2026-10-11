#ifndef DPPD_RECOVERY_MONITOR_H
#define DPPD_RECOVERY_MONITOR_H

#include "dppd/recovery_guard.h"

enum dppd_monitor_state { DPPD_MONITOR_ACTIVE = 1, DPPD_MONITOR_INVALID, DPPD_MONITOR_STOPPED };
enum dppd_monitor_reason {
    DPPD_MONITOR_NONE,
    DPPD_MONITOR_LINK_CHANGED,
    DPPD_MONITOR_OBSERVER_LOST,
    DPPD_MONITOR_RECORD_REPLACED,
    DPPD_MONITOR_STOP_REQUESTED
};

/** 独立证据只描述接口观察会话，不替代恢复文件，也不表示允许删除规则 */
struct dppd_monitor_record {
    uint8_t session[16];
    uint64_t sequence, recovery_device, recovery_inode, baseline_revision, baseline_attempt;
    uint64_t started_ns, updated_ns;
    uint32_t pid, count;
    enum dppd_monitor_state state;
    enum dppd_monitor_reason reason;
    int error;
    char recovery_path[DPPD_STATE_PATH_CAPACITY];
    struct {
        uint16_t port_id;
        struct dppd_recovery_identity identity;
    } ports[DPPD_MAX_PORTS];
};

struct dppd_monitor_store {
    int fd;
    bool faulted;
    char path[DPPD_STATE_PATH_CAPACITY];
    struct dppd_monitor_record record;
};

/** store 先设 fd 为 -1，只新建会话文件，不能用重启覆盖中断历史 */
int dppd_monitor_create(struct dppd_monitor_store *store, const char *path, const struct dppd_monitor_record *record);
/** 终态只允许写入一次，落盘失败后锁存故障，不能继续报告正在监护 */
int dppd_monitor_finish(struct dppd_monitor_store *store, enum dppd_monitor_reason reason, int error);
/** 只读检查完整编码，writer_locked 只表示本次检查时文件锁被占用，不证明进程仍在工作 */
int dppd_monitor_read(const char *path, struct dppd_monitor_record *record, bool *writer_locked);
void dppd_monitor_close(struct dppd_monitor_store *store);
const char *dppd_monitor_reason_name(enum dppd_monitor_reason reason);

#endif
