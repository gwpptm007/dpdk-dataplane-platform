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

/** 每次准备调用驱动前都重新检查，事件不能等到整轮端口扫描结束后才阻止下一次收发 */
static bool worker_io_quiesced(const struct dppd_worker *worker)
{
    return atomic_load_explicit(&worker->runtime->stop_requested, memory_order_acquire) ||
        dppd_devices_io_quiesced(&worker->runtime->devices);
}

/**
 * 向指定出口的当前 worker 队列批量提交报文，count 是本出口实际待发送数量
 * 提交前再次检查停止状态和出口可用性，失效则释放整批并计入入口丢弃
 * 驱动接收的报文移交驱动管理，未接收的尾部报文由 worker 释放，不在此重试
 * 成功提交和队列丢弃计入出口统计
 */
static void flush_egress(struct dppd_worker *worker, const struct dppd_port *egress, struct rte_mbuf **packets,
    const uint32_t *lengths, uint16_t count, struct dppd_stats_values *ingress, struct dppd_stats_values *output)
{
    if (count == 0)
        return;
    if (worker_io_quiesced(worker) || !dppd_port_tx_available(egress)) {
        ingress->policy_drops += count;
        ingress->egress_drops += count;
        free_packets(packets, count);
        return;
    }
    /**
     * sent 表示驱动接收了待发送数组中的前多少个包，不表示对端已经收到
     * packets 只包含本出口的待发送包，之前已丢弃的包不在其中，不能按原始 RX 数组编号理解 sent
     * 例：count 为 3、sent 为 2 → packets[0] 和 packets[1] 交给驱动，packets[2] 仍归 worker
     */
    uint16_t sent = rte_eth_tx_burst(egress->port_id, worker->queue_id, packets, count);
    output->tx_packets += sent;
    /** 使用入队前保存的 lengths 统计，避免访问已移交驱动的 mbuf */
    for (uint16_t i = 0; i < sent; ++i)
        output->tx_bytes += lengths[i];
    /**
     * packets + sent 指向第一个未被接收的包，只释放剩余 count - sent 个，不在这里重试
     * 例：3 个包只接收 2 个 → 从 &packets[2] 开始释放 1 个，前两个不能再次访问或释放
     */
    if (sent < count) {
        output->tx_drops += count - sent;
        output->tx_queue_drops += count - sent;
        free_packets(packets + sent, (uint16_t)(count - sent));
    }
}

/** 每个工作线程只写自己的 TX 队列，ND 不借主线程并发写同一个硬件队列 */
static int send_nd6(void *context, uint16_t port_id, const uint8_t *wire, uint16_t length)
{
    struct dppd_worker *worker = context;
    const struct dppd_port *port = dppd_devices_find(&worker->runtime->devices, port_id);
    if (port == NULL || !dppd_port_tx_available(port) || worker_io_quiesced(worker))
        return -ENODEV;
    struct rte_mbuf *mbuf = rte_pktmbuf_alloc(worker->runtime->devices.pools[port->socket_id]);
    if (mbuf == NULL)
        return -ENOMEM;
    void *bytes = rte_pktmbuf_append(mbuf, length);
    if (bytes == NULL) {
        rte_pktmbuf_free(mbuf);
        return -ENOMEM;
    }
    memcpy(bytes, wire, length);
    /** 分配与复制期间也可能收到事件，尚未交给驱动的报文仍由本线程归还 */
    if (worker_io_quiesced(worker) || !dppd_port_tx_available(port)) {
        rte_pktmbuf_free(mbuf);
        return -ENODEV;
    }
    if (rte_eth_tx_burst(port_id, worker->queue_id, &mbuf, 1) != 1) {
        rte_pktmbuf_free(mbuf);
        return -EAGAIN;
    }
    struct dppd_stats_values output = {.tx_packets = 1, .tx_bytes = length, .nd6_tx_packets = 1};
    dppd_stats_add(&worker->stats, &output);
    dppd_stats_add(&worker->port_stats[port - worker->runtime->devices.ports], &output);
    return 0;
}

