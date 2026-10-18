#define _GNU_SOURCE
#include "flow_state_io.h"
#include "persistence_codec.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#define STATE_MAX_BYTES (32U * 1024U * 1024U)

static int table_index(const struct dppd_flow_state *state, uint64_t key)
{
    for (uint32_t i = 0; i < state->table_count; ++i)
        if (state->tables[i].key == key)
            return (int)i;
    return -1;
}

static bool shared_valid(const struct dppd_flow_state *state, const struct dppd_rule *rule)
{
    for (uint16_t a = 0; a < rule->nb_actions; ++a) {
        if (rule->actions[a].type != DPPD_ACTION_SHARED_COUNT)
            continue;
        bool found = false;
        for (uint32_t j = 0; j < state->shared_count; ++j)
            found |= state->counts[j].id == rule->actions[a].conf.shared_count.id &&
                state->counts[j].generation == rule->actions[a].conf.shared_count.generation &&
                state->counts[j].install_port_id == rule->install_port_id && state->counts[j].domain == rule->domain;
        if (!found)
            return false;
    }
    return true;
}

int dppd_flow_state_validate(const struct dppd_flow_state *state)
{
    struct dppd_admission *probe = NULL;
    uint64_t required[DPPD_RESOURCE_KIND_COUNT] = {0}, shared_refs = 0;
    uint32_t table_flows[64] = {0}, queue_flows[8] = {0};
    int port = -1, rc;
    if (state == NULL)
        return -EINVAL;
    const struct dppd_flow_state_config *c = &state->config;
    if ((c->mode != DPPD_FLOW_STATE_SOFTWARE && c->mode != DPPD_FLOW_STATE_NATIVE) || c->rule_capacity == 0 ||
        c->rule_capacity > 16384 || c->queue_count == 0 || c->queue_count > 8 || c->depth == 0 || c->depth > 256 ||
        c->timeout_ns == 0 || c->template_leases == 0 || c->template_leases > 65536 || c->shared_leases == 0 ||
        c->shared_leases > 65536 || state->table_count > 64 || state->shared_count > 64 ||
        state->rule_count > c->rule_capacity || (state->rule_count != 0 && state->rules == NULL) ||
        state->revision == UINT64_MAX || state->shared_next_generation == 0 || c->port_id == UINT16_MAX)
        return -EINVAL;
    rc = dppd_admission_create(&c->admission, &probe);
    if (rc != 0)
        return rc;
    dppd_admission_close(&probe);
    for (uint32_t p = 0; p < c->admission.port_count; ++p)
        if (c->admission.ports[p].port_id == c->port_id)
            port = (int)p;
    if (port < 0)
        return -EINVAL;
    for (uint32_t q = 0; q < 8; ++q)
        if (q < c->queue_count ? c->queue_capacity[q] == 0 || c->queue_capacity[q] > 65536 : c->queue_capacity[q] != 0)
            return -EINVAL;
    for (uint32_t i = 0; i < state->shared_count; ++i) {
        const struct dppd_shared_count_spec *s = &state->counts[i];
        if (s->id == 0 || s->id == UINT64_MAX || s->generation == 0 || s->generation >= state->shared_next_generation ||
            s->install_port_id != c->port_id || s->domain < DPPD_RULE_DOMAIN_INGRESS ||
            s->domain > DPPD_RULE_DOMAIN_TRANSFER || (i != 0 && state->counts[i - 1U].id >= s->id))
            return -EINVAL;
        for (uint32_t j = 0; j < i; ++j)
            if (s->generation == state->counts[j].generation)
                return -EINVAL;
    }
    for (uint32_t i = 0; i < state->table_count; ++i) {
        const struct dppd_flow_state_table *t = &state->tables[i];
        if (t->key == 0 || t->key == UINT64_MAX || (i != 0 && state->tables[i - 1U].key >= t->key) ||
            t->spec.example.install_port_id != c->port_id || !dppd_flow_template_matches(&t->spec, &t->spec.example) ||
            !shared_valid(state, &t->spec.example) ||
            (c->mode == DPPD_FLOW_STATE_SOFTWARE && !dppd_software_backend_rule_supported_managed(&t->spec.example)))
            return -EINVAL;
        for (uint16_t a = 0; a < t->spec.example.nb_actions; ++a)
            shared_refs += t->spec.example.actions[a].type == DPPD_ACTION_SHARED_COUNT;
    }
    for (uint32_t i = 0; i < state->rule_count; ++i) {
        const struct dppd_flow_state_rule *entry = &state->rules[i];
        const struct dppd_rule *r = &entry->rule;
        int t = table_index(state, entry->table_key);
        if (r->id == 0 || r->id == UINT64_MAX || r->generation == 0 || r->generation > state->revision ||
            r->install_port_id != c->port_id || (i != 0 && state->rules[i - 1U].rule.id >= r->id) ||
            entry->queue >= c->queue_count || t < 0 || !dppd_flow_template_matches(&state->tables[t].spec, r) ||
            !shared_valid(state, r) ||
            (c->mode == DPPD_FLOW_STATE_SOFTWARE ? r->fallback != DPPD_FALLBACK_SOFTWARE_ONLY
                                                 : r->fallback == DPPD_FALLBACK_SOFTWARE_ONLY))
            return -EINVAL;
        ++table_flows[t];
        ++queue_flows[entry->queue];
        for (uint16_t a = 0; a < r->nb_actions; ++a) {
            required[DPPD_RESOURCE_COUNTER] += r->actions[a].type == DPPD_ACTION_COUNT;
            required[DPPD_RESOURCE_AGING_OBJECT] += r->actions[a].type == DPPD_ACTION_AGE;
            shared_refs +=
                (c->mode == DPPD_FLOW_STATE_SOFTWARE ? 3U : 2U) * (r->actions[a].type == DPPD_ACTION_SHARED_COUNT);
        }
    }
    for (uint32_t i = 0; i < state->table_count; ++i)
        if (table_flows[i] > state->tables[i].spec.capacity)
            return -ENOSPC;
    for (uint32_t q = 0; q < c->queue_count; ++q)
        if (queue_flows[q] > c->queue_capacity[q])
            return -ENOSPC;
    required[DPPD_RESOURCE_RULE] = state->rule_count;
    required[c->mode == DPPD_FLOW_STATE_SOFTWARE ? DPPD_RESOURCE_SOFTWARE_RULE : DPPD_RESOURCE_HARDWARE_FLOW] =
        state->rule_count;
    required[DPPD_RESOURCE_TEMPLATE_TABLE] = state->table_count;
    required[DPPD_RESOURCE_SHARED_ACTION] = state->shared_count;
    required[DPPD_RESOURCE_COUNTER] += state->shared_count;
    required[DPPD_RESOURCE_FLOW_QUEUE] = c->queue_count;
    uint64_t inflight = state->rule_count < 64 ? state->rule_count : 64;
    if (inflight > c->depth)
        inflight = c->depth;
    required[DPPD_RESOURCE_INFLIGHT_OP] = inflight;
    uint64_t recovery_window = c->rule_capacity < 64 ? c->rule_capacity : 64;
    if (recovery_window > c->depth)
        recovery_window = c->depth;
    if (c->template_leases < 2U * c->rule_capacity || c->shared_leases < shared_refs ||
        c->admission.ticket_capacity <
            2U * c->rule_capacity + state->table_count + state->shared_count + c->queue_count + recovery_window)
        return -ENOSPC;
    for (uint32_t k = 0; k < DPPD_RESOURCE_KIND_COUNT; ++k)
        if (required[k] > c->admission.limits[k] || required[k] > c->admission.ports[port].limits[k])
            return -ENOSPC;
    if (c->mode == DPPD_FLOW_STATE_NATIVE &&
        (required[DPPD_RESOURCE_COUNTER] > c->native_counters ||
            required[DPPD_RESOURCE_AGING_OBJECT] > c->native_aging))
        return -ENOSPC;
    return 0;
}

