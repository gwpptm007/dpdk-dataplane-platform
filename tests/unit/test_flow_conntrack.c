#define _GNU_SOURCE
#include "flow_state_fixture.h"
#include "dppd/flow_request.h"
#include "../../lib/control/flow_conntrack_io.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static unsigned int fail_file, fail_directory;
int __real_fsync(int fd);
int __wrap_fsync(int fd);

/** 分别让受理、最终配置或目录同步失败，观察真实文件是否仍能恢复成明确状态 */
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
    {.port_id = 7, .device = "net_tap7", .driver = "net_tap", .ifname = "nat7"},
    {.port_id = 9, .device = "net_tap9", .driver = "net_tap", .ifname = "nat9"},
};

static uint32_t ip(const char *text)
{
    uint32_t result;
    assert(inet_pton(AF_INET, text, &result) == 1);
    return result;
}

static uint64_t milliseconds(void)
{
    struct timespec t;
    assert(clock_gettime(CLOCK_MONOTONIC, &t) == 0);
    return (uint64_t)t.tv_sec * 1000U + (uint64_t)t.tv_nsec / 1000000U;
}

static struct dppd_flow_store *fresh(const char *path, bool routed)
{
    struct dppd_flow_state *state = flow_state_fixture(0);
    struct dppd_flow_store *store = NULL;
    assert(dppd_flow_state_save(path, state) == 0);
    dppd_flow_state_free(&state);
    assert(dppd_flow_store_open(path, NULL, &store) == 0);
    if (routed)
        assert(dppd_flow_store_routing_bind(store, bindings, 2) == 0);
    return store;
}

static struct dppd_flow_request request_for(struct dppd_flow_store *store, uint64_t sequence)
{
    struct dppd_flow_conntrack_view view;
    assert(dppd_flow_store_conntrack_get(store, &view) == 0);
    struct dppd_flow_request r = {.key = {702, sequence}, .operation = DPPD_FLOW_REQUEST_CONNTRACK_REPLACE,
        .count = 1, .expected_revision = view.revision};
    r.payload.conntrack.expected_session_epoch = view.session_epoch;
    r.payload.conntrack.expected_session_revision = view.session_revision;
    r.payload.conntrack.policy = (struct dppd_conntrack_policy){.capacity = 16, .timeout_s = 300,
        .snat = {.enabled = true, .inside_port = 7, .outside_port = 9, .first_port = 40000, .last_port = 40010,
            .address_be = ip("203.0.113.7")},
        .snat6 = {.enabled = true, .ipv6 = true, .inside_port = 7, .outside_port = 9,
            .first_port = 40000, .last_port = 40010}};
    assert(inet_pton(AF_INET6, "2001:db8:ffff::7", r.payload.conntrack.policy.snat6.address6) == 1);
    return r;
}

static struct dppd_flow_request_result apply(struct dppd_flow_store *store, const struct dppd_flow_request *request,
    int expected)
{
    struct dppd_flow_request_result result;
    assert(dppd_flow_store_request(store, request, 1, &result) == 0);
    assert(result.phase == DPPD_FLOW_REQUEST_DONE && result.status == expected && result.durable);
    assert(result.applied == (expected == 0));
    return result;
}

static uint64_t create_session(struct dppd_flow_store *store)
{
    struct dppd_packet p = {.packet_len = 64, .l2_len = 14, .l3_len = 20, .l4_len = 8,
        .ipv4_total_len = 28, .l3_type = DPPD_L3_IPV4, .l4_type = DPPD_L4_UDP, .ip_protocol = 17,
        .ipv4_src_be = ip("10.0.0.2"), .ipv4_dst_be = ip("198.51.100.2"),
        .l4_src_port_be = htons(1234), .l4_dst_port_be = htons(443)};
    struct dppd_connection_attempt attempt;
    assert(dppd_conntrack_begin(dppd_flow_store_conntrack(store), 7, &p, milliseconds(), &attempt) == 0);
    assert(attempt.nat && attempt.rewrite.source_be == ip("203.0.113.7"));
    assert(dppd_conntrack_finish(&attempt, 9, true) == 0);
    struct dppd_connection_page page;
    assert(dppd_flow_store_conntrack_list(store, milliseconds(), 0, 0, 0, &page) == 0 && page.count == 1);
    return page.entries[0].id;
}

