#ifndef DPPD_ND6_H
#define DPPD_ND6_H

#include "dppd/config.h"
#include "dppd/packet.h"

#define DPPD_ND6_LIMIT 256U
#define DPPD_ND6_PENDING_LIMIT 128U
#define DPPD_ND6_PAGE_SIZE 8U

struct dppd_nd6;
enum dppd_nd6_state {
    /** 尚无下一跳 MAC，等待邻居响应 */
    DPPD_ND6_INCOMPLETE = 1,
    /** 已确认可达，可在有效期内使用缓存 MAC */
    DPPD_ND6_REACHABLE,
    /** 可达确认已过期，MAC 仍保留，使用时转入 DELAY */
    DPPD_ND6_STALE,
    /** 暂时沿用 MAC，等待 5 秒后进入主动探测 */
    DPPD_ND6_DELAY,
    /** 沿用缓存 MAC 发送数据，同时通过单播 NS 探测邻居 */
    DPPD_ND6_PROBE,
    /** 探测次数耗尽，停止提供 MAC，冷却期后可重新解析 */
    DPPD_ND6_FAILED
};
enum dppd_nd6_local_state { DPPD_ND6_TENTATIVE, DPPD_ND6_PREFERRED, DPPD_ND6_DUPLICATE };

/** 一个动态邻居，例如网口 1 上 fe80::1 对应 MAC 02:11:22:33:44:55 */
struct dppd_nd6_entry {
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
    /** 邻居 IPv6 地址的 16 个网络字节，例如 fe80::1 以 fe 80 开头、01 结尾 */
    uint8_t address[16];
    /** 邻居的 6 字节 MAC，供转发时填写以太网目的地址；尚未解析时全为 0 */
    uint8_t mac[6];
    /** 当前邻居状态，取值见 dppd_nd6_state，决定 MAC 是否可用及下一步定时操作 */
    uint8_t state;
    /** 本轮 NS 探测尝试次数，发送失败也计数，达到 3 次后在下次到期时判定失败 */
    uint8_t probes;
};
/** ND 管理状态快照，数量反映当前占用，事件计数从创建管理表起累计 */
struct dppd_nd6_status {
    /** 是否创建了 ND 管理表，未启用时状态查询返回全零结果 */
    bool enabled;
    /** 当前有效邻居记录数，包含尚在解析和解析失败的记录 */
    uint32_t active;
    /** 当前等待数组中持有的报文数 */
    uint32_t pending;
    /** 已配置的本机 IPv6 地址数量，也是 locals 的有效项数 */
    uint32_t local_count;
    /** 可达有效期的基准毫秒数，实际期限在约 0.5 到 1.5 倍之间随机选择 */
    uint32_t reachable_ms;
    /** 邻居或本机地址状态等变化时递增，分页和清空接口据此检查查询版本是否过期 */
    uint64_t revision;
    /** 成功提交发送的 NS 请求数，包含本机重复地址检测和邻居探测 */
    uint64_t solicitations;
    /** 成功提交发送的 NA 通告数 */
    uint64_t advertisements;
    /** 被 ND 输入处理判为无效的报文或冲突事件数 */
    uint64_t invalid;
    /** NS 或 NA 发送回调返回失败的次数，未提供回调也计入 */
    uint64_t tx_errors;
    /** 等待包入队失败或 take 取出失效包的次数，不包含 worker 的全部丢包 */
    uint64_t queue_drops;
    /** 收到带 Solicited 标志的有效 NA 并确认可达的次数，同一邻居可以多次累计 */
    uint64_t resolved;
    /** 邻居因探测次数耗尽转入 FAILED 的次数 */
    uint64_t failed;
    /** 导出状态时填充的本机地址列表，不包含内部计时器和 MAC */
    struct {
        /** 本机 IPv6 地址、所属网口和 VLAN 配置 */
        struct dppd_nd6_local configuration;
        /** 本机地址检测结果，取值见 dppd_nd6_local_state */
        uint8_t state;
    } locals[DPPD_ND6_LOCAL_LIMIT];
};
/** 动态邻居分页查询结果，按记录 ID 升序返回，每页最多 8 条 */
struct dppd_nd6_page {
    /** 本页查询版本，继续查询时传回，用于检测分页过程中状态是否变化 */
    uint64_t revision;
    /** 本页最后一条记录的 ID，下次作为 after 传入；空页为 0 */
    uint64_t next_id;
    /** 本页 entries 中的有效记录数 */
    uint32_t count;
    /** 是否还有未放入本页的记录 */
    bool more;
    /** 邻居记录的副本，查询结果不持有内部缓存的指针 */
    struct dppd_nd6_entry entries[DPPD_ND6_PAGE_SIZE];
};
/** 等待下一跳 MAC 的报文记录，保存 mbuf 指针及取出后重新处理所需的信息 */
struct dppd_nd6_pending {
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
/** 回调只借用当前小报文，成功表示已交给驱动，失败时由 ND 定时器按预算重试 */
typedef int (*dppd_nd6_send)(void *context, uint16_t port, const uint8_t *wire, uint16_t length);
int dppd_nd6_create(struct dppd_nd6 **result, const struct dppd_config *configuration,
    const uint8_t macs[DPPD_MAX_PORTS][6], uint64_t now_ms);
/** 销毁前先停止全部工作线程，尚未发出的首包在这里归还内存池 */
void dppd_nd6_destroy(struct dppd_nd6 *table);
bool dppd_nd6_port_ready(struct dppd_nd6 *table, uint16_t port);
/** 只消费本机 ND 控制报文，不能把收到的 NA 当成跨接口可转发的数据 */
bool dppd_nd6_input(struct dppd_nd6 *table, uint16_t port, const struct rte_mbuf *mbuf,
    const struct dppd_packet *packet, uint64_t now_ms, dppd_nd6_send send, void *context);
void dppd_nd6_tick(struct dppd_nd6 *table, uint64_t now_ms, dppd_nd6_send send, void *context);
int dppd_nd6_resolve(struct dppd_nd6 *table, uint16_t port, const uint8_t address[16], const struct dppd_packet *packet,
    uint64_t now_ms, uint8_t mac[6]);
/** 只有入队成功才接管 mbuf，等待包按入口队列归还，并保持最初的超时边界 */
int dppd_nd6_hold(struct dppd_nd6 *table, uint16_t port, const uint8_t address[16], struct dppd_nd6_pending pending,
    uint64_t now_ms);
/** 取出本队列一个待重试或已失效的包，返回 true 时输出记录并将 mbuf 交回调用方 */
bool dppd_nd6_take(struct dppd_nd6 *table, uint16_t queue, uint64_t now_ms, struct dppd_nd6_pending *pending);
void dppd_nd6_status(struct dppd_nd6 *table, struct dppd_nd6_status *status);
int dppd_nd6_list(struct dppd_nd6 *table, uint64_t after, uint64_t revision, struct dppd_nd6_page *page);
int dppd_nd6_flush(struct dppd_nd6 *table, uint64_t revision);

#endif
