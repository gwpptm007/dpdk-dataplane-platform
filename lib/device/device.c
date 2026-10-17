#include "dppd/device.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <rte_dev.h>
#include <rte_errno.h>
#include <rte_ethdev.h>
#include <rte_mbuf.h>
#include <rte_mempool.h>
#include <rte_version.h>

#define DPPD_DEFAULT_RX_DESC 1024U
#define DPPD_DEFAULT_TX_DESC 1024U

/** 设备名称查询要求缓冲区至少达到 DPDK 规定的长度，编译时检查未来版本的变化 */
_Static_assert(DPPD_CAPABILITY_NAME_SIZE >= RTE_ETH_NAME_MAX_LEN, "capability device name buffer is too small");
_Static_assert(DPPD_MAX_PORTS <= 32U, "device event bitmap is too small");

static int reset_event_fence(const struct dppd_device_set *devices, uint64_t expected, bool allow_reset,
    uint64_t *observed);

/**
 * 保存本次启动环境的设备身份与描述符上限，固件查询不支持时保留未知状态
 * 它只补充观测信息，不改变端口能力选择，不根据厂商或固件字符串猜测规则支持情况
 */
static int capture_identity(uint16_t port_id, const struct rte_eth_dev_info *info,
    struct dppd_capability_identity *identity, const struct dppd_device_set *devices, uint64_t sequence)
{
    int rc;

    memset(identity, 0, sizeof(*identity));
    identity->device_name_known = rte_eth_dev_get_name_by_port(port_id, identity->device_name) == 0;
    if (!identity->device_name_known)
        identity->device_name[0] = '\0';
    if (devices != NULL && (rc = reset_event_fence(devices, sequence, false, NULL)) != 0)
        return rc;
    snprintf(identity->dpdk_version, sizeof(identity->dpdk_version), "%s", rte_version());
    rc = rte_eth_dev_fw_version_get(port_id, identity->firmware_version, sizeof(identity->firmware_version));
    identity->firmware_known = rc == 0 && identity->firmware_version[0] != '\0';
    identity->firmware_error = rc > 0 ? -ENOSPC : rc;
    if (!identity->firmware_known)
        identity->firmware_version[0] = '\0';
    identity->rx_desc_min = info->rx_desc_lim.nb_min;
    identity->rx_desc_max = info->rx_desc_lim.nb_max;
    identity->rx_desc_align = info->rx_desc_lim.nb_align;
    identity->tx_desc_min = info->tx_desc_lim.nb_min;
    identity->tx_desc_max = info->tx_desc_lim.nb_max;
    identity->tx_desc_align = info->tx_desc_lim.nb_align;
    return devices == NULL ? 0 : reset_event_fence(devices, sequence, false, NULL);
}

/**
 * 先封住被移除端口，再发布整个设备组的退出请求，工作线程只需读取原子状态
 * 移除标记不会在当前进程中复位，它与可以恢复的普通链路 down 状态具有不同含义
 */
static void request_removal(struct dppd_device_set *devices, struct dppd_port *port)
{
    atomic_store_explicit(&port->removed, true, memory_order_release);
    atomic_store_explicit(&port->link_state, DPPD_LINK_DOWN, memory_order_release);
    atomic_store_explicit(&devices->removal_requested, true, memory_order_release);
}

/**
 * DPDK 可能在后台线程调用此函数，因此这里只发布原子事件与收发门禁
 * 不在回调中停止设备、等待工作线程或释放内存，实际清理由主线程按顺序完成
 * 端口编号和数量在注册回调前初始化，并保持到全部回调成功注销之后
 */
static int handle_removal(uint16_t port_id, enum rte_eth_event_type event, void *context, void *ret_param)
{
    struct dppd_device_set *devices = context;

    (void)ret_param;
    if (devices == NULL ||
        (event != RTE_ETH_EVENT_INTR_RMV && event != RTE_ETH_EVENT_INTR_RESET && event != RTE_ETH_EVENT_DESTROY))
        return 0;
    for (uint16_t i = 0; i < devices->nb_ports; ++i) {
        if (devices->ports[i].port_id == port_id) {
            bool tracking = atomic_load_explicit(&devices->event_tracking_enabled, memory_order_acquire);
            if (!tracking) {
                if (event == RTE_ETH_EVENT_INTR_RMV)
                    request_removal(devices, &devices->ports[i]);
                break;
            }
            /** 先封住所有新收发，在途计数防止恢复线程误把尚未发布完的事件当作已处理 */
            atomic_fetch_add_explicit(&devices->event_callbacks_in_flight, 1, memory_order_acq_rel);
            atomic_store_explicit(&devices->event_io_quiesced, true, memory_order_release);
            unsigned int bit = UINT32_C(1) << i;
            atomic_uint *ports = event == RTE_ETH_EVENT_INTR_RESET ? &devices->reset_event_ports
                : event == RTE_ETH_EVENT_INTR_RMV                  ? &devices->removal_event_ports
                                                                   : &devices->destroy_event_ports;
            atomic_fetch_or_explicit(ports, bit, memory_order_release);
            if (event != RTE_ETH_EVENT_INTR_RESET)
                request_removal(devices, &devices->ports[i]);
            uint_fast64_t previous = atomic_load_explicit(&devices->event_sequence, memory_order_relaxed);
            while (previous != UINT64_MAX &&
                !atomic_compare_exchange_weak_explicit(&devices->event_sequence, &previous, previous + 1U,
                    memory_order_release, memory_order_relaxed))
                ;
            if (previous == UINT64_MAX)
                atomic_store_explicit(&devices->event_sequence_overflow, true, memory_order_release);
            atomic_fetch_sub_explicit(&devices->event_callbacks_in_flight, 1, memory_order_release);
            break;
        }
    }
    return 0;
}

