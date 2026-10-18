#define _GNU_SOURCE
#include "dppd/flow_management.h"
#include "dppd/runtime.h"

#include <errno.h>
#include <string.h>
#include <sys/random.h>

int dppd_flow_management_init(struct dppd_flow_management *flows, struct dppd_flow_store *store)
{
    if (flows == NULL || store == NULL)
        return -EINVAL;
    memset(flows, 0, sizeof(*flows));
    ssize_t bytes;
    do {
        bytes = getrandom(&flows->session, sizeof(flows->session), 0);
    } while (bytes < 0 && errno == EINTR);
    if (bytes != sizeof(flows->session) || flows->session == 0)
        return bytes < 0 ? -errno : -EIO;
    flows->store = store;
    return 0;
}

/** 内部任务号还要匹配本次启动和已登记回执，客户端不能直接领取其他实例的任务 */
static int receipt(struct dppd_flow_management *flows, const struct dppd_management_flow_token *token)
{
    if (token->session != flows->session)
        return -ESTALE;
    for (uint32_t i = 0; i < 2048; ++i) {
        const struct dppd_flow_job_id *job = &flows->jobs[i];
        if (job->owner != 0 && job->owner == token->job.owner && job->serial == token->job.serial &&
            job->slot == token->job.slot)
            return (int)i;
    }
    return -ENOENT;
}

/** 就绪同时依赖配置可靠落盘、线程和设备状态，进程能够响应不等于能够转发 */
static void health(struct dppd_runtime *runtime, const struct dppd_flow_store_status *status,
    struct dppd_management_health *output)
{
    output->persistence_enabled = true;
    output->persistence_dirty = status->dirty;
    output->persisted_generation = status->durable_revision;
    output->repository_generation = status->service.revision;
    output->rule_count = status->service.rules;
    output->persistence_last_error = status->last_error;
    output->recovery_state =
        status->ready ? DPPD_CONTROL_RECOVERY_READY : DPPD_CONTROL_RECOVERY_RECONCILIATION_REQUIRED;
    if (!status->ready)
        output->blockers |= DPPD_HEALTH_RECOVERY_REQUIRED;
    if (status->dirty)
        output->blockers |= DPPD_HEALTH_PERSISTENCE_DIRTY;
    if (runtime == NULL) {
        output->blockers |= DPPD_HEALTH_NO_RUNTIME;
        return;
    }
    output->workers_expected = runtime->nb_workers;
    output->ports_expected = runtime->devices.nb_ports;
    for (uint16_t i = 0; i < runtime->nb_workers; ++i) {
        unsigned int state = atomic_load_explicit(&runtime->workers[i].state, memory_order_acquire);
        output->workers_running += state == DPPD_WORKER_RUNNING;
        output->workers_failed += state == DPPD_WORKER_FAILED;
    }
    if (!runtime->workers_started || output->workers_running != output->workers_expected)
        output->blockers |= DPPD_HEALTH_WORKERS_NOT_RUNNING;
    if (output->workers_failed != 0)
        output->blockers |= DPPD_HEALTH_WORKER_FAILED;
    if (atomic_load_explicit(&runtime->stop_requested, memory_order_acquire))
        output->blockers |= DPPD_HEALTH_STOP_REQUESTED;
    for (uint16_t i = 0; i < runtime->devices.nb_ports; ++i) {
        const struct dppd_port *port = &runtime->devices.ports[i];
        unsigned int link = atomic_load_explicit(&port->link_state, memory_order_acquire);
        output->ports_started += port->configured && port->started;
        output->links_up += link == DPPD_LINK_UP;
        output->links_down += link == DPPD_LINK_DOWN;
        output->links_unsupported += link == DPPD_LINK_UNSUPPORTED;
        output->links_unknown += link == DPPD_LINK_UNKNOWN;
        if (link == DPPD_LINK_DOWN)
            output->blockers |= DPPD_HEALTH_LINK_DOWN;
        if (link == DPPD_LINK_UNKNOWN)
            output->blockers |= DPPD_HEALTH_LINK_UNKNOWN;
        if (atomic_load_explicit(&port->removed, memory_order_acquire))
            output->blockers |= DPPD_HEALTH_DEVICE_REMOVED;
    }
    if (output->ports_started != output->ports_expected)
        output->blockers |= DPPD_HEALTH_PORTS_NOT_STARTED;
    output->ready = output->blockers == 0;
}

