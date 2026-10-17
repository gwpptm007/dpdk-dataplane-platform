#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <rte_dev.h>
#include <rte_ethdev.h>
#include <rte_mbuf.h>
#include <rte_mempool.h>
#include <rte_version.h>
#include "dppd/device.h"

/** 所有 PMD 操作均为夹具，只验证主线程顺序和失败门禁，不接触真实设备 */
enum operation {
    OP_NONE,
    OP_INFO,
    OP_NAME,
    OP_FIRMWARE,
    OP_MAC,
    OP_STOP,
    OP_RESET,
    OP_CONFIGURE,
    OP_ADJUST,
    OP_RX,
    OP_TX,
    OP_START,
    OP_PROMISCUOUS,
    OP_MULTICAST,
    OP_TOPOLOGY,
    OP_LINK
};
enum mutation {
    MUT_NONE,
    MUT_NAME,
    MUT_FIRMWARE,
    MUT_MAC,
    MUT_DRIVER,
    MUT_BDF,
    MUT_QUEUES,
    MUT_RSS,
    MUT_OFFLOAD,
    MUT_DESCRIPTOR,
    MUT_ADJUST,
    MUT_MAC_AFTER_START,
    MUT_FIRMWARE_UNKNOWN
};
struct callback_entry {
    rte_eth_dev_cb_fn function;
    void *context;
};

static struct callback_entry callbacks[2][3];
static struct dppd_device_set *active_devices;
static struct rte_mempool pool;
static struct rte_eth_dev_info infos[2];
static struct rte_ether_addr macs[2];
static uint32_t flags[2];
static const char *bus_names[2];
#if RTE_VERSION < RTE_VERSION_NUM(22, 11, 0, 0)
static struct rte_device fake_devices[2];
#else
static char fake_devices[2];
#endif
static unsigned int stops, resets, configures, rx_queues, tx_queues, starts, closes, frees, topology_reads;
static unsigned int link_queries;
static unsigned int pmd_calls;
static enum operation fail_operation, event_operation;
static enum mutation reset_mutation;
static enum rte_eth_event_type injected_event;
static uint16_t fail_port, event_port;
static uint16_t invalid_probe_port, removed_probe_port;
static bool initializing, cleanup_mode, reset_events, reset_busy, outside_event;
static char order[256];
static size_t order_length;

static unsigned int index_of(uint16_t port)
{
    assert(port == 5 || port == 11);
    return port == 5 ? 0 : 1;
}

static void record(char value)
{
    assert(order_length + 1U < sizeof(order));
    order[order_length++] = value;
    order[order_length] = '\0';
}

static unsigned int event_index(enum rte_eth_event_type event)
{
    assert(event == RTE_ETH_EVENT_INTR_RMV || event == RTE_ETH_EVENT_INTR_RESET || event == RTE_ETH_EVENT_DESTROY);
    return event == RTE_ETH_EVENT_INTR_RMV ? 0 : event == RTE_ETH_EVENT_INTR_RESET ? 1 : 2;
}

static void deliver(uint16_t port, enum rte_eth_event_type event)
{
    const struct callback_entry *entry = &callbacks[index_of(port)][event_index(event)];
    assert(entry->function != NULL);
    assert(entry->function(port, event, entry->context, NULL) == 0);
}

static int finish(enum operation operation, uint16_t port)
{
    ++pmd_calls;
    if (event_operation == operation && event_port == port) {
        event_operation = OP_NONE;
        deliver(port, injected_event);
    }
    return fail_operation == operation && fail_port == port ? -EIO : 0;
}

static void set_bus_name(unsigned int index, const char *name)
{
    bus_names[index] = name;
#if RTE_VERSION < RTE_VERSION_NUM(22, 11, 0, 0)
    fake_devices[index].name = name;
#endif
}

#if RTE_VERSION >= RTE_VERSION_NUM(22, 11, 0, 0)
const char *__wrap_rte_dev_name(const struct rte_device *device);
const char *__wrap_rte_dev_name(const struct rte_device *device)
{
    for (unsigned int i = 0; i < 2; ++i)
        if (device == (const struct rte_device *)&fake_devices[i])
            return bus_names[i];
    assert(false);
    return NULL;
}
#endif

int __wrap_rte_eth_dev_callback_register(uint16_t port, enum rte_eth_event_type event, rte_eth_dev_cb_fn function,
    void *context);
