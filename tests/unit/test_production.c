#define _GNU_SOURCE
#include "dppd/production.h"
#include "dppd/flow_management.h"
#include "dppd/runtime.h"
#include "flow_state_fixture.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <rte_telemetry.h>
#include <rte_version.h>

static telemetry_cb callbacks[4];
static unsigned int registered, attempts;
static bool fail_sync, fail_field;
static struct {
    uint64_t revision, hits, bytes, rx_packets, generation, shared_id;
    int ready, snapshot_error, count_available, socket, stalled, shared;
} observed;

int __wrap_rte_telemetry_register_cmd(const char *name, telemetry_cb callback, const char *help);
int __wrap_rte_telemetry_register_cmd(const char *name, telemetry_cb callback, const char *help)
{
    static const char *const names[] = {"/dppd/managed/status", "/dppd/managed/stats", "/dppd/managed/rule",
        "/dppd/managed/worker"};
    assert(registered < 4 && strcmp(name, names[registered]) == 0 && help != NULL);
    /** 首次第二条命令失败，重试必须从未成功的一条继续，不重复注册已有命令 */
    if (attempts++ == 1)
        return -ENOMEM;
    callbacks[registered++] = callback;
    return 0;
}

#if RTE_VERSION >= RTE_VERSION_NUM(23, 3, 0, 0)
#define __wrap_rte_tel_data_add_dict_u64 __wrap_rte_tel_data_add_dict_uint
#define __real_rte_tel_data_add_dict_u64 __real_rte_tel_data_add_dict_uint
typedef int64_t telemetry_int;
#else
typedef int telemetry_int;
#endif

