#define _GNU_SOURCE
#include "dppd/device.h"
#include "dppd/management.h"

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define SIM_RULE_CAPACITY 128U
#define SIM_MESSAGE_CAPACITY 8192U
#define SIM_FRAME_CAPACITY 2048U
#define SIM_MARKER ".dppd-transfer-simulation"
#define SIM_LOCK ".dppd-transfer-simulation.lock"
#define SIM_SECOND_NS UINT64_C(1000000000)
#define SIM_AGE_SECONDS_LIMIT UINT32_C(0xffffff)

/** 此程序只演练控制逻辑和报文字节，不创建真实 ethdev，也不向任何网卡发送报文 */
struct simulated_shared_count {
    bool used;
    struct dppd_shared_count_spec spec;
    uint64_t hits, bytes;
    uint32_t references;
};

struct simulated_flow {
    bool used;
    struct dppd_rule rule;
    uint64_t hits, bytes, last_hit_ns;
    struct simulated_shared_count *shared_count;
};

struct simulator {
    struct dppd_control_service control;
    struct dppd_device_set devices;
    struct dppd_management_server management;
    struct simulated_flow flows[SIM_RULE_CAPACITY + 1U];
    struct simulated_shared_count shared_counts[DPPD_SHARED_COUNT_MAX];
    uint64_t validates, creates, removes, queries, forwarded, dropped;
    uint64_t faults[8];
    uint64_t clock_offset_ns, age_queries;
    uint64_t shared_creates, shared_queries, shared_destroys;
    int clock_error;
};

struct owned_socket {
    int fd;
    bool owned;
    dev_t device;
    ino_t inode;
    char path[sizeof(((struct sockaddr_un *)0)->sun_path)];
};

struct options {
    char directory[PATH_MAX];
    char management[sizeof(((struct sockaddr_un *)0)->sun_path)];
    char io[sizeof(((struct sockaddr_un *)0)->sun_path)];
    char state[PATH_MAX];
    bool persistent;
};

static struct simulator simulation;
static volatile sig_atomic_t stopping;
static const char marker_content[] = "dppd_transfer_sim/v1\nsimulation=yes\nhardware-acceptance=not-run\n";

/** 单调实时时间加上演练偏移，既能等待自然闲置，也能不等几秒就核对老化边界 */
static int simulation_now(uint64_t *now_ns)
{
    struct timespec current;
    if (clock_gettime(CLOCK_MONOTONIC, &current) != 0)
        return -errno;
    if (current.tv_sec < 0 || (uint64_t)current.tv_sec > UINT64_MAX / SIM_SECOND_NS)
        return -EOVERFLOW;
    uint64_t seconds = (uint64_t)current.tv_sec * SIM_SECOND_NS;
    if ((uint64_t)current.tv_nsec > UINT64_MAX - seconds)
        return -EOVERFLOW;
    uint64_t real_ns = seconds + (uint64_t)current.tv_nsec;
    if (simulation.clock_offset_ns > UINT64_MAX - real_ns)
        return -EOVERFLOW;
    *now_ns = real_ns + simulation.clock_offset_ns;
    return 0;
}

/** 老化失败仍保留管理入口，错误和重试节流由正式控制服务记录，不用退出掩盖残留对象 */
static void poll_ages(void)
{
    uint64_t now_ns;
    simulation.clock_error = simulation_now(&now_ns);
    if (simulation.clock_error == 0)
        (void)dppd_control_age_poll(&simulation.control, now_ns, DPPD_AGE_POLL_BUDGET);
}

static void on_signal(int signal_number)
{
    (void)signal_number;
    stopping = 1;
}

static int flow_error(struct dppd_flow_error *error, int rc, const char *message)
{
    if (error != NULL) {
        memset(error, 0, sizeof(*error));
        error->code = rc;
        if (message != NULL)
            snprintf(error->message, sizeof(error->message), "SIMULATION ONLY: %s", message);
    }
    return rc;
}

/** 故障倒计时只消费后续调用，到零后失败一次，不改变已存在的规则和 COUNT */
static bool should_fail(unsigned int operation)
{
    uint64_t *remaining = &simulation.faults[operation];
    if (*remaining == 0)
        return false;
    return --*remaining == 0;
}

static void increment(uint64_t *value)
{
    if (*value != UINT64_MAX)
        ++*value;
}

/** COUNT 和 AGE 各自按类型查找，是否有计数不能用动作个数来代替判断 */
static const struct dppd_action *find_action(const struct dppd_rule *rule, enum dppd_action_type type)
{
    for (uint16_t i = 0; i < rule->nb_actions; ++i)
        if (rule->actions[i].type == type)
            return &rule->actions[i];
    return NULL;
}

