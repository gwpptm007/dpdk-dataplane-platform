#ifndef DPPD_CONFIG_H
#define DPPD_CONFIG_H

#include <stdbool.h>
#include <stdint.h>

#define DPPD_MAX_PORTS 16U
#define DPPD_MAX_WORKERS 64U
#define DPPD_MAX_BURST 256U
#define DPPD_CONTROL_SOCKET_PATH_CAPACITY 108U
#define DPPD_STATE_PATH_CAPACITY 4096U
#define DPPD_DEFAULT_RULE_CAPACITY 1024U
#define DPPD_MAX_DNAT_RULES 32U
#define DPPD_ND6_LOCAL_LIMIT 32U

/** 同一端口使用同一组 VLAN，可配置多个本机地址，启用前逐个执行重复地址检测 */
struct dppd_nd6_local {
    /** 本机 IPv6 地址所属的 DPDK 网口编号 */
    uint16_t port_id;
    /** VLAN ID，双层标签时依次为外层、内层，仅前 vlan_depth 项有效 */
    uint16_t vlan[2];
    /** 本机 IPv6 地址的 16 个网络字节，用作 ND 请求源地址并执行重复地址检测 */
    uint8_t address[16];
    /** VLAN 标签层数：0 无标签，1 单层，2 双层 */
    uint8_t vlan_depth;
};

/** 出站 SNAT 配置在启动时确定，不修改内核接口地址，也不把易失会话写进规则快照 */
struct dppd_snat_config {
    bool enabled;
    uint16_t inside_port, outside_port, first_port, last_port;
    uint32_t address_be;
    /** IPv6 使用完整地址，两个地址族各有自己的端口池 */
    bool ipv6;
    uint8_t address6[16];
};

/** 静态目的转换按外口、协议、公网地址和端口段匹配，内网端口按相同偏移一一对应 */
struct dppd_dnat_rule {
    uint32_t public_address_be, private_address_be;
    uint16_t outside_port, inside_port, public_first, public_last, private_first;
    uint8_t protocol;
    bool ipv6;
    uint8_t public_address6[16], private_address6[16];
};

/** 程序启动配置：描述收发资源、可选协议功能和文件路径，由配置解析器填写，runtime 按此创建资源 */
struct dppd_config {
    /** 使用的 DPDK 网口 ID 列表，默认 [0, 1]；未启用路由时，相邻两项配对，如 0↔1、2↔3 */
    uint16_t ports[DPPD_MAX_PORTS];
    /** ports 中有效网口的数量，默认 2，最多 DPPD_MAX_PORTS 个 */
    uint16_t nb_ports;
    /** 每个网口的 RX/TX 队列数，也是 worker 数量，默认 1；每个 worker 处理各网口的同编号队列 */
    uint16_t nb_queues;
    /** 每次 RX burst 最多接收多少个包，默认 32，范围 1～DPPD_MAX_BURST；不要求凑满才处理 */
    uint16_t burst_size;
    /** 每个使用的 NUMA 节点创建多少个 mbuf 对象，默认 32768；这是对象数量，不是大页数量 */
    uint32_t mbufs_per_socket;
    /** 每个 lcore 的内存池本地缓存规模，单位为 mbuf 个数，默认 256，0 表示关闭缓存 */
    uint16_t mbuf_cache;
    /** 控制台定期输出统计的间隔，单位毫秒，默认 1000 */
    uint32_t stats_period_ms;
    /** worker 无进度的超时阈值，单位毫秒；受管理模式默认 5000，普通模式强制为 0，表示关闭 */
    uint32_t worker_timeout_ms;
    /** 启动 worker 后运行多少秒再退出，默认 0，表示持续运行直到收到停止信号或发生错误 */
    uint32_t duration_s;

