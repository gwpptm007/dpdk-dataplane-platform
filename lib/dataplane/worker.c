#include "dppd/runtime.h"
#include "dppd/software_backend.h"
#include "dppd/ipv4_rewrite.h"
#include "dppd/ipv6.h"

#include <string.h>
#include <errno.h>
#include <rte_ethdev.h>
#include <rte_mbuf.h>

/** 释放仍归本线程持有的报文；已被发送队列接收的报文交给驱动管理，不能再次释放 */
static void free_packets(struct rte_mbuf **packets, uint16_t count)
{
    uint16_t i;

    for (i = 0; i < count; ++i)
        rte_pktmbuf_free(packets[i]);
}

/** 一个出口独立提交一批报文，发送归出口，发送前失效仍计入入口的出口不可用丢弃 */
static void flush_egress(struct dppd_worker *worker, const struct dppd_port *egress, struct rte_mbuf **packets,
    const uint32_t *lengths, uint16_t count, struct dppd_stats_values *ingress, struct dppd_stats_values *output)
{
    if (count == 0)
        return;
    if (dppd_devices_removal_requested(&worker->runtime->devices) || !dppd_port_tx_available(egress)) {
        ingress->policy_drops += count;
        ingress->egress_drops += count;
        free_packets(packets, count);
        return;
    }
    uint16_t sent = rte_eth_tx_burst(egress->port_id, worker->queue_id, packets, count);
    output->tx_packets += sent;
    for (uint16_t i = 0; i < sent; ++i)
        output->tx_bytes += lengths[i];
    if (sent < count) {
        output->tx_drops += count - sent;
        output->tx_queue_drops += count - sent;
        free_packets(packets + sent, (uint16_t)(count - sent));
    }
}

static enum dppd_drop_reason stateful_error(int error)
{
    switch (error) {
    case -ENOSPC:
        return DPPD_DROP_CT_FULL;
    case -EADDRNOTAVAIL:
        return DPPD_DROP_NAT_POOL;
    case -ENOENT:
        return DPPD_DROP_NAT_MISS;
    case -EXDEV:
        return DPPD_DROP_CT_ZONE;
    case -ENOTSUP:
        return DPPD_DROP_CT_UNSUPPORTED;
    case -EBADMSG:
        return DPPD_DROP_CT_CHECKSUM;
    case -ETIMEDOUT:
        return DPPD_DROP_TTL;
    case -ENOMEM:
        return DPPD_DROP_REWRITE;
    default:
        return DPPD_DROP_CT_INVALID;
    }
}