struct codec {
    uint8_t *data;
    size_t size, offset;
    int error;
};

static void put(struct codec *c, uint64_t value)
{
    if (c->offset > c->size || c->size - c->offset < 8) {
        c->error = -EBADMSG;
        return;
    }
    for (unsigned int b = 0; b < 8; ++b)
        c->data[c->offset++] = (uint8_t)(value >> (8U * b));
}

static uint64_t get(struct codec *c)
{
    uint64_t value = 0;
    if (c->offset > c->size || c->size - c->offset < 8) {
        c->error = -EBADMSG;
        return 0;
    }
    for (unsigned int b = 0; b < 8; ++b)
        value |= (uint64_t)c->data[c->offset++] << (8U * b);
    return value;
}

/** 计算时把校验字段本身看作零，文件头和全部配置都一起参与，尾部损坏也不能漏掉 */
static uint32_t checksum(const uint8_t *data, size_t size)
{
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < size; ++i) {
        crc ^= i >= 24 && i < 32 ? 0 : data[i];
        for (unsigned int b = 0; b < 8; ++b)
            crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
    }
    return crc ^ UINT32_MAX;
}

static int wire_rule(struct codec *c, struct dppd_rule *rule, bool writing)
{
    if (c->offset > c->size || c->size - c->offset < DPPD_PERSISTENCE_RULE_BYTES)
        return -EBADMSG;
    int rc = writing ? dppd_persistence_rule_encode(rule, c->data + c->offset)
                     : dppd_persistence_rule_decode(c->data + c->offset, rule);
    c->offset += DPPD_PERSISTENCE_RULE_BYTES;
    return rc;
}