/** 模拟器只承诺平坦的端点直转，不接受无法在 inject 中等价解释的协议条件或动作 */
static int supported_rule(uint16_t port_id, const struct dppd_rule *rule)
{
    char reason[128];
    if (rule == NULL || dppd_rule_validate(rule, reason, sizeof(reason)) != 0)
        return -EINVAL;
    if (rule->domain != DPPD_RULE_DOMAIN_TRANSFER || rule->group != 0 || rule->nb_matches != 2 ||
        rule->matches[0].type != DPPD_MATCH_REPRESENTED_PORT || rule->matches[1].type != DPPD_MATCH_ETH ||
        rule->nb_actions < 1 || rule->nb_actions > 3 ||
        rule->actions[rule->nb_actions - 1U].type != DPPD_ACTION_REPRESENTED_PORT)
        return -ENOTSUP;
    for (uint16_t i = 0; i + 1U < rule->nb_actions; ++i)
        if (rule->actions[i].type != DPPD_ACTION_COUNT && rule->actions[i].type != DPPD_ACTION_AGE &&
            rule->actions[i].type != DPPD_ACTION_SHARED_COUNT)
            return -ENOTSUP;
    const struct dppd_endpoint *install = dppd_topology_find(&simulation.devices.topology, port_id);
    const struct dppd_endpoint *source =
        dppd_topology_find(&simulation.devices.topology, rule->matches[0].spec.ethdev_port_id);
    const struct dppd_endpoint *destination =
        dppd_topology_find(&simulation.devices.topology, rule->actions[rule->nb_actions - 1U].conf.ethdev_port_id);
    if (install == NULL || source == NULL || destination == NULL)
        return -ENOENT;
    if (!install->has_switch_domain || !source->has_switch_domain || !destination->has_switch_domain ||
        install->switch_domain_id == UINT16_MAX || install->switch_domain_id != source->switch_domain_id ||
        install->switch_domain_id != destination->switch_domain_id)
        return -EXDEV;
    if (source->ethdev_port_id == destination->ethdev_port_id || source->switch_port_id == destination->switch_port_id)
        return -EINVAL;
    return 0;
}

/** 只对照本进程固定对象地址，不读取外来 opaque 指针，身份还要核对精确版本和安装范围 */
static struct simulated_shared_count *find_shared_count(const struct dppd_shared_count_handle *handle)
{
    if (handle == NULL)
        return NULL;
    for (unsigned int i = 0; i < DPPD_SHARED_COUNT_MAX; ++i) {
        struct simulated_shared_count *counter = &simulation.shared_counts[i];
        if ((const void *)handle->opaque == (const void *)counter && counter->used &&
            counter->spec.id == handle->spec.id && counter->spec.generation == handle->spec.generation &&
            counter->spec.install_port_id == handle->spec.install_port_id &&
            counter->spec.domain == handle->spec.domain)
            return counter;
    }
    return NULL;
}

/** 绑定下标必须对应共享动作，空闲位置不能夹带句柄，重放时对象尚未创建就会明确失败 */
static int resolve_binding(uint16_t port_id, const struct dppd_rule *rule, const struct dppd_flow_binding *binding,
    struct simulated_shared_count **resolved)
{
    *resolved = NULL;
    for (unsigned int i = 0; i < DPPD_RULE_MAX_ACTIONS; ++i) {
        const struct dppd_action *action = i < rule->nb_actions ? &rule->actions[i] : NULL;
        struct rte_flow_action_handle *opaque = binding != NULL ? binding->actions[i] : NULL;
        if (action == NULL || action->type != DPPD_ACTION_SHARED_COUNT) {
            if (opaque != NULL)
                return -EINVAL;
            continue;
        }
        struct dppd_shared_count_handle handle = {.spec = {.id = action->conf.shared_count.id,
                                                      .generation = action->conf.shared_count.generation,
                                                      .install_port_id = port_id,
                                                      .domain = rule->domain},
            .opaque = opaque};
        struct simulated_shared_count *counter = find_shared_count(&handle);
        if (counter == NULL)
            return -ENOENT;
        if (*resolved != NULL)
            return -EINVAL;
        *resolved = counter;
    }
    return 0;
}

static int simulated_validate_bound(uint16_t port_id, const struct dppd_rule *rule,
    const struct dppd_flow_binding *binding, struct dppd_flow_error *error)
{
    increment(&simulation.validates);
    int rc = should_fail(0) ? -ENOTSUP : supported_rule(port_id, rule);
    struct simulated_shared_count *counter;
    if (rc == 0)
        rc = resolve_binding(port_id, rule, binding, &counter);
    return flow_error(error, rc, rc == 0 ? NULL : "validation rejected or injected failure");
}

static int simulated_validate(uint16_t port_id, const struct dppd_rule *rule, struct dppd_flow_error *error)
{
    return simulated_validate_bound(port_id, rule, NULL, error);
}

static int simulated_create_bound(uint16_t port_id, const struct dppd_rule *rule,
    const struct dppd_flow_binding *binding, struct dppd_flow_handle *handle, struct dppd_flow_error *error)
{
    increment(&simulation.creates);
    if (should_fail(1))
        return flow_error(error, -ENOSPC, "injected create failure");
    int rc = supported_rule(port_id, rule);
    if (rc != 0)
        return flow_error(error, rc, "unsupported simulated rule");
    if (handle == NULL || rule->generation == 0 || rule->generation == DPPD_RULE_GENERATION_ANY)
        return flow_error(error, -EINVAL, "invalid handle or generation");
    struct simulated_shared_count *counter;
    rc = resolve_binding(port_id, rule, binding, &counter);
    if (rc != 0 || (counter != NULL && counter->references == UINT32_MAX))
        return flow_error(error, rc != 0 ? rc : -EOVERFLOW, "invalid shared COUNT binding");
    uint64_t now_ns;
    rc = simulation_now(&now_ns);
    if (rc != 0)
        return flow_error(error, rc, "simulated clock exhausted");
    for (unsigned int i = 0; i < SIM_RULE_CAPACITY + 1U; ++i) {
        struct simulated_flow *flow = &simulation.flows[i];
        if (flow->used)
            continue;
        memset(flow, 0, sizeof(*flow));
        flow->used = true;
        flow->rule = *rule;
        flow->last_hit_ns = now_ns;
        flow->shared_count = counter;
        if (counter != NULL)
            ++counter->references;
        *handle = (struct dppd_flow_handle){.rule_id = rule->id,
            .rule_generation = rule->generation,
            .port_id = port_id,
            .flow = (struct rte_flow *)flow,
            .has_count = find_action(rule, DPPD_ACTION_COUNT) != NULL,
            .has_age = find_action(rule, DPPD_ACTION_AGE) != NULL};
        return flow_error(error, 0, NULL);
    }
    return flow_error(error, -ENOSPC, "simulated handle capacity exhausted");
}

