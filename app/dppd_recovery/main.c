#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dppd/recovery_guard.h"
#include "dppd/recovery_inspect.h"
#include "dppd/recovery_plan.h"
#include "dppd/recovery_watch.h"

/** 离线工具不初始化 EAL，inspect 和 plan 只读内核，确认命令要求已完成外部清理 */
static int usage(const char *program)
{
    fprintf(stderr, "usage: %s show PATH\n", program);
    fprintf(stderr, "       %s inspect PATH REVISION\n", program);
    fprintf(stderr, "       %s plan PATH REVISION\n", program);
    fprintf(stderr, "       %s acknowledge-clean PATH REVISION --external-cleanup-complete\n", program);
    return EXIT_FAILURE;
}

/** 精确版本只接受十进制整数，符号、空白、溢出和尾随字符都不能作为清理确认 */
static int revision_number(const char *text, uint64_t *revision)
{
    char *end;
    unsigned long long value;
    if (text[0] < '0' || text[0] > '9')
        return -EINVAL;
    errno = 0;
    value = strtoull(text, &end, 10);
    if (errno != 0 || *end != '\0' || value > UINT64_MAX)
        return -EINVAL;
    *revision = value;
    return 0;
}

/** 阶段名称只说明本程序记录过什么，不将 created 等同于重启后仍拥有该对象 */
static const char *phase_name(enum dppd_recovery_phase phase)
{
    switch (phase) {
    case DPPD_RECOVERY_INTENT:
        return "intent";
    case DPPD_RECOVERY_CREATED:
        return "created";
    case DPPD_RECOVERY_CREATE_FAILED:
        return "create-failed";
    case DPPD_RECOVERY_REMOVED:
        return "removed";
    default:
        return "unknown";
    }
}

/** 单一新增是观察证据等级，外部程序也可能在同一时间安装规则，因此不称为所有权 */
static const char *evidence_name(enum dppd_recovery_evidence evidence)
{
    switch (evidence) {
    case DPPD_EVIDENCE_SINGLE_ADDITION:
        return "single-addition";
    case DPPD_EVIDENCE_AMBIGUOUS:
        return "ambiguous";
    case DPPD_EVIDENCE_UNAVAILABLE:
        return "unavailable";
    default:
        return "none";
    }
}

/** 核验结论独立于标识结论，旧记录缺少预期内容时不能报告匹配 */
static const char *verification_name(int error)
{
    switch (error) {
    case 0:
        return "match";
    case -ENODATA:
        return "unchecked";
    case -ESTALE:
        return "mismatch";
    case -EMLINK:
        return "shared-action";
    default:
        return "unsupported";
    }
}

/** 逐次安装线索可以关联当前坐标，但删除后再创建同坐标的对象无法凭这些字段排除 */
static void correlate_attempts(const struct dppd_recovery_record *record, uint16_t port_id,
    const struct dppd_recovery_inspection *inspection, int inspect_error)
{
    for (uint32_t index = 0; index < record->attempt_count; ++index) {
        const struct dppd_recovery_attempt *attempt = &record->attempts[index];
        const char *status = "no-candidate";
        const char *owner = "unknown";
        const char *content = "unchecked", *scope = "unchecked";
        if (attempt->port_id != port_id)
            continue;
        if (attempt->phase == DPPD_RECOVERY_REMOVED)
            status = "removed-record";
        else if (dppd_tap_cookie_present(attempt->owner_cookie)) {
            content = scope = attempt->expected.version != 0 ? "unavailable" : "unchecked";
            int matches =
                inspect_error != 0 ? inspect_error : dppd_recovery_cookie_matches(inspection, attempt->owner_cookie);
            status = matches < 0 ? "inspection-unavailable"
                : matches == 1   ? "token-present"
                : matches == 0   ? "token-absent"
                                 : "token-ambiguous";
            if (matches == 1) {
                owner = "token-match";
                for (uint32_t row = 0; row < inspection->count; ++row) {
                    const struct dppd_recovery_filter *filter = &inspection->filters[row];
                    if (!filter->cookie_valid ||
                        memcmp(filter->cookie, attempt->owner_cookie, DPPD_TAP_COOKIE_SIZE) != 0)
                        continue;
                    content = verification_name(dppd_recovery_verify(&attempt->expected, filter, false));
                    scope = verification_name(dppd_recovery_verify(&attempt->expected, filter, true));
                }
            }
        } else if (attempt->evidence == DPPD_EVIDENCE_SINGLE_ADDITION) {
            uint32_t matches = 0;
            for (uint32_t row = 0; row < inspection->count; ++row)
                matches += dppd_recovery_filter_equal(&attempt->candidate, &inspection->filters[row]);
            status = inspect_error != 0 ? "inspection-unavailable"
                : matches == 1          ? "coordinate-present"
                : matches == 0          ? "coordinate-absent"
                                        : "coordinate-ambiguous";
        }
        printf("correlation attempt=%" PRIu64 " rule=%" PRIu64 " generation=%" PRIu64
               " port=%u phase=%s evidence=%s status=%s owner=%s content=%s scope=%s\n",
            attempt->id, attempt->rule_id, attempt->generation, port_id, phase_name(attempt->phase),
            evidence_name(attempt->evidence), status, owner, content, scope);
    }
}

