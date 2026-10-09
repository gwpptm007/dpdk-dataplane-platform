#ifndef DPPD_RECOVERY_GUARD_H
#define DPPD_RECOVERY_GUARD_H

#include <stdbool.h>
#include <stdint.h>
#include "dppd/config.h"
#include "dppd/tap_owner.h"

#define DPPD_RECOVERY_DEVICE_SIZE 128U
#define DPPD_RECOVERY_DRIVER_SIZE 64U
#define DPPD_RECOVERY_IFNAME_SIZE 16U
#define DPPD_RECOVERY_BOOT_SIZE 40U
#define DPPD_RECOVERY_ATTEMPT_LIMIT 256U
#define DPPD_RECOVERY_KIND_SIZE 32U

/** 内核坐标是观察线索，不包含跨进程有效的指针，也不构成对象所有权证明 */
struct dppd_recovery_filter {
    uint32_t parent, handle, chain;
    uint16_t priority, protocol;
    char kind[DPPD_RECOVERY_KIND_SIZE];
    /** 仅运行时观察保存动作标识，候选坐标编码不把它当作创建前的持久化意图 */
    bool cookie_valid;
    uint8_t cookie[DPPD_TAP_COOKIE_SIZE];
};

enum dppd_recovery_phase {
    DPPD_RECOVERY_INTENT = 1,
    DPPD_RECOVERY_CREATED,
    DPPD_RECOVERY_CREATE_FAILED,
    DPPD_RECOVERY_REMOVED
};

enum dppd_recovery_evidence {
    DPPD_EVIDENCE_NONE,
    DPPD_EVIDENCE_SINGLE_ADDITION,
    DPPD_EVIDENCE_AMBIGUOUS,
    DPPD_EVIDENCE_UNAVAILABLE
};

/** 每次尝试都有不复用的编号，同一业务版本补偿重建时也必须重新登记 */
struct dppd_recovery_attempt {
    uint64_t id, rule_id, generation;
    uint16_t port_id;
    enum dppd_recovery_phase phase;
    enum dppd_recovery_evidence evidence;
    int32_t create_error, remove_error, observation_error;
    struct dppd_recovery_filter candidate;
    /** v4 在驱动调用前同步保存，旧格式和普通模式始终为全零 */
    uint8_t owner_cookie[DPPD_TAP_COOKIE_SIZE];
};

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
    /** 只复用已成功删除的槽位，仍可能有残留的记录不可被覆盖 */
    uint64_t last_attempt;
    uint32_t attempt_count;
    struct dppd_recovery_attempt attempts[DPPD_RECOVERY_ATTEMPT_LIMIT];
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
/** 端口已登记后为本次驱动调用先写入意图，写盘失败或记录满时不能进入驱动 */
int dppd_recovery_guard_begin(struct dppd_recovery_guard *guard, uint16_t port_id,
    uint64_t rule_id, uint64_t generation, uint64_t *attempt);
/** 为已核对身份的 TAP 生成随机标识，写盘成功才向驱动调用方返回 */
int dppd_recovery_guard_begin_owned(struct dppd_recovery_guard *guard, uint16_t port_id,
    uint64_t rule_id, uint64_t generation, uint64_t *attempt, uint8_t cookie[DPPD_TAP_COOKIE_SIZE]);
/** 保存真实创建结果与可选候选坐标，失败时调用方仍持有 handle 并执行原事务回滚 */
int dppd_recovery_guard_created(struct dppd_recovery_guard *guard, uint64_t attempt, int create_error,
    enum dppd_recovery_evidence evidence, int observation_error, const struct dppd_recovery_filter *candidate);
/** 删除结果只更新记录，不能让记录失败阻止已知 handle 的实际清理 */
int dppd_recovery_guard_removed(struct dppd_recovery_guard *guard, uint64_t attempt, int remove_error);
/** 只释放文件锁，不删除记录、不清除待核对状态 */
void dppd_recovery_guard_close(struct dppd_recovery_guard *guard);

#endif