static int simulated_create(uint16_t port_id, const struct dppd_rule *rule, struct dppd_flow_handle *handle,
    struct dppd_flow_error *error)
{
    return simulated_create_bound(port_id, rule, NULL, handle, error);
}

/** 只比较固定槽位的地址，不解引用外来的 opaque 指针，失败时保留完整清理线索 */
static struct simulated_flow *find_flow(const struct dppd_flow_handle *handle)
{
    if (handle == NULL)
        return NULL;
    for (unsigned int i = 0; i < SIM_RULE_CAPACITY + 1U; ++i) {
        struct simulated_flow *flow = &simulation.flows[i];
        if ((const void *)handle->flow == (const void *)flow && flow->used && flow->rule.id == handle->rule_id &&
            flow->rule.generation == handle->rule_generation && flow->rule.install_port_id == handle->port_id &&
            handle->has_count == (find_action(&flow->rule, DPPD_ACTION_COUNT) != NULL) &&
            handle->has_age == (find_action(&flow->rule, DPPD_ACTION_AGE) != NULL))
            return flow;
    }
    return NULL;
}

static int simulated_remove(struct dppd_flow_handle *handle, struct dppd_flow_error *error)
{
    increment(&simulation.removes);
    if (should_fail(2))
        return flow_error(error, -EBUSY, "injected remove failure");
    struct simulated_flow *flow = find_flow(handle);
    if (flow == NULL)
        return flow_error(error, -ENOENT, "simulated handle is absent");
    if (flow->shared_count != NULL) {
        if (!flow->shared_count->used || flow->shared_count->references == 0)
            return flow_error(error, -EUCLEAN, "simulated shared COUNT reference is inconsistent");
        --flow->shared_count->references;
    }
    memset(flow, 0, sizeof(*flow));
    memset(handle, 0, sizeof(*handle));
    return flow_error(error, 0, NULL);
}

static int simulated_query(const struct dppd_flow_handle *handle, uint64_t *hits, uint64_t *bytes,
    struct dppd_flow_error *error)
{
    increment(&simulation.queries);
    if (should_fail(3))
        return flow_error(error, -EIO, "injected query failure");
    struct simulated_flow *flow = find_flow(handle);
    if (flow == NULL || hits == NULL || bytes == NULL)
        return flow_error(error, -EINVAL, "invalid simulated query");
    if (!handle->has_count)
        return flow_error(error, -ENODATA, "rule has no COUNT");
    *hits = flow->hits;
    *bytes = flow->bytes;
    return flow_error(error, 0, NULL);
}

/** AGE 只比较最近命中的闲置时长，不读取或清除 COUNT，也不在查询回调中自行删除规则 */
static int simulated_query_age(const struct dppd_flow_handle *handle, struct dppd_flow_age_result *result,
    struct dppd_flow_error *error)
{
    increment(&simulation.age_queries);
    if (should_fail(4))
        return flow_error(error, -EIO, "injected AGE query failure");
    struct simulated_flow *flow = find_flow(handle);
    if (flow == NULL || result == NULL)
        return flow_error(error, -EINVAL, "invalid simulated AGE query");
    const struct dppd_action *age = find_action(&flow->rule, DPPD_ACTION_AGE);
    if (age == NULL)
        return flow_error(error, -ENODATA, "rule has no AGE");
    uint64_t now_ns;
    int rc = simulation_now(&now_ns);
    if (rc != 0 || now_ns < flow->last_hit_ns)
        return flow_error(error, rc != 0 ? rc : -EUCLEAN, "invalid simulated idle clock");
    uint64_t idle_seconds = (now_ns - flow->last_hit_ns) / SIM_SECOND_NS;
    *result = (struct dppd_flow_age_result){.aged = idle_seconds >= age->conf.age_timeout_seconds,
        .sec_since_last_hit_valid = idle_seconds <= SIM_AGE_SECONDS_LIMIT,
        .sec_since_last_hit = idle_seconds <= SIM_AGE_SECONDS_LIMIT ? (uint32_t)idle_seconds : 0};
    return flow_error(error, 0, NULL);
}

/** 共享对象单独拥有累计值，规则只借用它，删除其中一条规则不能清掉其他规则的累计 */
static int simulated_shared_create(const struct dppd_shared_count_spec *spec, struct dppd_shared_count_handle *handle,
    struct dppd_flow_error *error)
{
    increment(&simulation.shared_creates);
    if (should_fail(5))
        return flow_error(error, -ENOSPC, "injected shared COUNT create failure");
    if (spec == NULL || handle == NULL || spec->id == 0 || spec->generation == 0 ||
        spec->generation == DPPD_RULE_GENERATION_ANY ||
        (spec->domain != DPPD_RULE_DOMAIN_INGRESS && spec->domain != DPPD_RULE_DOMAIN_EGRESS &&
            spec->domain != DPPD_RULE_DOMAIN_TRANSFER))
        return flow_error(error, -EINVAL, "invalid shared COUNT definition");
    if (dppd_topology_find(&simulation.devices.topology, spec->install_port_id) == NULL)
        return flow_error(error, -ENOENT, "unknown shared COUNT installation port");
    struct simulated_shared_count *available = NULL;
    for (unsigned int i = 0; i < DPPD_SHARED_COUNT_MAX; ++i) {
        struct simulated_shared_count *counter = &simulation.shared_counts[i];
        if (counter->used && counter->spec.id == spec->id)
            return flow_error(error, -EEXIST, "shared COUNT ID already exists");
        if (!counter->used && available == NULL)
            available = counter;
    }
    if (available == NULL)
        return flow_error(error, -ENOSPC, "shared COUNT capacity exhausted");
    *available = (struct simulated_shared_count){.used = true, .spec = *spec};
    *handle = (struct dppd_shared_count_handle){.spec = *spec, .opaque = (struct rte_flow_action_handle *)available};
    return flow_error(error, 0, NULL);
}