/** 标识、内容和范围分别展示，仍不是原子快照或清理确认，恢复标记保持原样 */
static int inspect_record(const struct dppd_recovery_record *record, uint64_t revision)
{
    int result = 0;
    if (record->revision != revision)
        return -ESTALE;
    printf("inspection revision=%" PRIu64 " ports=%u mode=read-only scope=tap-local-multiq-and-ingress\n", revision,
        record->count);
    printf("ownership=unproven remote-scope=excluded atomic-snapshot=no cleanup-confirmed=no\n");
    for (uint32_t index = 0; index < record->count; ++index) {
        const struct dppd_recovery_port *port = &record->ports[index];
        struct dppd_recovery_inspection inspection;
        int rc = dppd_recovery_inspect(port, &inspection);
        printf("port=%u device=%s driver=%s ifindex=%u ifname=%s status=%s complete=%s error=%d", port->port_id,
            port->device, port->driver, port->identity.ifindex,
            port->identity.ifindex ? port->identity.ifname : "unknown", dppd_recovery_inspection_name(inspection.state),
            rc == 0 ? "yes" : "no", rc);
        if (rc == 0)
            printf(" filters=%u", inspection.count);
        putchar('\n');
        correlate_attempts(record, port->port_id, &inspection, rc);
        if (rc != 0) {
            if (result == 0)
                result = rc;
            continue;
        }
        for (uint32_t row = 0; row < inspection.count; ++row) {
            const struct dppd_recovery_filter *filter = &inspection.filters[row];
            printf("filter port=%u parent=%08x handle=%08x chain=%u priority=%u protocol=%04x kind=%s owner=unknown\n",
                port->port_id, filter->parent, filter->handle, filter->chain, filter->priority, filter->protocol,
                filter->kind);
        }
    }
    return result;
}

/** 统一输出完整标识，不能用截断后的前缀定位内核对象 */
static void print_cookie(const uint8_t cookie[DPPD_TAP_COOKIE_SIZE])
{
    for (unsigned int byte = 0; byte < DPPD_TAP_COOKIE_SIZE; ++byte)
        printf("%02x", cookie[byte]);
}

/**
 * 先读取所有登记端口，再生成同一份计划，避免逐端口输出时漏掉跨端口复制的标识
 * 大块内核列表放在堆上，退出时统一释放，整个过程不产生删除请求或清理确认
 */
