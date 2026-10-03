#include "dppd/runtime.h"
#include "dppd/software_backend.h"

#include <string.h>
#include <rte_ethdev.h>
#include <rte_mbuf.h>

/** 释放仍归本线程持有的报文；已被发送队列接收的报文交给驱动管理，不能再次释放 */
static void free_packets(struct rte_mbuf **packets, uint16_t count)
{
    uint16_t i;

    for (i = 0; i < count; ++i)
        rte_pktmbuf_free(packets[i]);
}

/**
 * 从一个入口队列接收一批报文，逐个解析和判断，再集中提交给配对出口
 * 每个报文最终只能走向释放或交给发送队列两条路径之一，避免泄漏和重复释放
 * 本批次统计先在局部变量中累计，结束时统一写入当前工作线程的统计区
 */
static void process_ingress(struct dppd_worker *worker, const struct dppd_port *ingress)
{
    struct dppd_runtime *runtime = worker->runtime;
    const struct dppd_port *egress;
    struct rte_mbuf *rx[DPPD_MAX_BURST];
    struct rte_mbuf *tx[DPPD_MAX_BURST];
    uint32_t tx_lengths[DPPD_MAX_BURST];
    struct dppd_stats_values delta;
    uint16_t nb_rx;
    uint16_t nb_tx = 0;
    uint16_t i;

    /** 在进入驱动收包接口之前再次检查移除请求，减少通知后的设备访问 */
    if (dppd_devices_removal_requested(&runtime->devices))
        return;
    nb_rx = rte_eth_rx_burst(ingress->port_id,
                             worker->queue_id,
                             rx,
                             runtime->config.burst_size);
    if (nb_rx == 0)
        return;

    memset(&delta, 0, sizeof(delta));
    delta.rx_packets = nb_rx;
    egress = dppd_devices_find(&runtime->devices, ingress->peer_port_id);

    for (i = 0; i < nb_rx; ++i) {
        struct dppd_pipeline_decision decision;
        struct dppd_packet packet;
        const uint32_t packet_len = rte_pktmbuf_pkt_len(rx[i]);
        int parse_status;

        delta.rx_bytes += packet_len;
        parse_status = dppd_packet_parse_mbuf(rx[i], &packet);
        if (parse_status == DPPD_PARSE_MALFORMED)
            delta.rx_malformed++;
        else if (parse_status == DPPD_PARSE_UNSUPPORTED)
            delta.rx_unsupported++;

        dppd_pipeline_decide(&runtime->snapshot, runtime->software_backend,
                             ingress->port_id,
                             parse_status,
                             &packet,
                             &decision);
        /** 畸形报文已有专门统计，其余拒绝转发的结果计入策略丢弃，随后释放报文 */
        if (decision.action != DPPD_PACKET_FORWARD) {
            if (decision.drop_reason != DPPD_DROP_MALFORMED)
                delta.policy_drops++;
            if (decision.drop_reason == DPPD_DROP_POLICY)
                delta.rule_drops++;
            else if (decision.drop_reason == DPPD_DROP_NO_ROUTE)
                delta.no_route_drops++;
            rte_pktmbuf_free(rx[i]);
            continue;
        }

        /** 配对出口必须存在、与决策一致且未断开或移除，无法发送的报文由本线程释放 */
        if (egress == NULL || decision.egress_port != egress->port_id ||
            !dppd_port_tx_available(egress)) {
            delta.policy_drops++;
            delta.egress_drops++;
            rte_pktmbuf_free(rx[i]);
            continue;
        }

        /**
         * 复用 DPDK 的 flow mark 元数据约定，令下游观察者无需区分该标记来自
         * rte_flow 还是软件 classifier。这里不设置 TX offload，只声明 RX 侧标记。
         */
        if (decision.has_mark) {
            rx[i]->hash.fdir.hi = decision.mark_id;
            rx[i]->ol_flags |= RTE_MBUF_F_RX_FDIR_ID;
        }

        /** 出口不支持多段发送时先合并报文；合并失败就释放并计入发送丢弃 */
        if (rx[i]->nb_segs > 1U &&
            (egress->configured_tx_offloads & RTE_ETH_TX_OFFLOAD_MULTI_SEGS) == 0) {
            if (rte_pktmbuf_linearize(rx[i]) != 0) {
                delta.tx_drops++;
                delta.tx_linearize_drops++;
                rte_pktmbuf_free(rx[i]);
                continue;
            }
        }

        /** 提前保存长度，发送成功后报文已归驱动管理，统计时不能再读取其内存 */
        tx_lengths[nb_tx] = packet_len;
        tx[nb_tx++] = rx[i];
    }

    /**
     * 逐包处理期间出口也可能发生变化，提交发送批次前再检查一次
     * 这些报文尚未交给驱动，仍由当前线程持有，所以可以在此统计并释放
     * 原子检查只能减少通知后的访问，不能替代物理热拔插所需的总线访问保护
     */
    if (nb_tx != 0 && (dppd_devices_removal_requested(&runtime->devices) ||
                       !dppd_port_tx_available(egress))) {
        delta.policy_drops += nb_tx;
        delta.egress_drops += nb_tx;
        free_packets(tx, nb_tx);
        nb_tx = 0;
    }
    if (nb_tx != 0) {
        const uint16_t sent = rte_eth_tx_burst(egress->port_id,
                                               worker->queue_id,
                                               tx,
                                               nb_tx);

        delta.tx_packets += sent;
        for (i = 0; i < sent; ++i)
            delta.tx_bytes += tx_lengths[i];
        /** 本轮不重试未被队列接收的尾部报文，它们仍归本线程所有，必须主动释放 */
        if (sent < nb_tx) {
            delta.tx_drops += (uint64_t)(nb_tx - sent);
            delta.tx_queue_drops += (uint64_t)(nb_tx - sent);
            free_packets(&tx[sent], (uint16_t)(nb_tx - sent));
        }
    }

    dppd_stats_add(&worker->stats, &delta);
    {
        struct dppd_stats_values tx_delta = {0};
        uint16_t ingress_index = (uint16_t)(ingress - runtime->devices.ports);

        tx_delta.tx_packets = delta.tx_packets;
        tx_delta.tx_bytes = delta.tx_bytes;
        tx_delta.tx_drops = delta.tx_drops;
        tx_delta.tx_linearize_drops = delta.tx_linearize_drops;
        tx_delta.tx_queue_drops = delta.tx_queue_drops;
        delta.tx_packets = delta.tx_bytes = delta.tx_drops = 0;
        delta.tx_linearize_drops = delta.tx_queue_drops = 0;
        dppd_stats_add(&worker->port_stats[ingress_index], &delta);
        if (egress != NULL) {
            uint16_t egress_index = (uint16_t)(egress - runtime->devices.ports);
            dppd_stats_add(&worker->port_stats[egress_index], &tx_delta);
        }
    }
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
        dppd_software_backend_worker_register(runtime->software_backend,
                                              worker->queue_id) != 0) {
        /** 尚未开始收发就注册失败，也要通知主线程，不能留下貌似已启动的空线程 */
        atomic_store_explicit(&worker->state, DPPD_WORKER_FAILED, memory_order_release);
        return -1;
    }

    /** 完成规则读者注册后才宣布线程可以收发，健康查询据此统计实际运行的队列 */
    atomic_store_explicit(&worker->state, DPPD_WORKER_RUNNING, memory_order_release);

    /** 移除回调直接通知所有工作线程退出，不依赖主线程先设置普通停止标记 */
    while (!atomic_load_explicit(&runtime->stop_requested, memory_order_acquire) &&
           !dppd_devices_removal_requested(&runtime->devices)) {
        uint16_t i;

        for (i = 0; i < runtime->devices.nb_ports; ++i)
            process_ingress(worker, &runtime->devices.ports[i]);
        /** 即使所有端口都没有报文，也要报告安全点，避免空闲线程一直阻止旧表回收 */
        dppd_software_backend_worker_quiescent(runtime->software_backend,
                                                worker->queue_id);
    }
    /** 停止读取规则后注销；主线程还需等待本线程返回，之后才能销毁共享后端 */
    dppd_software_backend_worker_unregister(runtime->software_backend,
                                            worker->queue_id);
    /** 先注销借用的规则读者，再发布已结束状态，资源释放仍须由主线程等待返回 */
    atomic_store_explicit(&worker->state, DPPD_WORKER_STOPPED, memory_order_release);
    return 0;
}