    /** 控制面最多保存多少条规则，默认 DPPD_DEFAULT_RULE_CAPACITY；不代表网卡硬件规则容量 */
    uint32_t rule_capacity;
    /** 连接跟踪最多容纳多少条会话，默认 0 表示禁用；启用时要求配置三层路由 */
    uint32_t conntrack_capacity;
    /** 已建立会话的空闲超时，单位秒，默认 300；仅启用连接跟踪时使用 */
    uint32_t conntrack_timeout_s;
    /** IPv4 源地址转换配置，包括内外网口、公网地址和转换端口池，默认不启用 */
    struct dppd_snat_config snat;
    /** IPv6 源地址转换配置，使用独立于 IPv4 的地址和端口池，默认不启用 */
    struct dppd_snat_config snat6;
    /** dnat 数组中有效映射的数量，IPv4/IPv6 合计，默认 0 */
    uint32_t dnat_count;
    /** 静态目的地址和端口转换规则，只读取前 dnat_count 项，最多 DPPD_MAX_DNAT_RULES 条 */
    struct dppd_dnat_rule dnat[DPPD_MAX_DNAT_RULES];
    /** nd6 数组中本机 IPv6 地址配置的数量，默认 0 表示不启用动态邻居发现 */
    uint32_t nd6_count;
    /** IPv6 邻居可达状态的基础保持时间，单位毫秒，默认 30000；启用 ND 时使用 */
    uint32_t nd6_reachable_ms;
    /** 本机 IPv6 地址及所属网口、VLAN 配置，只读取前 nd6_count 项；启用时要求三层路由 */
    struct dppd_nd6_local nd6[DPPD_ND6_LOCAL_LIMIT];

    /** 接收 dppctl 请求的 Unix socket 路径，默认 /tmp/dppd-control.sock */
    char control_socket[DPPD_CONTROL_SOCKET_PATH_CAPACITY];
    /** 普通模式的规则配置文件路径，空字符串表示不保存；非空时启动恢复，变更后保存 */
    char state_path[DPPD_STATE_PATH_CAPACITY];
    /** 受管理模式的完整配置文件路径，非空时进入 dppd_managed_run，与普通 state/routing/recovery 路径互斥 */
    char flow_state_path[DPPD_STATE_PATH_CAPACITY];
    /** 显式启用受管理双栈路由，路由与静态邻居保存到 flow_state_path，不另开路由文件 */
    bool managed_routing;
    /** 普通模式的双栈路由与静态邻居文件路径，非空时启用独立路由控制面 */
    char routing_path[DPPD_STATE_PATH_CAPACITY];
    /** 硬件安装恢复保护记录的文件路径，与 state_path 配合，防止异常退出后未经核对就重装；默认关闭 */
    char recovery_path[DPPD_STATE_PATH_CAPACITY];
    /** 独立监护会话的文件路径，需同时设置 recovery_path；启用后启动和硬件安装前必须检查监护状态 */
    char recovery_monitor[DPPD_STATE_PATH_CAPACITY];

    /** 是否要求 TAP 驱动提供规则归属标识，默认 false；需要适配后的驱动及 recovery_path */
    bool tap_owner_cookie;
    /** 是否启用网口混杂接收模式，默认 false，由设备初始化调用 PMD 设置 */
    bool promiscuous;
    /** 是否初始化 DPDK pdump 抓包服务，默认 false；启用服务不等于自动将报文写入文件 */
    bool enable_pdump;
};

enum dppd_config_result {
    DPPD_CONFIG_OK = 0,
    DPPD_CONFIG_HELP = 1,
    DPPD_CONFIG_ERROR = -1,
};

void dppd_config_defaults(struct dppd_config *cfg);
/** 两种路由入口共用收发准备逻辑，判断时不把统一配置文件伪装成旧路由文件 */
bool dppd_config_uses_routing(const struct dppd_config *cfg);
/** 共用同一份 NAT 校验，直接构造配置和命令行启动都不能绕过冲突检查 */
int dppd_nat_validate(const struct dppd_snat_config *snat, const struct dppd_dnat_rule *dnat, uint32_t count);
int dppd_config_parse(int argc, char **argv, struct dppd_config *cfg);
int dppd_config_validate(const struct dppd_config *cfg, char *error, uint32_t error_len);
void dppd_config_print_usage(const char *program);
void dppd_config_dump(const struct dppd_config *cfg);

#endif
