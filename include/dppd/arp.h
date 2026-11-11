#ifndef DPPD_ARP_H
#define DPPD_ARP_H

#include "dppd/config.h"
#include "dppd/packet.h"

#define DPPD_ARP_LOCAL_LIMIT 32U
/** 本机 IPv4 地址按网络字节保存，同一端口只允许一个 VLAN 域 */
struct dppd_arp_local {
    uint16_t port_id;
    uint8_t address[4], vlan_depth;
    uint16_t vlan[2];
};

#define DPPD_ARP_LIMIT 256U
#define DPPD_ARP_PENDING_LIMIT 128U
#define DPPD_ARP_PAGE_SIZE 8U

struct dppd_arp;

/** 只保存本机地址和可达计时配置，动态邻居、ACD 进度和等待报文都属于易失状态 */
struct dppd_arp_policy {
    uint32_t local_count, reachable_ms;
    struct dppd_arp_local locals[DPPD_ARP_LOCAL_LIMIT];
};

/** 设备准备完成后提供真实 MAC，端口绑定由容器保存，不从策略文件恢复旧 MAC */
struct dppd_arp_port {
    uint16_t port_id;
    uint8_t mac[6];
};
enum dppd_arp_state {
    /** 下一跳 MAC 尚未解析完成，等待邻居响应；此状态不提供缓存 MAC */
    DPPD_ARP_INCOMPLETE = 1,
    /** 已确认可达，可在有效期内使用缓存 MAC */
    DPPD_ARP_REACHABLE,
    /** 已有 MAC，但当前可达性未确认，使用时转入 DELAY */
    DPPD_ARP_STALE,
    /** 暂时沿用 MAC，等待 5 秒后进入主动探测 */
    DPPD_ARP_DELAY,
    /** 沿用缓存 MAC 发送数据，同时通过广播 ARP 重新确认邻居 */
    DPPD_ARP_PROBE,
    /** 探测次数耗尽，停止提供 MAC；5 秒冷却后再次 resolve 查询才重启解析 */
    DPPD_ARP_FAILED
};
enum dppd_arp_local_state { DPPD_ARP_TENTATIVE, DPPD_ARP_PREFERRED, DPPD_ARP_DUPLICATE };

/** 一个动态邻居，例如网口 1 上 192.0.2.1 对应 MAC 02:11:22:33:44:55 */
struct dppd_arp_entry {
    /** 邻居记录的唯一编号，0 表示空槽位，等待包通过该编号关联邻居 */
    uint64_t id;
    /** 最近一次写入邻居状态的毫秒时间点 */
    uint64_t changed_ms;
    /** 当前状态的毫秒期限，用于可达过期、延迟探测、探测重试或失败冷却 */
    uint64_t due_ms;
    /** 创建记录或最近一次 resolve 查询的毫秒时间，清理长期未用记录时参考 */
    uint64_t used_ms;
    /** 到达该邻居的网口编号，与 address 共同定位记录 */
    uint16_t port_id;
    /** 邻居 IPv4 地址的 4 个网络字节，例如 192.0.2.1 保存为 c0 00 02 01 */
    uint8_t address[4];
    /** 邻居的 6 字节 MAC，供转发时填写以太网目的地址；尚未解析时全为 0 */
    uint8_t mac[6];
    /** 当前邻居状态，取值见 dppd_arp_state，决定 MAC 是否可用及下一步定时操作 */
    uint8_t state;
    /** 本轮 ARP 请求尝试次数，发送失败也计数，达到 3 次后在下次到期时判定失败 */
    uint8_t probes;
};
/** ARP 管理状态快照，数量反映当前占用，事件计数从创建管理表起累计 */
struct dppd_arp_status {
    /** 是否启用 ARP，未创建表时返回全零，统一管理的关闭容器仍保留启动身份和版本 */
    bool enabled;
    /** 当前有效邻居记录数，包含尚在解析和解析失败的记录 */
    uint32_t active;
    /** 当前等待数组中持有的报文数 */
    uint32_t pending;
    /** 已配置的本机 IPv4 地址数量，也是 locals 的有效项数 */
    uint32_t local_count;
    /** 收到有效邻居响应后的可达有效期，单位为毫秒 */
    uint32_t reachable_ms;
    /** epoch 标识本次容器，重启会改变；revision 覆盖邻居、本机状态及等待队列变化 */
    uint64_t epoch, revision;
    /** 成功提交发送的 ARP 请求数，包含地址冲突探测、通告和邻居查询 */
    uint64_t requests;
    /** 成功提交发送的 ARP 应答数 */
    uint64_t replies;
    /** 被 ARP 输入处理判为格式或地址域无效的报文数 */
    uint64_t invalid;
    /** ARP 发送回调返回失败的次数，未提供回调也计入 */
    uint64_t tx_errors;
    /** 等待包入队失败或 take 取出失效包的次数，不包含 worker 的全部丢包 */
    uint64_t queue_drops;
    /** 正在解析或刷新时收到有效 ARP 并确认映射的次数，同一邻居可以多次累计 */
    uint64_t resolved;
    /** 邻居因探测次数耗尽转入 FAILED 的次数 */
    uint64_t failed;
    /** 导出状态时填充的本机地址列表，不包含内部计时器和 MAC */
    struct {
        /** 本机 IPv4 地址、所属网口和 VLAN 配置 */
        struct dppd_arp_local configuration;
        /** 本机地址检测结果，取值见 dppd_arp_local_state */
        uint8_t state;
    } locals[DPPD_ARP_LOCAL_LIMIT];
};
/** 动态邻居分页查询结果，按记录 ID 升序返回，每页最多 8 条 */
struct dppd_arp_page {
    /** 与本次启动绑定，不能把旧进程的游标用于新进程 */
    uint64_t epoch;
    /** 本页查询版本，继续查询时传回，用于检测分页过程中状态是否变化 */
    uint64_t revision;
    /** 本页最后一条记录的 ID，下次作为 after 传入；空页为 0 */
    uint64_t next_id;
    /** 本页 entries 中的有效记录数 */
    uint32_t count;
    /** 是否还有未放入本页的记录 */
    bool more;
    /** 邻居记录的副本，查询结果不持有内部缓存的指针 */
    struct dppd_arp_entry entries[DPPD_ARP_PAGE_SIZE];
};
/** 等待下一跳 MAC 的报文记录，保存 mbuf 指针及取出后重新处理所需的信息 */
struct dppd_arp_pending {
    /** 原始报文指针，hold 成功后由等待队列持有，take 取出后交回处理线程 */
    struct rte_mbuf *mbuf;
    /** 毫秒截止时间，首次入队传 0 时设置为当前时间加 5000，重试时沿用原期限 */
    uint64_t expires_ms;
    /** 原入口网口编号，取出后用它恢复 process_batch 所需的入口信息 */
    uint16_t ingress;
    /** 原处理队列编号，take 只向匹配的队列交回此包 */
    uint16_t queue;
    /** take 写入的失效标志，超时、邻居记录消失或解析失败时为真，取出后须丢弃 */
    bool expired;
};
/** 回调只借用当前小报文，成功表示已交给驱动，失败时由 ARP 定时器按预算重试 */
typedef int (*dppd_arp_send)(void *context, uint16_t port, const uint8_t *wire, uint16_t length);
int dppd_arp_policy_validate(const struct dppd_arp_policy *policy);
/** 统一管理模式始终返回稳定容器，local_count 和 reachable_ms 同时为零表示关闭 */
int dppd_arp_create_policy(struct dppd_arp **result, const struct dppd_arp_policy *policy,
    const struct dppd_arp_port *ports, uint16_t count, uint64_t now_ms);
