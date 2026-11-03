/*
 * @Author: gwpptm007 wqgwpptm@gmail.com
 * @Date: 2026-10-19 16:38:52
 * @LastEditors: gwpptm007 wqgwpptm@gmail.com
 * @LastEditTime: 2026-11-03 20:11:41
 * @FilePath: \dpdk-dataplane-platform\app\dppd\main.c
 * @Description: 这是默认设置,请设置`customMade`, 打开koroFileHeader查看配置 进行设置: https://github.com/OBKoro1/koro1FileHeader/wiki/%E9%85%8D%E7%BD%AE
 */
#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <net/if.h>
#include <rte_cycles.h>
#include <rte_eal.h>
#include <rte_ethdev.h>
#include "dppd/config.h"
#include "dppd/control.h"
#include "dppd/management.h"
#include "dppd/runtime.h"
#include "dppd/recovery_guard.h"
#include "dppd/recovery_inspect.h"
#include "dppd/recovery_scope.h"
#include "dppd/monitor_channel.h"
#include "dppd/telemetry.h"
#include "dppd/managed.h"

static volatile sig_atomic_t stop_signal;
static struct dppd_recovery_scope managed_scope = {.fd = -1};

/** 恢复记录与已捕获设备身份只由主线程借用，软件 worker 不访问文件或这些指针 */
struct recovery_context {
    struct dppd_recovery_guard *guard;
    const struct dppd_device_set *devices;
    struct dppd_monitor_client *monitor;
    struct dppd_recovery_inspection before;
    uint16_t pending_port;
    int before_error;
    bool record_error_reported;
    bool native_owner;
    uint8_t pending_cookie[DPPD_TAP_COOKIE_SIZE];
    struct dppd_tap_content pending_content;
};

/** 从已落盘的端口身份读取观察范围，不再按可复用的 DPDK 名称推断内核接口 */
static int observe_recovery_port(struct recovery_context *context, struct dppd_recovery_inspection *inspection)
{
    for (uint32_t index = 0; index < context->guard->record.count; ++index)
        if (context->guard->record.ports[index].port_id == context->pending_port)
            return dppd_recovery_inspect(&context->guard->record.ports[index], inspection);
    return -ENOENT;
}

/** 不能只凭可重用的端口号记录身份，无法取得设备名称时拒绝这次硬件安装 */
static int prepare_recovery(void *context, uint16_t port_id, const struct dppd_rule *rule, uint64_t *attempt,
    uint8_t cookie[DPPD_TAP_COOKIE_SIZE])
{
    struct recovery_context *recovery = context;
    const struct dppd_port *port = dppd_devices_find(recovery->devices, port_id);
    const struct dppd_endpoint *endpoint = dppd_topology_find(&recovery->devices->topology, port_id);
    int rc;

    if (port == NULL || endpoint == NULL || !port->identity.device_name_known)
        return -ENODEV;
    rc = dppd_scope_check(&managed_scope);
    if (rc != 0)
        return rc;
    if (recovery->monitor->error != 0)
        return recovery->monitor->error;
    uint64_t shared_id = 0, shared_generation = 0;
    for (uint16_t index = 0; index < rule->nb_actions; ++index)
        if (rule->actions[index].type == DPPD_ACTION_SHARED_COUNT) {
            shared_id = rule->actions[index].conf.shared_count.id;
            shared_generation = rule->actions[index].conf.shared_count.generation;
        }
    /** TAP 所有权模式依赖可读回的独立动作，不能给间接共享动作伪造同样的证明 */
    if (shared_id != 0 && (recovery->native_owner || recovery->monitor->fd >= 0))
        return -ENOTSUP;
    memset(cookie, 0, DPPD_TAP_COOKIE_SIZE);
    memset(recovery->pending_cookie, 0, sizeof(recovery->pending_cookie));
    /** 此标识仅用于无副作用的能力检查，真正创建的随机标识稍后生成并落盘 */
    if (recovery->native_owner) {
        const uint8_t probe[DPPD_TAP_COOKIE_SIZE] = {1};
        struct dppd_flow_error error;
        rc = dppd_flow_validate_owned(port_id, rule, probe, &error);
        if (rc != 0) {
            fprintf(stderr, "[dppd] TAP owner validation failed: %s (%d)\n", error.message, rc);
            return rc;
        }
    }
    /** TAP 的 DPDK 名称和内核接口名不同，必须由驱动给出真实索引后再向内核核对 */
    if (strcmp(endpoint->driver_name, "net_tap") == 0) {
        struct rte_eth_dev_info info;
        struct dppd_recovery_identity identity;
        rc = rte_eth_dev_info_get(port_id, &info);
        if (rc == 0)
            rc = dppd_recovery_tap_identity(info.if_index, &identity);
        if (rc != 0)
            return recovery->monitor->fd >= 0 ? (recovery->monitor->error = rc) : rc;
        if (rc == 0 && recovery->native_owner)
            rc = dppd_tap_content_from_rule(rule, info.max_rx_queues, &recovery->pending_content);
        if (rc == 0)
            rc = dppd_monitor_client_identity(recovery->monitor, port_id, &identity);
        if (rc == 0)
            rc = dppd_monitor_client_check(recovery->monitor);
        if (rc != 0)
            return rc;
        rc = dppd_recovery_guard_prepare_identity(recovery->guard, port_id, port->identity.device_name,
            endpoint->driver_name, rule->id, rule->generation, &identity);
    } else if (recovery->monitor->fd >= 0)
        return recovery->monitor->error = -ENOTSUP;
    else
        rc = dppd_recovery_guard_prepare(recovery->guard, port_id, port->identity.device_name, endpoint->driver_name,
            rule->id, rule->generation);
    if (rc != 0)
        return rc;
    if (recovery->native_owner)
        rc = dppd_recovery_guard_begin_owned(recovery->guard, port_id, rule->id, rule->generation, attempt, cookie,
            &recovery->pending_content);
    else if (shared_id != 0)
        rc = dppd_recovery_guard_begin_flow(recovery->guard, port_id, rule->id, rule->generation, rule->domain,
            shared_id, shared_generation, attempt);
    else
        rc = dppd_recovery_guard_begin(recovery->guard, port_id, rule->id, rule->generation, attempt);
    if (rc != 0)
        return rc;
    recovery->pending_port = port_id;
    memcpy(recovery->pending_cookie, cookie, sizeof(recovery->pending_cookie));
    /** 观察是附加线索，失败仍允许有意图保护的创建，之后明确记录无法观察 */
    recovery->before_error = observe_recovery_port(recovery, &recovery->before);
    /** 写盘与内核查询可能耗时，进入驱动前再取一次新回复，不能复用启动时的确认 */
    rc = dppd_scope_check(&managed_scope);
    return rc != 0 ? rc : dppd_monitor_client_check(recovery->monitor);
}