static int plan_record(const struct dppd_recovery_record *record, uint64_t revision)
{
    struct dppd_recovery_plan plan;
    struct dppd_recovery_watch watch = {.fd = -1};
    int errors[DPPD_MAX_PORTS] = {0};
    int result = 0;
    if (record->revision != revision)
        return -ESTALE;
    struct dppd_recovery_inspection *inspections = calloc(record->count ? record->count : 1, sizeof(*inspections));
    if (inspections == NULL)
        return -ENOMEM;
    /* 必须先订阅再查询，否则查询早期发生过的删除和重建可能已经被遗漏 */
    int watch_error = record->count ? dppd_recovery_watch_open(&watch, record) : 0;
    for (uint32_t port = 0; port < record->count; ++port) {
        errors[port] = dppd_recovery_inspect(&record->ports[port], &inspections[port]);
        if (result == 0)
            result = errors[port];
    }
    if (watch_error == 0 && record->count != 0)
        watch_error = dppd_recovery_watch_check(&watch);
    dppd_recovery_watch_close(&watch);
    if (result == 0)
        result = watch_error;
    int rc = dppd_recovery_plan_build(record, inspections, errors, watch_error, &plan);
    if (rc != 0) {
        free(inspections);
        return rc;
    }
    printf("plan version=1 revision=%" PRIu64 " format=%u ports=%u attempts=%u mode=read-only inventory-complete=%s\n",
        revision, record->format, record->count, record->attempt_count, plan.inventory_complete ? "yes" : "no");
    printf("scope=tap-local-multiq-and-ingress remote-scope=excluded ownership=unproven "
           "atomic-snapshot=no cleanup-confirmed=no executable=no\n");
    const char *window = "no-change-observed";
    if (record->count == 0)
        window = "not-needed";
    else if (watch.error != 0)
        window = "unavailable";
    else if (watch_error != 0)
        window = "changed";
    printf(
        "observation-window status=%s error=%d link-changed=%s tc-changed=%s history-covered=no writer-exclusion=no\n",
        window, watch_error, watch.link_changed ? "yes" : "no", watch.tc_changed ? "yes" : "no");
    printf("prerequisites interface-instance-continuity=unproven check-delete-exclusion=unavailable "
           "token-authenticity=unproven deletion-support=unimplemented\n");
    for (uint32_t port = 0; port < record->count; ++port) {
        const struct dppd_recovery_port *source = &record->ports[port];
        const struct dppd_recovery_identity *identity = &source->identity;
        uint32_t attempts = 0;
        for (uint32_t index = 0; index < record->attempt_count; ++index)
            attempts += record->attempts[index].port_id == source->port_id;
        printf("plan-port port=%u ifindex=%u ifname=%s boot=%s netns-device=%" PRIu64 " netns-inode=%" PRIu64
               " status=%s error=%d attempts=%u reason=%s\n",
            source->port_id, identity->ifindex, identity->ifindex ? identity->ifname : "unknown",
            identity->ifindex ? identity->boot_id : "unknown", identity->netns_device, identity->netns_inode,
            dppd_recovery_inspection_name(inspections[port].state), errors[port], attempts,
            attempts ? "attempts-recorded" : "no-attempt-record");
    }
    for (uint32_t index = 0; index < record->attempt_count; ++index) {
        const struct dppd_recovery_attempt *attempt = &record->attempts[index];
        const struct dppd_recovery_plan_entry *entry = &plan.entries[index];
        const char *decision = entry->reason == DPPD_PLAN_MATCH ? "candidate"
            : entry->reason == DPPD_PLAN_REMOVED                ? "excluded"
                                                                : "rejected";
        printf("plan-attempt attempt=%" PRIu64 " rule=%" PRIu64 " generation=%" PRIu64
               " port=%u phase=%s decision=%s reason=%s token-matches=%u content=%s scope=%s executable=no\n",
            attempt->id, attempt->rule_id, attempt->generation, attempt->port_id, phase_name(attempt->phase), decision,
            dppd_recovery_plan_reason_name(entry->reason), entry->token_matches,
            verification_name(entry->content_error), verification_name(entry->scope_error));
        if (entry->token_matches != 1)
            continue;
        const struct dppd_recovery_filter *filter = &inspections[entry->matched_port].filters[entry->matched_filter];
        printf("plan-object attempt=%" PRIu64
               " observed-port=%u parent=%08x handle=%08x chain=%u priority=%u protocol=%04x kind=%s cookie=",
            attempt->id, record->ports[entry->matched_port].port_id, filter->parent, filter->handle, filter->chain,
            filter->priority, filter->protocol, filter->kind);
        print_cookie(attempt->owner_cookie);
        putchar('\n');
    }
    printf("plan-summary candidates=%u rejected=%u excluded=%u ports-without-attempts=%u unassociated-filters=%u "
           "executable=no\n",
        plan.candidates, plan.rejected, plan.excluded, plan.ports_without_attempts, plan.unassociated_filters);
    free(inspections);
    return result != 0 ? result : (plan.inventory_complete ? 0 : -ENODATA);
}