int __wrap_rte_eth_dev_callback_register(uint16_t port, enum rte_eth_event_type event, rte_eth_dev_cb_fn function,
    void *context)
{
    ++pmd_calls;
    callbacks[index_of(port)][event_index(event)] = (struct callback_entry){function, context};
    return 0;
}

int __wrap_rte_eth_dev_callback_unregister(uint16_t port, enum rte_eth_event_type event, rte_eth_dev_cb_fn function,
    void *context);
int __wrap_rte_eth_dev_callback_unregister(uint16_t port, enum rte_eth_event_type event, rte_eth_dev_cb_fn function,
    void *context)
{
    ++pmd_calls;
    struct callback_entry *entry = &callbacks[index_of(port)][event_index(event)];
    assert(entry->function == function && entry->context == context);
    *entry = (struct callback_entry){0};
    return 0;
}

int __wrap_rte_eth_dev_info_get(uint16_t port, struct rte_eth_dev_info *info);
int __wrap_rte_eth_dev_info_get(uint16_t port, struct rte_eth_dev_info *info)
{
    *info = infos[index_of(port)];
    return finish(OP_INFO, port);
}

int __wrap_rte_eth_dev_get_name_by_port(uint16_t port, char *name);
int __wrap_rte_eth_dev_get_name_by_port(uint16_t port, char *name)
{
    index_of(port);
    snprintf(name, RTE_ETH_NAME_MAX_LEN, "test-port%u", port);
    if (resets == 2 && reset_mutation == MUT_NAME && port == 11)
        strcpy(name, "new-port11");
    return finish(OP_NAME, port);
}

int __wrap_rte_eth_dev_fw_version_get(uint16_t port, char *version, size_t size);
int __wrap_rte_eth_dev_fw_version_get(uint16_t port, char *version, size_t size)
{
    index_of(port);
    if (resets == 2 && reset_mutation == MUT_FIRMWARE_UNKNOWN && port == 11)
        return -ENOTSUP;
    snprintf(version, size, "%s", resets == 2 && reset_mutation == MUT_FIRMWARE && port == 11 ? "fw-2" : "fw-1");
    return finish(OP_FIRMWARE, port);
}

int __wrap_rte_eth_macaddr_get(uint16_t port, struct rte_ether_addr *mac);
int __wrap_rte_eth_macaddr_get(uint16_t port, struct rte_ether_addr *mac)
{
    *mac = macs[index_of(port)];
    if (port == 11 && resets == 2 &&
        (reset_mutation == MUT_MAC || (reset_mutation == MUT_MAC_AFTER_START && starts == 2)))
        ++mac->addr_bytes[5];
    return finish(OP_MAC, port);
}

int __wrap_rte_eth_dev_is_valid_port(uint16_t port);
int __wrap_rte_eth_dev_is_valid_port(uint16_t port)
{
    ++pmd_calls;
    index_of(port);
    return port != invalid_probe_port;
}

int __wrap_rte_eth_dev_is_removed(uint16_t port);
int __wrap_rte_eth_dev_is_removed(uint16_t port)
{
    ++pmd_calls;
    index_of(port);
    return port == removed_probe_port;
}

int __wrap_rte_eth_dev_socket_id(uint16_t port);
int __wrap_rte_eth_dev_socket_id(uint16_t port)
{
    ++pmd_calls;
    index_of(port);
    return 0;
}

int __wrap_rte_eth_dev_stop(uint16_t port);
int __wrap_rte_eth_dev_stop(uint16_t port)
{
    index_of(port);
    ++stops;
    record('S');
    assert(initializing || cleanup_mode || (resets == 0 && configures == 0 && starts == 0));
    return finish(OP_STOP, port);
}

int __wrap_rte_eth_dev_reset(uint16_t port);
int __wrap_rte_eth_dev_reset(uint16_t port)
{
    index_of(port);
    assert(stops == 2 && configures == 0 && starts == 0 && dppd_devices_io_quiesced(active_devices));
    ++resets;
    record('R');
    if (reset_events)
        deliver(port, RTE_ETH_EVENT_INTR_RESET);
    if (reset_busy)
        atomic_store(&active_devices->event_callbacks_in_flight, 1);
    if (outside_event) {
        atomic_fetch_or(&active_devices->reset_event_ports, UINT32_C(1) << 8);
        atomic_fetch_add(&active_devices->event_sequence, 1);
    }
    if (resets == 2) {
        if (reset_mutation == MUT_DRIVER)
            infos[1].driver_name = "other_driver";
        if (reset_mutation == MUT_BDF)
            set_bus_name(1, "0000:04:00.1");
        if (reset_mutation == MUT_QUEUES)
            infos[1].max_rx_queues = 1;
        if (reset_mutation == MUT_RSS)
            infos[1].flow_type_rss_offloads = 0;
        if (reset_mutation == MUT_OFFLOAD)
            infos[1].tx_offload_capa = 0;
        if (reset_mutation == MUT_DESCRIPTOR)
            infos[1].rx_desc_lim.nb_max = 256;
        /** 未使用的能力可以变化，失败时仍不能把这份新观察提前发布 */
        infos[0].max_rx_queues = 8;
    }
    return finish(OP_RESET, port);
}

