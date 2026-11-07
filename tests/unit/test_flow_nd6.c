#define _GNU_SOURCE
#include "flow_state_fixture.h"
#include "dppd/flow_request.h"
#include "../../lib/control/flow_nd6_io.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static unsigned int fail_file, fail_directory;
int __real_fsync(int fd);
int __wrap_fsync(int fd);

/** 真正写临时文件，只在指定同步点返回错误，区分受理失败与发布后的不确定保存 */
int __wrap_fsync(int fd)
{
    struct stat st;
    assert(fstat(fd, &st) == 0);
    unsigned int *remaining = S_ISDIR(st.st_mode) ? &fail_directory : &fail_file;
    if (*remaining != 0 && --*remaining == 0) {
        errno = EIO;
        return -1;
    }
    return __real_fsync(fd);
}

static const struct dppd_routing_binding bindings[] = {
    {.port_id = 7, .device = "net_tap7", .driver = "net_tap", .ifname = "nd7"},
    {.port_id = 9, .device = "net_tap9", .driver = "net_tap", .ifname = "nd9"},
};
static const struct dppd_nd6_port ports[] = {
    {.port_id = 7, .mac = {2, 0, 0, 0, 0, 7}}, {.port_id = 9, .mac = {2, 0, 0, 0, 0, 9}},
};

static unsigned int file_version(const char *path)
{
    uint8_t bytes[16];
    int fd = open(path, O_RDONLY);
    assert(fd >= 0 && read(fd, bytes, sizeof(bytes)) == (ssize_t)sizeof(bytes));
    assert(close(fd) == 0);
    return bytes[8];
}

static void bind_nd6(struct dppd_flow_store *store)
{
    assert(dppd_flow_store_nd6_bind(store, ports, 2, dppd_monotonic_ms()) == 0);
}

static struct dppd_flow_store *fresh(const char *path, bool routed)
{
    struct dppd_flow_state *state = flow_state_fixture(0);
    struct dppd_flow_store *store = NULL;
    assert(dppd_flow_state_save(path, state) == 0);
    dppd_flow_state_free(&state);
    assert(dppd_flow_store_open(path, NULL, &store) == 0);
    if (routed) {
        assert(dppd_flow_store_routing_bind(store, bindings, 2) == 0);
        bind_nd6(store);
    }
    return store;
}

static struct dppd_flow_request request_for(struct dppd_flow_store *store, uint64_t sequence)
{
    struct dppd_flow_nd6_view view;
    assert(dppd_flow_store_nd6_get(store, &view) == 0);
    struct dppd_flow_request r = {.key = {703, sequence}, .operation = DPPD_FLOW_REQUEST_ND6_REPLACE,
        .count = 1, .expected_revision = view.revision};
    r.payload.nd6.expected_neighbor_epoch = view.neighbor_epoch;
    r.payload.nd6.expected_neighbor_revision = view.neighbor_revision;
    r.payload.nd6.policy = (struct dppd_nd6_policy){.local_count = 2, .reachable_ms = 30000};
    for (uint32_t i = 0; i < 2; ++i) {
        r.payload.nd6.policy.locals[i].port_id = ports[i].port_id;
        assert(inet_pton(AF_INET6, "fe80::1", r.payload.nd6.policy.locals[i].address) == 1);
    }
    return r;
}

static struct dppd_flow_request_result apply(struct dppd_flow_store *store, const struct dppd_flow_request *r, int error)
{
    struct dppd_flow_request_result result;
    assert(dppd_flow_store_request(store, r, 1, &result) == 0);
    assert(result.status == error && result.applied == (error == 0) && result.durable &&
        result.phase == DPPD_FLOW_REQUEST_DONE);
    return result;
}

static int send_nd(void *context, uint16_t port, const uint8_t *wire, uint16_t length)
{
    (void)context;
    assert((port == 7 || port == 9) && wire != NULL && length >= 78);
    return 0;
}

/** 用真实状态机完成 DAD，再创建未解析邻居，不能仅向内部数组填一条假记录 */
static uint64_t create_neighbor(struct dppd_flow_store *store)
{
    static uint64_t previous;
    struct dppd_nd6 *table = dppd_flow_store_nd6(store);
    uint64_t now = dppd_monotonic_ms();
    if (now < previous + 20000U)
        now = previous + 20000U;
    previous = now;
    dppd_nd6_tick(table, now + 10000, send_nd, NULL);
    dppd_nd6_tick(table, now + 11000, send_nd, NULL);
    assert(dppd_nd6_port_ready(table, 7));
    struct dppd_packet packet = {.l3_type = DPPD_L3_IPV6};
    uint8_t address[16], mac[6];
    assert(inet_pton(AF_INET6, "fe80::99", address) == 1);
    assert(dppd_nd6_resolve(table, 7, address, &packet, now + 11000, mac) == -EAGAIN);
    struct dppd_nd6_page page;
    assert(dppd_flow_store_nd6_list(store, 0, 0, 0, &page) == 0 && page.count == 1);
    return page.entries[0].id;
}

