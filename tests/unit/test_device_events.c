#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <rte_dev.h>
#include <rte_ethdev.h>
#include <rte_version.h>
#include "dppd/device.h"

/** 只替换回调登记与设备关闭，不创建真实 ethdev，也不执行复位或网络变更 */
struct registration {
    rte_eth_dev_cb_fn function;
    void *context;
    bool active;
};

static struct registration registrations[2][3];
static unsigned int register_calls, unregister_calls, stop_calls, close_calls, unregister_busy;
static int register_error, unregister_error;
static bool immediate_reset;
#if RTE_VERSION < RTE_VERSION_NUM(22, 11, 0, 0)
static struct rte_device fake_device = {.name = "test-device"};
#else
static char fake_device;
#endif

static unsigned int port_index(uint16_t port)
{
    assert(port == 5 || port == 11);
    return port == 5 ? 0 : 1;
}

static unsigned int event_index(enum rte_eth_event_type event)
{
    assert(event == RTE_ETH_EVENT_INTR_RMV || event == RTE_ETH_EVENT_INTR_RESET || event == RTE_ETH_EVENT_DESTROY);
    return event == RTE_ETH_EVENT_INTR_RMV ? 0 : event == RTE_ETH_EVENT_INTR_RESET ? 1 : 2;
}

/** 终结清理同样使用明确的设备身份夹具，不能靠空名称让两份未知观察互相作证 */
int __wrap_rte_eth_dev_info_get(uint16_t port, struct rte_eth_dev_info *info);
int __wrap_rte_eth_dev_info_get(uint16_t port, struct rte_eth_dev_info *info)
{
    (void)port_index(port);
    *info = (struct rte_eth_dev_info){.device = (struct rte_device *)&fake_device, .driver_name = "test_driver"};
    return 0;
}

int __wrap_rte_eth_dev_get_name_by_port(uint16_t port, char *name);
int __wrap_rte_eth_dev_get_name_by_port(uint16_t port, char *name)
{
    (void)port_index(port);
    snprintf(name, RTE_ETH_NAME_MAX_LEN, "test-port%u", port);
    return 0;
}

int __wrap_rte_eth_dev_is_valid_port(uint16_t port);
int __wrap_rte_eth_dev_is_valid_port(uint16_t port)
{
    (void)port_index(port);
    return 1;
}

#if RTE_VERSION >= RTE_VERSION_NUM(22, 11, 0, 0)
const char *__wrap_rte_dev_name(const struct rte_device *device);
const char *__wrap_rte_dev_name(const struct rte_device *device)
{
    assert(device == (const struct rte_device *)&fake_device);
    return "test-device";
}
#endif

int __wrap_rte_eth_dev_callback_register(uint16_t port, enum rte_eth_event_type event, rte_eth_dev_cb_fn function,
    void *context);
int __wrap_rte_eth_dev_callback_register(uint16_t port, enum rte_eth_event_type event, rte_eth_dev_cb_fn function,
    void *context)
{
    ++register_calls;
    assert(stop_calls == 0 && close_calls == 0);
    if (register_error != 0 && port == 5 && event == RTE_ETH_EVENT_DESTROY)
        return register_error;
    struct registration *entry = &registrations[port_index(port)][event_index(event)];
    assert(!entry->active);
    *entry = (struct registration){.function = function, .context = context, .active = true};
    if (immediate_reset && port == 5 && event == RTE_ETH_EVENT_INTR_RESET)
        assert(function(port, event, context, NULL) == 0);
    return 0;
}

int __wrap_rte_eth_dev_callback_unregister(uint16_t port, enum rte_eth_event_type event, rte_eth_dev_cb_fn function,
    void *context);
int __wrap_rte_eth_dev_callback_unregister(uint16_t port, enum rte_eth_event_type event, rte_eth_dev_cb_fn function,
    void *context)
{
    ++unregister_calls;
    assert(stop_calls == 0 && close_calls == 0);
    struct registration *entry = &registrations[port_index(port)][event_index(event)];
    assert(entry->active && entry->function == function && entry->context == context);
    if (unregister_busy != 0) {
        --unregister_busy;
        return -EAGAIN;
    }
    if (unregister_error != 0)
        return unregister_error;
    entry->active = false;
    return 0;
}

static void assert_no_callbacks(void)
{
    for (unsigned int i = 0; i < 2; ++i)
        for (unsigned int j = 0; j < 3; ++j)
            assert(!registrations[i][j].active);
}

int __wrap_rte_eth_dev_stop(uint16_t port);
int __wrap_rte_eth_dev_stop(uint16_t port)
{
    (void)port_index(port);
    assert_no_callbacks();
    ++stop_calls;
    return 0;
}