int __wrap_rte_eth_dev_configure(uint16_t port, uint16_t rx, uint16_t tx, const struct rte_eth_conf *conf);
int __wrap_rte_eth_dev_configure(uint16_t port, uint16_t rx, uint16_t tx, const struct rte_eth_conf *conf)
{
    index_of(port);
    assert(initializing || (stops == 2 && resets == 2 && starts == 0));
    assert(rx == 2 && tx == 2 && conf->rxmode.mq_mode == RTE_ETH_MQ_RX_RSS);
    assert(conf->rx_adv_conf.rss_conf.rss_hf == RTE_ETH_RSS_IP);
    assert(conf->txmode.offloads == RTE_ETH_TX_OFFLOAD_MULTI_SEGS && conf->intr_conf.rmv);
    ++configures;
    record('C');
    return finish(OP_CONFIGURE, port);
}

int __wrap_rte_eth_dev_adjust_nb_rx_tx_desc(uint16_t port, uint16_t *rx, uint16_t *tx);
int __wrap_rte_eth_dev_adjust_nb_rx_tx_desc(uint16_t port, uint16_t *rx, uint16_t *tx)
{
    index_of(port);
    assert(*rx == (initializing ? 1024 : 512) && *tx == (initializing ? 1024 : 512));
    if (resets == 2 && reset_mutation == MUT_ADJUST && port == 11)
        *rx = 256;
    return finish(OP_ADJUST, port);
}

int __wrap_rte_eth_rx_queue_setup(uint16_t port, uint16_t queue, uint16_t desc, unsigned int socket,
    const struct rte_eth_rxconf *conf, struct rte_mempool *value);
int __wrap_rte_eth_rx_queue_setup(uint16_t port, uint16_t queue, uint16_t desc, unsigned int socket,
    const struct rte_eth_rxconf *conf, struct rte_mempool *value)
{
    index_of(port);
    assert(queue < 2 && desc == (initializing ? 1024 : 512) && socket == 0 && value == &pool);
    assert(conf->offloads == 0 && starts == (initializing ? index_of(port) : 0));
    ++rx_queues;
    record('r');
    return finish(OP_RX, port);
}

int __wrap_rte_eth_tx_queue_setup(uint16_t port, uint16_t queue, uint16_t desc, unsigned int socket,
    const struct rte_eth_txconf *conf);
int __wrap_rte_eth_tx_queue_setup(uint16_t port, uint16_t queue, uint16_t desc, unsigned int socket,
    const struct rte_eth_txconf *conf)
{
    index_of(port);
    assert(queue < 2 && desc == (initializing ? 1024 : 512) && socket == 0);
    assert(conf->offloads == RTE_ETH_TX_OFFLOAD_MULTI_SEGS);
    ++tx_queues;
    record('t');
    return finish(OP_TX, port);
}

int __wrap_rte_eth_dev_start(uint16_t port);
int __wrap_rte_eth_dev_start(uint16_t port)
{
    index_of(port);
    assert(initializing || (configures == 2 && rx_queues == 4 && tx_queues == 4));
    ++starts;
    record('T');
    return finish(OP_START, port);
}

int __wrap_rte_eth_promiscuous_enable(uint16_t port);
int __wrap_rte_eth_promiscuous_enable(uint16_t port)
{
    record('p');
    return finish(OP_PROMISCUOUS, port);
}

int __wrap_rte_eth_allmulticast_enable(uint16_t port);
int __wrap_rte_eth_allmulticast_enable(uint16_t port)
{
    record('m');
    return finish(OP_MULTICAST, port);
}

int __wrap_rte_eth_dev_close(uint16_t port);
int __wrap_rte_eth_dev_close(uint16_t port)
{
    index_of(port);
    assert(cleanup_mode);
    ++closes;
    return 0;
}