static void create_connection(struct dppd_flow_store *store)
{
    struct dppd_flow_conntrack_view view;
    assert(dppd_flow_store_conntrack_get(store, &view) == 0);
    struct dppd_flow_request r = {.key = {704, 1}, .operation = DPPD_FLOW_REQUEST_CONNTRACK_REPLACE,
        .count = 1, .expected_revision = view.revision};
    r.payload.conntrack.policy = (struct dppd_conntrack_policy){.capacity = 16, .timeout_s = 300};
    r.payload.conntrack.expected_session_epoch = view.session_epoch;
    r.payload.conntrack.expected_session_revision = view.session_revision;
    apply(store, &r, 0);
    struct dppd_packet p = {.l3_type = DPPD_L3_IPV4, .l4_type = DPPD_L4_UDP, .ip_protocol = 17,
        .packet_len = 64, .l2_len = 14, .l3_len = 20, .l4_len = 8, .ipv4_total_len = 28,
        .ipv4_src_be = htonl(0x0a000002), .ipv4_dst_be = htonl(0xc6336402),
        .l4_src_port_be = htons(1234), .l4_dst_port_be = htons(443)};
    struct dppd_connection_attempt attempt;
    assert(dppd_conntrack_begin(dppd_flow_store_conntrack(store), 7, &p, dppd_monotonic_ms(), &attempt) == 0);
    assert(dppd_conntrack_finish(&attempt, 9, true) == 0);
}

static void test_bindings(const char *path)
{
    struct dppd_flow_store *store = fresh(path, false);
    assert(dppd_flow_store_nd6_bind(store, ports, 2, 0) == -ENOTSUP);
    assert(dppd_flow_store_routing_bind(store, bindings, 2) == 0);
    assert(dppd_flow_store_nd6_bind(store, ports, 1, 0) == -ESTALE);
    struct dppd_nd6_port changed[2] = {ports[1], ports[0]};
    changed[1].port_id = 10;
    assert(dppd_flow_store_nd6_bind(store, changed, 2, 0) == -ESTALE);
    changed[1] = ports[0];
    memset(changed[1].mac, 0, 6);
    assert(dppd_flow_store_nd6_bind(store, changed, 2, 0) == -EINVAL);
    bind_nd6(store);
    struct dppd_nd6 *stable = dppd_flow_store_nd6(store);
    changed[1] = ports[0];
    assert(dppd_flow_store_nd6_bind(store, changed, 2, 0) == 0 && stable == dppd_flow_store_nd6(store));
    ++changed[1].mac[5];
    assert(dppd_flow_store_nd6_bind(store, changed, 2, 0) == -ESTALE);
    assert(file_version(path) == 3);
    assert(dppd_flow_store_close(&store) == 0);
}