/** 反向 NAT 先恢复逻辑目的地址供 ACL 和路由使用，线缆字节只在最终放行后统一改写 */
static void stateful_decide(struct dppd_worker *worker, const struct dppd_port *ingress, struct rte_mbuf **mbuf,
    int parsed, const struct dppd_packet *packet, struct dppd_pipeline_decision *decision)
{
    struct dppd_runtime *runtime = worker->runtime;
    struct dppd_connection_attempt attempt = {0};
    struct dppd_packet policy = *packet;
    int rc = 0;
    if (parsed != DPPD_PARSE_MALFORMED) {
        if (packet->l4_type == DPPD_L4_TCP || packet->l4_type == DPPD_L4_UDP)
            rc = dppd_ipv4_transport_validate(*mbuf, packet);
        if (rc == 0)
            rc = dppd_conntrack_begin(runtime->conntrack, ingress->port_id, packet, dppd_monotonic_ms(), &attempt);
        policy.connection_state = rc == 0 ? attempt.state : DPPD_CT_INVALID;
        if (rc == 0) {
            policy.ipv4_src_be = attempt.policy.source_be;
            policy.ipv4_dst_be = attempt.policy.destination_be;
            policy.l4_src_port_be = attempt.policy.source_port_be;
            policy.l4_dst_port_be = attempt.policy.destination_port_be;
        }
    }
    if (parsed != DPPD_PARSE_MALFORMED && rc != 0) {
        /** INVALID 规则仍可记录拒绝计数，但允许动作不能把校验错误或无会话回包放行 */
        struct dppd_software_decision policy_result = {0};
        dppd_software_backend_decide(runtime->software_backend, ingress->port_id, &policy, &policy_result);
        memset(decision, 0, sizeof(*decision));
        decision->drop_reason = policy_result.drop ? DPPD_DROP_POLICY : stateful_error(rc);
        dppd_conntrack_finish(&attempt, 0, false);
        return;
    }
    dppd_pipeline_decide_routed(&runtime->snapshot, runtime->software_backend, runtime->ipv4_router, ingress->port_id,
        parsed, &policy, decision);
    if (decision->action != DPPD_PACKET_FORWARD) {
        dppd_conntrack_finish(&attempt, 0, false);
        return;
    }
    if (rc == 0) {
        const struct dppd_port *egress = dppd_devices_find(&runtime->devices, decision->egress_port);
        if (egress == NULL || !dppd_port_tx_available(egress)) {
            decision->action = DPPD_PACKET_DROP;
            decision->drop_reason = DPPD_DROP_EGRESS;
            dppd_conntrack_finish(&attempt, 0, false);
            return;
        }
        rc = dppd_ipv4_rewrite_flow(mbuf, packet, &decision->ipv4, egress->mac.addr_bytes,
            attempt.nat ? &attempt.rewrite : NULL);
    }
    int commit = dppd_conntrack_finish(&attempt, decision->egress_port, rc == 0);
    if (rc == 0)
        rc = commit;
    if (rc != 0) {
        decision->action = DPPD_PACKET_DROP;
        decision->drop_reason = stateful_error(rc);
    }
}