/** 所有数字显式编码为小端，模板示例允许零代次，单独保存后再复用已提交规则的规范编码 */
static int encode(const struct dppd_flow_state *s, uint8_t **output, size_t *size)
{
    size_t capacity = 4096U + DPPD_MAX_PORTS * 80U + s->shared_count * 32U +
        s->table_count * (48U + DPPD_PERSISTENCE_RULE_BYTES) + s->rule_count * (16U + DPPD_PERSISTENCE_RULE_BYTES);
    struct codec w = {.data = calloc(1, capacity), .size = capacity, .offset = 32};
    if (w.data == NULL)
        return -ENOMEM;
    const struct dppd_flow_state_config *c = &s->config;
    put(&w, c->mode);
    put(&w, c->port_id);
    put(&w, c->rule_capacity);
    put(&w, c->queue_count);
    put(&w, c->depth);
    put(&w, c->timeout_ns);
    put(&w, c->template_leases);
    put(&w, c->shared_leases);
    put(&w, c->native_counters);
    put(&w, c->native_aging);
    for (uint32_t q = 0; q < 8; ++q)
        put(&w, c->queue_capacity[q]);
    put(&w, c->admission.ticket_capacity);
    put(&w, c->admission.port_count);
    for (uint32_t k = 0; k < DPPD_RESOURCE_KIND_COUNT; ++k)
        put(&w, c->admission.limits[k]);
    for (uint32_t p = 0; p < c->admission.port_count; ++p) {
        put(&w, c->admission.ports[p].port_id);
        for (uint32_t k = 0; k < DPPD_RESOURCE_KIND_COUNT; ++k)
            put(&w, c->admission.ports[p].limits[k]);
    }
    put(&w, s->shared_count);
    put(&w, s->shared_next_generation);
    for (uint32_t i = 0; i < s->shared_count; ++i) {
        put(&w, s->counts[i].id);
        put(&w, s->counts[i].generation);
        put(&w, s->counts[i].install_port_id);
        put(&w, s->counts[i].domain);
    }
    put(&w, s->table_count);
    for (uint32_t i = 0; i < s->table_count; ++i) {
        struct dppd_rule example = s->tables[i].spec.example;
        put(&w, s->tables[i].key);
        put(&w, s->tables[i].spec.capacity);
        put(&w, s->tables[i].spec.dynamic_actions);
        put(&w, example.id);
        put(&w, example.generation);
        example.id = example.generation = 1;
        int rc = wire_rule(&w, &example, true);
        if (rc != 0) {
            free(w.data);
            return rc;
        }
    }
    put(&w, s->rule_count);
    put(&w, s->revision);
    for (uint32_t i = 0; i < s->rule_count; ++i) {
        struct dppd_rule rule = s->rules[i].rule;
        put(&w, s->rules[i].table_key);
        put(&w, s->rules[i].queue);
        int rc = wire_rule(&w, &rule, true);
        if (rc != 0) {
            free(w.data);
            return rc;
        }
    }
    if (w.error != 0) {
        free(w.data);
        return w.error;
    }
    *size = w.offset;
    memcpy(w.data, "DPPM4CFG", 8);
    w.offset = 8;
    put(&w, 1);
    put(&w, *size);
    put(&w, checksum(w.data, *size));
    *output = w.data;
    return 0;
}