static int simulated_shared_query(const struct dppd_shared_count_handle *handle, uint64_t *hits, uint64_t *bytes,
    struct dppd_flow_error *error)
{
    increment(&simulation.shared_queries);
    if (should_fail(6))
        return flow_error(error, -EIO, "injected shared COUNT query failure");
    struct simulated_shared_count *counter = find_shared_count(handle);
    if (counter == NULL || hits == NULL || bytes == NULL)
        return flow_error(error, -EINVAL, "invalid shared COUNT query");
    *hits = counter->hits;
    *bytes = counter->bytes;
    return flow_error(error, 0, NULL);
}

/** 只有成功删除最后一条实际规则后才能销毁对象，失败回滚残留也会继续保护借用句柄 */
static int simulated_shared_destroy(struct dppd_shared_count_handle *handle, struct dppd_flow_error *error)
{
    increment(&simulation.shared_destroys);
    if (should_fail(7))
        return flow_error(error, -EBUSY, "injected shared COUNT destroy failure");
    struct simulated_shared_count *counter = find_shared_count(handle);
    if (counter == NULL)
        return flow_error(error, -ENOENT, "shared COUNT handle is absent");
    if (counter->references != 0)
        return flow_error(error, -EBUSY, "shared COUNT still has installed references");
    memset(counter, 0, sizeof(*counter));
    memset(handle, 0, sizeof(*handle));
    return flow_error(error, 0, NULL);
}

static const struct dppd_flow_api simulated_api = {.validate = simulated_validate,
    .create = simulated_create,
    .remove = simulated_remove,
    .query_count = simulated_query,
    .query_age = simulated_query_age,
    .shared_count_create = simulated_shared_create,
    .shared_count_query = simulated_shared_query,
    .shared_count_destroy = simulated_shared_destroy,
    .validate_bound = simulated_validate_bound,
    .create_bound = simulated_create_bound};

/** 端点名称只是公开夹具，driver 明确标为 simulation，不能把解析来源当作真实硬件证据 */
static int make_devices(struct dppd_device_set *devices)
{
    static const char *const names[] = {"p0", "c0pf0", "c0pf0vf1", "c0pf0vf2", "c0pf0sf1", "c0pf0sf2", "", "p1"};
    memset(devices, 0, sizeof(*devices));
    devices->nb_ports = devices->topology.nb_endpoints = 8;
    atomic_init(&devices->removal_requested, false);
    for (uint16_t i = 0; i < 8; ++i) {
        struct dppd_port *port = &devices->ports[i];
        struct dppd_endpoint *endpoint = &devices->topology.endpoints[i];
        bool representor = i >= 1 && i <= 5;
        port->port_id = endpoint->ethdev_port_id = i;
        port->peer_port_id = UINT16_MAX;
        port->socket_id = -1;
        port->configured = port->started = true;
        atomic_init(&port->removed, false);
        atomic_init(&port->link_state, DPPD_LINK_UP);
        snprintf(port->identity.device_name, sizeof(port->identity.device_name), "simulation-fixture");
        snprintf(port->identity.dpdk_version, sizeof(port->identity.dpdk_version), "simulation-no-EAL");
        port->identity.device_name_known = true;
        port->identity.firmware_error = -ENOTSUP;
        endpoint->kind = representor ? DPPD_ENDPOINT_REPRESENTOR : DPPD_ENDPOINT_ETHDEV;
        endpoint->has_switch_domain = true;
        endpoint->switch_domain_id = i == 7 ? 88 : 77;
        endpoint->switch_port_id = 100 + i;
        snprintf(endpoint->driver_name, sizeof(endpoint->driver_name), "simulation");
        snprintf(endpoint->switch_name, sizeof(endpoint->switch_name), "simulation-domain-%u",
            endpoint->switch_domain_id);
        snprintf(endpoint->interface_name, sizeof(endpoint->interface_name), "sim%u", i);
        snprintf(endpoint->device_name, sizeof(endpoint->device_name), "0000:01:00.0");
        if (i == 6)
            endpoint->role_error = -ENODATA;
        else {
            int rc = dppd_topology_resolve_role(endpoint, "net_mlx5", representor, names[i]);
            if (rc != 0)
                return rc;
        }
    }
    return 0;
}

static int parse_number(const char *text, uint64_t maximum, uint64_t *output)
{
    uint64_t value = 0;
    if (text == NULL || *text == '\0')
        return -EINVAL;
    for (; *text != '\0'; ++text) {
        if (*text < '0' || *text > '9')
            return -EINVAL;
        unsigned int digit = (unsigned int)(*text - '0');
        if (value > maximum / 10U || (value == maximum / 10U && digit > maximum % 10U))
            return -ERANGE;
        value = value * 10U + digit;
    }
    *output = value;
    return 0;
}

