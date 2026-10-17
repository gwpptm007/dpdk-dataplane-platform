#define _GNU_SOURCE
#include <signal.h>
#include <unistd.h>
#include "dppd/cleanup_journal.h"
#include "dppd/recovery_cleanup.h"
#include "dppd/monitor_channel.h"

#define main recovery_cli_main
#include "../../app/dppd_recovery/main.c"
#undef main

static char window_path[DPPD_STATE_PATH_CAPACITY + 32];

/** 故障只编入专用测试工具，正式入口不接受文件或环境开关 */
static bool consume(const char *expected)
{
    FILE *file = fopen(window_path, "r");
    if (file == NULL)
        return false;
    char mode[64] = {0};
    bool matched = fgets(mode, sizeof(mode), file) != NULL && strcmp(mode, expected) == 0;
    fclose(file);
    if (!matched || unlink(window_path) != 0)
        return false;
    printf("cleanup-window mode=%s pid=%u\n", expected, (unsigned int)getpid());
    fflush(stdout);
    return true;
}

int __real_dppd_recovery_delete_filter(struct dppd_recovery_scope *scope, const struct dppd_recovery_port *port,
    const struct dppd_recovery_filter *filter);

int __wrap_dppd_recovery_delete_filter(struct dppd_recovery_scope *scope, const struct dppd_recovery_port *port,
    const struct dppd_recovery_filter *filter)
{
    if (consume("before-delete") || consume("journal-corrupt"))
        raise(SIGSTOP);
    if (consume("delete-fail"))
        return -EIO;
    int rc = __real_dppd_recovery_delete_filter(scope, port, filter);
    if (rc == 0 && consume("after-delete"))
        raise(SIGSTOP);
    return rc;
}

int __real_dppd_cleanup_journal_set(struct dppd_cleanup_journal *journal, uint32_t index, enum dppd_cleanup_state state,
    int error);

int __wrap_dppd_cleanup_journal_set(struct dppd_cleanup_journal *journal, uint32_t index, enum dppd_cleanup_state state,
    int error)
{
    if (state == DPPD_CLEANUP_REMOVED && consume("commit-fail"))
        return -ENOSPC;
    int rc = __real_dppd_cleanup_journal_set(journal, index, state, error);
    if (rc == 0 && state == DPPD_CLEANUP_REMOVED && consume("after-result"))
        raise(SIGSTOP);
    if (rc == 0 && state == DPPD_CLEANUP_FAILED) {
        printf("cleanup-window failure-saved error=%d sequence=%" PRIu64 "\n", error, journal->sequence);
        fflush(stdout);
    }
    return rc;
}

int __real_dppd_recovery_guard_acknowledge(struct dppd_recovery_guard *guard, uint64_t revision);

int __wrap_dppd_recovery_guard_acknowledge(struct dppd_recovery_guard *guard, uint64_t revision)
{
    int rc = __real_dppd_recovery_guard_acknowledge(guard, revision);
    if (rc == 0 && consume("after-ack"))
        raise(SIGSTOP);
    return rc;
}

int __real_dppd_monitor_recovery_open(struct dppd_monitor_client *client, const char *path,
    struct dppd_recovery_guard *guard, uint64_t revision);

int __wrap_dppd_monitor_recovery_open(struct dppd_monitor_client *client, const char *path,
    struct dppd_recovery_guard *guard, uint64_t revision)
{
    int rc = __real_dppd_monitor_recovery_open(client, path, guard, revision);
    if (rc == 0 && consume("before-inventory"))
        raise(SIGSTOP);
    return rc;
}

int main(int argc, char **argv)
{
    if (argc < 3 || snprintf(window_path, sizeof(window_path), "%s.test-window", argv[2]) >= (int)sizeof(window_path))
        return EXIT_FAILURE;
    return recovery_cli_main(argc, argv);
}