static int decode(uint8_t *data, size_t size, struct dppd_flow_state **output)
{
    struct codec r = {data, size, 8, 0};
    if (size < 32 || memcmp(data, "DPPM4CFG", 8) != 0)
        return -EBADMSG;
    if (get(&r) != 1)
        return -EPROTONOSUPPORT;
    if (get(&r) != size || get(&r) != checksum(data, size))
        return -EBADMSG;
    struct dppd_flow_state *s = calloc(1, sizeof(*s));
    if (s == NULL)
        return -ENOMEM;
    struct dppd_flow_state_config *c = &s->config;
    c->mode = get(&r);
    c->port_id = get(&r);
    c->rule_capacity = get(&r);
    c->queue_count = get(&r);
    c->depth = get(&r);
    c->timeout_ns = get(&r);
    c->template_leases = get(&r);
    c->shared_leases = get(&r);
    c->native_counters = get(&r);
    c->native_aging = get(&r);
    for (uint32_t q = 0; q < 8; ++q)
        c->queue_capacity[q] = get(&r);
    c->admission.ticket_capacity = get(&r);
    uint64_t count = get(&r);
    int rc = -EBADMSG;
    if (count > DPPD_MAX_PORTS)
        goto fail;
    c->admission.port_count = count;
    for (uint32_t k = 0; k < DPPD_RESOURCE_KIND_COUNT; ++k)
        c->admission.limits[k] = get(&r);
    for (uint32_t p = 0; p < c->admission.port_count; ++p) {
        c->admission.ports[p].port_id = get(&r);
        for (uint32_t k = 0; k < DPPD_RESOURCE_KIND_COUNT; ++k)
            c->admission.ports[p].limits[k] = get(&r);
    }
    count = get(&r);
    if (count > 64)
        goto fail;
    s->shared_count = count;
    s->shared_next_generation = get(&r);
    for (uint32_t i = 0; i < s->shared_count; ++i) {
        s->counts[i].id = get(&r);
        s->counts[i].generation = get(&r);
        s->counts[i].install_port_id = get(&r);
        s->counts[i].domain = get(&r);
    }
    count = get(&r);
    if (count > 64)
        goto fail;
    s->table_count = count;
    for (uint32_t i = 0; i < s->table_count; ++i) {
        struct dppd_rule *example = &s->tables[i].spec.example;
        s->tables[i].key = get(&r);
        s->tables[i].spec.capacity = get(&r);
        s->tables[i].spec.dynamic_actions = get(&r);
        uint64_t id = get(&r), generation = get(&r);
        if (wire_rule(&r, example, false) != 0)
            goto fail;
        example->id = id;
        example->generation = generation;
    }
    count = get(&r);
    if (count > 16384)
        goto fail;
    s->rule_count = count;
    s->revision = get(&r);
    if (count != 0) {
        s->rules = calloc((size_t)count, sizeof(*s->rules));
        if (s->rules == NULL) {
            rc = -ENOMEM;
            goto fail;
        }
    }
    for (uint32_t i = 0; i < s->rule_count; ++i) {
        s->rules[i].table_key = get(&r);
        s->rules[i].queue = get(&r);
        if (wire_rule(&r, &s->rules[i].rule, false) != 0)
            goto fail;
    }
    if (r.error != 0 || r.offset != size)
        goto fail;
    rc = dppd_flow_state_validate(s);
    if (rc != 0) {
        if (rc != -ENOMEM)
            rc = -EBADMSG;
        goto fail;
    }
    /** 重新规范编码可同时拒绝窄整数截断、额外尾部及非零保留字段 */
    uint8_t *canonical = NULL;
    size_t length;
    rc = encode(s, &canonical, &length);
    if (rc != 0)
        goto fail;
    bool equal = length == size && memcmp(canonical, data, size) == 0;
    free(canonical);
    if (!equal) {
        rc = -EBADMSG;
        goto fail;
    }
    *output = s;
    return 0;
fail:
    dppd_flow_state_free(&s);
    return rc;
}