/** 记录失败只报告一次并保持故障锁存，避免清理阶段反复刷屏或把不确定记录覆盖为干净 */
static void recovery_record_error(struct recovery_context *context, int error)
{
    context->guard->faulted = true;
    if (!context->record_error_reported) {
        fprintf(stderr, "[dppd] recovery attempt record failed: %d; external reconciliation required\n", error);
        context->record_error_reported = true;
    }
}

/** 共享计数器先持久化独立对象意图，不把 indirect 指针写入日志或伪装成 TAP filter */
static int prepare_shared_recovery(void *context, const struct dppd_shared_count_spec *spec, uint64_t *attempt)
{
    struct recovery_context *recovery = context;
    const struct dppd_port *port = dppd_devices_find(recovery->devices, spec->install_port_id);
    const struct dppd_endpoint *endpoint = dppd_topology_find(&recovery->devices->topology, spec->install_port_id);
    if (port == NULL || endpoint == NULL || !port->identity.device_name_known)
        return -ENODEV;
    if (recovery->native_owner || recovery->monitor->fd >= 0)
        return -ENOTSUP;
    int rc = dppd_scope_check(&managed_scope);
    if (rc != 0)
        return rc;
    if (recovery->monitor->error != 0)
        return recovery->monitor->error;
    struct dppd_recovery_identity identity, *captured = NULL;
    if (strcmp(endpoint->driver_name, "net_tap") == 0) {
        struct rte_eth_dev_info info;
        rc = rte_eth_dev_info_get(spec->install_port_id, &info);
        if (rc == 0)
            rc = dppd_recovery_tap_identity(info.if_index, &identity);
        if (rc != 0)
            return rc;
        captured = &identity;
    }
    rc = dppd_recovery_guard_prepare_shared(recovery->guard, spec, port->identity.device_name, endpoint->driver_name,
        captured);
    if (rc == 0)
        rc = dppd_recovery_guard_begin_shared(recovery->guard, spec, attempt);
    /** 日志同步期间设备或受控作用域也可能改变，进入 PMD 前再核对一次 */
    if (rc == 0)
        rc = dppd_scope_check(&managed_scope);
    return rc;
}

/** 只记录真实返回值，普通 flow 的 TC 新增观察不能用来定位 indirect 对象 */
static int finish_shared_recovery_create(void *context, uint64_t attempt, int create_error)
{
    struct recovery_context *recovery = context;
    int rc = dppd_recovery_guard_created(recovery->guard, attempt, create_error, DPPD_EVIDENCE_NONE, 0, NULL);
    if (rc != 0)
        recovery_record_error(recovery, rc);
    if (rc == 0 && create_error == 0)
        rc = dppd_scope_check(&managed_scope);
    return rc;
}

