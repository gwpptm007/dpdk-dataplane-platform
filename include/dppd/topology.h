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
    /** 基础核对失败时记录读取错误，否则记录角色解析结果，非 mlx5 成功仍为 ENOTSUP */
    int32_t role_error;
    /** PMD 提供的接口编号是原始观察，不能仅凭编号认领当前网络命名空间的接口 */
    uint32_t if_index;
    bool has_controller;
    bool has_pf;
    bool has_function;
    bool has_physical_port;
    uint32_t controller_id;
    uint32_t pf_id;
    uint32_t function_id;
    uint32_t physical_port_id;
    /** 正式 discover 只在 netns、sysfs、PCI 和二次读取全部一致后发布接口名 */
    char interface_name[16];
    char device_name[64];
    /** 正式 discover 只发布核对一致的名称，名称缺失不妨碍通用身份，但不能证明角色 */
    char phys_port_name[64];
};

struct dppd_topology {
    uint16_t nb_endpoints;
    struct dppd_endpoint endpoints[DPPD_MAX_PORTS];
};

/**
 * 读取器成功返回前必须核对当前 netns、接口编号、PCI 和二次观察，不能凭同名目录认领接口
 * phys_port_name 可以为空，若任一观察存在该名称则各处完整值必须一致
 * 失败时填写的原始字符串不作为可信身份发布，调用方不能用自定义读取器绕过这些前置条件
 */
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
