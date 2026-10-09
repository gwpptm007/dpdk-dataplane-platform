#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include "dppd/device.h"
#include "dppd/management.h"

static unsigned int allocations, clock_calls;
static bool reject_allocation;

void *__real_calloc(size_t count, size_t size);
void *__wrap_calloc(size_t count, size_t size);
int __real_clock_gettime(clockid_t clock_id, struct timespec *value);
int __wrap_clock_gettime(clockid_t clock_id, struct timespec *value);

/** 查询和追加不能动态分配，拒绝分配只用于本测试的链接引用，不影响其他进程 */
void *__wrap_calloc(size_t count, size_t size)
{
    allocations++;
    return reject_allocation ? NULL : __real_calloc(count, size);
}

/** 时钟继续提供真实结果，同时统计调用次数以检查历史路径不增加计时 */
int __wrap_clock_gettime(clockid_t clock_id, struct timespec *value)
{
    clock_calls++;
    return __real_clock_gettime(clock_id, value);
}

/** 预设一个带完整身份、主错误和补偿错误的请求，完成之前历史不能看见它 */
static void fail(struct dppd_rule_observation *observation, uint64_t id)
{
    uint64_t revision = observation->history.revision;

    dppd_rule_observation_begin(observation, DPPD_RULE_OPERATION_UPDATE_BATCH);
    observation->context.rule_id = id == 1 ? UINT64_MAX : id;
    observation->context.generation = id;
    observation->context.transaction_id = id * 3;
    observation->context.rule_count = 2;
    observation->context.install_port_id = 5;
    observation->context.port_known = true;
    observation->context.backend_known = true;
    observation->context.backend = DPPD_PLAN_BACKEND_RTE_FLOW;
    observation->context.stage = DPPD_RULE_STAGE_COMMIT;
    observation->applied = id % 5 == 0;
    dppd_rule_observation_compensation(observation, -EFAULT, id + 1);
    dppd_rule_observation_fault(observation, -EIO);
    assert(observation->history.revision == revision);
    dppd_rule_observation_finish(observation, -EUCLEAN);
    assert(!observation->active);
}

/** 每个失败后穿插成功操作，证明失败 ID 连续，但原有操作序号仍保留成功请求的间隔 */
static void retention(void)
{
    struct dppd_rule_observation observation = {0};
    struct dppd_rule_history_page page, copied;
    uint64_t cursor = 0;
    unsigned int seen = 0, before_allocations = allocations, before_clocks = clock_calls;

    reject_allocation = true;
    assert(dppd_rule_history_read(&observation.history, 0, 0, &page) == 0);
    assert(page.total == 0 && page.capacity == 64 && page.revision == 0 && !page.more && !page.gap);
    assert(dppd_rule_history_read(NULL, 0, 0, &page) == -EINVAL && page.capacity == 0);
    assert(dppd_rule_history_read(&observation.history, 0, 0, NULL) == -EINVAL);
    for (uint64_t id = 1; id <= 130; ++id) {
        fail(&observation, id);
        assert(dppd_rule_history_read(&observation.history, id - 1, id, &page) == 0);
        assert(page.returned == 1 && page.events[0].event_id == id);
        assert(page.events[0].failure.sequence == id * 2 - 1);
        assert(memcmp(&page.events[0].failure, &observation.metrics.last, sizeof(observation.metrics.last)) == 0);
        dppd_rule_observation_finish(&observation, -EIO);
        assert(observation.history.revision == id);
        dppd_rule_observation_begin(&observation, DPPD_RULE_OPERATION_APPLY);
        observation.unchanged = true;
        dppd_rule_observation_finish(&observation, 0);
        assert(observation.history.revision == id);
    }
    assert(observation.metrics.failed == 130 && observation.metrics.succeeded == 130);
    do {
        assert(dppd_rule_history_read(&observation.history, cursor, 130, &page) == 0);
        assert(page.total == 64 && page.overwritten == 66 && page.oldest_event_id == 67 && page.newest_event_id == 130);
        assert(page.gap == (cursor == 0) && page.returned == 4);
        for (unsigned int index = 0; index < page.returned; ++index) {
            const struct dppd_rule_history_entry *entry = &page.events[index];

            assert(entry->event_id == 67 + seen++ && entry->failure.sequence == entry->event_id * 2 - 1);
            assert(entry->failure.cause_code == -EIO && entry->failure.compensation_code == -EFAULT);
            assert(entry->failure.response_code == -EUCLEAN && entry->failure.compensation_rule_id == entry->event_id + 1);
            assert(entry->failure.applied == (entry->event_id % 5 == 0));
        }
        assert(dppd_rule_history_read(&observation.history, cursor, 130, &copied) == 0);
        assert(memcmp(&page, &copied, sizeof(page)) == 0);
        cursor = page.next_after;
    } while (page.more);
    assert(seen == 64 && cursor == 130);
    assert(dppd_rule_history_read(&observation.history, 65, 130, &page) == 0 && page.gap);
    assert(dppd_rule_history_read(&observation.history, 66, 130, &page) == 0 && !page.gap);
    assert(dppd_rule_history_read(&observation.history, 130, 130, &page) == 0 && page.returned == 0 && page.next_after == 130);
    assert(dppd_rule_history_read(&observation.history, UINT64_MAX, 130, &page) == 0 && page.next_after == UINT64_MAX);
    assert(dppd_rule_history_read(&observation.history, 0, 129, &page) == -ESTALE && page.returned == 0);
    assert(dppd_rule_history_read(&observation.history, 0, DPPD_RULE_HISTORY_REVISION_ANY, &page) == 0);
    assert(allocations == before_allocations && clock_calls == before_clocks);
    reject_allocation = false;
}

