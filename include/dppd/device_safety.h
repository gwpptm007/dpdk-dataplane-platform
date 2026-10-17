#ifndef DPPD_DEVICE_SAFETY_H
#define DPPD_DEVICE_SAFETY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DPPD_DEVICE_SAFETY_MAX_INTERFACES 256U
#define DPPD_DEVICE_SAFETY_MAX_ADDRESSES 1024U
#define DPPD_DEVICE_SAFETY_BDF_SIZE 13U
#define DPPD_DEVICE_SAFETY_IFNAME_SIZE 16U

struct dppd_device_set;

struct dppd_device_safety_interfaces {
    uint32_t count, messages;
    uint32_t indices[DPPD_DEVICE_SAFETY_MAX_INTERFACES];
};

struct dppd_device_safety_address {
    uint32_t ifindex;
    int family;
    uint8_t address[16];
    char interface_name[DPPD_DEVICE_SAFETY_IFNAME_SIZE];
};

/** 只读依赖可替换供单测使用，正式入口始终使用内置的内核和 sysfs 读取器 */
struct dppd_device_safety_reader {
    void *context;
    int (*routes)(void *context, struct dppd_device_safety_interfaces *interfaces);
    int (*ssh)(void *context, struct dppd_device_safety_interfaces *interfaces);
    int (*interface_parent)(void *context, uint32_t ifindex, char parent[DPPD_DEVICE_SAFETY_BDF_SIZE]);
    int (*device_parent)(void *context, const char *name, char parent[DPPD_DEVICE_SAFETY_BDF_SIZE]);
};

/**
 * 在任何 stop/reset 之前立即调用，只观察当前命名空间的管理链路
 * 管理 PCI parent 重合返回 EACCES，身份或路由不完整绝不放行
 * 不停止设备、不改路由，也不证明驱动复位不会影响其他板卡或外部对象
 */
int dppd_devices_check_reset_safety(const struct dppd_device_set *devices);
int dppd_devices_check_reset_safety_with_reader(const struct dppd_device_set *devices,
    const struct dppd_device_safety_reader *reader);

/** 以下纯解析接口不读取宿主机，不能单独作为设备可以复位的证据 */
int dppd_device_safety_parse_routes(const void *data, size_t length, uint32_t sequence,
    struct dppd_device_safety_interfaces *interfaces, bool *done);
int dppd_device_safety_extract_bdf(const char *name, char output[DPPD_DEVICE_SAFETY_BDF_SIZE]);
int dppd_device_safety_pci_network_class(const char *text);
int dppd_device_safety_ssh_interfaces(const char *connection, const struct dppd_device_safety_address *addresses,
    uint32_t count, struct dppd_device_safety_interfaces *interfaces);

#endif
