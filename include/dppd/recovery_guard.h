#ifndef DPPD_RECOVERY_GUARD_H
#define DPPD_RECOVERY_GUARD_H

#include <stdbool.h>
#include <stdint.h>
#include "dppd/config.h"

#define DPPD_RECOVERY_DEVICE_SIZE 128U
#define DPPD_RECOVERY_DRIVER_SIZE 64U
#define DPPD_RECOVERY_IFNAME_SIZE 16U
#define DPPD_RECOVERY_BOOT_SIZE 40U

/**
 * TAP 的内核定位信息必须连同启动身份和网络命名空间保存，避免到另一个环境查同号接口
 * ifindex 为零表示没有可靠定位信息，不能仅凭 DPDK 的 net_tap 名称猜测内核接口
 */
struct dppd_recovery_identity {
    uint32_t ifindex;
    char ifname[DPPD_RECOVERY_IFNAME_SIZE];
    char boot_id[DPPD_RECOVERY_BOOT_SIZE];
    uint64_t netns_device;
    uint64_t netns_inode;
};

/**
 * 每个可能安装过 flow 的端口保留一条线索，不把进程内端口号当作跨进程设备身份
 * first_rule 和 first_generation 是本轮首次尝试，不是残留规则总表或实际对象数量
 */
struct dppd_recovery_port {
    uint16_t port_id;
    uint64_t first_rule;
    uint64_t first_generation;
    char device[DPPD_RECOVERY_DEVICE_SIZE];
    char driver[DPPD_RECOVERY_DRIVER_SIZE];
    struct dppd_recovery_identity identity;
};

/** 文件使用独立编码和校验和，不直接写出此 C 结构体，快照路径用于防止误配 */
struct dppd_recovery_record {
    uint32_t format;
    uint64_t revision;
    uint32_t count;
    char state_path[DPPD_STATE_PATH_CAPACITY];
    struct dppd_recovery_port ports[DPPD_MAX_PORTS];
};

/**
 * 整个进程持有同一个文件锁，关闭不会自动宣称已清理
 * 写入或同步失败后 faulted 保持为真，不能在退出时把不确定记录覆盖为干净
 */
struct dppd_recovery_guard {
    int fd;
    bool writable;
    bool faulted;
    char path[DPPD_STATE_PATH_CAPACITY];
    struct dppd_recovery_record record;
};

/** state_path 非空时允许首次创建并核对快照路径，为空时只打开已有记录供离线工具使用 */
int dppd_recovery_guard_open(struct dppd_recovery_guard *guard,
    const char *path, const char *state_path);
/** 必须在驱动 create 之前成功，首次涉及该端口时先写入并 fsync，失败就不得安装 */
int dppd_recovery_guard_prepare(struct dppd_recovery_guard *guard, uint16_t port_id,
    const char *device, const char *driver, uint64_t rule_id, uint64_t generation);
/** 有可靠 TAP 定位时与端口线索一次落盘，同一端口身份发生变化时拒绝继续安装 */
int dppd_recovery_guard_prepare_identity(struct dppd_recovery_guard *guard, uint16_t port_id,
    const char *device, const char *driver, uint64_t rule_id, uint64_t generation,
    const struct dppd_recovery_identity *identity);
/** 仅本进程已确认全部 flow 删除后调用，软件状态是否成功保存由原持久化机制另行报告 */
int dppd_recovery_guard_clean(struct dppd_recovery_guard *guard);
/** 离线确认外部清理已经完成，精确版本防止确认旧记录，此函数本身不执行设备清理 */
int dppd_recovery_guard_acknowledge(struct dppd_recovery_guard *guard, uint64_t revision);
/** 只释放文件锁，不删除记录、不清除待核对状态 */
void dppd_recovery_guard_close(struct dppd_recovery_guard *guard);

#endif
