#ifndef DPPD_RECOVERY_WATCH_H
#define DPPD_RECOVERY_WATCH_H

#include "dppd/recovery_guard.h"

/**
 * 在读取内核之前订阅事件，期间发生过变化就一直保留，不因状态恢复原样而清除
 * 这只是一次观察窗口的变化记录，不能证明订阅之前的接口历史，也不能阻止后续修改
 */
struct dppd_recovery_watch {
    int fd, error;
    bool link_changed, tc_changed;
    /** 生命周期模式允许普通链路状态和 TC 更新，但删除、改名、身份冲突仍使会话失效 */
    bool lifetime_only;
    /** 受控清理允许载波等链路状态通知，但 TC 变化仍使本次完整查询失效 */
    bool identity_only_links;
    uint32_t count;
    struct {
        uint32_t ifindex;
        char ifname[DPPD_RECOVERY_IFNAME_SIZE];
    } links[DPPD_MAX_PORTS];
};

/** watch 首次使用时将 fd 初始化为 -1，订阅失败必须放弃本次计划的候选 */
int dppd_recovery_watch_open(struct dppd_recovery_watch *watch, const struct dppd_recovery_record *record);
int dppd_recovery_watch_open_lifetime(struct dppd_recovery_watch *watch, const struct dppd_recovery_record *record);
/** 非阻塞读取已到达的事件，0 仅表示暂未观察到变化，事件丢失或解析失败会保持错误 */
int dppd_recovery_watch_check(struct dppd_recovery_watch *watch);
/** 关闭不会清除变化和错误信息，供调用方输出最终诊断 */
void dppd_recovery_watch_close(struct dppd_recovery_watch *watch);

#endif