/** 策略版本、会话版本和启动身份各管一层，默认更新和过期清空都不能伤到现有映射 */
static void test_sessions(const char *path)
{
    struct dppd_flow_store *store = fresh(path, true);
    struct dppd_conntrack *stable = dppd_flow_store_conntrack(store);
    struct dppd_flow_request r = request_for(store, 1);
    struct dppd_flow_request_result first = apply(store, &r, 0);
    assert(first.revision == 93 && first.generations[0] == 1);
    uint64_t first_id = create_session(store);
    struct dppd_flow_request duplicate = r;
    duplicate.payload.conntrack.policy.dnat[31].public_first = 123;
    struct dppd_flow_request_result replay = apply(store, &duplicate, 0);
    assert(replay.revision == first.revision);
    duplicate.payload.conntrack.policy.timeout_s++;
    assert(dppd_flow_store_request(store, &duplicate, 1, &replay) == -EEXIST);
    r = request_for(store, 2);
    r.payload.conntrack.policy.timeout_s++;
    apply(store, &r, -EBUSY);
    r = request_for(store, 3);
    r.payload.conntrack.reset_sessions = true;
    --r.payload.conntrack.expected_session_revision;
    apply(store, &r, -ESTALE);
    r = request_for(store, 4);
    r.payload.conntrack.reset_sessions = true;
    ++r.payload.conntrack.expected_session_epoch;
    apply(store, &r, -ESTALE);
    r = request_for(store, 5);
    r.payload.conntrack.reset_sessions = true;
    --r.expected_revision;
    apply(store, &r, -ESTALE);
    r = request_for(store, 6);
    r.payload.conntrack.reset_sessions = true;
    r.payload.conntrack.policy.snat.outside_port = 11;
    apply(store, &r, -ENODEV);
    struct dppd_conntrack_status status;
    assert(dppd_flow_store_conntrack_status(store, milliseconds(), &status) == 0 && status.active == 1);
    r = request_for(store, 7);
    r.payload.conntrack.reset_sessions = true;
    apply(store, &r, 0);
    assert(stable == dppd_flow_store_conntrack(store));
    assert(dppd_flow_store_conntrack_status(store, milliseconds(), &status) == 0);
    assert(status.active == 0 && status.flushed == 1);
    assert(create_session(store) > first_id);
    struct dppd_flow_conntrack_view view;
    assert(dppd_flow_store_conntrack_get(store, &view) == 0);
    assert(dppd_flow_store_conntrack_flush(store, 0, view.session_revision) == -EINVAL);
    assert(dppd_flow_store_conntrack_flush(store, view.session_epoch + 1, view.session_revision) == -ESTALE);
    assert(dppd_flow_store_conntrack_flush(store, view.session_epoch, view.session_revision) == 0);
    assert(dppd_flow_store_conntrack_flush(store, view.session_epoch, view.session_revision) == -ESTALE);
    r = request_for(store, 8);
    memset(&r.payload.conntrack.policy, 0, sizeof(r.payload.conntrack.policy));
    apply(store, &r, 0);
    assert(dppd_flow_store_conntrack_status(store, milliseconds(), &status) == 0 && !status.enabled);
    r = request_for(store, 9);
    apply(store, &r, 0);
    create_session(store);
    assert(dppd_flow_store_conntrack_get(store, &view) == 0);
    assert(dppd_flow_store_close(&store) == 0 && dppd_flow_store_open(path, NULL, &store) == 0);
    assert(dppd_flow_store_conntrack_status(store, milliseconds(), &status) == 0);
    assert(status.enabled && status.active == 0 && status.epoch != view.session_epoch);
    assert(dppd_flow_store_conntrack_flush(store, view.session_epoch, status.revision) == -ESTALE);
    apply(store, &r, 0);
    r.key.sequence = 10;
    r.expected_revision = view.revision;
    r.payload.conntrack.expected_session_revision = status.revision;
    r.payload.conntrack.reset_sessions = true;
    apply(store, &r, -ESTALE);
    assert(dppd_flow_store_close(&store) == 0);
}

/** 写盘失败必须封闭写入，修复后才能给可靠回执，受理失败则不能应用候选策略 */
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
        struct dppd_flow_conntrack_view view;
        assert(dppd_flow_store_conntrack_get(store, &view) == 0);
        assert(view.policy.capacity == (phase == 0 ? 0U : 16U));
        assert(dppd_flow_store_conntrack_flush(store, view.session_epoch, view.session_revision) == -EUCLEAN);
        assert(dppd_flow_store_close(&store) == -EUCLEAN);
        assert(dppd_flow_store_flush(store) == 0);
        assert(dppd_flow_store_request(store, &r, 1, &result) == 0 && result.durable);
        assert(result.applied == (phase != 0));
        assert(dppd_flow_store_close(&store) == 0 && dppd_flow_store_open(path, NULL, &store) == 0);
        assert(dppd_flow_store_conntrack_get(store, &view) == 0);
        assert(view.policy.capacity == (phase == 0 ? 0U : 16U));
        assert(dppd_flow_store_close(&store) == 0);
    }
}

