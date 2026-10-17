#include "dppd/runtime.h"
#include "dppd/software_backend.h"
#include "dppd/ipv4_rewrite.h"

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
        struct dppd_packet packet;
        uint32_t length = rte_pktmbuf_pkt_len(rx[i]);
        delta.rx_bytes += length;
        int parsed = dppd_packet_parse_mbuf(rx[i], &packet);
        if (parsed == DPPD_PARSE_MALFORMED)
            delta.rx_malformed++;
        else if (parsed == DPPD_PARSE_UNSUPPORTED)
            delta.rx_unsupported++;
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
        if (decision.routed) {
            int rc = dppd_ipv4_rewrite(&rx[i], &packet, &decision.ipv4, egress->mac.addr_bytes);
            if (rc != 0) {
                delta.policy_drops++;
                if (rc == -ETIMEDOUT)
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