/** 对比创建前后的只读列表，保留原始驱动结果，候选坐标只表示观察到一次新增 */
static int finish_recovery_create(void *context, uint64_t attempt, int create_error)
{
    struct recovery_context *recovery = context;
    struct dppd_recovery_inspection after = {0};
    struct dppd_recovery_filter candidate;
    enum dppd_recovery_evidence evidence = DPPD_EVIDENCE_UNAVAILABLE;
    int after_error = recovery->native_owner || recovery->before_error == 0 ? observe_recovery_port(recovery, &after)
                                                                            : recovery->before_error;
    int observed = recovery->before_error != 0 ? recovery->before_error : after_error;
    if (observed == 0)
        evidence = dppd_recovery_compare(&recovery->before, &after, &candidate);
    int rc = dppd_recovery_guard_created(recovery->guard, attempt, create_error, evidence, observed,
        evidence == DPPD_EVIDENCE_SINGLE_ADDITION ? &candidate : NULL);
    if (rc != 0)
        recovery_record_error(recovery, rc);
    /** 创建期间监护也可能退出，失败时保留真实 handle 交给事务回滚 */
    if (rc == 0 && create_error == 0)
        rc = dppd_scope_check(&managed_scope);
    if (rc == 0 && create_error == 0)
        rc = dppd_monitor_client_check(recovery->monitor);
    /**
     * 驱动报告成功后还要读回唯一标识，防止内核忽略属性时把未标记规则当作受保护
     * 原始创建结果已经保存，返回错误会触发事务使用真实 handle 回滚
     */
    if (rc == 0 && create_error == 0 && recovery->native_owner) {
        int matches = after_error != 0 ? after_error : dppd_recovery_cookie_matches(&after, recovery->pending_cookie);
        if (matches != 1) {
            fprintf(stderr, "[dppd] TAP owner readback not unique: %d; rolling back\n", matches);
            return matches < 0 ? matches : -EUCLEAN;
        }
        /** 唯一标识还不足以接受创建结果，内容或范围无法核实时用真实 handle 回滚 */
        for (uint32_t i = 0; i < after.count; ++i) {
            const struct dppd_recovery_filter *filter = &after.filters[i];
            if (!filter->cookie_valid || memcmp(filter->cookie, recovery->pending_cookie, DPPD_TAP_COOKIE_SIZE) != 0)
                continue;
            int content = dppd_recovery_verify(&recovery->pending_content, filter, false);
            int scope = dppd_recovery_verify(&recovery->pending_content, filter, true);
            if (content != 0 || scope != 0) {
                fprintf(stderr, "[dppd] TAP rule readback failed: content=%d scope=%d; rolling back\n", content, scope);
                return -EUCLEAN;
            }
        }
    }
    return rc;
}

/** 已持有 handle 的删除不依赖监护回复，避免监护退出反而阻止本进程回收规则 */
static void finish_recovery_remove(void *context, uint64_t attempt, int remove_error)
{
    struct recovery_context *recovery = context;
    int rc = dppd_recovery_guard_removed(recovery->guard, attempt, remove_error);
    if (rc != 0)
        recovery_record_error(recovery, rc);
}

static void handle_signal(int signal_number)
{
    /** 信号处理器只设置退出标记，主线程跳出循环后再停止 worker 和回收资源 */
    (void)signal_number;
    stop_signal = 1;
}

/** 主线程每轮结束后休眠 100 毫秒，worker 继续收包；一轮总时长还包含前面的处理耗时 */
static void sleep_control_loop(void)
{
    struct timespec remaining = {
        .tv_sec = 0,
        .tv_nsec = 100000000L,
    };

    /* 被非退出信号打断时睡完剩余时间；退出信号到达后立即返回主循环 */
    while (nanosleep(&remaining, &remaining) != 0 && errno == EINTR && !stop_signal)
        ;
}

/** EAL 探测之后、项目配置队列和重放之前核对全部端口，缺少任何一个都不接受会话 */
static int bind_monitor(const struct dppd_config *config, struct dppd_recovery_guard *guard,
    struct dppd_monitor_client *client)
{
    struct dppd_recovery_record ports = {.count = config->nb_ports};
    char error[128];
    if (dppd_config_validate(config, error, sizeof(error)) != 0)
        return -EINVAL;
    for (uint32_t index = 0; index < ports.count; ++index) {
        struct rte_eth_dev_info info;
        uint16_t port = config->ports[index];
        int rc = rte_eth_dev_info_get(port, &info);
        if (rc != 0)
            return rc;
        if (info.driver_name == NULL || strcmp(info.driver_name, "net_tap") != 0)
            return -ENOTSUP;
        ports.ports[index].port_id = port;
        rc = dppd_recovery_tap_identity(info.if_index, &ports.ports[index].identity);
        if (rc != 0)
            return rc;
    }
    return dppd_monitor_client_open(client, config->recovery_monitor, guard, &ports);
}

