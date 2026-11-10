#ifndef DPPD_ROUTING_H
#define DPPD_ROUTING_H

#include "dppd/ipv4_router.h"

#define DPPD_ROUTING_FILE_SIZE 11712U
#define DPPD_ROUTING_DUAL_FILE_SIZE 28096U

struct dppd_routing_service;

/** 一条网口与设备的对应记录，随路由文件保存；恢复时核对整条记录，防止端口编号复用 */
struct dppd_routing_binding {
    /** 当前 DPDK 网口编号，例如 0，与下方设备信息一起核对 */
    uint16_t port_id;
    /**
     * device：DPDK 设备名，例如 PCI 的 0000:03:00.0 或 TAP 的 net_tap0
     * driver：PMD 驱动名，例如 net_mlx5、net_tap
     * ifname：TAP 对应的 Linux 接口名，例如 dppd0；其他驱动在本路径中保持为空
     */
    char device[128], driver[64], ifname[16];
};

struct dppd_routing_status {
    bool enabled, dirty;
    uint8_t reserved[2];
    int32_t last_error;
    uint64_t generation, persisted_generation;
    uint32_t route_count, neighbor_count, route6_count, neighbor6_count;
};

/** 创建文件前检查规范路径，路由快照和锁不得覆盖其他存储文件或管理 socket */
int dppd_routing_check_paths(const char *path, const char *const *other_paths, size_t count);

/** 新文件保存版本一空表，已有文件完整核验后恢复，长期持有独立单实例锁 */
int dppd_routing_open(struct dppd_routing_service **service, const char *path,
    const struct dppd_routing_binding *bindings, uint16_t count);
/** 控制面停止调用且所有工作线程退出后关闭，不隐式覆盖存储不确定状态 */
int dppd_routing_close(struct dppd_routing_service *service);
struct dppd_ipv4_router *dppd_routing_router(struct dppd_routing_service *service);
int dppd_routing_get(struct dppd_routing_service *service, struct dppd_ipv4_configuration *configuration);
/** 所有服务接口由管理线程串行调用，失败不会发布候选表，保存错误另置 dirty 并返回 EUCLEAN */
int dppd_routing_replace(struct dppd_routing_service *service, uint64_t expected_generation,
    const struct dppd_ipv4_configuration *configuration);
/** 显式把当前活跃表重新同步到磁盘，成功才解除 dirty，原有版本不增加 */
int dppd_routing_flush(struct dppd_routing_service *service);
void dppd_routing_status(struct dppd_routing_service *service, struct dppd_routing_status *status);

#endif