/** 同键重试不再次清空，三个精确版本分别拒绝过期请求，ND 更新不清理 CT 会话 */
static void test_policy_and_guards(const char *path)
{
    struct dppd_flow_store *store = fresh(path, true);
    create_connection(store);
    assert(file_version(path) == 4);
    struct dppd_nd6 *stable = dppd_flow_store_nd6(store);
    struct dppd_flow_request r = request_for(store, 1);
    struct dppd_flow_request_result first = apply(store, &r, 0);
    assert(first.generations[0] == 1 && file_version(path) == 5);
    uint64_t old_id = create_neighbor(store);
    struct dppd_flow_request duplicate = r;
    duplicate.payload.nd6.policy.locals[31].port_id = 101;
    duplicate.payload.nd6.policy.locals[0].vlan[1] = 4094;
    assert(apply(store, &duplicate, 0).revision == first.revision);
    ++duplicate.payload.nd6.policy.reachable_ms;
    struct dppd_flow_request_result result;
    assert(dppd_flow_store_request(store, &duplicate, 1, &result) == -EEXIST);
    r = request_for(store, 2);
    apply(store, &r, -EBUSY);
    r = request_for(store, 3);
    r.payload.nd6.reset_neighbors = true;
    --r.expected_revision;
    apply(store, &r, -ESTALE);
    r = request_for(store, 4);
    r.payload.nd6.reset_neighbors = true;
    ++r.payload.nd6.expected_neighbor_epoch;
    apply(store, &r, -ESTALE);
    r = request_for(store, 5);
    r.payload.nd6.reset_neighbors = true;
    --r.payload.nd6.expected_neighbor_revision;
    apply(store, &r, -ESTALE);
    r = request_for(store, 6);
    r.payload.nd6.reset_neighbors = true;
    r.payload.nd6.policy.locals[1].port_id = 10;
    apply(store, &r, -ENODEV);
    struct dppd_nd6_status status;
    assert(dppd_flow_store_nd6_status(store, &status) == 0 && status.active == 1);
    r = request_for(store, 7);
    r.payload.nd6.reset_neighbors = true;
    apply(store, &r, 0);
    assert(dppd_flow_store_nd6(store) == stable);
    assert(dppd_flow_store_nd6_status(store, &status) == 0 && status.active == 0);
    assert(status.locals[0].state == DPPD_ND6_TENTATIVE);
    assert(create_neighbor(store) > old_id);
    struct dppd_flow_nd6_view view;
    assert(dppd_flow_store_nd6_get(store, &view) == 0);
    assert(dppd_flow_store_nd6_flush(store, view.neighbor_epoch + 1, view.neighbor_revision) == -ESTALE);
    assert(dppd_flow_store_nd6_flush(store, view.neighbor_epoch, view.neighbor_revision) == 0);
    assert(dppd_nd6_port_ready(stable, 7));
    assert(dppd_flow_store_nd6_flush(store, view.neighbor_epoch, view.neighbor_revision) == -ESTALE);
    struct dppd_conntrack_status connections;
    assert(dppd_flow_store_conntrack_status(store, dppd_monotonic_ms(), &connections) == 0 && connections.active == 1);
    r = request_for(store, 8);
    memset(&r.payload.nd6.policy, 0, sizeof(r.payload.nd6.policy));
    apply(store, &r, 0);
    assert(dppd_flow_store_nd6_status(store, &status) == 0 && !status.enabled);
    r = request_for(store, 9);
    apply(store, &r, 0);
    create_neighbor(store);
    assert(dppd_flow_store_nd6_get(store, &view) == 0);
    assert(dppd_flow_store_close(&store) == 0 && dppd_flow_store_open(path, NULL, &store) == 0);
    bind_nd6(store);
    assert(dppd_flow_store_nd6_status(store, &status) == 0 && status.active == 0 && status.pending == 0);
    assert(status.enabled && status.locals[0].state == DPPD_ND6_TENTATIVE && status.epoch != view.neighbor_epoch);
    assert(dppd_flow_store_nd6_flush(store, view.neighbor_epoch, status.revision) == -ESTALE);
    apply(store, &r, 0);
    r = request_for(store, 10);
    r.payload.nd6.expected_neighbor_epoch = view.neighbor_epoch;
    apply(store, &r, -ESTALE);
    assert(dppd_flow_store_close(&store) == 0);
}

static void test_failures(const char *path)
{
    for (unsigned int phase = 0; phase < 3; ++phase) {
        struct dppd_flow_store *store = fresh(path, true);
        struct dppd_flow_request r = request_for(store, 1);
        if (phase == 2)
            fail_directory = 2;
        else
            fail_file = phase + 1;
        struct dppd_flow_request_result result;
        assert(dppd_flow_store_request(store, &r, 1, &result) == -EUCLEAN);
        assert(result.applied == (phase != 0) && !result.durable);
        struct dppd_flow_store_status status;
        assert(dppd_flow_store_status(store, &status) == 0 && status.dirty && !status.ready);
        struct dppd_flow_nd6_view view;
        assert(dppd_flow_store_nd6_get(store, &view) == 0);
        assert(view.policy.local_count == (phase == 0 ? 0U : 2U));
        assert(dppd_flow_store_nd6_flush(store, view.neighbor_epoch, view.neighbor_revision) == -EUCLEAN);
        assert(dppd_flow_store_close(&store) == -EUCLEAN);
        assert(dppd_flow_store_flush(store) == 0);
        assert(dppd_flow_store_request(store, &r, 1, &result) == 0 && result.durable);
        assert(dppd_flow_store_close(&store) == 0 && dppd_flow_store_open(path, NULL, &store) == 0);
        bind_nd6(store);
        assert(dppd_flow_store_nd6_get(store, &view) == 0 && view.policy.local_count == (phase == 0 ? 0U : 2U));
        assert(dppd_flow_store_close(&store) == 0);
    }
}