void dppd_flow_state_free(struct dppd_flow_state **state)
{
    if (state != NULL && *state != NULL) {
        free((*state)->rules);
        free(*state);
        *state = NULL;
    }
}

static int io_all(int fd, uint8_t *data, size_t size, bool writing)
{
    while (size != 0) {
        ssize_t done = writing ? write(fd, data, size) : read(fd, data, size);
        if (done < 0) {
            if (errno == EINTR)
                continue;
            return -errno;
        }
        if (done == 0)
            return writing ? -EIO : -EBADMSG;
        data += done;
        size -= (size_t)done;
    }
    return 0;
}

int dppd_flow_state_load(const char *path, struct dppd_flow_state **output)
{
    struct stat st;
    if (path == NULL || output == NULL || *output != NULL)
        return -EINVAL;
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0)
        return -errno;
    int rc = fstat(fd, &st) == 0 ? 0 : -errno;
    uint8_t *data = NULL;
    if (rc == 0 && (!S_ISREG(st.st_mode) || st.st_nlink != 1 || st.st_size < 32 || st.st_size > STATE_MAX_BYTES))
        rc = -EBADMSG;
    if (rc == 0) {
        data = malloc((size_t)st.st_size);
        rc = data == NULL ? -ENOMEM : io_all(fd, data, (size_t)st.st_size, false);
    }
    if (rc == 0)
        rc = decode(data, (size_t)st.st_size, output);
    free(data);
    close(fd);
    return rc;
}

static int lock_name(const char *path, char *output, size_t size)
{
    if (path == NULL || path[0] == '\0')
        return -EINVAL;
    return snprintf(output, size, "%s.lock", path) >= (int)size ? -ENAMETOOLONG : 0;
}