static int hex_digit(char value)
{
    if (value >= '0' && value <= '9')
        return value - '0';
    if (value >= 'a' && value <= 'f')
        return value - 'a' + 10;
    if (value >= 'A' && value <= 'F')
        return value - 'A' + 10;
    return -1;
}

static unsigned int installed_count(void)
{
    unsigned int count = 0;
    for (unsigned int i = 0; i < SIM_RULE_CAPACITY + 1U; ++i)
        count += simulation.flows[i].used;
    return count;
}

/** 此处记录驱动夹具实际持有的对象和借用，不把控制面的预留引用冒充已安装规则 */
static unsigned int shared_object_count(uint32_t *references)
{
    unsigned int count = 0;
    *references = 0;
    for (unsigned int i = 0; i < DPPD_SHARED_COUNT_MAX; ++i) {
        const struct simulated_shared_count *counter = &simulation.shared_counts[i];
        if (counter->used) {
            ++count;
            *references += counter->references;
        }
    }
    return count;
}

/** 只计算可安全推进的偏移，调用方完成全部参数检查前不能修改时钟或故障计划 */
static int advance_delta(const char *text, uint64_t *delta)
{
    uint64_t seconds, now_ns;
    int rc = parse_number(text, UINT64_MAX, &seconds);
    if (rc != 0)
        return rc;
    if (seconds > UINT64_MAX / SIM_SECOND_NS)
        return -EOVERFLOW;
    rc = simulation_now(&now_ns);
    if (rc != 0)
        return rc;
    *delta = seconds * SIM_SECOND_NS;
    return *delta > UINT64_MAX - now_ns ? -EOVERFLOW : 0;
}

static void advance_and_poll(uint64_t delta, char *reply, size_t capacity)
{
    simulation.clock_offset_ns += delta;
    poll_ages();
    snprintf(reply, capacity, "ok advanced_seconds=%" PRIu64 " age_error=%d installed=%u desired=%u simulation=yes",
        simulation.clock_offset_ns / SIM_SECOND_NS,
        simulation.clock_error != 0 ? simulation.clock_error : simulation.control.age.last_error, installed_count(),
        dppd_rule_repository_count(&simulation.control.rules));
}

/** 先核对完整输入和服务状态，再选择唯一最优规则，任何错误都不会增加命中计数 */
static int inject(const char *source_text, const char *hex, char *reply, size_t capacity)
{
    uint64_t source;
    if (parse_number(source_text, UINT16_MAX - 1U, &source) != 0)
        return -EINVAL;
    if (dppd_topology_find(&simulation.devices.topology, (uint16_t)source) == NULL)
        return -ENOENT;
    size_t digits = strlen(hex);
    if (digits < 28 || digits > SIM_FRAME_CAPACITY * 2U || digits % 2U != 0)
        return -EINVAL;
    for (size_t i = 0; i < digits; ++i)
        if (hex_digit(hex[i]) < 0)
            return -EINVAL;
    if (simulation.control.recovery_state != DPPD_CONTROL_RECOVERY_READY || simulation.control.persistence_dirty ||
        simulation.control.age.last_error != 0 || simulation.clock_error != 0)
        return -EUCLEAN;
    if (dppd_software_backend_count(&simulation.control.software) != 0)
        return -ENOTSUP;
    struct simulated_flow *selected = NULL;
    bool ambiguous = false;
    for (unsigned int i = 0; i < SIM_RULE_CAPACITY + 1U; ++i) {
        struct simulated_flow *flow = &simulation.flows[i];
        if (!flow->used || flow->rule.matches[0].spec.ethdev_port_id != source)
            continue;
        if (selected == NULL || flow->rule.priority < selected->rule.priority) {
            selected = flow;
            ambiguous = false;
        } else if (flow->rule.priority == selected->rule.priority)
            ambiguous = true;
    }
    if (ambiguous)
        return -EEXIST;
    if (selected == NULL) {
        if (simulation.dropped == UINT64_MAX)
            return -EOVERFLOW;
        ++simulation.dropped;
        snprintf(reply, capacity, "drop reason=no-match simulation=yes");
        return 0;
    }
    uint64_t now_ns;
    int rc = simulation_now(&now_ns);
    if (rc != 0)
        return rc;
    bool with_count = find_action(&selected->rule, DPPD_ACTION_COUNT) != NULL;
    struct simulated_shared_count *shared = selected->shared_count;
    size_t length = digits / 2U;
    if (shared != NULL && (!shared->used || shared->references == 0))
        return -EUCLEAN;
    if (simulation.forwarded == UINT64_MAX ||
        (with_count && (selected->hits == UINT64_MAX || selected->bytes > UINT64_MAX - length)) ||
        (shared != NULL && (shared->hits == UINT64_MAX || shared->bytes > UINT64_MAX - length)))
        return -EOVERFLOW;
    int written = snprintf(reply, capacity,
        "forward source=%" PRIu64 " destination=%u rule=%" PRIu64 " length=%zu frame=%s simulation=yes", source,
        selected->rule.actions[selected->rule.nb_actions - 1U].conf.ethdev_port_id, selected->rule.id, length, hex);
    if (written < 0 || (size_t)written >= capacity)
        return -EOVERFLOW;
    ++simulation.forwarded;
    selected->last_hit_ns = now_ns;
    if (with_count) {
        ++selected->hits;
        selected->bytes += length;
    }
    if (shared != NULL) {
        ++shared->hits;
        shared->bytes += length;
    }
    return 0;
}