/** 同一入口批次按实际出口分组，三层路由可以同时选择多个出口，报文始终只交付一次 */
static void process_ingress(struct dppd_worker *worker, const struct dppd_port *ingress)
{
    struct dppd_runtime *runtime = worker->runtime;
    struct rte_mbuf *rx[DPPD_MAX_BURST], *tx[DPPD_MAX_PORTS][DPPD_MAX_BURST];
    uint32_t tx_lengths[DPPD_MAX_PORTS][DPPD_MAX_BURST];
    uint16_t counts[DPPD_MAX_PORTS] = {0};
    struct dppd_stats_values delta = {0}, outputs[DPPD_MAX_PORTS] = {0};
    if (dppd_devices_removal_requested(&runtime->devices))
        return;
    uint16_t received = rte_eth_rx_burst(ingress->port_id, worker->queue_id, rx, runtime->config.burst_size);
    if (received == 0)
        return;
    delta.rx_packets = received;
    const struct dppd_port *paired =
        runtime->ipv4_router == NULL ? dppd_devices_find(&runtime->devices, ingress->peer_port_id) : NULL;
    for (uint16_t i = 0; i < received; ++i) {
        struct dppd_pipeline_decision decision;
        struct dppd_packet packet = {0};
        uint32_t length = rte_pktmbuf_pkt_len(rx[i]);
        delta.rx_bytes += length;
        int parsed = dppd_packet_parse_mbuf(rx[i], &packet);
        if (parsed == DPPD_PARSE_MALFORMED)
            delta.rx_malformed++;
        else if (parsed == DPPD_PARSE_UNSUPPORTED)
            delta.rx_unsupported++;
        /** IPv6 继续使用未跟踪的双栈路由，不能送入只认识 IPv4 五元组的会话与 NAT 表 */
        if (runtime->conntrack != NULL && packet.l3_type != DPPD_L3_IPV6)
            stateful_decide(worker, ingress, &rx[i], parsed, &packet, &decision);
        else
            dppd_pipeline_decide_routed(&runtime->snapshot, runtime->software_backend, runtime->ipv4_router,
                ingress->port_id, parsed, &packet, &decision);
        if (decision.action != DPPD_PACKET_FORWARD) {
            if (decision.drop_reason != DPPD_DROP_MALFORMED)
                delta.policy_drops++;
            if (decision.drop_reason == DPPD_DROP_POLICY)
                delta.rule_drops++;
            else if (decision.drop_reason == DPPD_DROP_NO_ROUTE)
                delta.no_route_drops++;
            else if (decision.drop_reason == DPPD_DROP_NO_NEIGHBOR)
                delta.no_neighbor_drops++;
            else if (decision.drop_reason == DPPD_DROP_IPV4_UNSUPPORTED)
                delta.ipv4_unsupported_drops++;
            else if (decision.drop_reason == DPPD_DROP_IPV6_UNSUPPORTED)
                delta.ipv6_unsupported_drops++;
            else if (decision.drop_reason == DPPD_DROP_CT_INVALID)
                delta.ct_invalid_drops++;
            else if (decision.drop_reason == DPPD_DROP_CT_FULL)
                delta.ct_full_drops++;
            else if (decision.drop_reason == DPPD_DROP_CT_UNSUPPORTED)
                delta.ct_unsupported_drops++;
            else if (decision.drop_reason == DPPD_DROP_CT_ZONE)
                delta.ct_zone_drops++;
            else if (decision.drop_reason == DPPD_DROP_CT_CHECKSUM)
                delta.ct_checksum_drops++;
            else if (decision.drop_reason == DPPD_DROP_NAT_POOL)
                delta.nat_pool_drops++;
            else if (decision.drop_reason == DPPD_DROP_NAT_MISS)
                delta.nat_miss_drops++;
            else if (decision.drop_reason == DPPD_DROP_TTL)
                delta.ipv4_ttl_drops++;
            else if (decision.drop_reason == DPPD_DROP_REWRITE)
                delta.ipv4_rewrite_drops++;
            else if (decision.drop_reason == DPPD_DROP_EGRESS)
                delta.egress_drops++;
            rte_pktmbuf_free(rx[i]);
            continue;
        }
        const struct dppd_port *egress =
            runtime->ipv4_router != NULL ? dppd_devices_find(&runtime->devices, decision.egress_port) : paired;
        if (egress == NULL || egress->port_id != decision.egress_port || !dppd_port_tx_available(egress)) {
            delta.policy_drops++;
            delta.egress_drops++;
            rte_pktmbuf_free(rx[i]);
            continue;
        }
        if (decision.routed && (runtime->conntrack == NULL || packet.l3_type == DPPD_L3_IPV6)) {
            bool ipv6 = packet.l3_type == DPPD_L3_IPV6;
            int rc = ipv6 ? dppd_ipv6_rewrite(&rx[i], &packet, &decision.ipv4, egress->mac.addr_bytes)
                          : dppd_ipv4_rewrite(&rx[i], &packet, &decision.ipv4, egress->mac.addr_bytes);
            if (rc != 0) {
                delta.policy_drops++;
                if (ipv6) {
                    if (rc == -ETIMEDOUT)
                        delta.ipv6_hop_drops++;
                    else if (rc == -EBADMSG)
                        delta.ipv6_checksum_drops++;
                    else if (rc == -ENOTSUP)
                        delta.ipv6_unsupported_drops++;
                    else
                        delta.ipv6_rewrite_drops++;
                } else if (rc == -ETIMEDOUT)
                    delta.ipv4_ttl_drops++;
                else if (rc == -EBADMSG)
                    delta.ipv4_checksum_drops++;
                else if (rc == -ENOTSUP)
                    delta.ipv4_unsupported_drops++;
                else
                    delta.ipv4_rewrite_drops++;
                rte_pktmbuf_free(rx[i]);
                continue;
            }
        }
        if (decision.has_mark) {
            rx[i]->hash.fdir.hi = decision.mark_id;
            rx[i]->ol_flags |= RTE_MBUF_F_RX_FDIR_ID;
        }
        uint16_t index = (uint16_t)(egress - runtime->devices.ports);
        if (rx[i]->nb_segs > 1U && (egress->configured_tx_offloads & RTE_ETH_TX_OFFLOAD_MULTI_SEGS) == 0 &&
            rte_pktmbuf_linearize(rx[i]) != 0) {
            outputs[index].tx_drops++;
            outputs[index].tx_linearize_drops++;
            rte_pktmbuf_free(rx[i]);
            continue;
        }
        /* 长度在交给驱动前保存，发送完成后不再读取已经转移所有权的 mbuf */
        tx_lengths[index][counts[index]] = length;
        tx[index][counts[index]++] = rx[i];
    }
    for (uint16_t index = 0; index < runtime->devices.nb_ports; ++index)
        flush_egress(worker, &runtime->devices.ports[index], tx[index], tx_lengths[index], counts[index], &delta,
            &outputs[index]);
    dppd_stats_add(&worker->port_stats[ingress - runtime->devices.ports], &delta);
    for (uint16_t index = 0; index < runtime->devices.nb_ports; ++index) {
        dppd_stats_add(&worker->port_stats[index], &outputs[index]);
        dppd_stats_accumulate(&delta, &outputs[index]);
    }
    /** 每批只更新一次工作线程汇总，避免入口丢弃已累计而出口字节还没累计的额外窗口 */
    dppd_stats_add(&worker->stats, &delta);
}