/** 固定父目录的绝对路径，服务随后切换工作目录也不会把配置写到另一个位置 */
int dppd_flow_state_path(const char *path, char **output)
{
    char input[PATH_MAX], parent[PATH_MAX], joined[PATH_MAX];
    if (path == NULL || output == NULL || *output != NULL || path[0] == '\0')
        return -EINVAL;
    if (snprintf(input, sizeof(input), "%s", path) >= (int)sizeof(input))
        return -ENAMETOOLONG;
    char *slash = strrchr(input, '/');
    char *name = slash == NULL ? input : slash + 1;
    if (name[0] == '\0' || strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
        return -EINVAL;
    if (slash != NULL)
        *slash = '\0';
    const char *directory = slash == NULL ? "." : slash == input ? "/" : input;
    if (realpath(directory, parent) == NULL)
        return -errno;
    if (snprintf(joined, sizeof(joined), "%s/%s", parent, name) >= (int)sizeof(joined))
        return -ENAMETOOLONG;
    *output = strdup(joined);
    return *output == NULL ? -ENOMEM : 0;
}

int dppd_flow_state_lock(const char *path)
{
    char name[PATH_MAX];
    struct stat st;
    int rc = lock_name(path, name, sizeof(name));
    if (rc != 0)
        return rc;
    int fd = open(name, O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK, 0600);
    if (fd < 0)
        return -errno;
    rc = fstat(fd, &st) == 0 ? 0 : -errno;
    if (rc == 0 && (!S_ISREG(st.st_mode) || st.st_nlink != 1 || st.st_uid != geteuid()))
        rc = -EPERM;
    if (rc == 0 && flock(fd, LOCK_EX | LOCK_NB) != 0)
        rc = errno == EWOULDBLOCK ? -EBUSY : -errno;
    if (rc != 0) {
        close(fd);
        return rc;
    }
    return fd;
}

/** 先在同目录写完并同步临时文件，再替换旧文件并同步目录，替换前失败时旧策略始终完整 */
int dppd_flow_state_write(const char *path, int guard, const struct dppd_flow_state *state)
{
    char name[PATH_MAX], temporary[PATH_MAX], parent[PATH_MAX];
    struct stat held, named;
    uint8_t *data = NULL;
    size_t size;
    int rc = dppd_flow_state_validate(state), fd = -1, directory = -1;
    if (rc != 0)
        return rc;
    rc = lock_name(path, name, sizeof(name));
    if (rc != 0)
        return rc;
    if (guard < 0 || fstat(guard, &held) != 0 || lstat(name, &named) != 0 || held.st_dev != named.st_dev ||
        held.st_ino != named.st_ino || held.st_nlink != 1 || !S_ISREG(named.st_mode))
        return -ESTALE;
    if (lstat(path, &named) == 0) {
        if (!S_ISREG(named.st_mode) || named.st_nlink != 1)
            return -EINVAL;
    } else if (errno != ENOENT)
        return -errno;
    if (snprintf(temporary, sizeof(temporary), "%s.tmp.XXXXXX", path) >= (int)sizeof(temporary) ||
        snprintf(parent, sizeof(parent), "%s", path) >= (int)sizeof(parent))
        return -ENAMETOOLONG;
    char *slash = strrchr(parent, '/');
    if (slash == NULL)
        strcpy(parent, ".");
    else if (slash == parent)
        slash[1] = '\0';
    else
        *slash = '\0';
    directory = open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory < 0)
        return -errno;
    rc = encode(state, &data, &size);
    if (rc != 0)
        goto done;
    fd = mkostemp(temporary, O_CLOEXEC);
    if (fd < 0) {
        rc = -errno;
        goto done;
    }
    rc = io_all(fd, data, size, true);
    if (rc == 0 && fsync(fd) != 0)
        rc = -errno;
    if (close(fd) != 0 && rc == 0)
        rc = -errno;
    fd = -1;
    if (rc == 0 && rename(temporary, path) != 0)
        rc = -errno;
    if (rc != 0)
        unlink(temporary);
    else if (fsync(directory) != 0)
        rc = -errno;
done:
    if (fd >= 0)
        close(fd);
    close(directory);
    free(data);
    return rc;
}

int dppd_flow_state_save(const char *path, const struct dppd_flow_state *state)
{
    char *canonical = NULL;
    int rc = dppd_flow_state_path(path, &canonical);
    if (rc != 0)
        return rc;
    int guard = dppd_flow_state_lock(canonical);
    if (guard < 0) {
        free(canonical);
        return guard;
    }
    rc = dppd_flow_state_write(canonical, guard, state);
    close(guard);
    free(canonical);
    return rc;
}

int dppd_flow_state_initialize(const char *path, const struct dppd_flow_state *state)
{
    struct stat st;
    char *canonical = NULL;
    int rc = dppd_flow_state_path(path, &canonical);
    if (rc != 0)
        return rc;
    int guard = dppd_flow_state_lock(canonical);
    if (guard < 0) {
        free(canonical);
        return guard;
    }
    rc = lstat(canonical, &st) == 0 ? -EEXIST : errno == ENOENT ? 0 : -errno;
    if (rc == 0)
        rc = dppd_flow_state_write(canonical, guard, state);
    close(guard);
    free(canonical);
    return rc;
}