int dppd_flow_management_handle(struct dppd_flow_management *flows, struct dppd_runtime *runtime,
    const struct dppd_management_request *request, struct dppd_management_response *response)
{
    if (flows == NULL || flows->store == NULL || request == NULL || response == NULL)
        return -EINVAL;
    memset(response, 0, sizeof(*response));
    response->version = DPPD_MANAGEMENT_VERSION;
    response->operation = request->operation;
    response->size = sizeof(*response);
    response->request_id = request->request_id;
    if (request->version != DPPD_MANAGEMENT_VERSION || request->size != sizeof(*request)) {
        response->status = -EPROTO;
        return 0;
    }
    struct dppd_flow_store_status status;
    int rc = dppd_flow_store_status(flows->store, &status);
    if (rc != 0) {
        response->status = rc;
        return 0;
    }
    switch (request->operation) {
    case DPPD_MANAGEMENT_PING:
        response->payload.pong.repository_generation = status.service.revision;
        response->payload.pong.rule_count = status.service.rules;
        break;
    case DPPD_MANAGEMENT_HEALTH_GET:
        health(runtime, &status, &response->payload.health);
        break;
    case DPPD_MANAGEMENT_FLOW_STATUS:
        response->payload.flow_status.session = flows->session;
        response->payload.flow_status.status = status;
        break;
    case DPPD_MANAGEMENT_PERSISTENCE_STATUS:
        response->payload.persistence.enabled = true;
        response->payload.persistence.dirty = status.dirty;
        response->payload.persistence.persisted_generation = status.durable_revision;
        response->payload.persistence.current_generation = status.service.revision;
        response->payload.persistence.last_error = status.last_error;
        break;
    case DPPD_MANAGEMENT_PERSISTENCE_FLUSH:
    case DPPD_MANAGEMENT_FLOW_RETRY:
        /** 失去就绪后先让主循环停止并注销读者，再允许恢复动作改变实际对象 */
        if (!status.ready && runtime != NULL && runtime->workers_started)
            rc = -EBUSY;
        else
            rc = request->operation == DPPD_MANAGEMENT_PERSISTENCE_FLUSH ? dppd_flow_store_flush(flows->store)
                                                                         : dppd_flow_store_retry(flows->store);
        break;
    case DPPD_MANAGEMENT_RULE_GET:
        rc = dppd_flow_store_get(flows->store, request->payload.get.rule_id, &response->payload.rule);
        break;
    case DPPD_MANAGEMENT_RULE_COUNT_QUERY:
        response->payload.count.rule_id = request->payload.count_query.rule_id;
        response->payload.count.generation = request->payload.count_query.expected_generation;
        rc = dppd_flow_store_query_count(flows->store, request->payload.count_query.rule_id,
            request->payload.count_query.expected_generation, &response->payload.count.hits,
            &response->payload.count.bytes);
        break;
    case DPPD_MANAGEMENT_RULE_LIST: {
        struct dppd_flow_state_rule rules[4];
        uint32_t count;
        struct dppd_management_rule_page *page = &response->payload.rule_page;
        rc = dppd_flow_store_list(flows->store, request->payload.list.after_rule_id,
            request->payload.list.expected_repository_generation, rules, 4, &count, &page->repository_generation,
            &page->has_more);
        if (rc != 0)
            break;
        page->total_count = status.service.rules;
        page->count = (uint16_t)count;
        page->next_after_rule_id = request->payload.list.after_rule_id;
        for (uint32_t i = 0; i < count; ++i) {
            const struct dppd_rule *rule = &rules[i].rule;
            struct dppd_management_rule_summary *summary = &page->rules[i];
            summary->rule_id = rule->id;
            summary->generation = rule->generation;
            summary->group = rule->group;
            summary->priority = rule->priority;
            summary->domain = (uint8_t)rule->domain;
            summary->fallback = (uint8_t)rule->fallback;
            summary->install_port_id = rule->install_port_id;
            summary->nb_matches = rule->nb_matches;
            summary->nb_actions = rule->nb_actions;
            for (uint16_t m = 0; m < rule->nb_matches; ++m)
                summary->match_mask |= UINT32_C(1) << rule->matches[m].type;
            for (uint16_t a = 0; a < rule->nb_actions; ++a)
                summary->action_mask |= UINT32_C(1) << rule->actions[a].type;
            page->next_after_rule_id = rule->id;
        }
        break;
    }
    case DPPD_MANAGEMENT_STATS_QUERY:
        rc = runtime == NULL ? -ENOTSUP
                             : dppd_runtime_stats_query(runtime, request->payload.stats_query.port_id,
                                   request->payload.stats_query.queue_id, &response->payload.stats);
        break;
    case DPPD_MANAGEMENT_FLOW_CREATE:
    case DPPD_MANAGEMENT_FLOW_UPDATE:
    case DPPD_MANAGEMENT_FLOW_REMOVE: {
        uint32_t slot = 0;
        while (slot < 2048 && flows->jobs[slot].owner != 0)
            ++slot;
        if (slot == 2048) {
            rc = -ENOSPC;
            break;
        }
        uint32_t count = request->operation == DPPD_MANAGEMENT_FLOW_REMOVE ? request->payload.flow_remove.count
                                                                           : request->payload.flow_change.count;
        if (count == 0 || count > 4) {
            rc = -EINVAL;
            break;
        }
        if (request->operation == DPPD_MANAGEMENT_FLOW_CREATE) {
            for (uint32_t i = 0; i < count; ++i)
                if (request->payload.flow_change.expected[i] != 0)
                    rc = -EINVAL;
            if (rc != 0)
                break;
        }
        uint64_t now = dppd_monotonic_ms() * UINT64_C(1000000);
        struct dppd_flow_job_id job = {0};
        if (request->operation == DPPD_MANAGEMENT_FLOW_REMOVE)
            rc = dppd_flow_store_remove(flows->store, request->payload.flow_remove.queue,
                request->payload.flow_remove.rules, count, now, &job);
        else if (request->operation == DPPD_MANAGEMENT_FLOW_CREATE)
            rc = dppd_flow_store_submit(flows->store, request->payload.flow_change.queue,
                request->payload.flow_change.rules, count, now, &job);
        else
            rc = dppd_flow_store_update(flows->store, request->payload.flow_change.queue,
                request->payload.flow_change.rules, request->payload.flow_change.expected, count, now, &job);
        if (rc == 0) {
            flows->jobs[slot] = job;
            response->payload.flow_token = (struct dppd_management_flow_token){flows->session, job};
        }
        break;
    }
    case DPPD_MANAGEMENT_FLOW_COLLECT:
    case DPPD_MANAGEMENT_FLOW_CANCEL: {
        int slot = receipt(flows, &request->payload.flow_token);
        if (slot < 0) {
            rc = slot;
            break;
        }
        if (request->operation == DPPD_MANAGEMENT_FLOW_CANCEL)
            rc = dppd_flow_store_cancel(flows->store, &flows->jobs[slot]);
        else {
            rc = dppd_flow_store_collect(flows->store, &flows->jobs[slot], &response->payload.flow_result);
            if (rc == 0)
                memset(&flows->jobs[slot], 0, sizeof(flows->jobs[slot]));
        }
        break;
    }
    default:
        /** 旧控制面写入和硬件复位在此模式明确拒绝，不能落入另一个空账本 */
        rc = -ENOTSUP;
        break;
    }
    response->status = rc;
    if (request->operation == DPPD_MANAGEMENT_PERSISTENCE_FLUSH) {
        dppd_flow_store_status(flows->store, &status);
        response->payload.persistence.enabled = true;
        response->payload.persistence.dirty = status.dirty;
        response->payload.persistence.persisted_generation = status.durable_revision;
        response->payload.persistence.current_generation = status.service.revision;
        response->payload.persistence.last_error = status.last_error;
    } else if (request->operation == DPPD_MANAGEMENT_FLOW_RETRY) {
        response->payload.flow_status.session = flows->session;
        dppd_flow_store_status(flows->store, &response->payload.flow_status.status);
    }
    return 0;
}

int dppd_flow_management_drain(struct dppd_flow_management *flows)
{
    if (flows == NULL || flows->store == NULL)
        return -EINVAL;
    for (uint32_t i = 0; i < 2048; ++i)
        if (flows->jobs[i].owner != 0)
            (void)dppd_flow_store_cancel(flows->store, &flows->jobs[i]);
    for (uint32_t round = 0; round < 8192; ++round) {
        int rc = dppd_flow_store_process(flows->store, dppd_monotonic_ms() * UINT64_C(1000000), 64);
        if (rc < 0)
            return rc;
        bool pending = false;
        for (uint32_t i = 0; i < 2048; ++i) {
            if (flows->jobs[i].owner == 0)
                continue;
            struct dppd_flow_job_result result;
            rc = dppd_flow_store_collect(flows->store, &flows->jobs[i], &result);
            if (rc == 0)
                memset(&flows->jobs[i], 0, sizeof(flows->jobs[i]));
            else if (rc == -EAGAIN)
                pending = true;
            else
                return rc;
        }
        if (!pending)
            return 0;
    }
    return -EBUSY;
}