static int io_request(char *request, char *reply, size_t capacity)
{
    char *tokens[5], *cursor = NULL;
    unsigned int count = 0;
    for (char *token = strtok_r(request, " \t\r\n", &cursor); token != NULL;
         token = strtok_r(NULL, " \t\r\n", &cursor)) {
        if (count == 5)
            return -EINVAL;
        tokens[count++] = token;
    }
    if (count == 0)
        return -EINVAL;
    if (count == 1 && strcmp(tokens[0], "status") == 0) {
        uint32_t shared_references;
        unsigned int shared_objects = shared_object_count(&shared_references);
        snprintf(reply, capacity,
            "status installed=%u validates=%" PRIu64 " creates=%" PRIu64 " removes=%" PRIu64 " queries=%" PRIu64
            " forwarded=%" PRIu64 " dropped=%" PRIu64 " desired=%u recovery=%u software=%u age_queries=%" PRIu64
            " age_error=%d advanced_seconds=%" PRIu64 " shared_objects=%u shared_refs=%u shared_creates=%" PRIu64
            " shared_queries=%" PRIu64 " shared_destroys=%" PRIu64 " simulation=yes hardware-acceptance=not-run",
            installed_count(), simulation.validates, simulation.creates, simulation.removes, simulation.queries,
            simulation.forwarded, simulation.dropped, dppd_rule_repository_count(&simulation.control.rules),
            (unsigned int)simulation.control.recovery_state, dppd_software_backend_count(&simulation.control.software),
            simulation.age_queries,
            simulation.clock_error != 0 ? simulation.clock_error : simulation.control.age.last_error,
            simulation.clock_offset_ns / SIM_SECOND_NS, shared_objects, shared_references, simulation.shared_creates,
            simulation.shared_queries, simulation.shared_destroys);
        return 0;
    }
    if (count == 1 && strcmp(tokens[0], "clear-faults") == 0) {
        memset(simulation.faults, 0, sizeof(simulation.faults));
        snprintf(reply, capacity, "ok simulation=yes");
        return 0;
    }
    if (count == 2 && strcmp(tokens[0], "advance") == 0) {
        uint64_t delta;
        int rc = advance_delta(tokens[1], &delta);
        if (rc != 0)
            return rc;
        /** 参数与溢出检查全部完成后才推进，失败的请求不会偷偷改变老化时间 */
        advance_and_poll(delta, reply, capacity);
        return 0;
    }
    if ((count == 3 || (count == 5 && strcmp(tokens[3], "advance") == 0)) && strcmp(tokens[0], "fault") == 0) {
        static const char *const operations[] = {"validate", "create", "remove", "query", "age-query", "shared-create",
            "shared-query", "shared-destroy"};
        uint64_t remaining, delta = 0;
        if (parse_number(tokens[2], UINT64_MAX, &remaining) != 0 || remaining == 0)
            return -EINVAL;
        if (count == 5) {
            int rc = advance_delta(tokens[4], &delta);
            if (rc != 0)
                return rc;
        }
        for (unsigned int i = 0; i < sizeof(operations) / sizeof(operations[0]); ++i)
            if (strcmp(tokens[1], operations[i]) == 0) {
                /** 一条消息内设置故障并推进，避免自动轮询抢先消费本来要观察的故障 */
                simulation.faults[i] = remaining;
                if (count == 5)
                    advance_and_poll(delta, reply, capacity);
                else
                    snprintf(reply, capacity, "ok simulation=yes");
                return 0;
            }
        return -EINVAL;
    }
    if (count == 3 && strcmp(tokens[0], "inject") == 0)
        return inject(tokens[1], tokens[2], reply, capacity);
    return -EINVAL;
}

static bool same_socket(const struct owned_socket *owned)
{
    struct stat status;
    return owned->owned && lstat(owned->path, &status) == 0 && S_ISSOCK(status.st_mode) &&
        status.st_dev == owned->device && status.st_ino == owned->inode;
}

static int remember_socket(struct owned_socket *owned, int fd, const char *path)
{
    struct stat status;
    if (lstat(path, &status) != 0)
        return -errno;
    if (!S_ISSOCK(status.st_mode) || status.st_uid != geteuid())
        return -EACCES;
    owned->fd = fd;
    owned->device = status.st_dev;
    owned->inode = status.st_ino;
    memcpy(owned->path, path, strlen(path) + 1U);
    owned->owned = true;
    return 0;
}

static void stop_io(struct owned_socket *owned)
{
    if (owned->fd >= 0)
        close(owned->fd);
    if (same_socket(owned))
        unlink(owned->path);
    owned->fd = -1;
    owned->owned = false;
}

/** bind 前不删除任何路径，只有本进程成功创建的 socket 才能进入清理名单 */
static int start_io(struct owned_socket *owned, const char *path)
{
    struct sockaddr_un address = {.sun_family = AF_UNIX};
    int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -errno;
    memcpy(address.sun_path, path, strlen(path) + 1U);
    if (bind(fd, (const struct sockaddr *)&address, sizeof(address)) != 0) {
        int rc = -errno;
        close(fd);
        return rc;
    }
    int rc = remember_socket(owned, fd, path);
    if (rc != 0) {
        close(fd);
        return rc;
    }
    if (chmod(path, S_IRUSR | S_IWUSR) != 0 || listen(fd, 16) != 0) {
        rc = -errno;
        stop_io(owned);
        return rc;
    }
    return 0;
}