/** 查询和确认均持有排他锁，正在运行的 daemon 不允许离线工具改写或观察半成品 */
int main(int argc, char **argv)
{
    struct dppd_recovery_guard guard;
    uint64_t revision = 0;
    bool acknowledge = argc == 5 && strcmp(argv[1], "acknowledge-clean") == 0;
    bool inspect = argc == 4 && strcmp(argv[1], "inspect") == 0;
    bool plan = argc == 4 && strcmp(argv[1], "plan") == 0;
    int rc;

    if (!(argc == 3 && strcmp(argv[1], "show") == 0) && !acknowledge && !inspect && !plan)
        return usage(argv[0]);
    if ((inspect || plan) && revision_number(argv[3], &revision) != 0)
        return usage(argv[0]);
    if (acknowledge &&
        (strcmp(argv[4], "--external-cleanup-complete") != 0 || revision_number(argv[3], &revision) != 0))
        return usage(argv[0]);
    rc = dppd_recovery_guard_open(&guard, argv[2], NULL);
    if (rc != 0) {
        fprintf(stderr, "recovery record unavailable: %s (%d)\n", strerror(-rc), rc);
        return EXIT_FAILURE;
    }
    if (plan) {
        rc = plan_record(&guard.record, revision);
        if (rc != 0)
            fprintf(stderr, "recovery plan incomplete: %s (%d)\n", strerror(-rc), rc);
    } else if (inspect) {
        rc = inspect_record(&guard.record, revision);
        if (rc != 0)
            fprintf(stderr, "recovery inspection incomplete: %s (%d)\n", strerror(-rc), rc);
    } else if (acknowledge) {
        rc = dppd_recovery_guard_acknowledge(&guard, revision);
        if (rc != 0)
            fprintf(stderr, "recovery acknowledgement failed: %s (%d)\n", strerror(-rc), rc);
        else
            printf("external cleanup acknowledged revision=%" PRIu64 "\n", guard.record.revision);
    } else {
        printf("recovery state=%s revision=%" PRIu64 " ports=%u\n",
            guard.record.count ? "external-reconciliation-required" : "clean", guard.record.revision,
            guard.record.count);
        printf("snapshot=%s\n", guard.record.state_path);
        printf("format=%u\n", guard.record.format);
        if (guard.record.format >= 6) {
            printf("monitor-required=yes session=");
            for (unsigned int index = 0; index < sizeof(guard.record.monitor_session); ++index)
                printf("%02x", guard.record.monitor_session[index]);
            printf(" freshness=unverified executable=no\n");
        }
        if (guard.record.format >= 3)
            printf("attempts=%u last-attempt=%" PRIu64 " capacity=%u\n", guard.record.attempt_count,
                guard.record.last_attempt, DPPD_RECOVERY_ATTEMPT_LIMIT);
        for (uint32_t index = 0; index < guard.record.count; ++index) {
            const struct dppd_recovery_port *port = &guard.record.ports[index];
            printf("port=%u device=%s driver=%s first-rule=%" PRIu64 " first-generation=%" PRIu64 "\n", port->port_id,
                port->device, port->driver, port->first_rule, port->first_generation);
            if (port->identity.ifindex != 0)
                printf("identity port=%u ifindex=%u ifname=%s boot=%s netns-device=%" PRIu64 " netns-inode=%" PRIu64
                       "\n",
                    port->port_id, port->identity.ifindex, port->identity.ifname, port->identity.boot_id,
                    port->identity.netns_device, port->identity.netns_inode);
        }
        for (uint32_t index = 0; index < guard.record.attempt_count; ++index) {
            const struct dppd_recovery_attempt *attempt = &guard.record.attempts[index];
            printf(
                "attempt=%" PRIu64 " rule=%" PRIu64 " generation=%" PRIu64
                " port=%u phase=%s create-error=%d remove-error=%d evidence=%s observation-error=%d ownership=unproven\n",
                attempt->id, attempt->rule_id, attempt->generation, attempt->port_id, phase_name(attempt->phase),
                attempt->create_error, attempt->remove_error, evidence_name(attempt->evidence),
                attempt->observation_error);
            if (dppd_tap_cookie_present(attempt->owner_cookie)) {
                printf("owner-token attempt=%" PRIu64 " cookie=", attempt->id);
                print_cookie(attempt->owner_cookie);
                putchar('\n');
            }
            if (attempt->evidence == DPPD_EVIDENCE_SINGLE_ADDITION)
                printf("candidate attempt=%" PRIu64
                       " parent=%08x handle=%08x chain=%u priority=%u protocol=%04x kind=%s\n",
                    attempt->id, attempt->candidate.parent, attempt->candidate.handle, attempt->candidate.chain,
                    attempt->candidate.priority, attempt->candidate.protocol, attempt->candidate.kind);
        }
    }
    dppd_recovery_guard_close(&guard);
    return rc == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
