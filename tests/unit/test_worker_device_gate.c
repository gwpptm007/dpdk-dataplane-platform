#include <assert.h>
#include <errno.h>
#include <string.h>
#include <rte_ethdev.h>
#include <rte_mbuf.h>
#include "dppd/runtime.h"

static struct dppd_runtime runtime;
static struct rte_mbuf packets[2];
static unsigned int rx_calls[2], tx_calls, freed;
static bool quiesce_in_rx, return_packet;

/** 模拟第一端口的 RX 调用中收到事件，用实际 worker 循环核对第二端口不会再开始收包 */
static uint16_t fixture_rx_burst(uint16_t port, uint16_t queue, struct rte_mbuf **received, uint16_t count)
{
    assert(port < 2 && queue == 0 && count != 0);
    ++rx_calls[port];
    if (quiesce_in_rx)
        atomic_store_explicit(&runtime.devices.event_io_quiesced, true, memory_order_release);
    if (return_packet) {
        received[0] = &packets[0];
        return 1;
    }
    return 0;
}

static uint16_t fixture_tx_burst(uint16_t port, uint16_t queue, struct rte_mbuf **sent, uint16_t count)
{
    assert(port < 2 && queue == 0 && sent != NULL);
    ++tx_calls;
    return count;
}

/** 这里只核对仍归 worker 所有的对象是否被归还，不分配 mbuf 或碰真实缓冲池 */
static void fixture_free(struct rte_mbuf *packet)
{
    assert(packet == &packets[0] || packet == &packets[1]);
    ++freed;
}

/** 只替换测试编译中的驱动边界，直接使用正式 worker 函数体和收发前的判断 */
#define rte_eth_rx_burst fixture_rx_burst
#define rte_eth_tx_burst fixture_tx_burst
#define rte_pktmbuf_free fixture_free
#include "../../lib/dataplane/worker.c"
#undef rte_pktmbuf_free
#undef rte_eth_tx_burst
#undef rte_eth_rx_burst

static struct dppd_worker *setup(void)
{
    memset(&runtime, 0, sizeof(runtime));
    memset(rx_calls, 0, sizeof(rx_calls));
    tx_calls = freed = 0;
    quiesce_in_rx = return_packet = false;
    runtime.config.burst_size = 16;
    runtime.devices.nb_ports = 2;
    atomic_init(&runtime.stop_requested, false);
    atomic_init(&runtime.devices.removal_requested, false);
    atomic_init(&runtime.devices.event_tracking_enabled, true);
    atomic_init(&runtime.devices.event_io_quiesced, false);
    atomic_init(&runtime.devices.event_sequence_overflow, false);
    atomic_init(&runtime.devices.event_callbacks_in_flight, 0);
    atomic_init(&runtime.devices.event_sequence, 0);
    atomic_init(&runtime.devices.resumed_event_sequence, 0);
    for (unsigned int i = 0; i < 2; ++i) {
        runtime.devices.ports[i].port_id = i;
        runtime.devices.ports[i].peer_port_id = 1U - i;
        atomic_init(&runtime.devices.ports[i].removed, false);
        atomic_init(&runtime.devices.ports[i].link_state, DPPD_LINK_UP);
    }
    struct dppd_worker *worker = &runtime.workers[0];
    worker->runtime = &runtime;
    atomic_init(&worker->state, DPPD_WORKER_IDLE);
    dppd_stats_init(&worker->stats);
    for (unsigned int i = 0; i < 2; ++i)
        dppd_stats_init(&worker->port_stats[i]);
    return worker;
}

int main(void)
{
    struct dppd_worker *worker = setup();
    process_ingress(worker, &runtime.devices.ports[0]);
    assert(rx_calls[0] == 1 && rx_calls[1] == 0);
    atomic_store(&runtime.devices.event_io_quiesced, true);
    process_ingress(worker, &runtime.devices.ports[1]);
    assert(rx_calls[1] == 0);

    /** 已开始的 RX 返回后归还这批报文，下一入口与所有 TX 都必须被门禁挡住 */
    worker = setup();
    quiesce_in_rx = return_packet = true;
    assert(dppd_worker_main(worker) == 0);
    assert(rx_calls[0] == 1 && rx_calls[1] == 0 && tx_calls == 0 && freed == 1);
    assert(atomic_load(&worker->state) == DPPD_WORKER_STOPPED);

    worker = setup();
    struct rte_mbuf *batch[] = {&packets[0], &packets[1]};
    uint32_t lengths[] = {64, 73};
    struct dppd_stats_values ingress = {0}, output = {0};
    flush_egress(worker, &runtime.devices.ports[1], batch, lengths, 2, &ingress, &output);
    assert(tx_calls == 1 && output.tx_packets == 2 && output.tx_bytes == 137 && freed == 0);
    atomic_store(&runtime.devices.event_io_quiesced, true);
    flush_egress(worker, &runtime.devices.ports[1], batch, lengths, 2, &ingress, &output);
    assert(tx_calls == 1 && freed == 2 && ingress.egress_drops == 2 && ingress.policy_drops == 2);
    uint8_t wire[14] = {0};
    assert(send_nd6(worker, 1, wire, sizeof(wire)) == -ENODEV && tx_calls == 1);

    /** 原有普通停止与永久移除仍能封住入口，不依赖新事件跟踪默认开启 */
    worker = setup();
    atomic_store(&runtime.devices.event_tracking_enabled, false);
    atomic_store(&runtime.stop_requested, true);
    process_ingress(worker, &runtime.devices.ports[0]);
    assert(rx_calls[0] == 0);
    atomic_store(&runtime.stop_requested, false);
    atomic_store(&runtime.devices.removal_requested, true);
    process_ingress(worker, &runtime.devices.ports[0]);
    flush_egress(worker, &runtime.devices.ports[1], batch, lengths, 2, &ingress, &output);
    assert(rx_calls[0] == 0 && tx_calls == 0 && freed == 2);
    return 0;
}
