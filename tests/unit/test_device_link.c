#include <assert.h>
#include <errno.h>
#include <string.h>
#include <rte_ethdev.h>
#include "dppd/device.h"

static int errors[2];
static bool up[2];
static unsigned int calls[2];

int __wrap_rte_eth_link_get_nowait(uint16_t port_id, struct rte_eth_link *link);
int __wrap_rte_eth_link_get_nowait(uint16_t port_id, struct rte_eth_link *link)
{
    unsigned int index = port_id == 5 ? 0 : 1;

    assert(port_id == 5 || port_id == 9);
    calls[index]++;
    memset(link, 0, sizeof(*link));
    link->link_status = up[index];
    return errors[index];
}

int main(void)
{
    struct dppd_device_set devices = {0};

    devices.nb_ports = 2;
    for (unsigned int i = 0; i < 2; ++i) {
        devices.ports[i].port_id = i == 0 ? 5 : 9;
        devices.ports[i].started = true;
        atomic_init(&devices.ports[i].link_state, DPPD_LINK_UNKNOWN);
        assert(!dppd_port_tx_available(&devices.ports[i]));
    }
    up[0] = true;
    assert(dppd_devices_poll_links(&devices) == 0);
    assert(dppd_port_tx_available(&devices.ports[0]));
    assert(!dppd_port_tx_available(&devices.ports[1]));
    up[1] = true;
    assert(dppd_devices_poll_links(&devices) == 0);
    assert(dppd_port_tx_available(&devices.ports[1]));
    up[0] = false;
    assert(dppd_devices_poll_links(&devices) == 0);
    assert(!dppd_port_tx_available(&devices.ports[0]));
    assert(dppd_port_tx_available(&devices.ports[1]));
    up[0] = true;
    assert(dppd_devices_poll_links(&devices) == 0);
    assert(dppd_port_tx_available(&devices.ports[0]));

    errors[1] = -ENODEV;
    assert(dppd_devices_poll_links(&devices) == -ENODEV);
    assert(!dppd_port_tx_available(&devices.ports[1]));
    errors[1] = -ENOTSUP;
    assert(dppd_devices_poll_links(&devices) == -ENOTSUP);
    assert(!dppd_port_tx_available(&devices.ports[1]));
    atomic_store(&devices.ports[1].link_state, DPPD_LINK_UNKNOWN);
    assert(dppd_devices_poll_links(&devices) == 0);
    assert(atomic_load(&devices.ports[1].link_state) == DPPD_LINK_UNSUPPORTED);
    assert(dppd_port_tx_available(&devices.ports[1]));
    {
        unsigned int previous = calls[1];
        assert(dppd_devices_poll_links(&devices) == 0 && calls[1] == previous);
    }
    errors[0] = -EIO;
    assert(dppd_devices_poll_links(&devices) == -EIO);
    assert(!dppd_port_tx_available(&devices.ports[0]));
    assert(dppd_devices_poll_links(NULL) == -EINVAL);
    return 0;
}