struct rte_mempool *__wrap_rte_pktmbuf_pool_create(const char *name, unsigned int count, unsigned int cache,
    uint16_t private_size, uint16_t data_size, int socket);
struct rte_mempool *__wrap_rte_pktmbuf_pool_create(const char *name, unsigned int count, unsigned int cache,
    uint16_t private_size, uint16_t data_size, int socket)
{
    assert(initializing && name != NULL && count == 32 && cache == 0 && private_size == 0 && socket == 0);
    assert(data_size == RTE_MBUF_DEFAULT_BUF_SIZE);
    return &pool;
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
    assert(cleanup_mode && value == &pool && closes == 2);
    ++frees;
}

int __wrap_rte_eth_link_get_nowait(uint16_t port, struct rte_eth_link *link);
int __wrap_rte_eth_link_get_nowait(uint16_t port, struct rte_eth_link *link)
{
    index_of(port);
    ++link_queries;
    *link = (struct rte_eth_link){.link_status = RTE_ETH_LINK_UP};
    return finish(OP_LINK, port);
}

static void fill_topology(const struct dppd_config *cfg, struct dppd_topology *topology)
{
    memset(topology, 0, sizeof(*topology));
    topology->nb_endpoints = cfg->nb_ports;
    for (uint16_t i = 0; i < cfg->nb_ports; ++i) {
        unsigned int index = index_of(cfg->ports[i]);
        struct dppd_endpoint *endpoint = &topology->endpoints[i];
        endpoint->ethdev_port_id = cfg->ports[i];
        endpoint->kind = DPPD_ENDPOINT_ETHDEV;
        snprintf(endpoint->driver_name, sizeof(endpoint->driver_name), "%s", infos[index].driver_name);
        snprintf(endpoint->device_name, sizeof(endpoint->device_name), "%s", bus_names[index]);
    }
}

int __wrap_dppd_topology_discover(const struct dppd_config *cfg, struct dppd_topology *topology);
int __wrap_dppd_topology_discover(const struct dppd_config *cfg, struct dppd_topology *topology)
{
    assert(initializing);
    fill_topology(cfg, topology);
    return 0;
}

int __wrap_dppd_topology_rediscover_verified(const struct dppd_config *cfg, const struct dppd_topology *expected,
    struct dppd_topology *candidate);
int __wrap_dppd_topology_rediscover_verified(const struct dppd_config *cfg, const struct dppd_topology *expected,
    struct dppd_topology *candidate)
{
    ++topology_reads;
    int rc = finish(OP_TOPOLOGY, 11);
    if (rc != 0)
        return rc;
    fill_topology(cfg, candidate);
    return dppd_topology_validate_identity(expected, candidate, cfg->ports, cfg->nb_ports);
}