int dppd_devices_enable_event_tracking(struct dppd_device_set *devices)
{
    if (devices == NULL || devices->nb_ports == 0 || devices->nb_ports > DPPD_MAX_PORTS)
        return -EINVAL;
    if (dppd_devices_removal_requested(devices))
        return -ENODEV;
    for (uint16_t i = 0; i < devices->nb_ports; ++i)
        for (uint16_t j = 0; j < i; ++j)
            if (devices->ports[i].port_id == devices->ports[j].port_id)
                return -EEXIST;
    if (!atomic_load_explicit(&devices->event_tracking_enabled, memory_order_acquire)) {
        atomic_init(&devices->event_io_quiesced, false);
        atomic_init(&devices->event_sequence_overflow, false);
        atomic_init(&devices->event_callbacks_in_flight, 0);
        atomic_init(&devices->event_sequence, 0);
        atomic_init(&devices->resumed_event_sequence, 0);
        atomic_init(&devices->reset_event_ports, 0);
        atomic_init(&devices->removal_event_ports, 0);
        atomic_init(&devices->destroy_event_ports, 0);
        /** 已有 RMV 回调也可能并发运行，因此所有新原子字段先初始化再开放记录 */
        atomic_store_explicit(&devices->event_tracking_enabled, true, memory_order_release);
    }
    static const enum rte_eth_event_type events[] = {RTE_ETH_EVENT_INTR_RMV, RTE_ETH_EVENT_INTR_RESET,
        RTE_ETH_EVENT_DESTROY};
    for (uint16_t i = 0; i < devices->nb_ports; ++i) {
        struct dppd_port *port = &devices->ports[i];
        bool *registered[] = {&port->removal_callback_registered, &port->reset_callback_registered,
            &port->destroy_callback_registered};
        for (unsigned int j = 0; j < sizeof(events) / sizeof(events[0]); ++j) {
            if (*registered[j])
                continue;
            int rc = rte_eth_dev_callback_register(port->port_id, events[j], handle_removal, devices);
            if (rc != 0) {
                atomic_store_explicit(&devices->event_io_quiesced, true, memory_order_release);
                return rc;
            }
            *registered[j] = true;
        }
    }
    return dppd_devices_removal_requested(devices) ? -ENODEV : 0;
}

void dppd_devices_event_snapshot(const struct dppd_device_set *devices, struct dppd_device_event_snapshot *snapshot)
{
    if (snapshot == NULL)
        return;
    memset(snapshot, 0, sizeof(*snapshot));
    if (devices == NULL)
        return;
    snapshot->enabled = atomic_load_explicit(&devices->event_tracking_enabled, memory_order_acquire);
    snapshot->quiesced = dppd_devices_io_quiesced(devices);
    if (!snapshot->enabled)
        return;
    /** 在途事件只作暂停依据，调用方应等 callbacks_in_flight 归零后重新读取完整证据 */
    snapshot->callbacks_in_flight = atomic_load_explicit(&devices->event_callbacks_in_flight, memory_order_acquire);
    uint64_t before = atomic_load_explicit(&devices->event_sequence, memory_order_acquire);
    snapshot->reset_ports = atomic_load_explicit(&devices->reset_event_ports, memory_order_acquire);
    snapshot->removal_ports = atomic_load_explicit(&devices->removal_event_ports, memory_order_acquire);
    snapshot->destroy_ports = atomic_load_explicit(&devices->destroy_event_ports, memory_order_acquire);
    snapshot->sequence = atomic_load_explicit(&devices->event_sequence, memory_order_acquire);
    snapshot->overflowed = atomic_load_explicit(&devices->event_sequence_overflow, memory_order_acquire);
    unsigned int active = atomic_load_explicit(&devices->event_callbacks_in_flight, memory_order_acquire);
    if (active != 0 || before != snapshot->sequence)
        snapshot->callbacks_in_flight = active != 0 ? active : 1U;
    snapshot->quiesced |= dppd_devices_io_quiesced(devices);
}

int dppd_devices_resume_after_event(struct dppd_device_set *devices, uint64_t expected_sequence)
{
    if (devices == NULL || !atomic_load_explicit(&devices->event_tracking_enabled, memory_order_acquire))
        return -EINVAL;
    if (devices->nb_ports == 0 || devices->nb_ports > DPPD_MAX_PORTS)
        return -EINVAL;
    if (dppd_devices_removal_requested(devices) ||
        atomic_load_explicit(&devices->removal_event_ports, memory_order_acquire) != 0 ||
        atomic_load_explicit(&devices->destroy_event_ports, memory_order_acquire) != 0)
        return -ENODEV;
    for (uint16_t i = 0; i < devices->nb_ports; ++i)
        if (!devices->ports[i].removal_callback_registered || !devices->ports[i].reset_callback_registered ||
            !devices->ports[i].destroy_callback_registered)
            return -EUCLEAN;
    if (atomic_load_explicit(&devices->event_sequence_overflow, memory_order_acquire))
        return -EOVERFLOW;
    if (atomic_load_explicit(&devices->event_callbacks_in_flight, memory_order_acquire) != 0)
        return -EBUSY;
    if (atomic_load_explicit(&devices->event_sequence, memory_order_acquire) != expected_sequence)
        return -ESTALE;
    atomic_store_explicit(&devices->resumed_event_sequence, expected_sequence, memory_order_release);
    atomic_store_explicit(&devices->event_io_quiesced, false, memory_order_release);
    /** 回调可在任意两次原子操作间到达，解除后再次核对，序号差本身也会继续封住收发 */
    if (atomic_load_explicit(&devices->event_callbacks_in_flight, memory_order_acquire) != 0 ||
        atomic_load_explicit(&devices->event_sequence, memory_order_acquire) != expected_sequence ||
        dppd_devices_removal_requested(devices) ||
        atomic_load_explicit(&devices->event_sequence_overflow, memory_order_acquire)) {
        atomic_store_explicit(&devices->event_io_quiesced, true, memory_order_release);
        return atomic_load_explicit(&devices->event_sequence_overflow, memory_order_acquire) ? -EOVERFLOW : -EAGAIN;
    }
    return 0;
}