/** 启动时绑定稳定端口映射，恢复配置不能按可复用的 DPDK 编号猜测设备或 TAP */
static int setup_ipv4_routing(struct dppd_runtime *runtime, struct dppd_control_service *control)
{
    if (runtime->config.routing_path[0] == '\0')
        return 0;
    struct dppd_routing_binding bindings[DPPD_MAX_PORTS] = {0};
    for (uint16_t i = 0; i < runtime->devices.nb_ports; ++i) {
        const struct dppd_port *port = &runtime->devices.ports[i];
        const struct dppd_endpoint *endpoint = dppd_topology_find(&runtime->devices.topology, port->port_id);
        if (endpoint == NULL || !port->identity.device_name_known || !rte_is_valid_assigned_ether_addr(&port->mac))
            return -ENODEV;
        bindings[i].port_id = port->port_id;
        strcpy(bindings[i].device, port->identity.device_name);
        strcpy(bindings[i].driver, endpoint->driver_name);
        if (strcmp(endpoint->driver_name, "net_tap") == 0) {
            struct rte_eth_dev_info information;
            int rc = rte_eth_dev_info_get(port->port_id, &information);
            if (rc != 0)
                return rc;
            if (if_indextoname(information.if_index, bindings[i].ifname) == NULL)
                return -errno;
        }
    }
    int rc = dppd_routing_open(&control->routing, runtime->config.routing_path, bindings, runtime->devices.nb_ports);
    if (rc == 0) {
        dppd_runtime_set_ipv4_router(runtime, dppd_routing_router(control->routing));
        printf("[dppd] IPv4 routing ready: path=%s\n", runtime->config.routing_path);
    }
    return rc;
}

/**
 * 程序入口：解析启动配置，准备数据面和控制面，启动工作线程并运行管理循环
 *
 * 运行范围检查 → rte_eal_init → dppd_config_parse
 *   ├─ 配置 flow_state_path → dppd_managed_run，使用独立的受管理流程
 *   └─ 普通启动路径
 *       → 恢复保护检查 → runtime_init：准备 worker 配置、网口、队列和内存池
 *       → control_init：创建控制服务 → 绑定软件后端、路由等运行时对象
 *       → 按需恢复规则 → management_start：创建管理入口
 *       → runtime_start：启动 worker → RX burst → 已学过的 process_batch
 *       → 主线程循环处理管理请求、设备事件和统计
 *
 * 退出时先停止 worker，再释放控制面和设备资源，最后清理 EAL
 */
