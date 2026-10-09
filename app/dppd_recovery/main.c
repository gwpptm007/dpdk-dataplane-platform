#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dppd/recovery_guard.h"

/** 离线工具不初始化 EAL，也不访问设备，确认命令明确要求操作者已完成外部清理 */
static int usage(const char *program)
{
    fprintf(stderr, "usage: %s show PATH\n", program);
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

/** 查询和确认均持有排他锁，正在运行的 daemon 不允许离线工具改写或观察半成品 */
int main(int argc, char **argv)
{
    struct dppd_recovery_guard guard;
    uint64_t revision = 0;
    bool acknowledge = argc == 5 && strcmp(argv[1], "acknowledge-clean") == 0;
    int rc;

    if (!(argc == 3 && strcmp(argv[1], "show") == 0) && !acknowledge)
        return usage(argv[0]);
    if (acknowledge && (strcmp(argv[4], "--external-cleanup-complete") != 0 ||
        revision_number(argv[3], &revision) != 0))
        return usage(argv[0]);
    rc = dppd_recovery_guard_open(&guard, argv[2], NULL);
    if (rc != 0) {
        fprintf(stderr, "recovery record unavailable: %s (%d)\n", strerror(-rc), rc);
        return EXIT_FAILURE;
    }
    if (acknowledge) {
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
        for (uint32_t index = 0; index < guard.record.count; ++index) {
            const struct dppd_recovery_port *port = &guard.record.ports[index];
            printf("port=%u device=%s driver=%s first-rule=%" PRIu64 " first-generation=%" PRIu64 "\n",
                port->port_id, port->device, port->driver, port->first_rule, port->first_generation);
        }
    }
    dppd_recovery_guard_close(&guard);
    return rc == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