/** 稳定序号只能证明本进程尚未收到下一条事件，不能替代硬件身份和旧对象清理证据 */
static int reset_event_fence(const struct dppd_device_set *devices, uint64_t expected, bool allow_reset,
    uint64_t *observed)
{
    struct dppd_device_event_snapshot snapshot;

    dppd_devices_event_snapshot(devices, &snapshot);
    if (observed != NULL)
        *observed = snapshot.sequence;
    if (!snapshot.enabled || devices->nb_ports == 0 || devices->nb_ports > DPPD_MAX_PORTS)
        return -EINVAL;
    uint32_t selected = UINT32_MAX >> (32U - devices->nb_ports);
    if ((snapshot.reset_ports | snapshot.removal_ports | snapshot.destroy_ports) & ~selected)
        return -EUCLEAN;
    if (snapshot.overflowed)
        return -EOVERFLOW;
    if (dppd_devices_removal_requested(devices) || snapshot.removal_ports != 0 || snapshot.destroy_ports != 0)
        return -ENODEV;
    for (uint16_t i = 0; i < devices->nb_ports; ++i) {
        const struct dppd_port *port = &devices->ports[i];
        if (atomic_load_explicit(&port->removed, memory_order_acquire))
            return -ENODEV;
        if (!port->removal_callback_registered || !port->reset_callback_registered ||
            !port->destroy_callback_registered)
            return -EUCLEAN;
    }
    if (snapshot.callbacks_in_flight != 0)
        return -EBUSY;
    if (snapshot.sequence != expected && (!allow_reset || snapshot.sequence < expected || snapshot.reset_ports == 0))
        return -ESTALE;
    return 0;
}

int dppd_devices_pause_io(struct dppd_device_set *devices, uint64_t expected_sequence)
{
    if (devices == NULL || !atomic_load_explicit(&devices->event_tracking_enabled, memory_order_acquire))
        return -EINVAL;
    atomic_store_explicit(&devices->event_io_quiesced, true, memory_order_release);
    return reset_event_fence(devices, expected_sequence, false, NULL);
}

static int port_socket(uint16_t port_id)
{
    int socket_id = rte_eth_dev_socket_id(port_id);

    if (socket_id < 0 || socket_id >= RTE_MAX_NUMA_NODES)
        socket_id = 0;
    return socket_id;
}

static int create_socket_pools(struct dppd_device_set *devices, const struct dppd_config *cfg)
{
    uint16_t i;

    if (cfg->mbuf_cache > RTE_MEMPOOL_CACHE_MAX_SIZE) {
        fprintf(stderr, "[dppd] mbuf cache %u exceeds DPDK maximum %u\n", cfg->mbuf_cache, RTE_MEMPOOL_CACHE_MAX_SIZE);
        return -EINVAL;
    }

    for (i = 0; i < cfg->nb_ports; ++i) {
        const int socket_id = port_socket(cfg->ports[i]);
        char name[64];

        if (devices->pools[socket_id] != NULL)
            continue;
        snprintf(name, sizeof(name), "dppd_mbuf_s%d", socket_id);
        devices->pools[socket_id] = rte_pktmbuf_pool_create(name, cfg->mbufs_per_socket, cfg->mbuf_cache, 0,
            RTE_MBUF_DEFAULT_BUF_SIZE, socket_id);
        if (devices->pools[socket_id] == NULL) {
            fprintf(stderr, "[dppd] cannot create mbuf pool on socket %d: %s\n", socket_id, rte_strerror(rte_errno));
            return -rte_errno;
        }
    }
    return 0;
}

/** 初次启动和复位使用同一份配置准备与队列逻辑，复位只允许重建原来已经使用的配置 */
struct port_configuration {
    struct rte_eth_dev_info info;
    struct rte_eth_conf conf;
    struct dppd_port_capabilities capabilities;
    struct dppd_capability_identity identity;
    struct rte_ether_addr mac;
    uint64_t rss_hf, tx_offloads;
    uint16_t rx_desc, tx_desc;
    bool preserve;
};

static bool descriptor_supported(uint16_t value, const struct rte_eth_desc_lim *limit)
{
    return value != 0 && limit->nb_max != 0 && limit->nb_align != 0 && value >= limit->nb_min &&
        value <= limit->nb_max && value % limit->nb_align == 0;
}

/** 两份未知固件不能互相作证，缺少启动时身份时在第一次 stop 之前就拒绝复位 */
static int saved_reset_identity(const struct dppd_port *port, const struct dppd_config *cfg)
{
    const struct dppd_capability_identity *identity = &port->identity;

    if (memchr(identity->device_name, '\0', sizeof(identity->device_name)) == NULL ||
        memchr(identity->firmware_version, '\0', sizeof(identity->firmware_version)) == NULL ||
        memchr(identity->dpdk_version, '\0', sizeof(identity->dpdk_version)) == NULL)
        return -EINVAL;
    if (!identity->device_name_known || !identity->firmware_known || identity->device_name[0] == '\0' ||
        identity->firmware_version[0] == '\0' || identity->dpdk_version[0] == '\0' ||
        !rte_is_valid_assigned_ether_addr(&port->mac))
        return -ENODATA;
    if (identity->configured_queues != cfg->nb_queues || identity->configured_rx_desc == 0 ||
        identity->configured_tx_desc == 0)
        return -EXDEV;
    if (port->capabilities.max_rx_queues == 0 || port->capabilities.max_tx_queues == 0 || identity->rx_desc_max == 0 ||
        identity->rx_desc_align == 0 || identity->tx_desc_max == 0 || identity->tx_desc_align == 0)
        return -ENODATA;
    struct rte_eth_desc_lim rx = {.nb_min = identity->rx_desc_min,
        .nb_max = identity->rx_desc_max,
        .nb_align = identity->rx_desc_align};
    struct rte_eth_desc_lim tx = {.nb_min = identity->tx_desc_min,
        .nb_max = identity->tx_desc_max,
        .nb_align = identity->tx_desc_align};
    if (cfg->nb_queues > port->capabilities.max_rx_queues || cfg->nb_queues > port->capabilities.max_tx_queues ||
        !descriptor_supported(identity->configured_rx_desc, &rx) ||
        !descriptor_supported(identity->configured_tx_desc, &tx))
        return -ENOTSUP;
    return 0;
}

