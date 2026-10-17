#ifndef DPPD_RECOVERY_SCOPE_H
#define DPPD_RECOVERY_SCOPE_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

#define DPPD_SCOPE_ENV "DPPD_SCOPE_FD"
#define DPPD_SCOPE_INHERITED_FD 3
#define DPPD_SCOPE_MESSAGE_SIZE 96U
#define DPPD_SCOPE_TIMEOUT_MS 1000U

enum dppd_scope_role { DPPD_SCOPE_MONITOR = 1, DPPD_SCOPE_BUSINESS, DPPD_SCOPE_RECOVERY };

/** 权限证据只在活着的 root 启动器和匿名通道中存在，不能从旧文件或环境字符串重建 */
struct dppd_recovery_scope {
    int fd, error;
    bool enabled;
    enum dppd_scope_role role;
    pid_t supervisor;
    uint8_t id[16];
    uint64_t netns_device, netns_inode;
};

/** 由启动器在新建的专用网络命名空间内生成，宿主机管理员和启动器属于信任边界 */
struct dppd_scope_authority {
    uint8_t id[16];
    uint64_t netns_device, netns_inode;
    uid_t uid;
    gid_t gid;
    pid_t supervisor;
};

/** root 子进程先把授权通道放到 fd 3，此函数关闭其他继承描述符并在 exec 前降权 */
int dppd_scope_child_prepare(uid_t uid, gid_t gid, bool writer, pid_t supervisor);
/** 未配置环境变量时保持旧入口，配置后必须在 EAL 或任何工作线程启动前调用 */
int dppd_scope_open(struct dppd_recovery_scope *scope, enum dppd_scope_role role);
/** 核对新随机数、内核对端凭据和当前命名空间，任何失败在本客户端永久锁存 */
int dppd_scope_check(struct dppd_recovery_scope *scope);
void dppd_scope_close(struct dppd_recovery_scope *scope);

/** 启动器捕获专用命名空间身份，拒绝使用宿主机初始网络命名空间 */
int dppd_scope_authority_init(struct dppd_scope_authority *authority, uid_t uid, gid_t gid);
/** pair[0] 留给父进程接收真实发送凭据，pair[1] 仅交给一个指定角色的子进程 */
int dppd_scope_socketpair(int pair[2]);
/** 启动器先核对进程退出及阶段，再回复该次请求，error 为零才表示当前阶段允许此角色 */
int dppd_scope_reply(int fd, const struct dppd_scope_authority *authority, pid_t child, enum dppd_scope_role role,
    int error);

#endif
