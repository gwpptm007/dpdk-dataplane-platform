#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <rte_ethdev.h>

/**
 * 此库仅供独立测试进程通过 LD_PRELOAD 显式加载，不链接到正式 daemon，也不安装
 * 测试分发事件需要 DPDK 21.11 的内部符号，仅借用不透明指针，不依赖内部结构体布局
 */
struct rte_eth_dev;
/** 保存真实回调及其上下文，用代理延长回调时间，验证注销与清理的先后关系 */
struct callback_entry {
    rte_eth_dev_cb_fn function;
    void *context;
};

/** 端口各有一份记录，所有真实符号只解析一次，避免后台线程并发解析引入测试竞争 */
static struct callback_entry entries[RTE_MAX_ETHPORTS];
static pthread_once_t symbols_once = PTHREAD_ONCE_INIT;
static int (*register_real)(uint16_t, enum rte_eth_event_type, rte_eth_dev_cb_fn, void *);
static int (*unregister_real)(uint16_t, enum rte_eth_event_type, rte_eth_dev_cb_fn, void *);
static int (*removed_real)(uint16_t);
static int (*link_real)(uint16_t, struct rte_eth_link *);
static struct rte_eth_dev *(*allocated_real)(const char *);
static int (*process_real)(struct rte_eth_dev *, enum rte_eth_event_type, void *);
/** 测试线程只负责等触发文件并分发事件，端口关闭前必须通知并等待它返回 */
static pthread_t event_thread;
static bool thread_started;
static atomic_bool thread_stop;

/** 用 memcpy 保存 dlsym 结果并检查指针大小，缺少所需符号时明确终止测试 */
#define RESOLVE(function, symbol_name) do { \
    void *symbol = dlsym(RTLD_NEXT, symbol_name); \
    _Static_assert(sizeof(function) == sizeof(symbol), "function pointer size"); \
    memcpy(&(function), &symbol, sizeof(function)); \
    if ((function) == NULL) { \
        fprintf(stderr, "[removal-fixture] missing symbol %s\n", symbol_name); \
        _exit(90); \
    } \
} while (0)

/** RTLD_NEXT 找到被本库拦截前的真实 DPDK 实现，避免代理再次调用自己 */
static void resolve_symbols(void)
{
    RESOLVE(register_real, "rte_eth_dev_callback_register");
    RESOLVE(unregister_real, "rte_eth_dev_callback_unregister");
    RESOLVE(removed_real, "rte_eth_dev_is_removed");
    RESOLVE(link_real, "rte_eth_link_get_nowait");
    RESOLVE(allocated_real, "rte_eth_dev_allocated");
    RESOLVE(process_real, "rte_eth_dev_callback_process");
}

/** 环境变量仅选择当前独立测试实例的故障类型，没有指定时不主动注入故障 */
static bool mode_is(const char *mode)
{
    const char *value = getenv("DPPD_TEST_REMOVAL_MODE");

    return value != NULL && strcmp(value, mode) == 0;
}

/** 默认选择第二个端口，也可指定第一个端口以覆盖端口扫描与初始化顺序 */
static uint16_t target_port(void)
{
    const char *value = getenv("DPPD_TEST_REMOVAL_PORT");

    return value == NULL ? 1 : (uint16_t)strtoul(value, NULL, 10);
}

/** 测试脚本在规则保存完成后创建触发文件，避免随机定时导致故障提前发生 */
static bool triggered(void)
{
    const char *path = getenv("DPPD_TEST_REMOVAL_FILE");

    return path != NULL && access(path, F_OK) == 0;
}

/** 被信号打断后继续等待剩余时间，让在途回调窗口和后台轮询间隔保持可控 */
static void pause_ms(long milliseconds)
{
    struct timespec remaining = {.tv_nsec = milliseconds * 1000000L};

    while (nanosleep(&remaining, &remaining) != 0 && errno == EINTR)
        ;
}

/**
 * 先执行生产回调发布移除状态，再让代理保持半秒钟仍在 DPDK 回调分发器中
 * 这段时间真实注销接口会返回 EAGAIN，可证明清理确实等待在途回调结束
 * 启动期间同步分发的场景不延时，专门覆盖注册尚未返回时已经收到事件的情况
 */
static int proxy_callback(uint16_t port_id, enum rte_eth_event_type event,
                           void *context, void *ret_param)
{
    struct callback_entry *entry = context;
    const int rc = entry->function(port_id, event, entry->context, ret_param);

    fprintf(stderr, "[removal-fixture] callback port=%u delivered\n", port_id);
    if (!mode_is("startup-event"))
        pause_ms(500);
    return rc;
}

