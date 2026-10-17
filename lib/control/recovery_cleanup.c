#define _GNU_SOURCE
#include "dppd/recovery_cleanup.h"
#include "dppd/cleanup_journal.h"
#include "dppd/monitor_channel.h"
#include "dppd/recovery_plan.h"
#include "dppd/recovery_watch.h"
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/** 把监护登记但尚未安装规则的端口也纳入核对，避免漏掉跨端口复制的标识或未知对象 */
static int inventory_record(const struct dppd_recovery_guard *guard, const struct dppd_monitor_client *monitor,
    const struct dppd_recovery_scope *scope, struct dppd_recovery_record *inventory)
{
    *inventory = guard->record;
    for (uint32_t i = 0; i < monitor->record.count; ++i) {
        const struct dppd_recovery_identity *identity = &monitor->record.ports[i].identity;
        if (identity->netns_device != scope->netns_device || identity->netns_inode != scope->netns_inode)
            return -EXDEV;
        bool found = false;
        for (uint32_t p = 0; p < inventory->count; ++p)
            found |= inventory->ports[p].port_id == monitor->record.ports[i].port_id;
        if (!found) {
            if (inventory->count == DPPD_MAX_PORTS)
                return -E2BIG;
            struct dppd_recovery_port *port = &inventory->ports[inventory->count++];
            memset(port, 0, sizeof(*port));
            port->port_id = monitor->record.ports[i].port_id;
            port->identity = *identity;
            strcpy(port->driver, "net_tap");
        }
    }
    return 0;
}

static int observe(const struct dppd_recovery_record *inventory, struct dppd_recovery_inspection *inspections,
    struct dppd_recovery_plan *plan)
{
    struct dppd_recovery_watch watch = {.fd = -1};
    int errors[DPPD_MAX_PORTS] = {0};
    int rc = dppd_recovery_watch_open(&watch, inventory);
    if (rc != 0)
        return rc;
    /* TAP 最后一个业务描述符关闭会产生载波通知，连续性由身份事件和独立监护共同核对 */
    watch.identity_only_links = true;
    for (uint32_t i = 0; i < inventory->count; ++i) {
        errors[i] = dppd_recovery_inspect(&inventory->ports[i], &inspections[i]);
        if (errors[i] != 0 || inspections[i].state != DPPD_INSPECT_PRESENT) {
            rc = errors[i] != 0 ? errors[i] : -EXDEV;
            break;
        }
    }
    if (rc == 0)
        rc = dppd_recovery_watch_check(&watch);
    if (rc != 0)
        fprintf(stderr, "cleanup-observation error=%d link-changed=%u tc-changed=%u\n", rc, watch.link_changed,
            watch.tc_changed);
    dppd_recovery_watch_close(&watch);
    if (rc != 0)
        return rc;
    rc = dppd_recovery_plan_build(inventory, inspections, errors, 0, plan);
    if (rc != 0)
        return rc;
    if (!plan->inventory_complete)
        return -EUCLEAN;
    for (uint32_t i = 0; i < inventory->attempt_count; ++i) {
        enum dppd_recovery_plan_reason reason = plan->entries[i].reason;
        if (reason != DPPD_PLAN_MATCH && reason != DPPD_PLAN_REMOVED && reason != DPPD_PLAN_TOKEN_ABSENT) {
            fprintf(stderr, "cleanup refused attempt=%" PRIu64 " reason=%s\n", inventory->attempts[i].id,
                dppd_recovery_plan_reason_name(reason));
            return -EUCLEAN;
        }
    }
    if (plan->unassociated_filters != 0) {
        fprintf(stderr, "cleanup refused reason=unassociated-filters count=%u\n", plan->unassociated_filters);
        return -EUCLEAN;
    }
    return 0;
}

/** 日志不改原记录，所以每次仍能用原 revision 向监护核对租约和完整恢复文件 */
static int preflight(struct dppd_recovery_guard *guard, struct dppd_recovery_scope *scope,
    struct dppd_monitor_client *monitor, struct dppd_cleanup_journal *journal)
{
    int rc = dppd_scope_check(scope);
    if (rc == 0)
        rc = dppd_monitor_client_check(monitor);
    if (rc == 0)
        rc = dppd_cleanup_journal_check(journal);
    unsigned char crc[4];
    if (rc == 0) {
        ssize_t size = pread(guard->fd, crc, sizeof(crc), 28);
        if (size != sizeof(crc))
            rc = size < 0 ? -errno : -EIO;
        else {
            uint32_t current =
                (uint32_t)crc[0] | (uint32_t)crc[1] << 8 | (uint32_t)crc[2] << 16 | (uint32_t)crc[3] << 24;
            if (current != journal->recovery_crc)
                rc = -ESTALE;
        }
    }
    return rc;
}

