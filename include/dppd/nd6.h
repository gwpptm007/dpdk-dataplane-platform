#ifndef DPPD_ND6_H
#define DPPD_ND6_H

#include "dppd/config.h"
#include "dppd/packet.h"

#define DPPD_ND6_LIMIT 256U
#define DPPD_ND6_PENDING_LIMIT 128U
#define DPPD_ND6_PAGE_SIZE 8U

struct dppd_nd6;
enum dppd_nd6_state {
    DPPD_ND6_INCOMPLETE = 1,
    DPPD_ND6_REACHABLE,
    DPPD_ND6_STALE,
    DPPD_ND6_DELAY,
    DPPD_ND6_PROBE,
    DPPD_ND6_FAILED
};
enum dppd_nd6_local_state { DPPD_ND6_TENTATIVE, DPPD_ND6_PREFERRED, DPPD_ND6_DUPLICATE };

struct dppd_nd6_entry {
    uint64_t id, changed_ms, due_ms, used_ms;
    uint16_t port_id;
    uint8_t address[16], mac[6], state, probes;
};
struct dppd_nd6_status {
    bool enabled;
    uint32_t active, pending, local_count, reachable_ms;
    uint64_t revision, solicitations, advertisements, invalid, tx_errors, queue_drops, resolved, failed;
    struct {
        struct dppd_nd6_local configuration;
        uint8_t state;
    } locals[DPPD_ND6_LOCAL_LIMIT];
};
struct dppd_nd6_page {
    uint64_t revision, next_id;
    uint32_t count;
    bool more;
    struct dppd_nd6_entry entries[DPPD_ND6_PAGE_SIZE];
};
struct dppd_nd6_pending {
    struct rte_mbuf *mbuf;
    uint64_t expires_ms;
    uint16_t ingress, queue;
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
bool dppd_nd6_take(struct dppd_nd6 *table, uint16_t queue, uint64_t now_ms, struct dppd_nd6_pending *pending);
void dppd_nd6_status(struct dppd_nd6 *table, struct dppd_nd6_status *status);
int dppd_nd6_list(struct dppd_nd6 *table, uint64_t after, uint64_t revision, struct dppd_nd6_page *page);
int dppd_nd6_flush(struct dppd_nd6 *table, uint64_t revision);

#endif
