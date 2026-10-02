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

struct rte_eth_dev;
struct callback_entry {
    rte_eth_dev_cb_fn function;
    void *context;
};

static struct callback_entry entries[RTE_MAX_ETHPORTS];
static pthread_once_t symbols_once = PTHREAD_ONCE_INIT;
static int (*register_real)(uint16_t, enum rte_eth_event_type, rte_eth_dev_cb_fn, void *);
static int (*unregister_real)(uint16_t, enum rte_eth_event_type, rte_eth_dev_cb_fn, void *);
static int (*removed_real)(uint16_t);
static int (*link_real)(uint16_t, struct rte_eth_link *);
static struct rte_eth_dev *(*allocated_real)(const char *);
static int (*process_real)(struct rte_eth_dev *, enum rte_eth_event_type, void *);
static pthread_t event_thread;
static bool thread_started;
static atomic_bool thread_stop;

#define RESOLVE(function, symbol_name) do { \
    void *symbol = dlsym(RTLD_NEXT, symbol_name); \
    _Static_assert(sizeof(function) == sizeof(symbol), "function pointer size"); \
    memcpy(&(function), &symbol, sizeof(function)); \
    if ((function) == NULL) { \
        fprintf(stderr, "[removal-fixture] missing symbol %s\n", symbol_name); \
        _exit(90); \
    } \
} while (0)

static void resolve_symbols(void)
{
    RESOLVE(register_real, "rte_eth_dev_callback_register");
    RESOLVE(unregister_real, "rte_eth_dev_callback_unregister");
    RESOLVE(removed_real, "rte_eth_dev_is_removed");
    RESOLVE(link_real, "rte_eth_link_get_nowait");
    RESOLVE(allocated_real, "rte_eth_dev_allocated");
    RESOLVE(process_real, "rte_eth_dev_callback_process");
}

static bool mode_is(const char *mode)
{
    const char *value = getenv("DPPD_TEST_REMOVAL_MODE");

    return value != NULL && strcmp(value, mode) == 0;
}

static uint16_t target_port(void)
{
    const char *value = getenv("DPPD_TEST_REMOVAL_PORT");

    return value == NULL ? 1 : (uint16_t)strtoul(value, NULL, 10);
}

static bool triggered(void)
{
    const char *path = getenv("DPPD_TEST_REMOVAL_FILE");

    return path != NULL && access(path, F_OK) == 0;
}

static void pause_ms(long milliseconds)
{
    struct timespec remaining = {.tv_nsec = milliseconds * 1000000L};

    while (nanosleep(&remaining, &remaining) != 0 && errno == EINTR)
        ;
}

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

int rte_eth_dev_is_removed(uint16_t port_id)
{
    pthread_once(&symbols_once, resolve_symbols);
    if (port_id == target_port() && triggered() &&
        (mode_is("probe") || mode_is("probe-no-link")))
        return 1;
    return removed_real(port_id);
}

int rte_eth_link_get_nowait(uint16_t port_id, struct rte_eth_link *link)
{
    pthread_once(&symbols_once, resolve_symbols);
    if (port_id == target_port() && (mode_is("probe-no-link") || mode_is("callback-no-link")))
        return -ENOTSUP;
    return link_real(port_id, link);
}
