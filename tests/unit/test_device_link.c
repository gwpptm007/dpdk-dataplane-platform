#include <assert.h>
#include <errno.h>
#include <string.h>
#include <rte_ethdev.h>
#include "dppd/device.h"

static int errors[2];
static bool up[2];
static unsigned int calls[2];
static bool removed[2];
static bool valid[2] = {true, true};

int __wrap_rte_eth_dev_is_valid_port(uint16_t port_id);
/** 与链路返回值独立控制端口是否仍存在，覆盖编号失效但没有链路错误的情况 */
int __wrap_rte_eth_dev_is_valid_port(uint16_t port_id)
{
    assert(port_id == 5 || port_id == 9);
    return valid[port_id == 5 ? 0 : 1];
}

int __wrap_rte_eth_dev_is_removed(uint16_t port_id);
/** 模拟 PMD 的移除探测，不需要真正拔出设备，也不借助 link API 返回错误 */
int __wrap_rte_eth_dev_is_removed(uint16_t port_id)
{
    assert(port_id == 5 || port_id == 9);
    return removed[port_id == 5 ? 0 : 1];
}

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
    atomic_init(&devices.removal_requested, false);
    for (unsigned int i = 0; i < 2; ++i) {
        devices.ports[i].port_id = i == 0 ? 5 : 9;
        devices.ports[i].started = true;
        atomic_init(&devices.ports[i].removed, false);
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
    errors[0] = 0;
    /** 即使第二端口已跳过不支持的链路查询，仍必须发布设备组移除请求 */
    removed[1] = true;
    assert(dppd_devices_poll_links(&devices) == -ENODEV);
    assert(dppd_devices_removal_requested(&devices));
    assert(atomic_load(&devices.ports[1].removed));
    assert(!dppd_port_tx_available(&devices.ports[1]));
    /** 后续探测恢复正常也不能清除历史移除标记，更不能重新允许发送 */
    removed[1] = false;
    atomic_store(&devices.ports[1].link_state, DPPD_LINK_UP);
    assert(!dppd_port_tx_available(&devices.ports[1]));
    assert(dppd_devices_poll_links(&devices) == -ENODEV);

    /** 仅在测试中重置场景，再验证端口编号失效也会触发移除退出 */
    atomic_store(&devices.ports[1].removed, false);
    atomic_store(&devices.ports[1].link_state, DPPD_LINK_UNSUPPORTED);
    atomic_store(&devices.removal_requested, false);
    valid[1] = false;
    assert(dppd_devices_poll_links(&devices) == -ENODEV);
    assert(dppd_devices_removal_requested(&devices));
    return 0;
}