static int reset_identity_ready(const struct dppd_device_set *devices, const struct dppd_config *cfg,
    uint16_t *failed_port)
{
    if (devices == NULL || cfg == NULL || cfg->nb_ports == 0 || cfg->nb_ports > DPPD_MAX_PORTS || cfg->nb_queues == 0 ||
        cfg->nd6_count > DPPD_ND6_LOCAL_LIMIT || cfg->nb_ports != devices->nb_ports)
        return -EINVAL;
    int rc = dppd_topology_validate_identity(&devices->topology, &devices->topology, cfg->ports, cfg->nb_ports);
    if (rc != 0)
        return rc;
    for (uint16_t i = 0; i < devices->nb_ports; ++i) {
        const struct dppd_port *port = &devices->ports[i];
        if (failed_port != NULL)
            *failed_port = port->port_id;
        if (dppd_topology_find(&devices->topology, port->port_id) == NULL)
            return -ENOENT;
        for (uint16_t j = 0; j < i; ++j)
            if (devices->ports[j].port_id == port->port_id)
                return -EUCLEAN;
        if (!port->configured || !port->started)
            return -EUCLEAN;
        if (port->socket_id < 0 || port->socket_id >= RTE_MAX_NUMA_NODES || devices->pools[port->socket_id] == NULL)
            return -ENODATA;
        rc = saved_reset_identity(port, cfg);
        if (rc != 0)
            return rc;
    }
    return 0;
}

int dppd_devices_reset_identity_ready(const struct dppd_device_set *devices, const struct dppd_config *cfg)
{
    return reset_identity_ready(devices, cfg, NULL);
}

static int prepare_port_configuration(const struct dppd_port *port, const struct dppd_config *cfg,
    struct port_configuration *plan, const struct dppd_device_set *devices, uint64_t sequence)
{
    memset(plan, 0, sizeof(*plan));
    plan->preserve = devices != NULL;
    int rc = rte_eth_dev_info_get(port->port_id, &plan->info);
    if (rc != 0)
        return rc;
    if (devices != NULL && (rc = reset_event_fence(devices, sequence, false, NULL)) != 0)
        return rc;
    rc = capture_identity(port->port_id, &plan->info, &plan->identity, devices, sequence);
    if (rc != 0)
        return rc;
    const struct rte_eth_dev_info *info = &plan->info;
    plan->capabilities = (struct dppd_port_capabilities){.max_rx_queues = info->max_rx_queues,
        .max_tx_queues = info->max_tx_queues,
        .reta_size = info->reta_size,
        .rss_offloads = info->flow_type_rss_offloads,
        .rx_offloads = info->rx_offload_capa,
        .tx_offloads = info->tx_offload_capa,
        .device_capabilities = info->dev_capa};
    bool removal_interrupt = info->dev_flags != NULL && (*info->dev_flags & RTE_ETH_DEV_INTR_RMV) != 0;
    plan->conf.intr_conf.rmv = removal_interrupt;
    if (cfg->nb_queues > info->max_rx_queues || cfg->nb_queues > info->max_tx_queues)
        return -ENOTSUP;
    uint64_t requested_rss = RTE_ETH_RSS_IP | RTE_ETH_RSS_UDP | RTE_ETH_RSS_TCP;
    plan->rss_hf = cfg->nb_queues > 1U ? requested_rss & info->flow_type_rss_offloads : 0;
    plan->tx_offloads = info->tx_offload_capa & RTE_ETH_TX_OFFLOAD_MULTI_SEGS;
    plan->rx_desc = DPPD_DEFAULT_RX_DESC;
    plan->tx_desc = DPPD_DEFAULT_TX_DESC;
    if (devices != NULL) {
        const struct dppd_capability_identity *old = &port->identity, *current = &plan->identity;
        if (memchr(current->device_name, '\0', sizeof(current->device_name)) == NULL ||
            memchr(current->firmware_version, '\0', sizeof(current->firmware_version)) == NULL)
            return -EINVAL;
        if (!current->device_name_known || !current->firmware_known || current->device_name[0] == '\0' ||
            current->firmware_version[0] == '\0')
            return -ENODATA;
        if (current->device_name_known != old->device_name_known || current->firmware_known != old->firmware_known ||
            current->firmware_error != old->firmware_error || strcmp(current->device_name, old->device_name) != 0 ||
            strcmp(current->firmware_version, old->firmware_version) != 0 ||
            strcmp(current->dpdk_version, old->dpdk_version) != 0)
            return -EXDEV;
        if ((port->configured_rss_hf & ~info->flow_type_rss_offloads) != 0 ||
            (port->configured_tx_offloads & ~info->tx_offload_capa) != 0 ||
            (port->configured_removal_interrupt && !removal_interrupt) ||
            !descriptor_supported(old->configured_rx_desc, &info->rx_desc_lim) ||
            !descriptor_supported(old->configured_tx_desc, &info->tx_desc_lim))
            return -ENOTSUP;
        plan->rss_hf = port->configured_rss_hf;
        plan->tx_offloads = port->configured_tx_offloads;
        plan->conf.intr_conf.rmv = port->configured_removal_interrupt;
        plan->rx_desc = old->configured_rx_desc;
        plan->tx_desc = old->configured_tx_desc;
        rc = rte_eth_macaddr_get(port->port_id, &plan->mac);
        if (rc != 0)
            return rc;
        rc = reset_event_fence(devices, sequence, false, NULL);
        if (rc != 0)
            return rc;
        if (!rte_is_same_ether_addr(&port->mac, &plan->mac))
            return -EXDEV;
    }
    if (cfg->nb_queues > 1U && plan->rss_hf == 0)
        return -ENOTSUP;
    plan->conf.rxmode.mq_mode = cfg->nb_queues > 1U ? RTE_ETH_MQ_RX_RSS : RTE_ETH_MQ_RX_NONE;
    plan->conf.rx_adv_conf.rss_conf.rss_hf = plan->rss_hf;
    plan->conf.txmode.offloads = plan->tx_offloads;
    return 0;
}

static void publish_port_configuration(struct dppd_port *port, const struct port_configuration *plan)
{
    port->capabilities = plan->capabilities;
    port->identity = plan->identity;
    port->configured_rss_hf = plan->rss_hf;
    port->configured_tx_offloads = plan->tx_offloads;
    port->configured_removal_interrupt = plan->conf.intr_conf.rmv;
    port->mac = plan->mac;
}

static int configure_port_queues(struct dppd_port *port, const struct dppd_config *cfg, struct rte_mempool *pool,
    struct port_configuration *plan, const struct dppd_device_set *devices, uint64_t sequence)
{
    struct rte_eth_rxconf rx_conf;
    struct rte_eth_txconf tx_conf;
    int rc;