/** 工作线程先登记为规则读者，再循环处理端口；收到停止请求后注销并返回 */
int dppd_worker_main(void *arg)
{
    struct dppd_worker *worker = arg;
    struct dppd_runtime *runtime;

    if (worker == NULL || worker->runtime == NULL)
        return -1;
    runtime = worker->runtime;

    /**
     * queue_id 是启动前分配的连续队列编号，同时作为 QSBR 的读者编号
     * 首次读取软件规则之前必须注册并上线，否则回收器不知道本线程正在使用旧表
     * 本实现统一在一轮端口扫描结束后报告安全点，此时各次规则匹配均已返回
     */
    if (runtime->software_backend != NULL &&
        dppd_software_backend_worker_register(runtime->software_backend, worker->queue_id) != 0) {
        /** 尚未开始收发就注册失败，也要通知主线程，不能留下貌似已启动的空线程 */
        atomic_store_explicit(&worker->state, DPPD_WORKER_FAILED, memory_order_release);
        return -1;
    }

    if (runtime->ipv4_router != NULL && dppd_ipv4_router_reader_register(runtime->ipv4_router, worker->queue_id) != 0) {
        dppd_software_backend_worker_unregister(runtime->software_backend, worker->queue_id);
        atomic_store_explicit(&worker->state, DPPD_WORKER_FAILED, memory_order_release);
        return -1;
    }
    /** 完成全部读者注册后才宣布线程可以收发，健康查询据此统计实际运行的队列 */
    atomic_store_explicit(&worker->state, DPPD_WORKER_RUNNING, memory_order_release);

    /** 移除回调直接通知所有工作线程退出，不依赖主线程先设置普通停止标记 */
    while (!atomic_load_explicit(&runtime->stop_requested, memory_order_acquire) &&
        !dppd_devices_removal_requested(&runtime->devices)) {
        uint16_t i;

        for (i = 0; i < runtime->devices.nb_ports; ++i)
            process_ingress(worker, &runtime->devices.ports[i]);
        /** 即使所有端口都没有报文，也要报告安全点，避免空闲线程一直阻止旧表回收 */
        dppd_software_backend_worker_quiescent(runtime->software_backend, worker->queue_id);
        dppd_ipv4_router_reader_quiescent(runtime->ipv4_router, worker->queue_id);
    }
    /** 停止读取规则后注销；主线程还需等待本线程返回，之后才能销毁共享后端 */
    dppd_ipv4_router_reader_unregister(runtime->ipv4_router, worker->queue_id);
    dppd_software_backend_worker_unregister(runtime->software_backend, worker->queue_id);
    /** 先注销借用的规则读者，再发布已结束状态，资源释放仍须由主线程等待返回 */
    atomic_store_explicit(&worker->state, DPPD_WORKER_STOPPED, memory_order_release);
    return 0;
}