static void fixture(struct dppd_device_set *devices, struct dppd_config *cfg)
{
    memset(devices, 0, sizeof(*devices));
    memset(infos, 0, sizeof(infos));
    memset(callbacks, 0, sizeof(callbacks));
    memset(order, 0, sizeof(order));
    order_length = 0;
    stops = resets = configures = rx_queues = tx_queues = starts = closes = frees = topology_reads = 0;
    link_queries = 0;
    fail_operation = event_operation = OP_NONE;
    reset_mutation = MUT_NONE;
    fail_port = event_port = 11;
    invalid_probe_port = removed_probe_port = UINT16_MAX;
    injected_event = RTE_ETH_EVENT_INTR_RESET;
    initializing = cleanup_mode = reset_events = reset_busy = outside_event = false;
    active_devices = devices;
    pool.size = 32;
    *cfg = (struct dppd_config){.nb_ports = 2,
        .ports = {5, 11},
        .nb_queues = 2,
        .mbufs_per_socket = 32,
        .promiscuous = true,
        .nd6_count = 1,
        .nd6 = {{.port_id = 5}}};
    devices->nb_ports = 2;
    devices->pools[0] = &pool;
    atomic_init(&devices->removal_requested, false);
    atomic_init(&devices->event_tracking_enabled, false);
    for (unsigned int i = 0; i < 2; ++i) {
        set_bus_name(i, i == 0 ? "0000:03:00.0" : "0000:04:00.0");
        flags[i] = RTE_ETH_DEV_INTR_RMV;
        infos[i] = (struct rte_eth_dev_info){.device = (struct rte_device *)&fake_devices[i],
            .driver_name = "test_driver",
            .dev_flags = &flags[i],
            .max_rx_queues = 4,
            .max_tx_queues = 4,
            .flow_type_rss_offloads = RTE_ETH_RSS_IP,
            .tx_offload_capa = RTE_ETH_TX_OFFLOAD_MULTI_SEGS,
            .rx_desc_lim = {.nb_min = 64, .nb_max = 4096, .nb_align = 32},
            .tx_desc_lim = {.nb_min = 64, .nb_max = 4096, .nb_align = 32}};
        macs[i] = (struct rte_ether_addr){{2, 0, 0, 0, 0, (uint8_t)(i + 1U)}};
        struct dppd_port *port = &devices->ports[i];
        port->port_id = cfg->ports[i];
        port->socket_id = 0;
        port->configured = port->started = true;
        port->configured_removal_interrupt = true;
        port->configured_rss_hf = RTE_ETH_RSS_IP;
        port->configured_tx_offloads = RTE_ETH_TX_OFFLOAD_MULTI_SEGS;
        port->mac = macs[i];
        port->capabilities.max_rx_queues = port->capabilities.max_tx_queues = 4;
        atomic_init(&port->removed, false);
        atomic_init(&port->link_state, DPPD_LINK_UP);
        port->identity.device_name_known = port->identity.firmware_known = true;
        snprintf(port->identity.device_name, sizeof(port->identity.device_name), "test-port%u", port->port_id);
        strcpy(port->identity.firmware_version, "fw-1");
        snprintf(port->identity.dpdk_version, sizeof(port->identity.dpdk_version), "%s", rte_version());
        port->identity.configured_queues = 2;
        port->identity.configured_rx_desc = port->identity.configured_tx_desc = 512;
        port->identity.rx_desc_min = port->identity.tx_desc_min = 64;
        port->identity.rx_desc_max = port->identity.tx_desc_max = 4096;
        port->identity.rx_desc_align = port->identity.tx_desc_align = 32;
    }
    fill_topology(cfg, &devices->topology);
    assert(dppd_devices_enable_event_tracking(devices) == 0);
    pmd_calls = 0;
}

static void check_local_precheck(const struct dppd_device_set *devices, const struct dppd_config *cfg, int expected)
{
    unsigned char saved_devices[sizeof(*devices)], saved_config[sizeof(*cfg)];
    memcpy(saved_devices, devices, sizeof(saved_devices));
    memcpy(saved_config, cfg, sizeof(saved_config));
    unsigned int calls_before = pmd_calls;
    assert(dppd_devices_reset_identity_ready(devices, cfg) == expected);
    assert(pmd_calls == calls_before && stops == 0 && resets == 0 && topology_reads == 0);
    assert(memcmp(saved_devices, devices, sizeof(saved_devices)) == 0);
    assert(memcmp(saved_config, cfg, sizeof(saved_config)) == 0);
}