    rc = rte_eth_dev_configure(port->port_id, cfg->nb_queues, cfg->nb_queues, &plan->conf);
    if (rc != 0)
        return rc;
    port->configured = true;
    if (devices != NULL && (rc = reset_event_fence(devices, sequence, false, NULL)) != 0)
        return rc;
    uint16_t rx_desc = plan->rx_desc, tx_desc = plan->tx_desc;
    rc = rte_eth_dev_adjust_nb_rx_tx_desc(port->port_id, &rx_desc, &tx_desc);
    if (rc != 0)
        return rc;
    if (devices != NULL && (rc = reset_event_fence(devices, sequence, false, NULL)) != 0)
        return rc;
    if (plan->preserve && (rx_desc != plan->rx_desc || tx_desc != plan->tx_desc))
        return -EXDEV;
    plan->identity.configured_queues = cfg->nb_queues;
    plan->identity.configured_rx_desc = rx_desc;
    plan->identity.configured_tx_desc = tx_desc;
    rx_conf = plan->info.default_rxconf;
    rx_conf.offloads = plan->conf.rxmode.offloads;
    tx_conf = plan->info.default_txconf;
    tx_conf.offloads = plan->conf.txmode.offloads;
    for (uint16_t queue = 0; queue < cfg->nb_queues; ++queue) {
        rc = rte_eth_rx_queue_setup(port->port_id, queue, rx_desc, port->socket_id, &rx_conf, pool);
        if (rc != 0)
            return rc;
        if (devices != NULL && (rc = reset_event_fence(devices, sequence, false, NULL)) != 0)
            return rc;
        rc = rte_eth_tx_queue_setup(port->port_id, queue, tx_desc, port->socket_id, &tx_conf);
        if (rc != 0)
            return rc;
        if (devices != NULL && (rc = reset_event_fence(devices, sequence, false, NULL)) != 0)
            return rc;
    }
    return 0;
}

static int start_configured_port(struct dppd_port *port, const struct dppd_config *cfg, struct port_configuration *plan,
    const struct dppd_device_set *devices, uint64_t sequence)
{
    int rc = rte_eth_dev_start(port->port_id);
    if (rc != 0)
        return rc;
    port->started = true;
    if (devices != NULL && (rc = reset_event_fence(devices, sequence, false, NULL)) != 0)
        return rc;

    if (cfg->promiscuous) {
        rc = rte_eth_promiscuous_enable(port->port_id);
        if (rc != 0)
            return rc;
        if (devices != NULL && (rc = reset_event_fence(devices, sequence, false, NULL)) != 0)
            return rc;
    }
    /** ND 需要接收本链路的 solicited-node 和 all-nodes 组播，失败不能冒充已启用 */
    for (uint32_t i = 0; i < cfg->nd6_count; ++i)
        if (cfg->nd6[i].port_id == port->port_id) {
            rc = rte_eth_allmulticast_enable(port->port_id);
            if (rc != 0)
                return rc;
            if (devices != NULL && (rc = reset_event_fence(devices, sequence, false, NULL)) != 0)
                return rc;
            break;
        }
    rc = rte_eth_macaddr_get(port->port_id, &plan->mac);
    if (rc != 0)
        return rc;
    if (devices != NULL && (rc = reset_event_fence(devices, sequence, false, NULL)) != 0)
        return rc;
    if (plan->preserve && !rte_is_same_ether_addr(&port->mac, &plan->mac))
        return -EXDEV;
    return 0;
}

static void print_port_configuration(const struct dppd_port *port, const struct dppd_config *cfg)
{
    printf("[dppd] port=%u peer=%u socket=%d queues=%u rss=0x%" PRIx64 " tx-offloads=0x%" PRIx64
           " mac=%02x:%02x:%02x:%02x:%02x:%02x\n",
        port->port_id, port->peer_port_id, port->socket_id, cfg->nb_queues, port->configured_rss_hf,
        port->configured_tx_offloads, port->mac.addr_bytes[0], port->mac.addr_bytes[1], port->mac.addr_bytes[2],
        port->mac.addr_bytes[3], port->mac.addr_bytes[4], port->mac.addr_bytes[5]);
}

static int configure_port(struct dppd_port *port, const struct dppd_config *cfg, struct rte_mempool *pool)
{
    struct port_configuration plan;
    int rc = prepare_port_configuration(port, cfg, &plan, NULL, 0);
    if (rc == 0)
        rc = configure_port_queues(port, cfg, pool, &plan, NULL, 0);
    if (rc == 0)
        rc = start_configured_port(port, cfg, &plan, NULL, 0);
    /** 初次启动失败后的 configured 和 started 仍反映已发生的操作，供原有 stop 回收 */
    if (rc != 0)
        return rc;
    publish_port_configuration(port, &plan);
    print_port_configuration(port, cfg);
    return 0;
}

static int reset_failure(struct dppd_device_set *devices, struct dppd_device_reset_result *result, int error)
{
    result->last_error = error;
    if (devices != NULL && atomic_load_explicit(&devices->event_tracking_enabled, memory_order_acquire)) {
        atomic_store_explicit(&devices->event_io_quiesced, true, memory_order_release);
        result->sequence_after = atomic_load_explicit(&devices->event_sequence, memory_order_acquire);
    }
    return error;
}