int __wrap_rte_eth_dev_close(uint16_t port);
int __wrap_rte_eth_dev_close(uint16_t port)
{
    (void)port_index(port);
    assert_no_callbacks();
    ++close_calls;
    return 0;
}

static void setup(struct dppd_device_set *devices)
{
    memset(devices, 0, sizeof(*devices));
    memset(registrations, 0, sizeof(registrations));
    register_calls = unregister_calls = stop_calls = close_calls = unregister_busy = 0;
    register_error = unregister_error = 0;
    immediate_reset = false;
    devices->nb_ports = 2;
    atomic_init(&devices->removal_requested, false);
    atomic_init(&devices->event_tracking_enabled, false);
    for (unsigned int i = 0; i < 2; ++i) {
        devices->ports[i].port_id = i == 0 ? 5 : 11;
        devices->ports[i].started = devices->ports[i].configured = true;
        devices->ports[i].identity.device_name_known = true;
        snprintf(devices->ports[i].identity.device_name, sizeof(devices->ports[i].identity.device_name), "test-port%u",
            devices->ports[i].port_id);
        devices->topology.endpoints[i].ethdev_port_id = devices->ports[i].port_id;
        strcpy(devices->topology.endpoints[i].device_name, "test-device");
        strcpy(devices->topology.endpoints[i].driver_name, "test_driver");
        atomic_init(&devices->ports[i].removed, false);
        atomic_init(&devices->ports[i].link_state, DPPD_LINK_UP);
    }
    devices->topology.nb_endpoints = 2;
}

static void deliver(uint16_t port, enum rte_eth_event_type event)
{
    const struct registration *entry = &registrations[port_index(port)][event_index(event)];
    assert(entry->active);
    assert(entry->function(port, event, entry->context, NULL) == 0);
}

static void *many_resets(void *argument)
{
    uint16_t port = *(const uint16_t *)argument;
    for (unsigned int i = 0; i < 1000; ++i)
        deliver(port, RTE_ETH_EVENT_INTR_RESET);
    return NULL;
}