/** 模拟最终配置尚未替换就崩溃，磁盘上的 pending 只能中止，不能在重启时补做清空 */
static void test_pending_restart(const char *path)
{
    struct dppd_flow_store *store = fresh(path, true);
    assert(dppd_flow_store_close(&store) == 0);
    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        assert(dppd_flow_store_open(path, NULL, &store) == 0);
        struct dppd_flow_request r = request_for(store, 1);
        struct dppd_flow_request_result result;
        fail_file = 2;
        assert(dppd_flow_store_request(store, &r, 1, &result) == -EUCLEAN && result.applied);
        _exit(0);
    }
    int status;
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    assert(dppd_flow_store_open(path, NULL, &store) == 0);
    struct dppd_flow_request_result result;
    assert(dppd_flow_store_request_get(store, (struct dppd_flow_request_key){702, 1}, &result) == 0);
    assert(result.phase == DPPD_FLOW_REQUEST_DONE && result.status == -ECANCELED && !result.applied && result.durable);
    struct dppd_flow_conntrack_view view;
    assert(dppd_flow_store_conntrack_get(store, &view) == 0 && view.policy.capacity == 0 && view.generation == 0);
    assert(dppd_flow_store_close(&store) == 0);
}

/** 满额双栈端口转发能够原样恢复，旧端口对模式则在受理前拒绝新操作 */
static void test_full_policy(const char *path)
{
    struct dppd_flow_store *store = fresh(path, true);
    struct dppd_flow_request r = request_for(store, 1);
    struct dppd_conntrack_policy *p = &r.payload.conntrack.policy;
    p->dnat_count = DPPD_MAX_DNAT_RULES;
    for (uint32_t i = 0; i < p->dnat_count; ++i) {
        struct dppd_dnat_rule *d = &p->dnat[i];
        *d = (struct dppd_dnat_rule){.inside_port = 7, .outside_port = 9, .protocol = 17,
            .public_first = 8000 + i, .public_last = 8000 + i, .private_first = 80 + i, .ipv6 = i % 2 != 0};
        if (d->ipv6) {
            assert(inet_pton(AF_INET6, "2001:db8:ffff::8", d->public_address6) == 1);
            assert(inet_pton(AF_INET6, "fd00:1::8", d->private_address6) == 1);
        } else {
            d->public_address_be = ip("203.0.113.8");
            d->private_address_be = ip("10.0.0.8");
        }
    }
    uint8_t encoded[DPPD_FLOW_CONNTRACK_BYTES];
    size_t length;
    struct dppd_conntrack_policy decoded = {0};
    assert(dppd_flow_conntrack_encode(p, encoded, sizeof(encoded), &length) == 0);
    assert(dppd_flow_conntrack_decode(encoded, length, &decoded) == 0 && memcmp(p, &decoded, sizeof(*p)) == 0);
    /** 恶意高位、非法布尔值、截断和尾随字节都要拒绝，不能静默变成另一个合法策略 */
    encoded[4] = 1;
    assert(dppd_flow_conntrack_decode(encoded, length, &decoded) == -EBADMSG);
    encoded[4] = 0;
    encoded[16] = 2;
    assert(dppd_flow_conntrack_decode(encoded, length, &decoded) == -EBADMSG);
    encoded[16] = 1;
    assert(dppd_flow_conntrack_decode(encoded, length - 1U, &decoded) == -EBADMSG);
    encoded[length] = 0;
    assert(dppd_flow_conntrack_decode(encoded, length + 1U, &decoded) == -EBADMSG);
    apply(store, &r, 0);
    assert(dppd_flow_store_close(&store) == 0);
    struct dppd_flow_state *disk = NULL;
    assert(dppd_flow_state_load(path, &disk) == 0 && disk->conntrack != NULL);
    assert(memcmp(&disk->conntrack->policy, p, sizeof(*p)) == 0);
    disk->conntrack->policy.dnat[31].inside_port = 12;
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
    char directory[] = "/tmp/dppd-flow-conntrack-XXXXXX", path[160], lock[170];
    assert(mkdtemp(directory) != NULL);
    snprintf(path, sizeof(path), "%s/state", directory);
    snprintf(lock, sizeof(lock), "%s.lock", path);
    test_sessions(path);
    test_failures(path);
    test_pending_restart(path);
    test_full_policy(path);
    assert(unlink(path) == 0 && unlink(lock) == 0 && rmdir(directory) == 0);
    return 0;
}