int dppd_devices_reset_reconfigure(struct dppd_device_set *devices, const struct dppd_config *cfg,
    uint64_t expected_sequence, struct dppd_device_reset_result *result)
{
    if (result == NULL)
        return -EINVAL;
    *result = (struct dppd_device_reset_result){.phase = DPPD_DEVICE_RESET_PRECHECK,
        .port_id = UINT16_MAX,
        .sequence_after = expected_sequence};
    int rc = reset_identity_ready(devices, cfg, &result->port_id);
    if (rc != 0)
        return reset_failure(devices, result, rc);
    rc = dppd_devices_pause_io(devices, expected_sequence);
    if (rc != 0)
        return reset_failure(devices, result, rc);
    struct port_configuration plans[DPPD_MAX_PORTS];
    /** 先核对全组证据，再执行任何 stop，不让第二口的缺失身份在第一口已停止后才暴露 */
    struct dppd_topology candidate;
    rc = dppd_topology_rediscover_verified(cfg, &devices->topology, &candidate);
    if (rc != 0)
        return reset_failure(devices, result, rc);
    for (uint16_t i = 0; i < devices->nb_ports; ++i) {
        const struct dppd_port *port = &devices->ports[i];
        result->port_id = port->port_id;
        rc = reset_event_fence(devices, expected_sequence, false, NULL);
        if (rc != 0)
            return reset_failure(devices, result, rc);
        if (!rte_eth_dev_is_valid_port(port->port_id) || rte_eth_dev_is_removed(port->port_id))
            return reset_failure(devices, result, -ENODEV);
        if (port_socket(port->port_id) != port->socket_id)
            return reset_failure(devices, result, -EXDEV);
        rc = reset_event_fence(devices, expected_sequence, false, NULL);
        if (rc == 0)
            rc = prepare_port_configuration(port, cfg, &plans[i], devices, expected_sequence);
        if (rc != 0)
            return reset_failure(devices, result, rc);
    }

    result->phase = DPPD_DEVICE_RESET_STOP;
    int stop_error = 0;
    uint16_t stop_error_port = UINT16_MAX;
    for (uint16_t i = 0; i < devices->nb_ports; ++i) {
        struct dppd_port *port = &devices->ports[i];
        result->port_id = port->port_id;
        rc = reset_event_fence(devices, expected_sequence, false, NULL);
        if (rc != 0)
            return reset_failure(devices, result, rc);
        rc = rte_eth_dev_stop(port->port_id);
        if (rc == 0) {
            port->started = false;
            atomic_store_explicit(&port->link_state, DPPD_LINK_DOWN, memory_order_release);
            result->stopped_ports |= UINT32_C(1) << i;
        } else if (stop_error == 0) {
            stop_error = rc;
            stop_error_port = port->port_id;
        }
        rc = reset_event_fence(devices, expected_sequence, false, NULL);
        if (rc != 0)
            return reset_failure(devices, result, rc);
    }
    if (stop_error != 0) {
        result->port_id = stop_error_port;
        return reset_failure(devices, result, stop_error);
    }

    result->phase = DPPD_DEVICE_RESET_RESET;
    uint64_t sequence = expected_sequence;
    for (uint16_t i = 0; i < devices->nb_ports; ++i) {
        struct dppd_port *port = &devices->ports[i];
        result->port_id = port->port_id;
        rc = reset_event_fence(devices, sequence, false, NULL);
        if (rc != 0)
            return reset_failure(devices, result, rc);
        rc = rte_eth_dev_reset(port->port_id);
        if (rc != 0)
            return reset_failure(devices, result, rc);
        port->configured = false;
        result->reset_ports |= UINT32_C(1) << i;
        /** 只有 reset 调用窗口内允许新 RESET 证据，记录序号但不确认事件已恢复 */
        rc = reset_event_fence(devices, sequence, true, &sequence);
        result->sequence_after = sequence;
        if (rc != 0)
            return reset_failure(devices, result, rc);
    }

    result->phase = DPPD_DEVICE_RESET_VERIFY;
    result->port_id = UINT16_MAX;
    rc = dppd_topology_rediscover_verified(cfg, &devices->topology, &candidate);
    if (rc != 0)
        return reset_failure(devices, result, rc);
    for (uint16_t i = 0; i < devices->nb_ports; ++i) {
        result->port_id = devices->ports[i].port_id;
        rc = reset_event_fence(devices, sequence, false, NULL);
        if (rc == 0)
            rc = prepare_port_configuration(&devices->ports[i], cfg, &plans[i], devices, sequence);
        if (rc != 0)
            return reset_failure(devices, result, rc);
    }

    result->phase = DPPD_DEVICE_RESET_CONFIGURE;
    for (uint16_t i = 0; i < devices->nb_ports; ++i) {
        struct dppd_port *port = &devices->ports[i];
        result->port_id = port->port_id;
        rc = reset_event_fence(devices, sequence, false, NULL);
        if (rc == 0)
            rc = configure_port_queues(port, cfg, devices->pools[port->socket_id], &plans[i], devices, sequence);
        if (port->configured)
            result->configured_ports |= UINT32_C(1) << i;
        if (rc != 0)
            return reset_failure(devices, result, rc);
    }

    result->phase = DPPD_DEVICE_RESET_START;
    for (uint16_t i = 0; i < devices->nb_ports; ++i) {
        struct dppd_port *port = &devices->ports[i];
        result->port_id = port->port_id;
        rc = reset_event_fence(devices, sequence, false, NULL);
        if (rc == 0)
            rc = start_configured_port(port, cfg, &plans[i], devices, sequence);
        if (port->started)
            result->started_ports |= UINT32_C(1) << i;
        if (rc != 0)
            return reset_failure(devices, result, rc);
    }
    rc = reset_event_fence(devices, sequence, false, NULL);
    if (rc != 0)
        return reset_failure(devices, result, rc);
    for (uint16_t i = 0; i < devices->nb_ports; ++i)
        publish_port_configuration(&devices->ports[i], &plans[i]);
    result->phase = DPPD_DEVICE_RESET_READY;
    result->port_id = UINT16_MAX;
    result->sequence_after = sequence;
    return 0;
}