/** 最终文件尚未替换就崩溃时，只取消 pending，不重做 ND 替换或恢复旧动态缓存 */
static void test_pending_restart(const char *path)
{
    struct dppd_flow_store *store = fresh(path, true);
    assert(dppd_flow_store_close(&store) == 0);
    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        assert(dppd_flow_store_open(path, NULL, &store) == 0);
        bind_nd6(store);
        struct dppd_flow_request r = request_for(store, 1);
        struct dppd_flow_request_result result;
        fail_file = 2;
        assert(dppd_flow_store_request(store, &r, 1, &result) == -EUCLEAN && result.applied);
        _exit(0);
    }
    int status;
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    assert(dppd_flow_store_open(path, NULL, &store) == 0);
    bind_nd6(store);
    struct dppd_flow_request_result result;
    assert(dppd_flow_store_request_get(store, (struct dppd_flow_request_key){703, 1}, &result) == 0);
    assert(result.status == -ECANCELED && result.phase == DPPD_FLOW_REQUEST_DONE && !result.applied && result.durable);
    struct dppd_flow_nd6_view view;
    assert(dppd_flow_store_nd6_get(store, &view) == 0 && view.policy.local_count == 0 && view.generation == 0);
    assert(dppd_flow_store_close(&store) == 0);
}

/** 满额地址含双层 VLAN 可完整往返，坏编码和未绑定端口均明确拒绝 */
static void test_full_policy(const char *path)
{
    struct dppd_flow_store *store = fresh(path, true);
    struct dppd_flow_request r = request_for(store, 1);
    struct dppd_nd6_policy *p = &r.payload.nd6.policy;
    p->local_count = DPPD_ND6_LOCAL_LIMIT;
    for (uint32_t i = 0; i < p->local_count; ++i) {
        p->locals[i] = (struct dppd_nd6_local){.port_id = ports[i % 2].port_id,
            .vlan_depth = 2, .vlan = {100, 200}};
        assert(inet_pton(AF_INET6, "2001:db8::", p->locals[i].address) == 1);
        p->locals[i].address[15] = i + 1;
    }
    uint8_t encoded[DPPD_FLOW_ND6_BYTES + 1];
    size_t length;
    struct dppd_nd6_policy decoded = {0};
    assert(dppd_flow_nd6_encode(p, encoded, sizeof(encoded), &length) == 0);
    assert(dppd_flow_nd6_decode(encoded, length, &decoded) == 0 && memcmp(p, &decoded, sizeof(*p)) == 0);
    encoded[4] = 1;
    assert(dppd_flow_nd6_decode(encoded, length, &decoded) == -EBADMSG);
    encoded[4] = 0;
    encoded[24] = 3;
    assert(dppd_flow_nd6_decode(encoded, length, &decoded) == -EBADMSG);
    encoded[24] = 2;
    assert(dppd_flow_nd6_decode(encoded, length - 1U, &decoded) == -EBADMSG);
    encoded[length] = 0;
    assert(dppd_flow_nd6_decode(encoded, length + 1U, &decoded) != 0);
    apply(store, &r, 0);
    assert(dppd_flow_store_close(&store) == 0);
    struct dppd_flow_state *disk = NULL;
    assert(dppd_flow_state_load(path, &disk) == 0 && disk->nd6 != NULL && disk->conntrack == NULL);
    assert(memcmp(&disk->nd6->policy, p, sizeof(*p)) == 0);
    disk->nd6->policy.locals[31].port_id = 12;
    assert(dppd_flow_state_validate(disk) == -ENODEV);
    dppd_flow_state_free(&disk);
    store = fresh(path, false);
    struct stat before, after;
    assert(stat(path, &before) == 0);
    struct dppd_flow_request_result result;
    assert(dppd_flow_store_request(store, &r, 1, &result) == -ENOTSUP);
    assert(stat(path, &after) == 0 && before.st_ino == after.st_ino && before.st_size == after.st_size);
    assert(dppd_flow_store_close(&store) == 0);
}

int main(void)
{
    char directory[] = "/tmp/dppd-flow-nd6-XXXXXX", path[160], lock[170];
    assert(mkdtemp(directory) != NULL);
    snprintf(path, sizeof(path), "%s/state", directory);
    snprintf(lock, sizeof(lock), "%s.lock", path);
    test_bindings(path);
    test_policy_and_guards(path);
    test_failures(path);
    test_pending_restart(path);
    test_full_policy(path);
    assert(unlink(path) == 0 && unlink(lock) == 0 && rmdir(directory) == 0);
    return 0;
}
