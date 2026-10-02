#include <assert.h>
#include <errno.h>
#include <string.h>
#include <rte_ethdev.h>
#include <rte_mempool.h>
#include "dppd/device.h"

static int stop_errors[2], close_errors[2], unregister_error;
static unsigned int stops, closes, frees, unregisters, busy_count;
static struct rte_mempool pool;

int __wrap_rte_eth_dev_stop(uint16_t port_id);
int __wrap_rte_eth_dev_stop(uint16_t port_id)
{
    assert(port_id == 7 || port_id == 11);
    stops++;
    return stop_errors[port_id == 7 ? 0 : 1];
}

int __wrap_rte_eth_dev_close(uint16_t port_id);
int __wrap_rte_eth_dev_close(uint16_t port_id)
{
    assert(port_id == 7 || port_id == 11);
    closes++;
    return close_errors[port_id == 7 ? 0 : 1];
}

unsigned int __wrap_rte_mempool_avail_count(const struct rte_mempool *value);
unsigned int __wrap_rte_mempool_avail_count(const struct rte_mempool *value)
{
    assert(value == &pool);
    return pool.size;
}

void __wrap_rte_mempool_free(struct rte_mempool *value);
void __wrap_rte_mempool_free(struct rte_mempool *value)
{
    assert(value == &pool && closes == 2);
    frees++;
}

int __wrap_rte_eth_dev_callback_unregister(uint16_t port_id, enum rte_eth_event_type event,
                                           rte_eth_dev_cb_fn function, void *context);
int __wrap_rte_eth_dev_callback_unregister(uint16_t port_id, enum rte_eth_event_type event,
                                           rte_eth_dev_cb_fn function, void *context)
{
    assert(port_id == 7 && event == RTE_ETH_EVENT_INTR_RMV);
    assert(function != NULL && context != NULL);
    assert(stops == 0 && closes == 0 && frees == 0);
    unregisters++;
    if (busy_count != 0) {
        busy_count--;
        return -EAGAIN;
    }
    return unregister_error;
}

static void reset(struct dppd_device_set *devices)
{
    memset(devices, 0, sizeof(*devices));
    memset(stop_errors, 0, sizeof(stop_errors));
    memset(close_errors, 0, sizeof(close_errors));
    stops = closes = frees = unregisters = busy_count = 0;
    unregister_error = 0;
    devices->nb_ports = 2;
    atomic_init(&devices->removal_requested, false);
    for (unsigned int i = 0; i < 2; ++i) {
        devices->ports[i].port_id = i == 0 ? 7 : 11;
        devices->ports[i].configured = true;
        devices->ports[i].started = true;
        atomic_init(&devices->ports[i].removed, false);
        atomic_init(&devices->ports[i].link_state, DPPD_LINK_UP);
    }
    pool.size = 32;
    devices->pools[0] = &pool;
}

int main(void)
{
    struct dppd_device_set devices;

    reset(&devices);
    assert(dppd_devices_stop(&devices) == 0);
    assert(stops == 2 && closes == 2 && frees == 1);
    assert(devices.nb_ports == 0 && devices.pools[0] == NULL);

    reset(&devices);
    stop_errors[0] = -EIO;
    assert(dppd_devices_stop(&devices) == -EIO);
    assert(stops == 2 && closes == 2 && frees == 1);
    assert(devices.nb_ports == 0 && devices.pools[0] == NULL);

    reset(&devices);
    close_errors[0] = -EIO;
    assert(dppd_devices_stop(&devices) == -EIO);
    assert(stops == 2 && closes == 2 && frees == 0);
    assert(devices.nb_ports == 2 && devices.pools[0] == &pool);
    assert(devices.ports[0].configured && !devices.ports[1].configured);

    reset(&devices);
    devices.ports[0].removal_callback_registered = true;
    unregister_error = -EINVAL;
    assert(dppd_devices_stop(&devices) == -EINVAL);
    assert(unregisters == 1 && stops == 0 && closes == 0 && frees == 0);
    assert(devices.ports[0].removal_callback_registered && devices.pools[0] == &pool);

    reset(&devices);
    devices.ports[0].removal_callback_registered = true;
    busy_count = 2;
    assert(dppd_devices_stop(&devices) == 0);
    assert(unregisters == 3 && stops == 2 && closes == 2 && frees == 1);
    assert(!devices.ports[0].removal_callback_registered && devices.nb_ports == 0);
    assert(dppd_devices_stop(NULL) == -EINVAL);
    return 0;
}