int dppd_devices_init(struct dppd_device_set *devices, const struct dppd_config *cfg)
{
    uint16_t i;
    int rc;

    if (devices == NULL || cfg == NULL)
        return -EINVAL;
    memset(devices, 0, sizeof(*devices));
    atomic_init(&devices->removal_requested, false);
    atomic_init(&devices->event_tracking_enabled, false);

    for (i = 0; i < cfg->nb_ports; ++i) {
        if (!rte_eth_dev_is_valid_port(cfg->ports[i])) {
            fprintf(stderr, "[dppd] invalid ethdev port %u\n", cfg->ports[i]);
            return -ENODEV;
        }
    }

    /**
     * 一次性初始化所有端口元数据，再开始注册回调
     * 注册过程中就可能收到移除事件，回调不能读取尚未初始化的其他端口状态
     */
    devices->nb_ports = cfg->nb_ports;
    for (i = 0; i < cfg->nb_ports; ++i) {
        struct dppd_port *port = &devices->ports[i];
        const uint16_t pair_base = (uint16_t)(i & ~1U);

        port->port_id = cfg->ports[i];
        atomic_init(&port->removed, false);
        atomic_init(&port->link_state, DPPD_LINK_UNKNOWN);
        port->peer_port_id =
            cfg->routing_path[0] == '\0' ? cfg->ports[pair_base + (i % 2U == 0 ? 1U : 0U)] : UINT16_MAX;
        port->socket_id = port_socket(port->port_id);
    }

    rc = create_socket_pools(devices, cfg);
    if (rc != 0)
        goto fail;

    for (i = 0; i < cfg->nb_ports; ++i) {
        struct dppd_port *port = &devices->ports[i];

        rc = rte_eth_dev_callback_register(port->port_id, RTE_ETH_EVENT_INTR_RMV, handle_removal, devices);
        if (rc != 0) {
            fprintf(stderr, "[dppd] port=%u removal callback registration failed: %d\n", port->port_id, rc);
            goto fail;
        }
        port->removal_callback_registered = true;
        /** 注册调用返回前也可能已触发移除，此时直接清理，不再配置或启动端口 */
        if (dppd_devices_removal_requested(devices)) {
            rc = -ENODEV;
            goto fail;
        }
        rc = configure_port(port, cfg, devices->pools[port->socket_id]);
        if (rc != 0) {
            fprintf(stderr, "[dppd] failed to configure port %u: %s (%d)\n", port->port_id, rte_strerror(-rc), rc);
            goto fail;
        }
    }

    rc = dppd_devices_poll_links(devices);
    if (rc != 0)
        goto fail;
    rc = dppd_topology_discover(cfg, &devices->topology);
    if (rc != 0)
        goto fail;
    dppd_topology_dump(&devices->topology);
    return 0;

fail:
    (void)dppd_devices_stop(devices);
    return rc;
}

int dppd_devices_poll_links(struct dppd_device_set *devices)
{
    if (devices == NULL || devices->nb_ports > DPPD_MAX_PORTS)
        return -EINVAL;
    bool tracking = atomic_load_explicit(&devices->event_tracking_enabled, memory_order_acquire);
    uint64_t sequence = tracking ? atomic_load_explicit(&devices->event_sequence, memory_order_acquire) : 0;
    int result;
    if (tracking && (result = reset_event_fence(devices, sequence, false, NULL)) != 0)
        return result;
    for (uint16_t i = 0; i < devices->nb_ports; ++i) {
        struct dppd_port *port = &devices->ports[i];
        struct rte_eth_link link = {0};
        unsigned int previous, next;
        int rc;

        if (!port->started)
            continue;
        if (tracking && (rc = reset_event_fence(devices, sequence, false, NULL)) != 0)
            return rc;
        /**
         * 先检查设备身份和移除状态，再判断是否跳过链路查询
         * 即使 PMD 不支持 link API，也必须独立发现设备失效，不能永远跳过移除检测
         */
        bool removed = atomic_load_explicit(&port->removed, memory_order_acquire);
        bool valid = removed ? false : rte_eth_dev_is_valid_port(port->port_id) != 0;
        if (tracking && (rc = reset_event_fence(devices, sequence, false, NULL)) != 0)
            return rc;
        if (valid)
            removed = rte_eth_dev_is_removed(port->port_id) != 0;
        if (tracking && (rc = reset_event_fence(devices, sequence, false, NULL)) != 0)
            return rc;
        if (removed || !valid) {
            /** 主动探测和异步 RMV 使用相同证据，避免主线程只看到退出请求却漏掉句柄隔离 */
            if (tracking)
                (void)handle_removal(port->port_id, RTE_ETH_EVENT_INTR_RMV, devices, NULL);
            else
                request_removal(devices, port);
            fprintf(stderr, "[dppd] port=%u device removal detected\n", port->port_id);
            return -ENODEV;
        }
        previous = atomic_load_explicit(&port->link_state, memory_order_acquire);
        if (previous == DPPD_LINK_UNSUPPORTED)
            continue;
        rc = rte_eth_link_get_nowait(port->port_id, &link);
        /** 主动暂停时仍可查询链路，但查询中出现新事件就停止，不访问下一口旧实例 */
        if (tracking && (result = reset_event_fence(devices, sequence, false, NULL)) != 0)
            return result;
        if (rc == -ENOTSUP && previous == DPPD_LINK_UNKNOWN) {
            next = DPPD_LINK_UNSUPPORTED;
        } else if (rc != 0) {
            atomic_store_explicit(&port->link_state, DPPD_LINK_DOWN, memory_order_release);
            fprintf(stderr, "[dppd] port=%u link query failed: %d\n", port->port_id, rc);
            return rc;
        } else {
            next = link.link_status == RTE_ETH_LINK_UP ? DPPD_LINK_UP : DPPD_LINK_DOWN;
        }
        atomic_store_explicit(&port->link_state, next, memory_order_release);
        if (previous != next)
            fprintf(stderr, "[dppd] port=%u link=%s\n", port->port_id, dppd_link_state_name(next));
    }
    if (tracking && (result = reset_event_fence(devices, sequence, false, NULL)) != 0)
        return result;
    /** 覆盖遍历期间收到事件或尚未启动的端口已移除的情况，避免错误返回正常状态 */
    return dppd_devices_removal_requested(devices) ? -ENODEV : 0;
}