int main(void)
{
    struct dppd_device_set devices;
    struct dppd_device_event_snapshot snapshot;
    setup(&devices);
    dppd_devices_event_snapshot(&devices, &snapshot);
    assert(!snapshot.enabled && !snapshot.quiesced && snapshot.sequence == 0);
    assert(!dppd_devices_io_quiesced(&devices));
    assert(dppd_devices_resume_after_event(&devices, 0) == -EINVAL);
    assert(dppd_devices_enable_event_tracking(&devices) == 0 && register_calls == 6);
    assert(dppd_devices_enable_event_tracking(&devices) == 0 && register_calls == 6);
    deliver(11, RTE_ETH_EVENT_INTR_RESET);
    dppd_devices_event_snapshot(&devices, &snapshot);
    assert(snapshot.enabled && snapshot.quiesced && snapshot.sequence == 1 && snapshot.reset_ports == 2);
    assert(snapshot.removal_ports == 0 && snapshot.destroy_ports == 0 && snapshot.callbacks_in_flight == 0);
    assert(!dppd_devices_removal_requested(&devices));
    assert(atomic_load(&devices.ports[1].link_state) == DPPD_LINK_UP);
    assert(dppd_devices_resume_after_event(&devices, 0) == -ESTALE);
    atomic_store(&devices.event_callbacks_in_flight, 1);
    assert(dppd_devices_resume_after_event(&devices, 1) == -EBUSY);
    atomic_store(&devices.event_callbacks_in_flight, 0);
    assert(dppd_devices_resume_after_event(&devices, 1) == 0 && !dppd_devices_io_quiesced(&devices));
    deliver(5, RTE_ETH_EVENT_INTR_RESET);
    assert(dppd_devices_resume_after_event(&devices, 1) == -ESTALE);
    dppd_devices_event_snapshot(&devices, &snapshot);
    assert(snapshot.sequence == 2 && snapshot.reset_ports == 3);
    assert(dppd_devices_resume_after_event(&devices, 2) == 0);
    const struct registration *entry = &registrations[0][1];
    assert(entry->function(5, RTE_ETH_EVENT_INTR_LSC, entry->context, NULL) == 0);
    assert(entry->function(99, RTE_ETH_EVENT_INTR_RESET, entry->context, NULL) == 0);
    dppd_devices_event_snapshot(&devices, &snapshot);
    assert(snapshot.sequence == 2 && !snapshot.quiesced);
    unregister_busy = 2;
    assert(dppd_devices_stop(&devices) == 0 && unregister_calls == 8 && stop_calls == 2 && close_calls == 2);

    /** 登记返回之前同步收到事件也必须保留，不能在后续登记时重置序号或门禁 */
    setup(&devices);
    immediate_reset = true;
    assert(dppd_devices_enable_event_tracking(&devices) == 0);
    dppd_devices_event_snapshot(&devices, &snapshot);
    assert(snapshot.sequence == 1 && snapshot.reset_ports == 1 && snapshot.quiesced);
    assert(dppd_devices_stop(&devices) == 0);

    /** 多个后台回调只改原子字段，不丢失序号，也不调用 stop 或 close */
    setup(&devices);
    assert(dppd_devices_enable_event_tracking(&devices) == 0);
    pthread_t threads[2];
    uint16_t ports[] = {5, 11};
    for (unsigned int i = 0; i < 2; ++i)
        assert(pthread_create(&threads[i], NULL, many_resets, &ports[i]) == 0);
    for (unsigned int i = 0; i < 2; ++i)
        assert(pthread_join(threads[i], NULL) == 0);
    dppd_devices_event_snapshot(&devices, &snapshot);
    assert(snapshot.sequence == 2000 && snapshot.reset_ports == 3 && snapshot.callbacks_in_flight == 0);
    assert(register_calls == 6 && stop_calls == 0 && close_calls == 0);
    assert(dppd_devices_resume_after_event(&devices, 2000) == 0);
    atomic_store(&devices.event_sequence, UINT64_MAX);
    deliver(5, RTE_ETH_EVENT_INTR_RESET);
    dppd_devices_event_snapshot(&devices, &snapshot);
    assert(snapshot.sequence == UINT64_MAX && snapshot.overflowed && snapshot.quiesced);
    assert(dppd_devices_resume_after_event(&devices, UINT64_MAX) == -EOVERFLOW);
    /** 模拟饱和回调恰好先完成而恢复方随后清门的终态，溢出本身必须永久封门 */
    atomic_store(&devices.resumed_event_sequence, UINT64_MAX);
    atomic_store(&devices.event_io_quiesced, false);
    assert(dppd_devices_io_quiesced(&devices));
    assert(dppd_devices_stop(&devices) == 0);

    /** 移除与销毁均为永久事件，本接口不能因为链路再次变好而重新放行 */
    const enum rte_eth_event_type fatal[] = {RTE_ETH_EVENT_INTR_RMV, RTE_ETH_EVENT_DESTROY};
    for (unsigned int i = 0; i < 2; ++i) {
        setup(&devices);
        assert(dppd_devices_enable_event_tracking(&devices) == 0);
        deliver(11, fatal[i]);
        dppd_devices_event_snapshot(&devices, &snapshot);
        assert(snapshot.sequence == 1 && snapshot.quiesced && dppd_devices_removal_requested(&devices));
        assert(snapshot.removal_ports == (i == 0 ? 2U : 0U) && snapshot.destroy_ports == (i == 1 ? 2U : 0U));
        assert(atomic_load(&devices.ports[1].removed) && !dppd_port_tx_available(&devices.ports[1]));
        assert(dppd_devices_resume_after_event(&devices, 1) == -ENODEV);
        int result = dppd_devices_stop(&devices);
        assert(result == (i == 0 ? 0 : -ENODEV));
        assert(stop_calls == (i == 0 ? 2U : 1U) && close_calls == (i == 0 ? 2U : 1U));
    }

    setup(&devices);
    register_error = -ENOMEM;
    assert(dppd_devices_enable_event_tracking(&devices) == -ENOMEM && register_calls == 3);
    assert(devices.ports[0].removal_callback_registered && devices.ports[0].reset_callback_registered);
    assert(!devices.ports[0].destroy_callback_registered && dppd_devices_io_quiesced(&devices));
    assert(dppd_devices_resume_after_event(&devices, 0) == -EUCLEAN);
    register_error = 0;
    assert(dppd_devices_enable_event_tracking(&devices) == 0 && register_calls == 7);
    assert(dppd_devices_resume_after_event(&devices, 0) == 0);
    unregister_error = -EINVAL;
    assert(dppd_devices_stop(&devices) == -EINVAL && stop_calls == 0 && close_calls == 0);
    unregister_error = 0;
    assert(dppd_devices_stop(&devices) == 0 && stop_calls == 2 && close_calls == 2);

    setup(&devices);
    devices.ports[1].port_id = 5;
    assert(dppd_devices_enable_event_tracking(&devices) == -EEXIST && register_calls == 0);
    assert(dppd_devices_enable_event_tracking(NULL) == -EINVAL);
    assert(dppd_devices_resume_after_event(NULL, 0) == -EINVAL);
    dppd_devices_event_snapshot(NULL, &snapshot);
    assert(!snapshot.enabled && snapshot.sequence == 0);
    dppd_devices_event_snapshot(&devices, NULL);
    return 0;
}