/** 一次连接只处理一个有界文本消息，超长消息和内嵌 NUL 不能伪装成合法前缀 */
static int poll_io(const struct owned_socket *owned)
{
    for (unsigned int handled = 0; handled < 8 && !stopping; ++handled) {
        int client = accept4(owned->fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (client < 0)
            return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR ? 0 : -errno;
        struct pollfd ready = {.fd = client, .events = POLLIN};
        int polled = poll(&ready, 1, 20);
        if (polled > 0 && (ready.revents & POLLIN) != 0) {
            char request[SIM_MESSAGE_CAPACITY + 1U], reply[SIM_MESSAGE_CAPACITY];
            ssize_t received = recv(client, request, SIM_MESSAGE_CAPACITY, MSG_TRUNC);
            int rc = received < 0 ? -errno : received == 0 ? -EINVAL : received > SIM_MESSAGE_CAPACITY ? -EMSGSIZE : 0;
            if (rc == 0) {
                if (memchr(request, '\0', (size_t)received) != NULL)
                    rc = -EINVAL;
                else {
                    request[received] = '\0';
                    rc = io_request(request, reply, sizeof(reply));
                }
            }
            if (rc != 0)
                snprintf(reply, sizeof(reply), "error code=%d simulation=yes", -rc);
            (void)send(client, reply, strlen(reply), MSG_NOSIGNAL);
        }
        close(client);
    }
    return 0;
}

/** 路径统一落在当前用户拥有的 0700 目录，不允许接管生产 socket 或任意快照路径 */
static int resolve_option_path(const char *input, char *output, size_t capacity, struct options *options)
{
    char parent[PATH_MAX], resolved[PATH_MAX];
    struct stat status;
    if (input == NULL || input[0] != '/' || strnlen(input, sizeof(parent)) >= sizeof(parent))
        return -EINVAL;
    const char *slash = strrchr(input, '/');
    const char *base = slash + 1;
    if (*base == '\0' || strcmp(base, ".") == 0 || strcmp(base, "..") == 0 || strcmp(base, SIM_MARKER) == 0 ||
        strcmp(base, SIM_LOCK) == 0)
        return -EINVAL;
    size_t length = (size_t)(slash - input);
    memcpy(parent, input, length);
    parent[length] = '\0';
    if (length == 0)
        memcpy(parent, "/", 2);
    if (realpath(parent, resolved) == NULL)
        return -errno;
    if (stat(resolved, &status) != 0)
        return -errno;
    if (!S_ISDIR(status.st_mode) || status.st_uid != geteuid() || (status.st_mode & 0777) != 0700)
        return -EACCES;
    if (options->directory[0] == '\0')
        memcpy(options->directory, resolved, strlen(resolved) + 1U);
    else if (strcmp(options->directory, resolved) != 0)
        return -EXDEV;
    int written = snprintf(output, capacity, "%s/%s", resolved, base);
    return written < 0 || (size_t)written >= capacity ? -ENAMETOOLONG : 0;
}

static int parse_options(int argc, char **argv, struct options *options)
{
    static const struct option arguments[] = {{"socket", required_argument, NULL, 's'},
        {"io-socket", required_argument, NULL, 'i'}, {"state-path", required_argument, NULL, 'p'}, {NULL, 0, NULL, 0}};
    const char *management = NULL, *io = NULL, *state = NULL;
    int option;
    while ((option = getopt_long(argc, argv, "", arguments, NULL)) != -1) {
        if (option == 's' && management == NULL)
            management = optarg;
        else if (option == 'i' && io == NULL)
            io = optarg;
        else if (option == 'p' && state == NULL)
            state = optarg;
        else
            return -EINVAL;
    }
    if (optind != argc || management == NULL || io == NULL)
        return -EINVAL;
    int rc = resolve_option_path(management, options->management, sizeof(options->management), options);
    if (rc == 0)
        rc = resolve_option_path(io, options->io, sizeof(options->io), options);
    if (rc != 0)
        return rc;
    if (strcmp(options->management, options->io) == 0)
        return -EINVAL;
    if (state != NULL) {
        rc = resolve_option_path(state, options->state, sizeof(options->state), options);
        if (rc != 0)
            return rc;
        if (strcmp(strrchr(options->state, '/') + 1, "simulation-state.bin") != 0 ||
            strcmp(options->state, options->management) == 0 || strcmp(options->state, options->io) == 0)
            return -EINVAL;
        options->persistent = true;
    }
    return 0;
}

static bool private_file(const struct stat *status)
{
    return S_ISREG(status->st_mode) && status->st_uid == geteuid() && status->st_nlink == 1 &&
        (status->st_mode & 0777) == 0600;
}

/** 专用目录锁不会随快照原子替换而失效，锁文件保留供下一次启动使用 */
static int lock_directory(const struct options *options, int *directory_fd, int *lock_fd)
{
    struct stat status;
    *directory_fd = open(options->directory, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (*directory_fd < 0)
        return -errno;
    if (fstat(*directory_fd, &status) != 0)
        return -errno;
    if (!S_ISDIR(status.st_mode) || status.st_uid != geteuid() || (status.st_mode & 0777) != 0700)
        return -EACCES;
    *lock_fd = openat(*directory_fd, SIM_LOCK, O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (*lock_fd < 0)
        return -errno;
    if (fstat(*lock_fd, &status) != 0)
        return -errno;
    if (!private_file(&status))
        return -EACCES;
    return flock(*lock_fd, LOCK_EX | LOCK_NB) == 0 ? 0 : -errno;
}

/** 没有模拟标记的已有快照永不接管，标记本身也必须是当前用户的私有普通文件 */
static int check_state_marker(int directory_fd)
{
    struct stat state, marker;
    bool state_exists = fstatat(directory_fd, "simulation-state.bin", &state, AT_SYMLINK_NOFOLLOW) == 0;
    if (!state_exists && errno != ENOENT)
        return -errno;
    if (state_exists && !private_file(&state))
        return -EACCES;
    int fd = openat(directory_fd, SIM_MARKER, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0 && errno == ENOENT && !state_exists) {
        fd = openat(directory_fd, SIM_MARKER, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd < 0)
            return -errno;
        ssize_t written = write(fd, marker_content, sizeof(marker_content) - 1U);
        int rc = written == (ssize_t)(sizeof(marker_content) - 1U) && fsync(fd) == 0 ? 0 : -EIO;
        close(fd);
        if (rc == 0 && fsync(directory_fd) != 0)
            rc = -errno;
        return rc;
    }
    if (fd < 0)
        return -errno;
    int rc = fstat(fd, &marker) == 0 ? 0 : -errno;
    if (rc == 0 && !private_file(&marker))
        rc = -EACCES;
    char content[sizeof(marker_content)];
    ssize_t received = rc == 0 ? read(fd, content, sizeof(content)) : -1;
    if (rc == 0 &&
        (received != (ssize_t)(sizeof(marker_content) - 1U) ||
            memcmp(content, marker_content, sizeof(marker_content) - 1U) != 0))
        rc = -EACCES;
    close(fd);
    return rc;
}

/** 在读取或建立模拟快照之前拒绝已有 socket 路径，不给失败启动留下状态文件变化 */
static int require_new_socket_path(const char *path)
{
    struct stat status;
    if (lstat(path, &status) == 0)
        return -EEXIST;
    return errno == ENOENT ? 0 : -errno;
}

int main(int argc, char **argv)
{
    struct options options = {0};
    struct owned_socket io = {.fd = -1}, management = {.fd = -1};
    struct sigaction action = {.sa_handler = on_signal};
    bool initialized = false;
    int directory_fd = -1, lock_fd = -1;
    int rc;

    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("SIMULATION ONLY hardware-acceptance=not-run driver=simulation no-EAL no-real-packets\n");
    rc = parse_options(argc, argv, &options);
    if (rc != 0) {
        fprintf(stderr, "usage: %s --socket PATH --io-socket PATH [--state-path DIR/simulation-state.bin]\n", argv[0]);
        fprintf(stderr, "all paths require one caller-owned 0700 directory and dedicated simulation state\n");
        goto cleanup;
    }
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) != 0 || sigaction(SIGTERM, &action, NULL) != 0) {
        rc = -errno;
        goto cleanup;
    }
    rc = lock_directory(&options, &directory_fd, &lock_fd);
    if (rc != 0)
        goto cleanup;
    rc = require_new_socket_path(options.management);
    if (rc == 0)
        rc = require_new_socket_path(options.io);
    if (rc != 0)
        goto cleanup;
    if (options.persistent) {
        rc = check_state_marker(directory_fd);
        if (rc != 0)
            goto cleanup;
    }
    rc = make_devices(&simulation.devices);
    if (rc != 0)
        goto cleanup;
    rc = dppd_control_init(&simulation.control, &simulation.devices.topology, SIM_RULE_CAPACITY, &simulated_api);
    if (rc != 0)
        goto cleanup;
    initialized = true;
    if (options.persistent) {
        rc = dppd_control_persistence_restore(&simulation.control, options.state);
        if (rc != 0)
            goto cleanup;
        if (dppd_software_backend_count(&simulation.control.software) != 0) {
            rc = -ENOTSUP;
            goto cleanup;
        }
    }
    rc = dppd_management_start(&simulation.management, &simulation.control, &simulation.devices, NULL,
        options.management);
    if (rc != 0)
        goto cleanup;
    rc = remember_socket(&management, simulation.management.socket_fd, options.management);
    if (rc != 0)
        goto cleanup;
    rc = start_io(&io, options.io);
    if (rc != 0)
        goto cleanup;
    printf("READY simulation=yes socket=%s io-socket=%s state=%s hardware-acceptance=not-run\n", options.management,
        options.io, options.persistent ? options.state : "none");
    while (!stopping) {
        poll_ages();
        struct pollfd ready[2] = {{.fd = simulation.management.socket_fd, .events = POLLIN},
            {.fd = io.fd, .events = POLLIN}};
        int polled = poll(ready, 2, 100);
        if (polled < 0) {
            if (errno == EINTR)
                continue;
            rc = -errno;
            break;
        }
        if ((ready[0].revents | ready[1].revents) & (POLLERR | POLLNVAL)) {
            rc = -EIO;
            break;
        }
        if ((ready[0].revents & POLLIN) != 0)
            rc = dppd_management_poll(&simulation.management);
        if (rc == 0 && (ready[1].revents & POLLIN) != 0)
            rc = poll_io(&io);
        if (rc != 0)
            break;
    }

cleanup:
    stop_io(&io);
    if (simulation.management.started) {
        /** 路径已被替换时只关自己的 fd，不让正式 stop 删除另一个对象 */
        if (!same_socket(&management))
            simulation.management.socket_path[0] = '\0';
        dppd_management_stop(&simulation.management);
    }
    memset(simulation.faults, 0, sizeof(simulation.faults));
    if (initialized) {
        int cleanup_rc = dppd_control_fini(&simulation.control);
        if (rc == 0)
            rc = cleanup_rc;
    }
    if (lock_fd >= 0)
        close(lock_fd);
    if (directory_fd >= 0)
        close(directory_fd);
    if (rc != 0)
        fprintf(stderr, "SIMULATION ONLY failed code=%d: %s\n", -rc, strerror(-rc));
    return rc == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