/**
 * 先准备候选再锁表核对版本，只有明确允许重置才清理已有邻居和等待包
 * 正式在线更新先暂停包线程，避免 resolve 到 hold、take 到重试跨越两份策略
 * publish 在表锁内执行，不能重入 ARP 接口，失败保留全部旧状态
 */
int dppd_arp_replace(struct dppd_arp *table, const struct dppd_arp_policy *policy, bool reset_neighbors,
    uint64_t expected_epoch, uint64_t expected_revision, uint64_t now_ms, int (*publish)(void *), void *context);
/** 销毁前先停止全部工作线程，尚未发出的首包在这里归还内存池 */
void dppd_arp_destroy(struct dppd_arp *table);
/** 仅用于全部 worker 已停止后的结束清理，归还等待包，不受持久化失败或版本触顶限制 */
void dppd_arp_discard_pending(struct dppd_arp *table);
bool dppd_arp_port_ready(struct dppd_arp *table, uint16_t port);
/** 只消费本机 ARP 控制报文，不能把收到的 ARP 当成跨接口可转发的数据 */
bool dppd_arp_input(struct dppd_arp *table, uint16_t port, const struct rte_mbuf *mbuf,
    const struct dppd_packet *packet, uint64_t now_ms, dppd_arp_send send, void *context);
void dppd_arp_tick(struct dppd_arp *table, uint64_t now_ms, dppd_arp_send send, void *context);
int dppd_arp_resolve(struct dppd_arp *table, uint16_t port, const uint8_t address[4], const struct dppd_packet *packet,
    uint64_t now_ms, uint8_t mac[6]);
/** 只有入队成功才接管 mbuf，等待包按入口队列归还，并保持最初的超时边界 */
int dppd_arp_hold(struct dppd_arp *table, uint16_t port, const uint8_t address[4], struct dppd_arp_pending pending,
    uint64_t now_ms);
/** 取出本队列一个待重试或已失效的包，返回 true 时输出记录并将 mbuf 交回调用方 */
bool dppd_arp_take(struct dppd_arp *table, uint16_t queue, uint64_t now_ms, struct dppd_arp_pending *pending);
void dppd_arp_status(struct dppd_arp *table, struct dppd_arp_status *status);
int dppd_arp_list(struct dppd_arp *table, uint64_t after, uint64_t revision, struct dppd_arp_page *page);
int dppd_arp_list_guarded(struct dppd_arp *table, uint64_t after, uint64_t epoch, uint64_t revision,
    struct dppd_arp_page *page);
int dppd_arp_flush(struct dppd_arp *table, uint64_t revision);
/** 精确核对启动身份与版本，清缓存和仍由队列持有的包，不改变本机地址的检测结果 */
int dppd_arp_flush_guarded(struct dppd_arp *table, uint64_t epoch, uint64_t revision);

#endif