/**
 * 执行软件规则与出口选择，将动作、原因和出口写入 decision
 * IPv6 缺静态邻居且已启用 ND 时，用出口和下一跳查询动态邻居表，补齐 MAC 或标记等待
 * 转发前检查出口 IPv6 地址是否就绪，将决定交回 process_batch 或 stateful_decide
 */
static void routed_decide(struct dppd_runtime *runtime, uint16_t ingress, int parsed, const struct dppd_packet *packet,
    struct dppd_pipeline_decision *decision, struct dppd_software_count_batch *counts)
{
    dppd_pipeline_decide_counted(&runtime->snapshot, runtime->software_backend, runtime->ipv4_router, ingress, parsed,
        packet, decision, counts);
    if (runtime->nd6 == NULL || packet->l3_type != DPPD_L3_IPV6)
        return;
    /**
     * 静态邻居未命中时，pipeline 返回 DROP / NO_NEIGHBOR，但已留下出口和下一跳
     * 上方检查保证此处是 IPv6 且已启用 ND（IPv6 邻居发现）
     *
     * 沿用示例：
     *   decision->ipv4.port_id = 1，next_hop6 = fe80::1，destination_mac 仍为全 0
     *   用网口 1 和 fe80::1 查询 ND 动态邻居表
     *   若找到可用 MAC，dppd_nd6_resolve 直接写入 decision->ipv4.destination_mac
     * 静态表没有邻居，并不代表动态表也没有，因此这里还需再查一次
     */
    if (decision->drop_reason == DPPD_DROP_NO_NEIGHBOR) {
        int rc = dppd_nd6_resolve(runtime->nd6, decision->ipv4.port_id, decision->ipv4.next_hop6, packet,
            dppd_monotonic_ms(), decision->ipv4.destination_mac);

        /**
         * rc = 0       → MAC 已写入，改为 FORWARD，填写出口并设置 routed
         * rc = -EAGAIN → MAC 尚未取得，改为 ND_PENDING，让 process_batch 尝试暂存报文
         * 其他错误     → 保持 DROP / NO_NEIGHBOR，交回调用方处理失败
         * ND_PENDING 时 action 仍是 DROP，但 process_batch 会先处理等待入队，再判断丢弃
         */
        if (rc == 0) {
            decision->action = DPPD_PACKET_FORWARD;
            decision->drop_reason = DPPD_DROP_NONE;
            decision->egress_port = decision->ipv4.port_id;
            decision->routed = true;
        } else if (rc == -EAGAIN)
            decision->drop_reason = DPPD_DROP_ND_PENDING;
    }
    /** 取得下一跳 MAC 后，仍须确认出口 IPv6 地址已就绪，否则改为 DROP / ND_NOT_READY */
    if (decision->action == DPPD_PACKET_FORWARD && !dppd_nd6_port_ready(runtime->nd6, decision->egress_port)) {
        decision->action = DPPD_PACKET_DROP;
        decision->drop_reason = DPPD_DROP_ND_NOT_READY;
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

/**
 * 结合连接状态生成处理决定，完成报文校验、会话/NAT 准备及策略匹配
 * 流程：准备候选会话 → 构造 policy → routed_decide → 改写报文 → 提交或取消会话
 * policy 使用会话得到的逻辑地址和端口匹配规则，packet 保留原包字段供改写校验
 * decision 输出动作与出口，mbuf 可能被改写函数替换，实际发送与丢弃仍由 process_batch 执行
 * 放行且改写成功才提交会话，失败取消候选并归还临时 NAT 端口，不刷新会话超时
 */
static void stateful_decide(struct dppd_worker *worker, const struct dppd_port *ingress, struct rte_mbuf **mbuf,
    int parsed, const struct dppd_packet *packet, struct dppd_pipeline_decision *decision,
    struct dppd_software_count_batch *counts)
{
    struct dppd_runtime *runtime = worker->runtime;
    struct dppd_connection_attempt attempt = {0};
    struct dppd_packet policy = *packet;
    int rc = 0;
    /** 先校验 TCP/UDP 报文，再准备候选会话，并将连接状态交给后面的策略匹配 */
    if (parsed != DPPD_PARSE_MALFORMED) {
        if (packet->l4_type == DPPD_L4_TCP || packet->l4_type == DPPD_L4_UDP)
            rc = packet->l3_type == DPPD_L3_IPV6 ? dppd_ipv6_transport_validate(*mbuf, packet)
                                                 : dppd_ipv4_transport_validate(*mbuf, packet);
        if (rc == 0)
            rc = dppd_conntrack_begin(runtime->conntrack, ingress->port_id, packet, dppd_monotonic_ms(), &attempt);
        policy.connection_state = rc == 0 ? attempt.state : DPPD_CT_INVALID;
        if (rc == 0) {
            memcpy(policy.ipv6_src, attempt.policy.source6, 16);
            memcpy(policy.ipv6_dst, attempt.policy.destination6, 16);
            policy.ipv4_src_be = attempt.policy.source_be;
            policy.ipv4_dst_be = attempt.policy.destination_be;
            policy.l4_src_port_be = attempt.policy.source_port_be;
            policy.l4_dst_port_be = attempt.policy.destination_port_be;
        }
    }
    if (parsed != DPPD_PARSE_MALFORMED && rc != 0) {
        /** INVALID 规则仍可记录拒绝计数，但允许动作不能把校验错误或无会话回包放行 */
        struct dppd_software_decision policy_result = {0};
        dppd_software_backend_decide_counted(runtime->software_backend, ingress->port_id, &policy, &policy_result,
            counts);
        memset(decision, 0, sizeof(*decision));
        decision->drop_reason = policy_result.drop ? DPPD_DROP_POLICY : stateful_error(rc);
        dppd_conntrack_finish(&attempt, 0, false);
        return;
    }
    /** 用带连接状态和逻辑地址的 policy，复用通用软件规则与出口选择 */
    routed_decide(runtime, ingress->port_id, parsed, &policy, decision, counts);
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
        /** 出口可用后改写实际报文，调整 MAC、TTL/HopLimit，并按需应用 NAT 和更新校验和 */
        rc = packet->l3_type == DPPD_L3_IPV6 ? dppd_ipv6_rewrite_flow(mbuf, packet, &decision->ipv4,
                                                   egress->mac.addr_bytes, attempt.nat ? &attempt.rewrite : NULL)
                                             : dppd_ipv4_rewrite_flow(mbuf, packet, &decision->ipv4,
                                                   egress->mac.addr_bytes, attempt.nat ? &attempt.rewrite : NULL);
    }
    /** finish 提交或取消候选并释放会话表锁，此时尚未调用网卡发送 */
    int commit = dppd_conntrack_finish(&attempt, decision->egress_port, rc == 0);
    if (rc == 0)
        rc = commit;
    if (rc != 0) {
        decision->action = DPPD_PACKET_DROP;
        decision->drop_reason = stateful_error(rc);
    }
}

/**
 * 处理同一入口的一批报文，逐包决定去向，再按出口批量发送并提交统计
 *
 * worker：当前处理线程，提供运行配置、收发队列编号和统计对象
 * ingress：这批报文的原入口网口，ND 重试也沿用原入口
 * rx / received：待处理的 mbuf 指针数组及实际包数，received 不超过 DPPD_MAX_BURST
 * retry：普通收包传 false，ND 等待包取出重试传 true，避免重复累计 RX、隧道观察和规则 COUNT
 * expires：ND 等待的毫秒截止时间，首次传 0，由 hold 设置期限；重试沿用原期限，不延长等待
 *
 * 处理路径：
 *   逐包解析 → 处理本机 ND 控制报文、检查 IPv6 入口地址是否就绪
 *   未启用连接跟踪 → routed_decide 匹配软件规则，选择路由或配对出口
 *   启用连接跟踪 → stateful_decide 处理会话，并完成决策及报文改写
 *   等待下一跳 MAC → 尝试交给 ND 等待队列；拒绝或处理失败 → 释放当前包
 *   允许转发 → 检查出口，按需完成普通路由改写、MARK 和多段报文合并，加入对应出口批次
 *   逐包处理结束 → 提交规则 COUNT → flush_egress 提交发送 → 累加入口、出口和 worker 统计
 *
 * 普通三层转发修改源/目的 MAC、递减 TTL 或 Hop Limit，IPv4 还更新头部校验和
 * 连接跟踪路径已完成改写时不重复执行；NAT 按配置另行改写 IP、端口和相关校验和
 *
 * 本函数接管 rx 中的报文，改写时可能替换 rx[i]，返回后调用方不能再访问或释放这些 mbuf
 *   ND 入队成功：等待队列持有；TX 提交成功：驱动持有；其余已处理报文：本线程释放
 * 无返回值；received 为 0 时直接结束，停止收发时释放整批，不等待凑满或重试 TX 未接收的包
 * 函数返回前提交本批 COUNT，QSBR 静默点由 dppd_worker_main 在一轮端口扫描结束后报告
 */
static void process_batch(struct dppd_worker *worker, const struct dppd_port *ingress, struct rte_mbuf **rx,
    uint16_t received, bool retry, uint64_t expires)
{
    struct dppd_runtime *runtime = worker->runtime;
    /** 按出口暂存待发送的 mbuf 指针、长度和数量，逐包处理结束后统一批量发送 */
    struct rte_mbuf *tx[DPPD_MAX_PORTS][DPPD_MAX_BURST];
    uint32_t tx_lengths[DPPD_MAX_PORTS][DPPD_MAX_BURST];
    uint16_t counts[DPPD_MAX_PORTS] = {0};
    struct dppd_stats_values delta = {0}, outputs[DPPD_MAX_PORTS] = {0};
    struct dppd_software_count_batch rule_counts = {0};
    if (worker_io_quiesced(worker)) {
        /** RX 返回的报文已归线程所有，事件不能让这批 mbuf 因提前返回而泄漏 */
        free_packets(rx, received);
        return;
    }
    if (received == 0)
        return;
    delta.rx_packets = retry ? 0 : received;
    const struct dppd_port *paired =
        runtime->ipv4_router == NULL ? dppd_devices_find(&runtime->devices, ingress->peer_port_id) : NULL;
    for (uint16_t i = 0; i < received; ++i) {
        /** packet 保存解析字段，decision 保存处理决定，rx[i] 指向原始报文 */
        struct dppd_pipeline_decision decision;
        struct dppd_packet packet = {0};
        uint32_t length = rte_pktmbuf_pkt_len(rx[i]);
        if (!retry)
            delta.rx_bytes += length;
        /** 逐包进入 packet_mbuf.c 提取报文头字段，burst 只批量收发，不省略逐包解析 */
        int parsed = dppd_packet_parse_mbuf(rx[i], &packet);
        packet.neighbor_retry = retry;
        /** 隧道统计是外层中转观察，既不替换 ACL 字段，也不因内层错误改变已有放行结果 */
        if (!retry) {
            delta.tunnel_vxlan_packets += packet.tunnel.type == DPPD_TUNNEL_VXLAN;
            delta.tunnel_geneve_packets += packet.tunnel.type == DPPD_TUNNEL_GENEVE;
            delta.tunnel_gre_packets += packet.tunnel.type == DPPD_TUNNEL_GRE;
            delta.tunnel_malformed += packet.tunnel.status == DPPD_PARSE_MALFORMED;
            delta.tunnel_unsupported += packet.tunnel.status == DPPD_PARSE_UNSUPPORTED;
        }
        if (parsed == DPPD_PARSE_MALFORMED)
            delta.rx_malformed++;
        else if (parsed == DPPD_PARSE_UNSUPPORTED)
            delta.rx_unsupported++;
        if (runtime->nd6 != NULL && parsed != DPPD_PARSE_MALFORMED &&
            dppd_nd6_input(runtime->nd6, ingress->port_id, rx[i], &packet, dppd_monotonic_ms(), send_nd6, worker)) {
            delta.nd6_rx_packets++;
            rte_pktmbuf_free(rx[i]);
            continue;
        }
        if (packet.l3_type == DPPD_L3_IPV6 && !dppd_nd6_port_ready(runtime->nd6, ingress->port_id)) {
            delta.nd6_not_ready_drops++;
            delta.policy_drops++;
            rte_pktmbuf_free(rx[i]);
            continue;
        }
        /**
         * 未启用连接跟踪
         *   → routed_decide → decision
         *
         * 启用连接跟踪
         *   → stateful_decide
         *       校验、准备候选会话
         *         → routed_decide
         *         → 检查出口、改写实际报文
         *         → 成功提交会话，失败取消候选
         */
        if (runtime->conntrack != NULL)
            stateful_decide(worker, ingress, &rx[i], parsed, &packet, &decision, &rule_counts);
        else
            routed_decide(runtime, ingress->port_id, parsed, &packet, &decision, &rule_counts);
            
        /**
         * ND_PENDING 表示下一跳 MAC 尚未取得，先尝试把当前 mbuf 交给 ND 等待队列
         * 入队成功 → 队列接管 mbuf，continue 处理下一个包，此处不能释放或发送它
         * 入队失败 → mbuf 仍归当前线程，继续下面的丢弃分支并释放
         */
        if (decision.drop_reason == DPPD_DROP_ND_PENDING) {
            struct dppd_nd6_pending pending = {.mbuf = rx[i],
                .ingress = ingress->port_id,
                .queue = worker->queue_id,
                .expires_ms = expires};
            if (dppd_nd6_hold(runtime->nd6, decision.ipv4.port_id, decision.ipv4.next_hop6, pending,
                    dppd_monotonic_ms()) == 0) {
                delta.nd6_queued++;
                continue;
            }
            delta.nd6_queue_drops++;
        }
        /**
         * 决定函数只填写结果，worker 在这里执行丢弃并按原因更新统计
         * 释放当前报文后继续处理本批下一个包，不影响其他报文
         */
        if (decision.action != DPPD_PACKET_FORWARD) {
            if (decision.drop_reason != DPPD_DROP_MALFORMED)
                delta.policy_drops++;
            if (decision.drop_reason == DPPD_DROP_ND_NOT_READY)
                delta.nd6_not_ready_drops++;
            else if (decision.drop_reason == DPPD_DROP_POLICY)
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
            else if (decision.drop_reason == DPPD_DROP_TTL) {
                if (packet.l3_type == DPPD_L3_IPV6)
                    delta.ipv6_hop_drops++;
                else
                    delta.ipv4_ttl_drops++;
            } else if (decision.drop_reason == DPPD_DROP_REWRITE) {
                if (packet.l3_type == DPPD_L3_IPV6)
                    delta.ipv6_rewrite_drops++;
                else
                    delta.ipv4_rewrite_drops++;
            } else if (decision.drop_reason == DPPD_DROP_EGRESS)
                delta.egress_drops++;
            /** 归还当前 mbuf，再进入逐包循环的下一轮 */
            rte_pktmbuf_free(rx[i]);
            continue;
        }
        /**
         * 根据处理决定取得实际出口对象，三层模式按出口编号查找，端口配对模式复用 paired
         * FORWARD 只表示策略允许，发送前仍须确认出口存在、编号一致且可发送
         */
        const struct dppd_port *egress =
            runtime->ipv4_router != NULL ? dppd_devices_find(&runtime->devices, decision.egress_port) : paired;
        if (egress == NULL || egress->port_id != decision.egress_port || !dppd_port_tx_available(egress)) {
            delta.policy_drops++;
            delta.egress_drops++;
            rte_pktmbuf_free(rx[i]);
            continue;
        }
        /**
         * 三层转发在这里改写报文头，启用连接跟踪时已由 stateful_decide 改写，不能再次递减跳数
         * IPv6 普通转发：目的 MAC 改为下一跳 MAC，源 MAC 改为出口 MAC，Hop Limit 减 1
         *
         * &rx[i] 允许改写函数在需要复制报文时替换 mbuf 指针，后续发送使用替换后的指针
         * &packet 提供已解析的头部信息，decision.ipv4 是 IPv4/IPv6 共用的转发结果
         * 改写失败则释放当前包；成功后继续加入出口发送批次，此处尚未交给网卡
         */
        if (decision.routed && runtime->conntrack == NULL) {
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
        /** 将规则 MARK 写入 mbuf 的附加信息并设置有效标志，不向报文字节中插入标记 */
        if (decision.has_mark) {
            rx[i]->hash.fdir.hi = decision.mark_id;
            rx[i]->ol_flags |= RTE_MBUF_F_RX_FDIR_ID;
        }
        /**
         * 以出口在 devices.ports 中的数组下标分组，指针相减得到元素间距
         * 例：ports[0].port_id = 1、ports[1].port_id = 5，出口为网口 5 时 index 为 1
         * tx[index]、tx_lengths[index]、counts[index] 和 outputs[index] 都属于这个出口
         */
        uint16_t index = (uint16_t)(egress - runtime->devices.ports);
        /**
         * 多段报文且出口未启用多段发送时，尝试将全部数据合并到第一段
         * 第一段空间不足等原因导致合并失败时，统计并释放当前包，继续处理下一个
         */
        if (rx[i]->nb_segs > 1U && (egress->configured_tx_offloads & RTE_ETH_TX_OFFLOAD_MULTI_SEGS) == 0 &&
            rte_pktmbuf_linearize(rx[i]) != 0) {
            outputs[index].tx_drops++;
            outputs[index].tx_linearize_drops++;
            rte_pktmbuf_free(rx[i]);
            continue;
        }
        /**
         * 将当前包追加到对应出口的待发送批次，只保存指针，此处尚未交给驱动
         * 提前保存长度供发送统计使用，成功提交后不再读取已转移所有权的 mbuf
         * 例：counts[index] 为 2，长度和指针都写入下标 2，然后 counts[index] 增为 3
         * counts 只统计本次 process_batch 收集的包，本批处理结束就发送，不等待凑满
         */
        tx_lengths[index][counts[index]] = length;
        tx[index][counts[index]++] = rx[i];
    }
    /** 接收批次内所有路径都会汇合到这里，先提交 COUNT 再发送或报告 QSBR 静默 */
    dppd_software_count_batch_flush(&rule_counts);
    /** 逐包处理已完成，进入本文件的 flush_egress，按出口提交各自的待发送批次 */
    for (uint16_t index = 0; index < runtime->devices.nb_ports; ++index)
        flush_egress(worker, &runtime->devices.ports[index], tx[index], tx_lengths[index], counts[index], &delta, &outputs[index]);
    /**
     * 此时 delta 保存本批入口接收和处理中的丢弃统计，outputs 按出口保存发送及发送失败统计
     * 先将 delta 记入入口网口，再将各 outputs 记入对应出口，并合并到 delta 供 worker 汇总
     * 例：入口 0 收到 3 包，均送往出口 1，其中 2 包提交成功、1 包因发送队列未接收而丢弃
     *   入口 0：RX 加 3；出口 1：TX 加 2、tx_queue_drops 加 1；worker 汇总包含这三项
     */
    dppd_stats_add(&worker->port_stats[ingress - runtime->devices.ports], &delta);
    for (uint16_t index = 0; index < runtime->devices.nb_ports; ++index) {
        dppd_stats_add(&worker->port_stats[index], &outputs[index]);
        dppd_stats_accumulate(&delta, &outputs[index]);
    }
    /** 每批只更新一次工作线程汇总，避免入口丢弃已累计而出口字节还没累计的额外窗口 */
    dppd_stats_add(&worker->stats, &delta);
}

/**
 * 从入口端口的当前 worker 队列收包，再交给本文件的 process_batch 处理
 */
static void process_ingress(struct dppd_worker *worker, const struct dppd_port *ingress)
{
    if (worker_io_quiesced(worker) || atomic_load_explicit(&ingress->removed, memory_order_acquire))
        return;
    struct rte_mbuf *packets[DPPD_MAX_BURST];
    /** 最多接收 burst_size 个包，不等待凑满 */
    uint16_t count = rte_eth_rx_burst(ingress->port_id, worker->queue_id, packets, worker->runtime->config.burst_size);
    process_batch(worker, ingress, packets, count, false, 0);
}

/**
 * 从 ND 等待数组取回本队列的报文，每次最多处理 DPPD_MAX_BURST 个
 *
 * dppd_nd6_take 返回 true → 当前线程重新持有 pending.mbuf
 *   报文失效或原入口已不存在 → 释放 mbuf，更新丢弃统计
 *   否则 → process_batch 重新解析，并按最新软件规则、会话和路由决定去向
 *
 * retry 传 true，避免重复累计 RX 和规则 COUNT；expires_ms 沿用首次入队的截止时间
 * 重新处理后仍缺 MAC 的报文可以再次入队，不能依靠反复入队延长等待期限
 */
static void process_waiting(struct dppd_worker *worker)
{
    struct dppd_nd6_pending pending;
    for (unsigned int i = 0;
         i < DPPD_MAX_BURST && dppd_nd6_take(worker->runtime->nd6, worker->queue_id, dppd_monotonic_ms(), &pending);
         ++i) {
        const struct dppd_port *ingress = dppd_devices_find(&worker->runtime->devices, pending.ingress);
        /** 取回不代表可以发送，失效包或已失去原入口的包在此归还 mbuf */
        if (pending.expired || ingress == NULL) {
            rte_pktmbuf_free(pending.mbuf);
            struct dppd_stats_values dropped = {
                .policy_drops = 1, .nd6_queue_drops = 1
            };
            dppd_stats_add(&worker->stats, &dropped);
            if (ingress != NULL)
                dppd_stats_add(&worker->port_stats[ingress - worker->runtime->devices.ports], &dropped);
        } else
            process_batch(worker, ingress, &pending.mbuf, 1, true, pending.expires_ms);
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

    /** 设备事件直接通知所有工作线程退出，不依赖主线程先设置普通停止标记 */
    while (!worker_io_quiesced(worker)) {
        uint16_t i;

        if (runtime->nd6 != NULL) {
            if (worker->queue_id == 0)
                dppd_nd6_tick(runtime->nd6, dppd_monotonic_ms(), send_nd6, worker);
            process_waiting(worker);
        }
        /** 依次处理各网口的本 worker 队列，一个 process_batch 返回后继续下一个网口 */
        for (i = 0; i < runtime->devices.nb_ports; ++i)
            process_ingress(worker, &runtime->devices.ports[i]);
        /**
         * 一轮扫描结束后报告本线程已不再使用之前借用的规则和路由表，供回收器判断旧表能否释放
         * 静默点是读者进度报告，不是线程休眠；即使没有收包也要报告，然后继续下一轮
         * 例：控制面将规则 A 换成 B，仍在使用 A 的线程未报告安全点前，回收器必须保留 A
         * 本轮规则匹配和 COUNT 提交均已结束，此处报告进度；回收条件满足后由控制面释放旧表
         */
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