int __real_rte_tel_data_add_dict_u64(struct rte_tel_data *, const char *, uint64_t);
int __wrap_rte_tel_data_add_dict_u64(struct rte_tel_data *, const char *, uint64_t);
int __wrap_rte_tel_data_add_dict_u64(struct rte_tel_data *data, const char *key, uint64_t value)
{
    if (fail_field)
        return -E2BIG;
#define CAPTURE(field)            \
    if (strcmp(key, #field) == 0) \
        observed.field = value;
    CAPTURE(revision)
    CAPTURE(hits)
    CAPTURE(bytes)
    CAPTURE(rx_packets)
    CAPTURE(generation)
    CAPTURE(shared_id)
#undef CAPTURE
    return __real_rte_tel_data_add_dict_u64(data, key, value);
}

int __real_rte_tel_data_add_dict_int(struct rte_tel_data *, const char *, telemetry_int);
int __wrap_rte_tel_data_add_dict_int(struct rte_tel_data *, const char *, telemetry_int);
int __wrap_rte_tel_data_add_dict_int(struct rte_tel_data *data, const char *key, telemetry_int value)
{
#define CAPTURE(field)            \
    if (strcmp(key, #field) == 0) \
        observed.field = (int)value;
    CAPTURE(ready)
    CAPTURE(snapshot_error)
    CAPTURE(count_available)
    CAPTURE(socket)
    CAPTURE(stalled)
    CAPTURE(shared)
#undef CAPTURE
    return __real_rte_tel_data_add_dict_int(data, key, value);
}

int __real_fsync(int fd);
int __wrap_fsync(int fd);
int __wrap_fsync(int fd)
{
    if (fail_sync) {
        errno = EIO;
        return -1;
    }
    return __real_fsync(fd);
}

static int query(unsigned int command, const char *parameters)
{
    struct rte_tel_data *data = rte_tel_data_alloc();
    assert(data != NULL);
    memset(&observed, 0, sizeof(observed));
    int rc = callbacks[command](NULL, parameters, data);
    rte_tel_data_free(data);
    return rc;
}

/** 用真实管理状态、原子统计和软件表提供数据，仅替换 telemetry 注册与字段采集边界 */
static void prepare(struct dppd_runtime *runtime)
{
    memset(runtime, 0, sizeof(*runtime));
    runtime->nb_workers = 2;
    runtime->workers_started = true;
    runtime->config.worker_timeout_ms = 5000;
    runtime->devices.nb_ports = 2;
    atomic_init(&runtime->stop_requested, false);
    atomic_init(&runtime->devices.removal_requested, false);
    atomic_init(&runtime->devices.event_tracking_enabled, false);
    for (uint16_t i = 0; i < 2; ++i) {
        struct dppd_worker *worker = &runtime->workers[i];
        worker->queue_id = i;
        worker->lcore_id = i + 4U;
        worker->socket_id = i;
        atomic_init(&worker->state, DPPD_WORKER_RUNNING);
        atomic_init(&worker->heartbeat_ms, dppd_monotonic_ms());
        dppd_stats_init(&worker->stats);
        for (uint16_t port = 0; port < 2; ++port)
            dppd_stats_init(&worker->port_stats[port]);
        runtime->devices.ports[i].port_id = 7U + i;
        runtime->devices.ports[i].configured = runtime->devices.ports[i].started = true;
        atomic_init(&runtime->devices.ports[i].removed, false);
        atomic_init(&runtime->devices.ports[i].link_state, DPPD_LINK_UP);
    }
}

static void heartbeats(struct dppd_runtime *runtime)
{
    atomic_store(&runtime->workers[0].heartbeat_ms, 1000);
    assert(!dppd_runtime_worker_stalled(runtime, 0, 6000));
    assert(dppd_runtime_worker_stalled(runtime, 0, 6001));
    /** 心跳读到较晚的采样值可能只是正常并发更新，不能用无符号减法误判成超时 */
    assert(!dppd_runtime_worker_stalled(runtime, 0, 999));
    atomic_store(&runtime->stop_requested, true);
    assert(!dppd_runtime_worker_stalled(runtime, 0, 7000));
    atomic_store(&runtime->stop_requested, false);
    runtime->workers_started = false;
    assert(!dppd_runtime_worker_stalled(runtime, 0, 7000));
    runtime->workers_started = true;
    atomic_store(&runtime->devices.removal_requested, true);
    assert(!dppd_runtime_worker_stalled(runtime, 0, 7000));
    atomic_store(&runtime->devices.removal_requested, false);
    runtime->config.worker_timeout_ms = 0;
    assert(!dppd_runtime_worker_stalled(runtime, 0, 7000));
    runtime->config.worker_timeout_ms = 5000;
    assert(dppd_runtime_check_workers(runtime) == -ETIMEDOUT);
    atomic_store(&runtime->workers[0].heartbeat_ms, dppd_monotonic_ms());
    assert(dppd_runtime_check_workers(runtime) == 0);
}

/** 使用真实满管道与 socket，子进程的闹钟保证日志回归为阻塞时能明确失败 */
static void logging_backpressure(void)
{
    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        alarm(3);
        signal(SIGPIPE, SIG_DFL);
        sigset_t signals, previous, current, pending;
        sigemptyset(&signals);
        sigaddset(&signals, SIGPIPE);
        assert(pthread_sigmask(SIG_UNBLOCK, &signals, &previous) == 0);
        uint64_t dropped = dppd_production_log_dropped();
        for (unsigned int kind = 0; kind < 2; ++kind) {
            int endpoints[2];
            if (kind == 0)
                assert(pipe2(endpoints, O_NONBLOCK | O_CLOEXEC) == 0);
            else
                assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, endpoints) == 0);
            char buffer[512] = {0};
            while (write(endpoints[1], buffer, sizeof(buffer)) > 0)
                ;
            assert(errno == EAGAIN || errno == EWOULDBLOCK);
            int flags = fcntl(endpoints[1], F_GETFL);
            assert(flags >= 0 && fcntl(endpoints[1], F_SETFL, flags & ~O_NONBLOCK) == 0);
            assert(dup2(endpoints[1], STDERR_FILENO) == STDERR_FILENO);
            errno = EDOM;
            dppd_production_log(DPPD_PRODUCTION_WORKER_TIMEOUT, -ETIMEDOUT, 1);
            assert(errno == EDOM && dppd_production_log_dropped() == ++dropped);
            /** 独立非阻塞写不能给继承同一描述符的父进程悄悄打开 O_NONBLOCK */
            assert(fcntl(endpoints[1], F_GETFL) == (flags & ~O_NONBLOCK));
            assert(close(endpoints[0]) == 0);
            dppd_production_log(DPPD_PRODUCTION_WORKER_TIMEOUT, -ETIMEDOUT, 1);
            assert(dppd_production_log_dropped() == ++dropped);
            assert(pthread_sigmask(SIG_SETMASK, NULL, &current) == 0 && !sigismember(&current, SIGPIPE));
            assert(sigpending(&pending) == 0 && !sigismember(&pending, SIGPIPE));
            assert(close(endpoints[1]) == 0);
        }
        /** 普通文件连续日志必须沿同一偏移追加，不能因重新打开 stderr 而覆盖上一条 */
        FILE *file = tmpfile();
        assert(file != NULL && dup2(fileno(file), STDERR_FILENO) == STDERR_FILENO);
        dppd_production_log(DPPD_PRODUCTION_START, 0, 1);
        off_t first = lseek(fileno(file), 0, SEEK_CUR);
        assert(first > 0);
        dppd_production_log(DPPD_PRODUCTION_READY, 0, 1);
        assert(lseek(fileno(file), 0, SEEK_CUR) > first && dppd_production_log_dropped() == dropped);
        assert(fclose(file) == 0);
        _Exit(EXIT_SUCCESS);
    }
    int status;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == EXIT_SUCCESS);
}

