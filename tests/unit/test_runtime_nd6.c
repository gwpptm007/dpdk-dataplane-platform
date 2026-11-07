#include <assert.h>
#include <errno.h>
#include <string.h>
#include "dppd/runtime.h"

/** 用端口号索引驱动夹具，保持端口号与 runtime 数组位置不一致 */
static int modes[16], get_error_port, enable_error_port, disable_error_port;
static int unreadable_port, unchanged_port, disable_unchanged_port;
static bool mutate_on_failure;
static unsigned int gets, enables, disables;

int __wrap_rte_eth_allmulticast_get(uint16_t port);
int __wrap_rte_eth_allmulticast_enable(uint16_t port);
int __wrap_rte_eth_allmulticast_disable(uint16_t port);

int __wrap_rte_eth_allmulticast_get(uint16_t port)
{
    assert(port < 16);
    ++gets;
    if (port == get_error_port)
        return -ENOTSUP;
    /** 只破坏开启后的第一次回读，补偿仍能重新核对关闭结果 */
    if (port == unreadable_port && enables != 0) {
        unreadable_port = -1;
        return -EIO;
    }
    return modes[port];
}

int __wrap_rte_eth_allmulticast_enable(uint16_t port)
{
    assert(port < 16);
    ++enables;
    if (port == enable_error_port) {
        if (mutate_on_failure)
            modes[port] = 1;
        return -ENOTSUP;
    }
    if (port != unchanged_port)
        modes[port] = 1;
    return 0;
}

int __wrap_rte_eth_allmulticast_disable(uint16_t port)
{
    assert(port < 16);
    ++disables;
    if (port == disable_error_port)
        return -EIO;
    if (port != disable_unchanged_port)
        modes[port] = 0;
    return 0;
}

static void reset(struct dppd_runtime *runtime)
{
    memset(runtime, 0, sizeof(*runtime));
    runtime->devices.nb_ports = 2;
    atomic_init(&runtime->devices.removal_requested, false);
    atomic_init(&runtime->devices.event_tracking_enabled, false);
    atomic_init(&runtime->devices.event_io_quiesced, false);
    atomic_init(&runtime->devices.event_sequence_overflow, false);
    atomic_init(&runtime->devices.event_callbacks_in_flight, 0);
    atomic_init(&runtime->devices.event_sequence, 0);
    atomic_init(&runtime->devices.resumed_event_sequence, 0);
    for (uint16_t i = 0; i < 2; ++i) {
        runtime->devices.ports[i].port_id = (uint16_t)(3 + 4 * i);
        runtime->devices.ports[i].started = true;
        atomic_init(&runtime->devices.ports[i].removed, false);
    }
    memset(modes, 0, sizeof(modes));
    get_error_port = enable_error_port = disable_error_port = -1;
    unreadable_port = unchanged_port = disable_unchanged_port = -1;
    mutate_on_failure = false;
    gets = enables = disables = 0;
}

static struct dppd_nd6_policy policy(void)
{
    struct dppd_nd6_policy result = {.local_count = 3, .reachable_ms = 1000};
    for (uint32_t i = 0; i < 3; ++i) {
        result.locals[i].port_id = i == 2 ? 7 : 3;
        result.locals[i].address[0] = 0xfe;
        result.locals[i].address[1] = 0x80;
        result.locals[i].address[15] = (uint8_t)(i + 1);
    }
    return result;
}

static void prerequisites(void)
{
    struct dppd_runtime runtime;
    struct dppd_nd6_rx_preparation preparation;
    struct dppd_nd6_policy current = {0};

    reset(&runtime);
    runtime.devices.nb_ports = 0;
    get_error_port = 3;
    assert(dppd_runtime_prepare_nd6(&runtime, &current, &preparation) == 0);
    assert(preparation.count == 0 && gets == 0 && enables == 0 && disables == 0);
    reset(&runtime);
    current = policy();
    runtime.workers_started = true;
    assert(dppd_runtime_prepare_nd6(&runtime, &current, &preparation) == -EBUSY);
    runtime.workers_started = false;
    current.reachable_ms = 1;
    assert(dppd_runtime_prepare_nd6(&runtime, &current, &preparation) == -EINVAL);
    current = policy();
    current.locals[2].port_id = 15;
    assert(dppd_runtime_prepare_nd6(&runtime, &current, &preparation) == -ENODEV);
    assert(gets == 0 && enables == 0 && disables == 0);
    current = policy();
    runtime.devices.ports[1].started = false;
    assert(dppd_runtime_prepare_nd6(&runtime, &current, &preparation) == -ENODEV);
    runtime.devices.ports[1].started = true;
    atomic_store(&runtime.devices.ports[1].removed, true);
    assert(dppd_runtime_prepare_nd6(&runtime, &current, &preparation) == -ENODEV);
    assert(gets == 0 && enables == 0 && disables == 0);
    reset(&runtime);
    atomic_store(&runtime.devices.removal_requested, true);
    assert(dppd_runtime_prepare_nd6(&runtime, &current, &preparation) == -ENODEV);
    assert(gets == 0);
    reset(&runtime);
    get_error_port = 7;
    assert(dppd_runtime_prepare_nd6(&runtime, &current, &preparation) == -ENOTSUP);
    assert(gets == 2 && enables == 0 && disables == 0);
}