/** 在清理规则前只读检查完整本地证据，成功和各种拒绝都不能调用 PMD 或改变门禁 */
static void test_local_precheck(void)
{
    struct dppd_device_set devices;
    struct dppd_config cfg;
    fixture(&devices, &cfg);
    check_local_precheck(&devices, &cfg, 0);
    assert(!dppd_devices_io_quiesced(&devices));
    for (unsigned int i = 0; i < 3; ++i) {
        fixture(&devices, &cfg);
        if (i == 0)
            devices.ports[1].identity.firmware_known = false;
        else if (i == 1)
            devices.ports[1].identity.device_name_known = false;
        else
            devices.ports[1].identity.device_name[0] = '\0';
        check_local_precheck(&devices, &cfg, -ENODATA);
    }
    for (unsigned int i = 0; i < 3; ++i) {
        fixture(&devices, &cfg);
        struct dppd_capability_identity *identity = &devices.ports[1].identity;
        if (i == 0)
            memset(identity->device_name, 'x', sizeof(identity->device_name));
        else if (i == 1)
            memset(identity->firmware_version, 'x', sizeof(identity->firmware_version));
        else
            memset(identity->dpdk_version, 'x', sizeof(identity->dpdk_version));
        check_local_precheck(&devices, &cfg, -EINVAL);
    }
    fixture(&devices, &cfg);
    memset(&devices.ports[1].mac, 0, sizeof(devices.ports[1].mac));
    check_local_precheck(&devices, &cfg, -ENODATA);
    fixture(&devices, &cfg);
    devices.ports[1].mac.addr_bytes[0] = 1;
    check_local_precheck(&devices, &cfg, -ENODATA);
    fixture(&devices, &cfg);
    devices.ports[1].identity.configured_queues = 1;
    check_local_precheck(&devices, &cfg, -EXDEV);
    fixture(&devices, &cfg);
    devices.ports[1].identity.configured_rx_desc = 0;
    check_local_precheck(&devices, &cfg, -EXDEV);
    fixture(&devices, &cfg);
    devices.ports[1].capabilities.max_tx_queues = 1;
    check_local_precheck(&devices, &cfg, -ENOTSUP);
    fixture(&devices, &cfg);
    devices.ports[1].identity.rx_desc_max = 256;
    check_local_precheck(&devices, &cfg, -ENOTSUP);
    fixture(&devices, &cfg);
    devices.ports[1].identity.tx_desc_align = 3;
    check_local_precheck(&devices, &cfg, -ENOTSUP);
    fixture(&devices, &cfg);
    devices.ports[1].identity.tx_desc_max = 0;
    check_local_precheck(&devices, &cfg, -ENODATA);
    fixture(&devices, &cfg);
    devices.pools[0] = NULL;
    check_local_precheck(&devices, &cfg, -ENODATA);
    fixture(&devices, &cfg);
    devices.ports[1].started = false;
    check_local_precheck(&devices, &cfg, -EUCLEAN);
    fixture(&devices, &cfg);
    devices.ports[1].port_id = 5;
    check_local_precheck(&devices, &cfg, -EUCLEAN);
    fixture(&devices, &cfg);
    cfg.ports[1] = 5;
    check_local_precheck(&devices, &cfg, -EEXIST);
    fixture(&devices, &cfg);
    cfg.nb_ports = DPPD_MAX_PORTS + 1U;
    check_local_precheck(&devices, &cfg, -EINVAL);
    assert(dppd_devices_reset_identity_ready(NULL, &cfg) == -EINVAL);
    assert(dppd_devices_reset_identity_ready(&devices, NULL) == -EINVAL);
    assert(pmd_calls == 0);
}

static void test_success_and_init(void)
{
    struct dppd_device_set devices;
    struct dppd_config cfg;
    struct dppd_device_reset_result result;
    fixture(&devices, &cfg);
    struct dppd_topology old = devices.topology;
    reset_events = true;
    assert(dppd_devices_reset_reconfigure(&devices, &cfg, 0, &result) == 0);
    assert(strcmp(order, "SSRRCrtrtCrtrtTpmTp") == 0);
    assert(result.phase == DPPD_DEVICE_RESET_READY && result.last_error == 0 && result.port_id == UINT16_MAX);
    assert(result.stopped_ports == 3 && result.reset_ports == 3 && result.configured_ports == 3 &&
        result.started_ports == 3);
    assert(result.sequence_after == 2 && topology_reads == 2 && dppd_devices_io_quiesced(&devices));
    assert(memcmp(&old, &devices.topology, sizeof(old)) == 0 && devices.pools[0] == &pool && closes == 0 && frees == 0);
    assert(devices.ports[0].capabilities.max_rx_queues == 8);
    assert(devices.ports[0].identity.configured_rx_desc == 512);
    assert(dppd_devices_resume_after_event(&devices, 0) == -ESTALE);
    assert(dppd_devices_resume_after_event(&devices, result.sequence_after) == 0);

    /** 初次启动仍按每口配置后启动的原顺序，复用拆分后的同一组 helper */
    fixture(&devices, &cfg);
    memset(callbacks, 0, sizeof(callbacks));
    initializing = true;
    assert(dppd_devices_init(&devices, &cfg) == 0);
    assert(strcmp(order, "CrtrtTpmCrtrtTp") == 0 && stops == 0 && resets == 0);
    assert(devices.ports[0].identity.configured_rx_desc == 1024 && !dppd_devices_io_quiesced(&devices));
    cleanup_mode = true;
    assert(dppd_devices_stop(&devices) == 0 && closes == 2 && frees == 1);
}

