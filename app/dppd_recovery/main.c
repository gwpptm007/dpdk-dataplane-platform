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
        for (uint32_t index = 0; index < guard.record.count; ++index) {
            const struct dppd_recovery_port *port = &guard.record.ports[index];
            printf("port=%u device=%s driver=%s first-rule=%" PRIu64 " first-generation=%" PRIu64 "\n",
                port->port_id, port->device, port->driver, port->first_rule, port->first_generation);
            if (port->identity.ifindex != 0)
                printf("identity port=%u ifindex=%u ifname=%s boot=%s netns-device=%" PRIu64 " netns-inode=%" PRIu64 "\n",
                    port->port_id, port->identity.ifindex, port->identity.ifname, port->identity.boot_id,
                    port->identity.netns_device, port->identity.netns_inode);
        }
    }
    dppd_recovery_guard_close(&guard);
    return rc == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