static void successful_preparation(void)
{
    struct dppd_runtime runtime;
    struct dppd_nd6_rx_preparation preparation, repeated;
    struct dppd_nd6_policy current = policy();

    reset(&runtime);
    modes[7] = 1;
    assert(dppd_runtime_prepare_nd6(&runtime, &current, &preparation) == 0);
    /** 同一网口配置两个地址只开一次，原本已开启的网口不加入补偿清单 */
    assert(enables == 1 && preparation.count == 1 && preparation.ports[0] == 3);
    assert(modes[3] == 1 && modes[7] == 1);
    assert(dppd_runtime_prepare_nd6(&runtime, &current, &repeated) == 0);
    assert(enables == 1 && repeated.count == 0);
    assert(dppd_runtime_rollback_nd6(&runtime, &preparation) == 0);
    assert(preparation.count == 0 && modes[3] == 0 && modes[7] == 1 && disables == 1);
    assert(dppd_runtime_rollback_nd6(&runtime, &preparation) == 0 && disables == 1);
}

static void rejected_preparation(void)
{
    struct dppd_runtime runtime;
    struct dppd_nd6_rx_preparation preparation;
    struct dppd_nd6_policy current = policy();

    reset(&runtime);
    enable_error_port = 7;
    assert(dppd_runtime_prepare_nd6(&runtime, &current, &preparation) == -ENOTSUP);
    assert(modes[3] == 0 && modes[7] == 0 && preparation.count == 0 && disables == 1);
    reset(&runtime);
    enable_error_port = 3;
    assert(dppd_runtime_prepare_nd6(&runtime, &current, &preparation) == -ENOTSUP);
    assert(enables == 1 && disables == 0 && preparation.count == 0);
    reset(&runtime);
    enable_error_port = 7;
    mutate_on_failure = true;
    assert(dppd_runtime_prepare_nd6(&runtime, &current, &preparation) == -ENOTSUP);
    assert(modes[3] == 0 && modes[7] == 0 && disables == 2 && preparation.count == 0);
    reset(&runtime);
    unchanged_port = 3;
    assert(dppd_runtime_prepare_nd6(&runtime, &current, &preparation) == -EIO);
    assert(enables == 1 && disables == 1 && preparation.count == 0);
    reset(&runtime);
    unreadable_port = 7;
    assert(dppd_runtime_prepare_nd6(&runtime, &current, &preparation) == -EIO);
    assert(modes[3] == 0 && modes[7] == 0 && disables == 2 && preparation.count == 0);
}

static void uncertain_rollback(void)
{
    struct dppd_runtime runtime;
    struct dppd_nd6_rx_preparation preparation;
    struct dppd_nd6_policy current = policy();

    reset(&runtime);
    assert(dppd_runtime_prepare_nd6(&runtime, &current, &preparation) == 0);
    disable_error_port = 3;
    assert(dppd_runtime_rollback_nd6(&runtime, &preparation) == -EUCLEAN);
    assert(preparation.count == 1 && preparation.ports[0] == 3 && modes[3] == 1 && modes[7] == 0);
    assert(disables == 2);
    disable_error_port = -1;
    assert(dppd_runtime_rollback_nd6(&runtime, &preparation) == 0 && preparation.count == 0);
    reset(&runtime);
    enable_error_port = 7;
    disable_error_port = 3;
    assert(dppd_runtime_prepare_nd6(&runtime, &current, &preparation) == -EUCLEAN);
    assert(modes[3] == 1 && preparation.count == 1);
    reset(&runtime);
    assert(dppd_runtime_prepare_nd6(&runtime, &current, &preparation) == 0);
    disable_unchanged_port = 7;
    assert(dppd_runtime_rollback_nd6(&runtime, &preparation) == -EUCLEAN);
    assert(modes[3] == 0 && modes[7] == 1 && preparation.count == 1 && preparation.ports[0] == 7);
}

int main(void)
{
    prerequisites();
    successful_preparation();
    rejected_preparation();
    uncertain_rollback();
    return 0;
}
