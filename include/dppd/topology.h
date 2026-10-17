#ifndef DPPD_TOPOLOGY_H
#define DPPD_TOPOLOGY_H

#include <stdbool.h>
#include <stdint.h>
#include "dppd/config.h"

enum dppd_endpoint_kind {
    DPPD_ENDPOINT_ETHDEV = 0,
    DPPD_ENDPOINT_REPRESENTOR,
};

/** 端口种类和被代表的角色分开记录，普通 ethdev 不会自动被当成 PF 或物理上联口 */
enum dppd_endpoint_role {
    DPPD_ENDPOINT_ROLE_UNKNOWN = 0,
    DPPD_ENDPOINT_ROLE_UPLINK,
    DPPD_ENDPOINT_ROLE_PF,
    DPPD_ENDPOINT_ROLE_VF,
    DPPD_ENDPOINT_ROLE_SF,
};

enum dppd_endpoint_role_source {
    DPPD_ENDPOINT_ROLE_SOURCE_UNKNOWN = 0,
    DPPD_ENDPOINT_ROLE_SOURCE_MLX5_PHYS_PORT_NAME,
};

struct dppd_endpoint {
    uint16_t ethdev_port_id;
    enum dppd_endpoint_kind kind;
    bool has_switch_domain;
    uint16_t switch_domain_id;
    uint16_t switch_port_id;
    char switch_name[64];
    char driver_name[64];
    /** 身份信息缺失时保留 UNKNOWN 和错误码，不根据交换端口号猜测 PF、VF 或 SF */
    enum dppd_endpoint_role role;
    enum dppd_endpoint_role_source role_source;
    int32_t role_error;
    uint32_t if_index;
    bool has_controller;
    bool has_pf;
    bool has_function;
    bool has_physical_port;
    uint32_t controller_id;
    uint32_t pf_id;
    uint32_t function_id;
    uint32_t physical_port_id;
    char interface_name[16];
    char device_name[64];
    char phys_port_name[64];
};

struct dppd_topology {
    uint16_t nb_endpoints;
    struct dppd_endpoint endpoints[DPPD_MAX_PORTS];
};

/** 读取器必须核对当前网络命名空间、接口编号和 PCI 设备，不能只凭同名 sysfs 目录认领接口 */
struct dppd_topology_identity {
    uint32_t if_index;
    char interface_name[16];
    char device_name[64];
    char phys_port_name[64];
};

typedef int (*dppd_topology_identity_reader)(uint32_t if_index, const char *device_name,
    struct dppd_topology_identity *identity, void *opaque);

int dppd_topology_discover(const struct dppd_config *cfg, struct dppd_topology *topology);
int dppd_topology_discover_with_reader(const struct dppd_config *cfg, struct dppd_topology *topology,
    dppd_topology_identity_reader reader, void *opaque);
/**
 * 两份拓扑都必须恰好包含选定端口集合，逐字段核对身份，忽略数组顺序和结构体填充
 * 未知角色与可选编号只原样比较，不能据此补出 PF/VF/SF 身份
 * 设备名和驱动缺失时返回 ENODATA，重复拓扑返回 EUCLEAN，身份变化返回 EXDEV
 * 该接口核对调用方提供的观察值，不能代替真实发现或证明硬件复位已经完成
 */
int dppd_topology_validate_identity(const struct dppd_topology *expected, const struct dppd_topology *candidate,
    const uint16_t *ports, uint16_t count);
/**
 * 先发现到临时对象，只有全部端口身份核对通过才写出结果，失败保持 output 的全部字节
 * output 可与 expected 相同，但调用方仍应先停止 worker 并核对设备事件序号再发布
 */
int dppd_topology_rediscover_verified(const struct dppd_config *cfg, const struct dppd_topology *expected,
    struct dppd_topology *output);
int dppd_topology_rediscover_verified_with_reader(const struct dppd_config *cfg, const struct dppd_topology *expected,
    struct dppd_topology *output, dppd_topology_identity_reader reader, void *opaque);
/** 纯解析接口只检查可信观察值的语法，调用方仍须证明该名称属于当前端口 */
int dppd_topology_resolve_role(struct dppd_endpoint *endpoint, const char *driver_name, bool is_representor,
    const char *phys_port_name);
/** 管理客户端共用名称映射，无需链接 DPDK 或读取进程内的设备对象 */
static inline const char *dppd_endpoint_role_name(enum dppd_endpoint_role role)
{
    switch (role) {
    case DPPD_ENDPOINT_ROLE_UPLINK:
        return "uplink";
    case DPPD_ENDPOINT_ROLE_PF:
        return "pf";
    case DPPD_ENDPOINT_ROLE_VF:
        return "vf";
    case DPPD_ENDPOINT_ROLE_SF:
        return "sf";
    case DPPD_ENDPOINT_ROLE_UNKNOWN:
        return "unknown";
    }
    return "unknown";
}

static inline const char *dppd_endpoint_role_source_name(enum dppd_endpoint_role_source source)
{
    return source == DPPD_ENDPOINT_ROLE_SOURCE_MLX5_PHYS_PORT_NAME ? "mlx5-phys-port-name" : "unknown";
}
const struct dppd_endpoint *dppd_topology_find(const struct dppd_topology *topology, uint16_t ethdev_port_id);
void dppd_topology_dump(const struct dppd_topology *topology);

#endif