int dppd_recovery_cleanup(struct dppd_recovery_guard *guard, const char *session, struct dppd_recovery_scope *scope)
{
    if (guard == NULL || scope == NULL || !scope->enabled || scope->role != DPPD_SCOPE_RECOVERY)
        return -EACCES;
    if (guard->fd < 0 || guard->faulted || guard->record.format != 6 || guard->record.count == 0 ||
        guard->record.attempt_count == 0)
        return -ENODATA;
    int rc = dppd_scope_check(scope);
    const char *stage = "scope";
    struct dppd_monitor_client monitor = {.fd = -1};
    struct dppd_cleanup_journal journal = {.fd = -1};
    struct dppd_recovery_record *inventory = calloc(1, sizeof(*inventory));
    struct dppd_recovery_inspection *inspections = calloc(DPPD_MAX_PORTS, sizeof(*inspections));
    struct dppd_recovery_plan *plan = calloc(1, sizeof(*plan));
    if (inventory == NULL || inspections == NULL || plan == NULL)
        rc = -ENOMEM;
    if (rc == 0) {
        stage = "lease";
        rc = dppd_monitor_recovery_open(&monitor, session, guard, guard->record.revision);
    }
    if (rc == 0)
        rc = dppd_monitor_client_check(&monitor);
    if (rc == 0)
        rc = inventory_record(guard, &monitor, scope, inventory);
    if (rc == 0) {
        stage = "inventory";
        rc = observe(inventory, inspections, plan);
    }
    if (rc == 0) {
        stage = "journal";
        rc = dppd_scope_check(scope);
        if (rc == 0)
            rc = dppd_monitor_client_check(&monitor);
        if (rc == 0)
            rc = dppd_cleanup_journal_open(&journal, guard, scope);
    }
    for (uint32_t i = 0; rc == 0 && i < guard->record.attempt_count; ++i) {
        stage = "attempt";
        struct dppd_recovery_plan_entry *entry = &plan->entries[i];
        if (journal.entries[i].state == DPPD_CLEANUP_REMOVED) {
            if (entry->token_matches != 0)
                rc = -EUCLEAN;
            continue;
        }
        rc = preflight(guard, scope, &monitor, &journal);
        if (rc == 0 && entry->reason == DPPD_PLAN_MATCH) {
            rc = dppd_cleanup_journal_set(&journal, i, DPPD_CLEANUP_DELETING, 0);
            if (rc == 0)
                rc = preflight(guard, scope, &monitor, &journal);
            if (rc == 0) {
                int removed = dppd_recovery_delete_filter(scope, &inventory->ports[entry->matched_port],
                    &inspections[entry->matched_port].filters[entry->matched_filter]);
                if (removed != 0) {
                    int saved = dppd_cleanup_journal_set(&journal, i, DPPD_CLEANUP_FAILED, removed);
                    rc = saved != 0 ? saved : removed;
                }
            }
            if (rc == 0)
                rc = observe(inventory, inspections, plan);
            if (rc == 0 && plan->entries[i].token_matches != 0)
                rc = -EUCLEAN;
        }
        if (rc == 0)
            rc = preflight(guard, scope, &monitor, &journal);
        if (rc == 0)
            rc = dppd_cleanup_journal_set(&journal, i, DPPD_CLEANUP_REMOVED, 0);
        if (rc == 0) {
            printf("cleanup-attempt id=%" PRIu64 " result=removed journal-sequence=%" PRIu64 "\n",
                guard->record.attempts[i].id, journal.sequence);
            fflush(stdout);
        }
    }
    if (rc == 0) {
        stage = "final-inventory";
        rc = observe(inventory, inspections, plan);
    }
    if (rc == 0 && plan->candidates != 0)
        rc = -EUCLEAN;
    for (uint32_t i = 0; rc == 0 && i < journal.count; ++i)
        if (journal.entries[i].state != DPPD_CLEANUP_REMOVED)
            rc = -EUCLEAN;
    if (rc == 0)
        rc = preflight(guard, scope, &monitor, &journal);
    if (rc == 0) {
        stage = "acknowledge";
        rc = dppd_recovery_guard_acknowledge(guard, journal.revision);
    }
    if (rc == 0)
        printf("cleanup-complete revision=%" PRIu64 " journal=%s\n", guard->record.revision, journal.path);
    else
        fprintf(stderr, "cleanup-stage name=%s error=%d\n", stage, rc);
    dppd_cleanup_journal_close(&journal);
    dppd_monitor_client_close(&monitor);
    free(inventory);
    free(inspections);
    free(plan);
    return rc;
}

bool dppd_recovery_cleanup_retryable(int error)
{
    return error == -EINTR || error == -EAGAIN || error == -ENOBUFS || error == -ETIMEDOUT || error == -EIO ||
        error == -ENOSPC || error == -EDQUOT || error == -EROFS;
}
