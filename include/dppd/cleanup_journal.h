#ifndef DPPD_CLEANUP_JOURNAL_H
#define DPPD_CLEANUP_JOURNAL_H

#include "dppd/recovery_guard.h"
#include "dppd/recovery_scope.h"

#define DPPD_CLEANUP_JOURNAL_SIZE 4224U

enum dppd_cleanup_state { DPPD_CLEANUP_PENDING = 1, DPPD_CLEANUP_DELETING, DPPD_CLEANUP_REMOVED, DPPD_CLEANUP_FAILED };

/** 日志只保存清理进展，不授予删除权限，实际删除仍须重新核对作用域、监护和内核对象 */
struct dppd_cleanup_journal {
    int fd;
    bool faulted;
    uint64_t sequence, recovery_device, recovery_inode, revision;
    uint32_t recovery_crc, count;
    uint8_t scope[16], session[16];
    char path[DPPD_STATE_PATH_CAPACITY];
    struct {
        uint64_t attempt;
        enum dppd_cleanup_state state;
        int error;
    } entries[DPPD_RECOVERY_ATTEMPT_LIMIT];
};

/** 按原恢复版本选择日志，校验当前作用域和监护绑定，不采用其他事故留下的日志 */
int dppd_cleanup_journal_open(struct dppd_cleanup_journal *journal, const struct dppd_recovery_guard *guard,
    const struct dppd_recovery_scope *scope);
/** 先完整同步临时文件再原子替换，已删除状态不可回退，任何写盘错误都锁存 */
int dppd_cleanup_journal_set(struct dppd_cleanup_journal *journal, uint32_t index, enum dppd_cleanup_state state,
    int error);
/** 核对日志路径、inode 和完整内容，原地破坏或文件替换都锁存为失败 */
int dppd_cleanup_journal_check(struct dppd_cleanup_journal *journal);
void dppd_cleanup_journal_close(struct dppd_cleanup_journal *journal);

#endif