int main(int argc, char **argv)
{
    /** 在 EAL 创建线程和访问设备前，先执行受控运行范围检查 */
    int scope_error = dppd_scope_open(&managed_scope, DPPD_SCOPE_BUSINESS);
    if (scope_error != 0) {
        fprintf(stderr, "[dppd] managed scope refused: %d\n", scope_error);
        return EXIT_FAILURE;
    }
    struct dppd_config config;
    struct dppd_control_service control;
    struct dppd_management_server management;
    struct dppd_runtime runtime;
    struct dppd_recovery_guard recovery_guard = {.fd = -1};
    struct dppd_monitor_client monitor_client = {.fd = -1};
    struct recovery_context recovery_context = {.guard = &recovery_guard,
        .devices = &runtime.devices,
        .monitor = &monitor_client};
    uint64_t started_at;
    uint64_t next_stats;
    uint64_t timer_hz;
    int parse_result;
    int eal_consumed;
    int loop_error = 0;
    int rc = EXIT_FAILURE;
    bool recovery_isolation = false;

    /**
     * 启动分三阶段：主机先准备资源 → EAL 建立 DPDK 环境 → 本项目配置收发资源
     *
     * 1）运行 dppd 之前，由管理员或部署脚本准备 Linux 主机
     *    大页模式：预留系统大页，按环境准备 hugetlbfs 挂载和访问权限
     *      → 预留数量通常通过 nr_hugepages 或内核启动参数设置，本项目没有自动配置脚本
     *    VFIO 模式：加载 vfio-pci，并把指定 PCI 设备绑定给它
     *      → scripts/bind_nic.sh 中执行 modprobe 和 dpdk-devbind.py --bind=vfio-pci
     *    这些步骤不由本次 rte_eal_init 调用自动完成
     *    scripts/run_platform.sh 只启动程序，不预留大页，也不调用网卡绑定脚本
     *
     * 2）下面的 rte_eal_init 在 DPDK 库内部完成基础初始化
     *    解析 DPDK 参数，准备 lcore 线程环境和内存映射
     *      → 大页模式使用已预留的大页；--no-huge 使用非大页模式
     *    根据设备参数由对应 PMD（用户态轮询驱动）探测设备，建立 ethdev 网口对象
     *    VFIO 提供内核侧设备访问，PMD 负责具体网卡操作，两者不是同一个驱动
     *    例外：mlx5 等分叉驱动保留相应内核驱动，不能照搬 VFIO 解绑步骤；虚拟设备不一定需要 PCI 绑定
     *
     * 3）EAL 返回后，本项目再通过 runtime_init → dppd_devices_init（lib/device/device.c）
     *    创建 mbuf 池 → configure 网口 → setup RX/TX 队列 → start 网口
     *    随后 runtime_start 才启动 worker 执行业务收包循环
     *
     * 参数分工：dppd [EAL 参数：-l、-a、--vdev 等] -- [本项目参数：--ports、--queues 等]
     * 成功返回已消费的参数数量，后面移动 argc/argv，再交给应用配置解析器；失败返回负值
     */
    eal_consumed = rte_eal_init(argc, argv);
    if (eal_consumed < 0) {
        fprintf(stderr, "[dppd] EAL initialization failed\n");
        return EXIT_FAILURE;
    }

    argc -= eal_consumed;
    argv += eal_consumed;
    parse_result = dppd_config_parse(argc, argv, &config);
    if (parse_result == DPPD_CONFIG_HELP) {
        dppd_config_print_usage(argv[0]);
        rc = EXIT_SUCCESS;
        goto cleanup_eal;
    }
    if (parse_result != DPPD_CONFIG_OK) {
        fprintf(stderr, "[dppd] invalid application arguments\n");
        dppd_config_print_usage(argv[0]);
        goto cleanup_eal;
    }

    if (config.flow_state_path[0] != '\0') {
        char error[256];
        if (dppd_config_validate(&config, error, sizeof(error)) != 0) {
            fprintf(stderr, "[dppd] managed configuration refused: %s\n", error);
            goto cleanup_eal;
        }
        rc = dppd_managed_run(&config, &managed_scope);
        goto cleanup_eal;
    }
    const char *other_storage[] = {config.state_path, config.recovery_path, config.recovery_monitor,
        config.control_socket};
    int routing_paths_error =
        dppd_routing_check_paths(config.routing_path, other_storage, sizeof(other_storage) / sizeof(other_storage[0]));
    if (routing_paths_error != 0) {
        fprintf(stderr, "[dppd] IPv4 storage paths refused: %d\n", routing_paths_error);
        goto cleanup_eal;
    }
    dppd_config_dump(&config);
    /**
     * EAL 已完成参数和设备探测，但本项目尚未配置队列、启动 worker 或重放规则
     * 旧进程的待核对标记必须在这些步骤之前拦住启动，空 backend 不能证明设备已干净
     */
    if (config.recovery_path[0] != '\0') {
        int guard_rc = dppd_recovery_guard_open(&recovery_guard, config.recovery_path, config.state_path);
        if (guard_rc != 0) {
            fprintf(stderr, "[dppd] recovery guard open failed: %d\n", guard_rc);
            goto cleanup_eal;
        }
        if (recovery_guard.record.count != 0) {
            fprintf(stderr,
                "[dppd] prior hardware activity requires external reconciliation: "
                "revision=%" PRIu64 " ports=%u; inspect with dppd-recovery show PATH\n",
                recovery_guard.record.revision, recovery_guard.record.count);
            goto cleanup_eal;
        }
        if (dppd_recovery_record_requires_monitor(&recovery_guard.record) && config.recovery_monitor[0] == '\0') {
            fprintf(stderr, "[dppd] this recovery record requires --recovery-monitor\n");
            goto cleanup_eal;
        }
        if (config.recovery_monitor[0] != '\0') {
            int monitor_rc = bind_monitor(&config, &recovery_guard, &monitor_client);
            if (monitor_rc != 0) {
                fprintf(stderr, "[dppd] recovery monitor handshake failed: %d\n", monitor_rc);
                goto cleanup_eal;
            }
            printf("[dppd] recovery monitor bound; fresh replies required for hardware installation\n");
        }
    }
    /*
     * 初始化依赖顺序：先发现并启动 ethdev，得到拓扑；control 引用该拓扑；
     * management 再引用 control。goto 清理标签严格按相反顺序释放
     */
    if (dppd_runtime_init(&runtime, &config) != 0)
        goto cleanup_eal;
    if (dppd_control_init(&control, &runtime.devices.topology, config.rule_capacity, NULL) != 0) {
        fprintf(stderr, "[dppd] control service initialization failed\n");
        goto cleanup_runtime;
    }
    /** 快照重放前就启用事件门禁，不能等到 worker 启动后才开始保护驱动句柄 */
    int device_binding = dppd_runtime_device_bind(&runtime, &control);
    if (device_binding != 0) {
        fprintf(stderr, "[dppd] device event tracking refused: %d\n", device_binding);
        goto cleanup_control;
    }
    /*
     * worker 只借用 control 持有的 backend；退出时 main 的清理顺序会先停止并 wait 所有
     * worker（使其注销 QSBR reader），再调用 control_fini 释放 snapshot/QSBR。这里不能
     * 绑定临时对象，也不能在 runtime_start 后替换该地址
     */
    dppd_runtime_set_software_backend(&runtime, &control.software);
    control.conntrack = runtime.conntrack;
    control.nd6 = runtime.nd6;
    int routing_error = setup_ipv4_routing(&runtime, &control);
    if (routing_error != 0) {
        fprintf(stderr, "[dppd] IPv4 routing restore refused: %d\n", routing_error);
        goto cleanup_control;
    }
    /** 启动重放也会安装硬件规则，所以必须在读取并重放快照之前绑定保护 */
    if (recovery_guard.fd >= 0) {
        recovery_context.native_owner = config.tap_owner_cookie;
        control.rte_flow.before_create = prepare_recovery;
        control.rte_flow.before_create_context = &recovery_context;
        control.rte_flow.after_create = finish_recovery_create;
        control.rte_flow.after_remove = finish_recovery_remove;
        control.rte_flow.before_shared_create = prepare_shared_recovery;
        control.rte_flow.after_shared_create = finish_shared_recovery_create;
        control.rte_flow.after_shared_remove = finish_recovery_remove;
    }
    if (config.state_path[0] != '\0') {
        int restore_rc = dppd_control_persistence_restore(&control, config.state_path);

        if (restore_rc != 0) {
            if (restore_rc != -EUCLEAN || control.recovery_state != DPPD_CONTROL_RECOVERY_RECONCILIATION_REQUIRED) {
                fprintf(stderr, "[dppd] rule snapshot restore failed: path=%s error=%s (%d)\n", config.state_path,
                    restore_rc < 0 && restore_rc >= -4095 ? strerror(-restore_rc) : "unknown", restore_rc);
                goto cleanup_control;
            }
            /*
             * transaction 回滚未完成：绝不能启动 worker 或开放普通规则接口，但保留
             * 当前进程的 backend handle，供受限的 reconcile-retry 再次删除
             */
            recovery_isolation = true;
            fprintf(stderr,
                "[dppd] snapshot rollback incomplete; entering recovery isolation "
                "for path=%s\n",
                config.state_path);
        }
        if (!recovery_isolation)
            printf("[dppd] rule snapshot ready: path=%s generation=%" PRIu64 " rules=%u\n", config.state_path,
                dppd_rule_repository_generation(&control.rules), dppd_rule_repository_count(&control.rules));
    }
    if (dppd_management_start(&management, &control, &runtime.devices, &runtime, config.control_socket) != 0) {
        fprintf(stderr, "[dppd] management socket initialization failed: %s\n", config.control_socket);
        goto cleanup_control;
    }
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    /** 先发布本次重放或隔离的完成态，诊断线程只读取副本，不访问规则或驱动 */
    if (dppd_telemetry_register(&runtime, &control) != 0) {
        fprintf(stderr, "[dppd] telemetry command registration failed\n");
        goto cleanup_management;
    }
    if (dppd_runtime_device_poll(&runtime) != 0)
        goto cleanup_telemetry;
    recovery_isolation |= control.recovery_state != DPPD_CONTROL_RECOVERY_READY;
    if (recovery_isolation) {
        /**
         * 隔离保留只读诊断，不启动 ethdev worker，也不输出正常转发统计
         * 清理重试结束后更新诊断副本，成功进入 RESTART_REQUIRED 后仍必须退出重启
         */
        while (!stop_signal) {
            loop_error = dppd_runtime_device_poll(&runtime);
            if (loop_error != 0)
                break;
            loop_error = dppd_monitor_client_check(&monitor_client);
            if (loop_error != 0) {
                fprintf(stderr, "[dppd] recovery monitor unavailable during isolation: %d\n", loop_error);
                break;
            }
            /** 隔离仅停止软件转发，设备仍需监控，移除时应退出并清理而不是继续等待 */
            loop_error = dppd_devices_io_quiesced(&runtime.devices) ? 0 : dppd_devices_poll_links(&runtime.devices);
            if (loop_error != 0) {
                fprintf(stderr, "[dppd] device monitoring failed during recovery isolation\n");
                break;
            }
            if (runtime.conntrack != NULL)
                dppd_conntrack_gc(runtime.conntrack, dppd_monotonic_ms(), 128);
            loop_error = dppd_management_poll(&management);
            if (loop_error != 0) {
                fprintf(stderr, "[dppd] recovery management processing failed: %d\n", loop_error);
                break;
            }
            if (dppd_telemetry_publish_rules(&control) != 0)
                fprintf(stderr, "[dppd] rule telemetry snapshot unavailable\n");
            if (control.recovery_state == DPPD_CONTROL_RECOVERY_RESTART_REQUIRED) {
                fprintf(stderr, "[dppd] residual flows cleared; exiting for clean snapshot replay\n");
                loop_error = -EUCLEAN;
                break;
            }
            sleep_control_loop();
        }
        goto cleanup_telemetry;
    }

    loop_error = dppd_monitor_client_check(&monitor_client);
    if (loop_error != 0) {
        fprintf(stderr, "[dppd] recovery monitor unavailable before worker startup: %d\n", loop_error);
        goto cleanup_telemetry;
    }
    if (dppd_runtime_start(&runtime) != 0)
        goto cleanup_telemetry;

    /** 使用 DPDK 单调计时器计算运行时长和统计周期，避免系统时间调整影响判断 */
    timer_hz = rte_get_timer_hz();
    started_at = rte_get_timer_cycles();
    next_stats = started_at + (timer_hz * config.stats_period_ms) / 1000U;
    /**
     * worker 启动后，主线程进入管理循环，与各 lcore 上的报文循环并行执行
     *
     * 主线程：检查设备和 worker → 输出统计、清理过期会话 → 处理 dppctl 请求
     *           → 再查设备事件、轮询规则老化、发布查询快照 → 处理恢复状态 → 休眠
     * worker：按 queue_id 收包 → process_batch → 报告安全点和心跳 → 下一轮
     *
     * 正常运行到达 duration 或收到信号时结束循环；无法继续运行的检查错误也会跳出
     * 规则清理失败则先停止并等待 worker，保留管理入口供恢复查询和清理重试
     * 跳出循环后，统一关闭管理入口、停止并等待 worker，再回收共享资源
     */
    while (!stop_signal) {
        const uint64_t now = rte_get_timer_cycles();

        /** 事件优先于链路、AGE 和管理处理，先等待 worker 退出，再保留受限诊断入口 */
        loop_error = dppd_runtime_device_poll(&runtime);
        if (loop_error != 0)
            break;
        if (control.recovery_state != DPPD_CONTROL_RECOVERY_READY)
            recovery_isolation = true;
        else if (runtime.workers_started && runtime.device_recovery.status.state == DPPD_DEVICE_RECOVERY_RUNNING)
            recovery_isolation = false;

        loop_error = dppd_scope_check(&managed_scope);
        if (loop_error != 0) {
            fprintf(stderr, "[dppd] managed scope lost; stopping workers: %d\n", loop_error);
            break;
        }
        loop_error = dppd_monitor_client_check(&monitor_client);
        if (loop_error != 0) {
            fprintf(stderr, "[dppd] recovery monitor unavailable; stopping workers: %d\n", loop_error);
            break;
        }

        /**
         * 进入恢复隔离后，不再按普通运行时长退出，也不继续周期性输出转发统计
         * 这样用户仍有机会查看残留对象并发起清理，避免定时退出丢失本进程的 handle
         */
        if (!recovery_isolation && config.duration_s != 0 && now - started_at >= timer_hz * config.duration_s)
            break;
        loop_error = dppd_devices_io_quiesced(&runtime.devices) ? 0 : dppd_devices_poll_links(&runtime.devices);
        if (loop_error != 0) {
            fprintf(stderr, "[dppd] device monitoring failed; stopping workers\n");
            break;
        }
        /**
         * 线程可能在完成启动请求后才遇到规则读者注册失败
         * 检测到异常结束就停止其他线程并退出，避免管理进程存活却缺少转发队列
         */
        loop_error = dppd_runtime_check_workers(&runtime);
        if (loop_error != 0) {
            fprintf(stderr, "[dppd] worker monitoring failed; stopping workers\n");
            break;
        }
        if (!recovery_isolation && now >= next_stats) {
            dppd_runtime_stats_dump(&runtime);
            next_stats = now + (timer_hz * config.stats_period_ms) / 1000U;
        }
        /** 分批清理过期会话，限制本轮工作量，避免长时间占用主线程 */
        if (runtime.conntrack != NULL)
            dppd_conntrack_gc(runtime.conntrack, dppd_monotonic_ms(), 128);
        /**
         * 普通管理入口在主线程串行处理请求，每轮连接数有预算，处理后继续设备检查
         * 规则操作的业务错误通过响应交给 dppctl，不等同于本次 poll 的服务处理错误
         */
        loop_error = dppd_management_poll(&management);
        if (loop_error != 0) {
            fprintf(stderr, "[dppd] management request processing failed: %d\n", loop_error);
            break;
        }
        loop_error = dppd_runtime_device_poll(&runtime);
        if (loop_error != 0)
            break;
        /** AGE 查询失败只保留错误并重试，不把缺少驱动数据当作已经过期 */
        (void)dppd_control_age_poll(&control, dppd_monotonic_ms() * UINT64_C(1000000), DPPD_AGE_POLL_BUDGET);
        /** 整轮请求处理完再发布规则查询快照，独立查询线程读取这份完整结果 */
        if (dppd_telemetry_publish_rules(&control) != 0)
            fprintf(stderr, "[dppd] rule telemetry snapshot unavailable\n");
        if (control.recovery_state != DPPD_CONTROL_RECOVERY_READY) {
            if (!recovery_isolation) {
                /**
                 * 启动之后发生的补偿失败，也要执行与启动恢复失败相同的隔离约束
                 * 先发送停止请求，再等待已经启动的工作线程结束，之后才处理清理重试
                 * 停止软件转发不代表硬件残留规则停止工作，硬件对象仍要单独清理
                 */
                dppd_runtime_request_stop(&runtime);
                if (dppd_runtime_wait(&runtime) != 0) {
                    loop_error = -EIO;
                    break;
                }
                recovery_isolation = true;
                fprintf(stderr, "[dppd] rule recovery required; workers stopped\n");
            }
            if (control.recovery_state == DPPD_CONTROL_RECOVERY_RESTART_REQUIRED) {
                /**
                 * 残留对象已清理，不在原进程中直接恢复正常服务
                 * 用失败码退出，让操作者或服务管理器重启并从旧快照重新建立完整状态
                 */
                loop_error = -EUCLEAN;
                break;
            }
        }
        sleep_control_loop();
    }

    /**
     * 正常退出主线：关闭管理入口 → 请求 worker 停止 → 等待工作函数返回 → 释放共享资源
     * request_stop 只写停止标记，wait 才确认任务已结束，不能发送通知后就立即释放规则或设备
     * worker 退出前会注销规则和路由读者，等待成功后才能销毁这些后端
     */
    dppd_management_stop(&management);
    dppd_runtime_request_stop(&runtime);
    if (dppd_runtime_wait(&runtime) != 0)
        loop_error = -EIO;
    dppd_runtime_stats_dump(&runtime);
    /** 隔离期间即使由用户发信号结束，也不能把这次故障退出报告为正常运行成功 */
    if (loop_error == 0 && !recovery_isolation)
        rc = EXIT_SUCCESS;

    /**
     * 按资源依赖释放：查询与管理入口 → control 的规则后端和路由 → runtime 的协议与设备 → EAL
     * 网口关闭后才能释放 mbuf 池，避免驱动仍持有接收或发送缓冲
     * cleanup_* 是跳转标签，执行后会继续向下清理；启动失败只跳到已完成初始化的那一层
     */
cleanup_telemetry:
    dppd_telemetry_unregister_runtime();
cleanup_management:
    /** stop 可重复调用，启动失败和正常退出都先停止接收新管理请求 */
    dppd_management_stop(&management);
cleanup_control:
    /** 退出窗口也可能收到复位事件，未知句柄不能落入普通 fini 的逐项删除 */
    (void)dppd_runtime_device_poll(&runtime);
    if (dppd_control_fini(&control) != 0 && rc == EXIT_SUCCESS)//清理路由、规则后端、控制面记录
        rc = EXIT_FAILURE;
    /** backend 真正释放才证明已删除所有本进程 handle，失败时保留磁盘标记等待下次核对 */
    if (recovery_guard.fd >= 0 && control.rte_flow.objects == NULL && dppd_recovery_guard_clean(&recovery_guard) != 0) {
        fprintf(stderr, "[dppd] recovery guard could not record completed cleanup\n");
        rc = EXIT_FAILURE;
    }
cleanup_runtime:
    /** 关闭抓包服务，释放邻居等待报文、连接会话及设备资源 */
    if (dppd_runtime_destroy(&runtime) != 0 || dppd_devices_io_quiesced(&runtime.devices))
        rc = EXIT_FAILURE;
cleanup_eal:
    dppd_monitor_client_close(&monitor_client);
    /** 启动被旧标记阻断时同样只解锁，绝不能自动清除上一进程的线索 */
    dppd_recovery_guard_close(&recovery_guard);
    if (rte_eal_cleanup() != 0 && rc == EXIT_SUCCESS)
        rc = EXIT_FAILURE;
    return rc;
}


/**
 * 主机准备
  → EAL 初始化
  → 配置解析
  → worker 布局、内存池和队列准备
  → 控制服务及管理入口准备
  → 启动 worker
  → 主线程管理、worker 收包
  → 请求停止并等待
  → 释放控制面、设备和内存池
  → 清理 EAL，退出
*/