static void test_failures(void)
{
    struct dppd_device_set devices;
    struct dppd_config cfg;
    struct dppd_device_reset_result result;
    fixture(&devices, &cfg);
    devices.ports[1].identity.firmware_known = false;
    assert(dppd_devices_reset_reconfigure(&devices, &cfg, 0, &result) == -ENODATA);
    assert(result.phase == DPPD_DEVICE_RESET_PRECHECK && stops == 0 && resets == 0 && topology_reads == 0);
    fixture(&devices, &cfg);
    memset(devices.ports[1].identity.device_name, 'x', sizeof(devices.ports[1].identity.device_name));
    assert(dppd_devices_reset_reconfigure(&devices, &cfg, 0, &result) == -EINVAL && stops == 0);
    fixture(&devices, &cfg);
    cfg.ports[1] = 5;
    assert(dppd_devices_reset_reconfigure(&devices, &cfg, 0, &result) == -EEXIST && stops == 0);
    fixture(&devices, &cfg);
    fail_operation = OP_STOP;
    fail_port = 5;
    assert(dppd_devices_reset_reconfigure(&devices, &cfg, 0, &result) == -EIO);
    assert(result.phase == DPPD_DEVICE_RESET_STOP && result.port_id == 5 && result.stopped_ports == 2);
    assert(stops == 2 && resets == 0 && devices.ports[0].started && !devices.ports[1].started);
    fixture(&devices, &cfg);
    fail_operation = OP_RESET;
    assert(dppd_devices_reset_reconfigure(&devices, &cfg, 0, &result) == -EIO);
    assert(result.phase == DPPD_DEVICE_RESET_RESET && result.reset_ports == 1 && resets == 2 && configures == 0);
    assert(!devices.ports[0].configured && devices.ports[1].configured && devices.pools[0] == &pool);

    const enum operation failures[] = {OP_INFO, OP_NAME, OP_FIRMWARE, OP_MAC, OP_CONFIGURE, OP_ADJUST, OP_RX, OP_TX,
        OP_START, OP_PROMISCUOUS, OP_MULTICAST, OP_TOPOLOGY};
    for (unsigned int i = 0; i < sizeof(failures) / sizeof(failures[0]); ++i) {
        fixture(&devices, &cfg);
        fail_operation = failures[i];
        fail_port = fail_operation == OP_MULTICAST ? 5 : 11;
        assert(dppd_devices_reset_reconfigure(&devices, &cfg, 0, &result) != 0);
        assert(dppd_devices_io_quiesced(&devices) && closes == 0 && frees == 0 && devices.pools[0] == &pool);
        assert(devices.ports[0].capabilities.max_rx_queues == 4);
        if (fail_operation == OP_RX || fail_operation == OP_TX || fail_operation == OP_CONFIGURE ||
            fail_operation == OP_ADJUST)
            assert(starts == 0);
    }
    const enum mutation changes[] = {MUT_NAME, MUT_FIRMWARE, MUT_MAC, MUT_DRIVER, MUT_BDF, MUT_QUEUES, MUT_RSS,
        MUT_OFFLOAD, MUT_DESCRIPTOR, MUT_ADJUST, MUT_MAC_AFTER_START, MUT_FIRMWARE_UNKNOWN};
    for (unsigned int i = 0; i < sizeof(changes) / sizeof(changes[0]); ++i) {
        fixture(&devices, &cfg);
        reset_mutation = changes[i];
        assert(dppd_devices_reset_reconfigure(&devices, &cfg, 0, &result) < 0);
        assert(dppd_devices_io_quiesced(&devices) && devices.ports[0].capabilities.max_rx_queues == 4);
        assert(closes == 0 && frees == 0);
        if (changes[i] != MUT_ADJUST && changes[i] != MUT_MAC_AFTER_START)
            assert(configures == 0 && starts == 0);
    }
}

