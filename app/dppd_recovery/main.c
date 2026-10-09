#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dppd/recovery_guard.h"
#include "dppd/recovery_inspect.h"

/** 离线工具不初始化 EAL，inspect 只读内核，确认命令仍明确要求操作者已完成外部清理 */
static int usage(const char *program)
{
    fprintf(stderr, "usage: %s show PATH\n", program);
    fprintf(stderr, "       %s inspect PATH REVISION\n", program);
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
    case DPPD_RECOVERY_INTENT: return "intent";
    case DPPD_RECOVERY_CREATED: return "created";
    case DPPD_RECOVERY_CREATE_FAILED: return "create-failed";
    case DPPD_RECOVERY_REMOVED: return "removed";
    default: return "unknown";
    }
}

/** 单一新增是观察证据等级，外部程序也可能在同一时间安装规则，因此不称为所有权 */
static const char *evidence_name(enum dppd_recovery_evidence evidence)
{
    switch (evidence) {
    case DPPD_EVIDENCE_SINGLE_ADDITION: return "single-addition";
    case DPPD_EVIDENCE_AMBIGUOUS: return "ambiguous";
    case DPPD_EVIDENCE_UNAVAILABLE: return "unavailable";
    default: return "none";
    }
}

/** 逐次安装线索可以关联当前坐标，但删除后再创建同坐标的对象无法凭这些字段排除 */
static void correlate_attempts(const struct dppd_recovery_record *record, uint16_t port_id,
    const struct dppd_recovery_inspection *inspection, int inspect_error)
{
    for (uint32_t index = 0; index < record->attempt_count; ++index) {
        const struct dppd_recovery_attempt *attempt = &record->attempts[index];
        const char *status = "no-candidate";
        if (attempt->port_id != port_id)
            continue;
        if (attempt->phase == DPPD_RECOVERY_REMOVED)
            status = "removed-record";
        else if (attempt->evidence == DPPD_EVIDENCE_SINGLE_ADDITION) {
            uint32_t matches = 0;
            for (uint32_t row = 0; row < inspection->count; ++row)
                matches += dppd_recovery_filter_equal(&attempt->candidate, &inspection->filters[row]);
            status = inspect_error != 0 ? "inspection-unavailable" : matches == 1 ? "coordinate-present" :
                matches == 0 ? "coordinate-absent" : "coordinate-ambiguous";
        }
        printf("correlation attempt=%" PRIu64 " rule=%" PRIu64 " generation=%" PRIu64
            " port=%u phase=%s evidence=%s status=%s owner=unknown\n", attempt->id, attempt->rule_id,
            attempt->generation, port_id, phase_name(attempt->phase), evidence_name(attempt->evidence), status);
    }
}

/** 查询成功只代表本地范围已读完，所有规则仍显示归属未知，恢复标记保持原样 */
static int inspect_record(const struct dppd_recovery_record *record, uint64_t revision)
{
    int result = 0;
    if (record->revision != revision)
        return -ESTALE;
    printf("inspection revision=%" PRIu64 " ports=%u mode=read-only scope=tap-local-multiq-and-ingress\n",
        revision, record->count);
    printf("ownership=unproven remote-scope=excluded atomic-snapshot=no cleanup-confirmed=no\n");
    for (uint32_t index = 0; index < record->count; ++index) {
        const struct dppd_recovery_port *port = &record->ports[index];
        struct dppd_recovery_inspection inspection;
        int rc = dppd_recovery_inspect(port, &inspection);
        printf("port=%u device=%s driver=%s ifindex=%u ifname=%s status=%s complete=%s error=%d",
            port->port_id, port->device, port->driver, port->identity.ifindex,
            port->identity.ifindex ? port->identity.ifname : "unknown",
            dppd_recovery_inspection_name(inspection.state), rc == 0 ? "yes" : "no", rc);
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
                port->port_id, filter->parent, filter->handle, filter->chain,
                filter->priority, filter->protocol, filter->kind);
        }
    }
    return result;
}

/** 查询和确认均持有排他锁，正在运行的 daemon 不允许离线工具改写或观察半成品 */
int main(int argc, char **argv)
{
    struct dppd_recovery_guard guard;
    uint64_t revision = 0;
    bool acknowledge = argc == 5 && strcmp(argv[1], "acknowledge-clean") == 0;
    bool inspect = argc == 4 && strcmp(argv[1], "inspect") == 0;
    int rc;

    if (!(argc == 3 && strcmp(argv[1], "show") == 0) && !acknowledge && !inspect)
        return usage(argv[0]);
    if (inspect && revision_number(argv[3], &revision) != 0)
        return usage(argv[0]);
    if (acknowledge && (strcmp(argv[4], "--external-cleanup-complete") != 0 ||
        revision_number(argv[3], &revision) != 0))
        return usage(argv[0]);
    rc = dppd_recovery_guard_open(&guard, argv[2], NULL);
    if (rc != 0) {
        fprintf(stderr, "recovery record unavailable: %s (%d)\n", strerror(-rc), rc);
        return EXIT_FAILURE;
    }
    if (inspect) {
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
            guard.record.count ? "external-reconciliation-required" : "clean",
            guard.record.revision, guard.record.count);
        printf("snapshot=%s\n", guard.record.state_path);
        printf("format=%u\n", guard.record.format);
        if (guard.record.format == 3)
            printf("attempts=%u last-attempt=%" PRIu64 " capacity=%u\n", guard.record.attempt_count,
                guard.record.last_attempt, DPPD_RECOVERY_ATTEMPT_LIMIT);
        for (uint32_t index = 0; index < guard.record.count; ++index) {
            const struct dppd_recovery_port *port = &guard.record.ports[index];
            printf("port=%u device=%s driver=%s first-rule=%" PRIu64 " first-generation=%" PRIu64 "\n",
                port->port_id, port->device, port->driver, port->first_rule, port->first_generation);
            if (port->identity.ifindex != 0)
                printf("identity port=%u ifindex=%u ifname=%s boot=%s netns-device=%" PRIu64 " netns-inode=%" PRIu64 "\n",
                    port->port_id, port->identity.ifindex, port->identity.ifname, port->identity.boot_id,
                    port->identity.netns_device, port->identity.netns_inode);
        }
        for (uint32_t index = 0; index < guard.record.attempt_count; ++index) {
            const struct dppd_recovery_attempt *attempt = &guard.record.attempts[index];
            printf("attempt=%" PRIu64 " rule=%" PRIu64 " generation=%" PRIu64
                " port=%u phase=%s create-error=%d remove-error=%d evidence=%s observation-error=%d ownership=unproven\n",
                attempt->id, attempt->rule_id, attempt->generation, attempt->port_id, phase_name(attempt->phase),
                attempt->create_error, attempt->remove_error, evidence_name(attempt->evidence), attempt->observation_error);
            if (attempt->evidence == DPPD_EVIDENCE_SINGLE_ADDITION)
                printf("candidate attempt=%" PRIu64 " parent=%08x handle=%08x chain=%u priority=%u protocol=%04x kind=%s\n",
                    attempt->id, attempt->candidate.parent, attempt->candidate.handle, attempt->candidate.chain,
                    attempt->candidate.priority, attempt->candidate.protocol, attempt->candidate.kind);
        }
    }
    dppd_recovery_guard_close(&guard);
    return rc == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