int main(void)
{
    logging_backpressure();
    char directory[] = "/tmp/dppd-production-XXXXXX", path[256], lock[272];
    assert(mkdtemp(directory) != NULL);
    snprintf(path, sizeof(path), "%s/state", directory);
    snprintf(lock, sizeof(lock), "%s.lock", path);
    struct dppd_flow_state *state = flow_state_fixture(2);
    struct dppd_flow_store *store = NULL;
    assert(dppd_flow_state_save(path, state) == 0 && dppd_flow_store_open(path, NULL, &store) == 0);
    struct dppd_runtime runtime;
    prepare(&runtime);
    heartbeats(&runtime);
    struct dppd_flow_management flows;
    assert(dppd_flow_management_init(&flows, store) == 0);
    struct dppd_production *production = NULL, *other = NULL;
    assert(dppd_production_start(&runtime, &flows, &production) == -ENOMEM && production == NULL);
    assert(query(0, "") == -EAGAIN);
    assert(dppd_production_start(&runtime, &flows, &production) == 0 && registered == 4);
    assert(dppd_production_start(&runtime, &flows, &other) == -EALREADY && other == NULL);
    assert(query(0, "") == 0 && observed.ready && observed.revision == 111);
    assert(query(0, "1") == -EINVAL);
    struct dppd_stats_values stats = {.rx_packets = 9, .rx_bytes = 576};
    dppd_stats_add(&runtime.workers[1].port_stats[0], &stats);
    assert(query(1, "7,1") == 0 && observed.rx_packets == 9);
    assert(query(1, "65535,65535") == 0 && observed.rx_packets == 9);
    assert(query(1, "7,0") == 0 && observed.rx_packets == 0);
    assert(query(1, "7") == -EINVAL && query(1, "7,") == -EINVAL && query(1, "7,0x1") == -EINVAL);
    assert(query(1, "7,2") == -ENOENT && query(1, "65536,1") == -EINVAL);
    assert(query(2, "18446744073709551616") == -EINVAL && query(2, "9") == -ENOENT);
    assert(query(2, "1") == -EAGAIN);
    assert(dppd_production_poll(production, dppd_monotonic_ms()) == 0);
    assert(query(2, "1") == 0 && observed.count_available && observed.hits == 0 && observed.generation == 3);
    struct dppd_software_backend *software = dppd_flow_store_software(store);
    assert(dppd_software_backend_worker_register(software, 0) == 0);
    struct dppd_packet packet = {.packet_len = 64};
    struct dppd_software_decision decision;
    for (unsigned int i = 0; i < 3; ++i)
        dppd_software_backend_decide(software, 7, &packet, &decision);
    dppd_software_backend_worker_quiescent(software, 0);
    dppd_software_backend_worker_unregister(software, 0);
    assert(dppd_production_poll(production, dppd_monotonic_ms()) == 0);
    assert(query(2, "1") == 0 && observed.hits == 3 && observed.bytes == 192 && !observed.shared);
    assert(query(3, "1") == 0 && observed.socket == 1 && !observed.stalled);
    assert(query(3, "2") == -ENOENT && query(3, "-1") == -EINVAL);
    fail_field = true;
    assert(query(0, "") == -E2BIG);
    fail_field = false;
    /** 写盘失败后旧副本不能继续伪装成当前配置，修复后才重新发布可读版本 */
    struct dppd_flow_config_change change = {.operation = DPPD_FLOW_CONFIG_TEMPLATE_ADD, .key = 200};
    change.spec.table = state->tables[0].spec;
    struct dppd_flow_config_result result;
    fail_sync = true;
    assert(dppd_flow_store_configure(store, 111, &change, 1, &result) == -EUCLEAN && result.applied);
    fail_sync = false;
    assert(dppd_production_poll(production, dppd_monotonic_ms()) == 0);
    assert(query(0, "") == 0 && !observed.ready && observed.snapshot_error == -EAGAIN);
    assert(query(2, "1") == -EAGAIN);
    assert(dppd_flow_store_flush(store) == 0);
    assert(dppd_production_poll(production, dppd_monotonic_ms()) == 0);
    assert(query(0, "") == 0 && observed.ready && observed.revision == 112);
    dppd_production_stop(&production);
    assert(production == NULL && query(0, "") == -EAGAIN && query(1, "") == -EAGAIN && query(2, "1") == -EAGAIN);
    assert(dppd_flow_store_close(&store) == 0);
    dppd_flow_state_free(&state);
    /** 共享累计保留独立身份与范围，查询两条规则不能误报成两个独立计数器 */
    state = flow_state_fixture(2);
    state->shared_count = 1;
    state->counts[0] =
        (struct dppd_shared_count_spec){.id = 99, .generation = 20, .install_port_id = 7, .domain = DPPD_RULE_DOMAIN_INGRESS};
    state->tables[0].spec.example.actions[1] =
        (struct dppd_action){.type = DPPD_ACTION_SHARED_COUNT, .conf.shared_count = {99, 20}};
    for (uint32_t i = 0; i < 2; ++i)
        state->rules[i].rule.actions[1] = state->tables[0].spec.example.actions[1];
    assert(dppd_flow_state_save(path, state) == 0 && dppd_flow_store_open(path, NULL, &store) == 0);
    assert(dppd_flow_management_init(&flows, store) == 0);
    assert(dppd_production_start(&runtime, &flows, &production) == 0 && registered == 4);
    assert(query(2, "1") == -EAGAIN && query(2, "2") == -EAGAIN);
    software = dppd_flow_store_software(store);
    assert(dppd_software_backend_worker_register(software, 0) == 0);
    for (unsigned int i = 0; i < 3; ++i)
        dppd_software_backend_decide(software, 7, &packet, &decision);
    dppd_software_backend_worker_quiescent(software, 0);
    dppd_software_backend_worker_unregister(software, 0);
    assert(dppd_production_poll(production, dppd_monotonic_ms()) == 0);
    assert(query(2, "1") == 0 && observed.shared && observed.shared_id == 99 && observed.hits == 3);
    assert(query(2, "2") == 0 && observed.shared && observed.shared_id == 99 && observed.hits == 3);
    dppd_production_stop(&production);
    assert(dppd_flow_store_close(&store) == 0);
    dppd_flow_state_free(&state);
    assert(unlink(path) == 0 && unlink(lock) == 0 && rmdir(directory) == 0);
    return 0;
}