/** DESTROY 后端口编号可以分给新设备，必须保留资源而不是向这个编号发送 stop 或 close */
static int verify_teardown_identity(const struct dppd_device_set *devices, uint16_t index)
{
    if (!atomic_load_explicit(&devices->event_tracking_enabled, memory_order_acquire))
        return 0;
    if ((atomic_load_explicit(&devices->destroy_event_ports, memory_order_acquire) & (UINT32_C(1) << index)) != 0)
        return -ENODEV;
    const struct dppd_port *port = &devices->ports[index];
    const struct dppd_endpoint *endpoint = dppd_topology_find(&devices->topology, port->port_id);
    const struct dppd_capability_identity *identity = &port->identity;
    if (endpoint == NULL || !identity->device_name_known)
        return -ENODATA;
    if (memchr(endpoint->device_name, '\0', sizeof(endpoint->device_name)) == NULL ||
        memchr(endpoint->driver_name, '\0', sizeof(endpoint->driver_name)) == NULL ||
        memchr(identity->device_name, '\0', sizeof(identity->device_name)) == NULL)
        return -EINVAL;
    if (endpoint->device_name[0] == '\0' || endpoint->driver_name[0] == '\0' ||
        strcmp(endpoint->driver_name, "unknown") == 0 || identity->device_name[0] == '\0')
        return -ENODATA;
    if (!rte_eth_dev_is_valid_port(port->port_id))
        return -ENODEV;
    char device_name[DPPD_CAPABILITY_NAME_SIZE] = {0};
    int rc = rte_eth_dev_get_name_by_port(port->port_id, device_name);
    if (rc != 0)
        return rc;
    if (memchr(device_name, '\0', sizeof(device_name)) == NULL)
        return -EINVAL;
    if (strcmp(device_name, identity->device_name) != 0)
        return -EXDEV;
    struct rte_eth_dev_info info = {0};
    rc = rte_eth_dev_info_get(port->port_id, &info);
    if (rc != 0)
        return rc;
    const char *bus_name = NULL;
    if (info.device != NULL) {
#if RTE_VERSION >= RTE_VERSION_NUM(22, 11, 0, 0)
        bus_name = rte_dev_name(info.device);
#else
        bus_name = info.device->name;
#endif
    }
    if (bus_name == NULL || info.driver_name == NULL)
        return -ENODATA;
    if (strnlen(bus_name, sizeof(endpoint->device_name)) >= sizeof(endpoint->device_name) ||
        strnlen(info.driver_name, sizeof(endpoint->driver_name)) >= sizeof(endpoint->driver_name))
        return -ENAMETOOLONG;
    return strcmp(bus_name, endpoint->device_name) == 0 && strcmp(info.driver_name, endpoint->driver_name) == 0
        ? 0
        : -EXDEV;
}

int dppd_devices_stop(struct dppd_device_set *devices)
{
    uint16_t i;
    int socket_id;
    int result = 0;
    bool all_closed = true;

    if (devices == NULL)
        return -EINVAL;
    if (devices->nb_ports > DPPD_MAX_PORTS)
        return -EINVAL;
    /**
     * 必须先注销所有端口的回调，再关闭任何端口或修改回调借用的元数据
     * 按端口单独注销并检查结果，不能假定一次批量调用能准确报告每个在途回调
     */
    for (i = 0; i < devices->nb_ports; ++i) {
        struct dppd_port *port = &devices->ports[i];
        const enum rte_eth_event_type events[] = {RTE_ETH_EVENT_INTR_RESET, RTE_ETH_EVENT_DESTROY,
            RTE_ETH_EVENT_INTR_RMV};
        bool *registered[] = {&port->reset_callback_registered, &port->destroy_callback_registered,
            &port->removal_callback_registered};
        for (unsigned int j = 0; j < sizeof(events) / sizeof(events[0]); ++j) {
            if (!*registered[j])
                continue;
            int rc;
            /** EAGAIN 表示回调仍在执行，短暂休眠后重试，避免忙等并保证上下文仍然有效 */
            do {
                rc = rte_eth_dev_callback_unregister(port->port_id, events[j], handle_removal, devices);
                if (rc == -EAGAIN) {
                    struct timespec remaining = {.tv_nsec = 1000000L};
                    while (nanosleep(&remaining, &remaining) != 0 && errno == EINTR)
                        ;
                }
            } while (rc == -EAGAIN);
            if (rc != 0) {
                /** 其他注销错误不能当作成功，保留设备和缓冲池供调用方报告故障 */
                const char *name = events[j] == RTE_ETH_EVENT_INTR_RMV ? "removal" : "device event";
                fprintf(stderr, "[dppd] port=%u %s callback unregister failed: %d\n", port->port_id, name, rc);
                return rc;
            }
            *registered[j] = false;
        }
    }
    for (i = 0; i < devices->nb_ports; ++i) {
        struct dppd_port *port = &devices->ports[i];
        int rc;

        if (port->started || port->configured) {
            rc = verify_teardown_identity(devices, i);
            if (rc != 0) {
                if (result == 0)
                    result = rc;
                all_closed = false;
                continue;
            }
        }
        if (port->started) {
            rc = rte_eth_dev_stop(port->port_id);
            if (rc != 0) {
                fprintf(stderr, "[dppd] port=%u device stop failed: %d\n", port->port_id, rc);
                if (result == 0)
                    result = rc;
            } else {
                port->started = false;
                atomic_store_explicit(&port->link_state, DPPD_LINK_DOWN, memory_order_release);
            }
        }
        if (port->configured) {
            rc = verify_teardown_identity(devices, i);
            if (rc != 0) {
                if (result == 0)
                    result = rc;
                all_closed = false;
                continue;
            }
            /** stop 失败后仍尝试 close，成功关闭后才有机会安全归还该端口使用的资源 */
            rc = rte_eth_dev_close(port->port_id);
            if (rc != 0) {
                fprintf(stderr, "[dppd] port=%u device close failed: %d; retaining mbuf pools\n", port->port_id, rc);
                if (result == 0)
                    result = rc;
                all_closed = false;
                continue;
            }
        }
        port->started = false;
        port->configured = false;
    }
    /** 多个端口可能共用同一缓冲池，任一 close 失败就不能释放任何共享池 */
    if (!all_closed)
        return result;
    for (socket_id = 0; socket_id < RTE_MAX_NUMA_NODES; ++socket_id) {
        if (devices->pools[socket_id] != NULL) {
            const struct rte_mempool *pool = devices->pools[socket_id];
            const unsigned int available = rte_mempool_avail_count(pool);

            printf("[dppd] mbuf pool socket=%d available=%u capacity=%u in-use=%u\n", socket_id, available, pool->size,
                pool->size - available);
            rte_mempool_free(devices->pools[socket_id]);
            devices->pools[socket_id] = NULL;
        }
    }
    devices->nb_ports = 0;
    return result;
}

const struct dppd_port *dppd_devices_find(const struct dppd_device_set *devices, uint16_t port_id)
{
    uint16_t i;

    if (devices == NULL)
        return NULL;
    for (i = 0; i < devices->nb_ports; ++i) {
        if (devices->ports[i].port_id == port_id)
            return &devices->ports[i];
    }
    return NULL;
}