/** 编号边界不能回绕，历史冻结后最近失败仍更新，操作序号与失败 ID 的空间相互独立 */
static void exhaustion(void)
{
    struct dppd_rule_observation observation = {0};
    struct dppd_rule_failure_history frozen;
    struct dppd_rule_history_page page;

    observation.history.revision = UINT64_MAX - 2U;
    fail(&observation, 1);
    assert(observation.history.revision == UINT64_MAX - 1U && !observation.history.exhausted);
    assert(dppd_rule_history_read(&observation.history, 0, UINT64_MAX - 1U, &page) == 0);
    assert(page.events[0].event_id == UINT64_MAX - 1U && page.events[0].failure.rule_id == UINT64_MAX);
    frozen = observation.history;
    fail(&observation, 2);
    assert(observation.history.exhausted && observation.history.revision == frozen.revision);
    assert(observation.history.overwritten == frozen.overwritten && observation.history.count == frozen.count);
    assert(memcmp(observation.history.entries, frozen.entries, sizeof(frozen.entries)) == 0);
    assert(observation.metrics.last.rule_id == 2);
    fail(&observation, 3);
    assert(observation.metrics.last.rule_id == 3 && observation.history.revision == frozen.revision);
    assert(dppd_rule_history_read(&observation.history, 0, UINT64_MAX - 1U, &page) == 0 && page.exhausted);
    memset(&observation, 0, sizeof(observation));
    observation.metrics.operations = UINT64_MAX - 1U;
    fail(&observation, 1);
    fail(&observation, 2);
    assert(dppd_rule_history_read(&observation.history, 0, 2, &page) == 0);
    assert(page.events[0].event_id == 1 && page.events[1].event_id == 2);
    assert(page.events[0].failure.sequence == UINT64_MAX && page.events[1].failure.sequence == 0);
}

/** 真实 socket 验证版本拒绝、隔离可读和过期页，同时读取不能调用时钟或消耗分配 */
static void control_and_socket(void)
{
    const struct dppd_topology topology = {.nb_endpoints = 1, .endpoints = {{.ethdev_port_id = 5}}};
    struct dppd_control_service control;
    struct dppd_management_server server;
    struct dppd_device_set devices = {0};
    struct dppd_control_apply_result applied;
    struct dppd_rule value = {.id = UINT64_MAX, .fallback = DPPD_FALLBACK_SOFTWARE_ONLY,
        .nb_matches = 1, .nb_actions = 1};
    struct dppd_rule_history_page expected, copied;
    struct dppd_management_response response;
    struct sockaddr_un address = {.sun_family = AF_UNIX};
    char path[108];

    value.matches[0].type = DPPD_MATCH_ETH;
    value.actions[0].type = DPPD_ACTION_DROP;
    assert(dppd_control_init(&control, &topology, 2, NULL) == 0);
    assert(dppd_control_apply(&control, 5, &value, 0, &applied) == 0);
    /** 完全相同的规则走幂等成功，因此改变内容后再验证过期版本失败 */
    value.priority = 9;
    assert(dppd_control_apply(&control, 5, &value, 999, &applied) == -ESTALE);
    assert(dppd_control_rule_history(&control, 0, 1, &expected) == 0);
    assert(expected.events[0].event_id == 1 && expected.events[0].failure.sequence == 2);
    assert(expected.events[0].failure.generation == 999 && expected.events[0].failure.rule_id == UINT64_MAX);
    snprintf(path, sizeof(path), "/tmp/dppd-history-%ld", (long)getpid());
    memcpy(address.sun_path, path, strlen(path) + 1U);
    assert(dppd_management_start(&server, &control, &devices, NULL, path) == 0);
    unsigned int before_allocations = allocations, before_clocks = clock_calls;
    uint64_t operations = control.observation.metrics.operations;
    reject_allocation = true;
    assert(dppd_control_rule_history(&control, 0, 1, &copied) == 0);
    assert(memcmp(&copied, &expected, sizeof(copied)) == 0);
    for (unsigned int attempt = 0; attempt < 4; ++attempt) {
        struct dppd_management_request request = {.version = DPPD_MANAGEMENT_VERSION,
            .size = sizeof(request), .operation = DPPD_MANAGEMENT_RULE_HISTORY, .request_id = 23};
        int client = socket(AF_UNIX, SOCK_SEQPACKET, 0);

        request.payload.rule_history.expected_revision = attempt == 3 ? 0 : 1;
        if (attempt == 0)
            request.version--;
        if (attempt == 2)
            control.recovery_state = DPPD_CONTROL_RECOVERY_RECONCILIATION_REQUIRED;
        assert(client >= 0 && connect(client, (const struct sockaddr *)&address, sizeof(address)) == 0);
        assert(send(client, &request, sizeof(request), 0) == (ssize_t)sizeof(request));
        assert(dppd_management_poll(&server) == 0);
        assert(recv(client, &response, sizeof(response), 0) == (ssize_t)sizeof(response));
        assert(response.status == (attempt == 0 ? -EPROTO : attempt == 3 ? -ESTALE : 0));
        if (response.status == 0)
            assert(memcmp(&response.payload.rule_history, &expected, sizeof(expected)) == 0);
        assert(close(client) == 0 && control.observation.metrics.operations == operations);
    }
    assert(allocations == before_allocations && clock_calls == before_clocks);
    reject_allocation = false;
    control.recovery_state = DPPD_CONTROL_RECOVERY_READY;
    dppd_management_stop(&server);
    assert(dppd_control_fini(&control) == 0 && access(path, F_OK) != 0);
}

/** 完整窗口、分页数学边界与真实控制/socket 分开验证，覆盖生命周期和无副作用契约 */
int main(void)
{
    retention();
    exhaustion();
    control_and_socket();
    return 0;
}