/**
 * 按端口名字找到真实 ethdev，再调用 DPDK 分发器，而不是绕过分发器直接调用回调
 * 因此注销忙状态来自 DPDK 自己的在途记录，能验证真实的回调生命周期约束
 */
static void dispatch_event(uint16_t port_id)
{
    char name[RTE_ETH_NAME_MAX_LEN];
    struct rte_eth_dev *device;

    if (rte_eth_dev_get_name_by_port(port_id, name) != 0 ||
        (device = allocated_real(name)) == NULL) {
        fprintf(stderr, "[removal-fixture] cannot find port=%u\n", port_id);
        _exit(91);
    }
    process_real(device, RTE_ETH_EVENT_INTR_RMV, NULL);
    fprintf(stderr, "[removal-fixture] dispatch port=%u finished\n", port_id);
}

/** 接到停止通知就返回，不再访问端口；触发后只分发一次，防止重复事件干扰断言 */
static void *wait_for_event(void *unused)
{
    (void)unused;
    while (!atomic_load_explicit(&thread_stop, memory_order_acquire)) {
        if (triggered()) {
            dispatch_event(target_port());
            break;
        }
        pause_ms(10);
    }
    return NULL;
}

/**
 * 仅代理移除事件，其余事件交给真实接口
 * 可模拟注册失败、注册过程中立即移除，以及运行中由独立线程分发移除事件
 */
int rte_eth_dev_callback_register(uint16_t port_id, enum rte_eth_event_type event,
                                  rte_eth_dev_cb_fn function, void *context)
{
    int rc;

    pthread_once(&symbols_once, resolve_symbols);
    if (event != RTE_ETH_EVENT_INTR_RMV || port_id >= RTE_MAX_ETHPORTS)
        return register_real(port_id, event, function, context);
    if (mode_is("register-error") && port_id == target_port())
        return -ENOMEM;
    entries[port_id].function = function;
    entries[port_id].context = context;
    rc = register_real(port_id, event, proxy_callback, &entries[port_id]);
    if (rc != 0)
        return rc;
    fprintf(stderr, "[removal-fixture] register port=%u\n", port_id);
    if (port_id == target_port()) {
        if (mode_is("startup-event")) {
            dispatch_event(port_id);
        } else if (mode_is("callback") || mode_is("callback-no-link")) {
            atomic_init(&thread_stop, false);
            if (pthread_create(&event_thread, NULL, wait_for_event, NULL) != 0)
                _exit(92);
            thread_started = true;
        }
    }
    return 0;
}

/**
 * 把生产回调参数还原成登记时使用的代理参数，再调用真实注销接口
 * 注销成功后等待测试线程结束，保证生产代码随后 close 时没有测试线程仍借用端口
 */
int rte_eth_dev_callback_unregister(uint16_t port_id, enum rte_eth_event_type event,
                                    rte_eth_dev_cb_fn function, void *context)
{
    int rc;

    pthread_once(&symbols_once, resolve_symbols);
    if (event != RTE_ETH_EVENT_INTR_RMV || port_id >= RTE_MAX_ETHPORTS ||
        entries[port_id].function != function || entries[port_id].context != context)
        return unregister_real(port_id, event, function, context);
    rc = unregister_real(port_id, event, proxy_callback, &entries[port_id]);
    if (rc == 0 && port_id == target_port() && thread_started) {
        atomic_store_explicit(&thread_stop, true, memory_order_release);
        pthread_join(event_thread, NULL);
        thread_started = false;
    }
    if (rc == -EAGAIN)
        fprintf(stderr, "[removal-fixture] unregister port=%u busy\n", port_id);
    else
        fprintf(stderr, "[removal-fixture] unregister port=%u rc=%d\n", port_id, rc);
    return rc;
}

/** 查询故障模式只改变目标端口的移除返回值，用于证明无需回调也能检测移除 */
int rte_eth_dev_is_removed(uint16_t port_id)
{
    pthread_once(&symbols_once, resolve_symbols);
    if (port_id == target_port() && triggered() &&
        (mode_is("probe") || mode_is("probe-no-link")))
        return 1;
    return removed_real(port_id);
}

/** 初始返回 ENOTSUP，验证链路查询不可用时仍会独立处理移除事件或移除查询 */
int rte_eth_link_get_nowait(uint16_t port_id, struct rte_eth_link *link)
{
    pthread_once(&symbols_once, resolve_symbols);
    if (port_id == target_port() && (mode_is("probe-no-link") || mode_is("callback-no-link")))
        return -ENOTSUP;
    return link_real(port_id, link);
}