static void test_events_and_terminal_identity(void)
{
    struct dppd_device_set devices;
    struct dppd_config cfg;
    struct dppd_device_reset_result result;
    const enum operation events[] = {OP_INFO, OP_NAME, OP_FIRMWARE, OP_MAC, OP_STOP, OP_CONFIGURE, OP_ADJUST, OP_RX,
        OP_TX, OP_START, OP_PROMISCUOUS, OP_MULTICAST, OP_TOPOLOGY};
    for (unsigned int i = 0; i < sizeof(events) / sizeof(events[0]); ++i) {
        fixture(&devices, &cfg);
        event_operation = events[i];
        event_port = event_operation == OP_MULTICAST ? 5 : 11;
        assert(dppd_devices_reset_reconfigure(&devices, &cfg, 0, &result) == -ESTALE);
        assert(dppd_devices_io_quiesced(&devices) && closes == 0 && frees == 0);
        if (events[i] == OP_STOP)
            assert(resets == 0);
    }
    fixture(&devices, &cfg);
    event_operation = OP_RESET;
    injected_event = RTE_ETH_EVENT_INTR_RMV;
    assert(dppd_devices_reset_reconfigure(&devices, &cfg, 0, &result) == -ENODEV && configures == 0);
    fixture(&devices, &cfg);
    reset_busy = true;
    assert(dppd_devices_reset_reconfigure(&devices, &cfg, 0, &result) == -EBUSY && resets == 1 && configures == 0);
    fixture(&devices, &cfg);
    outside_event = true;
    assert(dppd_devices_reset_reconfigure(&devices, &cfg, 0, &result) == -EUCLEAN && resets == 1 && configures == 0);
    fixture(&devices, &cfg);
    atomic_store(&devices.event_sequence_overflow, true);
    assert(dppd_devices_reset_reconfigure(&devices, &cfg, 0, &result) == -EOVERFLOW && stops == 0);
    fixture(&devices, &cfg);
    assert(dppd_devices_pause_io(&devices, 0) == 0 && dppd_devices_io_quiesced(&devices));
    assert(dppd_devices_resume_after_event(&devices, 0) == 0 && !dppd_devices_io_quiesced(&devices));

    /** 主动暂停不阻止稳定链路查询，一口查询中收到 RESET 后不能再查询下一口 */
    fixture(&devices, &cfg);
    assert(dppd_devices_pause_io(&devices, 0) == 0);
    assert(dppd_devices_poll_links(&devices) == 0 && link_queries == 2 && dppd_devices_io_quiesced(&devices));
    fixture(&devices, &cfg);
    event_operation = OP_LINK;
    event_port = 5;
    assert(dppd_devices_poll_links(&devices) == -ESTALE && link_queries == 1);
    assert(dppd_devices_io_quiesced(&devices));

    /** 主动探测到编号失效或 PMD 已移除时，也要发布完整 RMV 证据供控制面隔离 */
    for (unsigned int i = 0; i < 2; ++i) {
        fixture(&devices, &cfg);
        if (i == 0)
            invalid_probe_port = 11;
        else
            removed_probe_port = 11;
        assert(dppd_devices_poll_links(&devices) == -ENODEV);
        struct dppd_device_event_snapshot snapshot;
        dppd_devices_event_snapshot(&devices, &snapshot);
        assert(snapshot.sequence == 1 && snapshot.removal_ports == 2 && snapshot.reset_ports == 0);
        assert(snapshot.destroy_ports == 0 && snapshot.callbacks_in_flight == 0 && snapshot.quiesced);
        assert(dppd_devices_removal_requested(&devices) && atomic_load(&devices.ports[1].removed));
        assert(dppd_devices_poll_links(&devices) == -ENODEV);
        dppd_devices_event_snapshot(&devices, &snapshot);
        assert(snapshot.sequence == 1 && snapshot.removal_ports == 2);
    }

    /** DESTROY 后即使编号复用为同样名称，也不能向新实例发送控制操作 */
    fixture(&devices, &cfg);
    deliver(11, RTE_ETH_EVENT_DESTROY);
    cleanup_mode = true;
    assert(dppd_devices_stop(&devices) == -ENODEV && stops == 1 && closes == 1 && frees == 0);
    assert(devices.ports[1].configured && devices.pools[0] == &pool);
    /** 没有观察到 DESTROY 时，异 BDF、异驱动和缺失的原身份也不能凭相同 port_id 认领 */
    for (unsigned int i = 0; i < 3; ++i) {
        fixture(&devices, &cfg);
        if (i == 0)
            set_bus_name(1, "0000:04:00.1");
        else if (i == 1)
            infos[1].driver_name = "other_driver";
        else
            devices.ports[1].identity.device_name_known = false;
        cleanup_mode = true;
        assert(dppd_devices_stop(&devices) == (i == 2 ? -ENODATA : -EXDEV));
        assert(stops == 1 && closes == 1 && frees == 0 && devices.pools[0] == &pool);
    }
    assert(dppd_devices_pause_io(NULL, 0) == -EINVAL);
    assert(dppd_devices_reset_reconfigure(NULL, &cfg, 0, &result) == -EINVAL);
    assert(dppd_devices_reset_reconfigure(&devices, NULL, 0, &result) == -EINVAL);
    assert(dppd_devices_reset_reconfigure(&devices, &cfg, 0, NULL) == -EINVAL);
}

int main(void)
{
    test_local_precheck();
    test_success_and_init();
    test_failures();
    test_events_and_terminal_identity();
    return 0;
}
