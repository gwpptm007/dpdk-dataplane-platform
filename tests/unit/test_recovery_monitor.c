#define _GNU_SOURCE
#include <assert.h>
#include <signal.h>
#include <sys/random.h>
#include <sys/wait.h>
#include <unistd.h>

static int test_sync(int fd);
static ssize_t test_random(void *buffer, size_t size, unsigned int flags);
static ssize_t test_write(int fd, const void *buffer, size_t size, off_t offset);
#define fsync test_sync
#define getrandom test_random
#define pwrite test_write
#include "../../lib/control/recovery_monitor.c"
#undef fsync
#undef getrandom
#undef pwrite

static unsigned int syncs, fail_sync;
static bool fail_random, fail_write;

/** 只在测试入口替换系统调用，故障恢复后也不能再次写入已经失效的会话 */
static int test_sync(int fd)
{
    if (++syncs == fail_sync) {
        errno = EIO;
        return -1;
    }
    return fsync(fd);
}

static ssize_t test_random(void *buffer, size_t size, unsigned int flags)
{
    if (fail_random) {
        errno = EIO;
        return -1;
    }
    return getrandom(buffer, size, flags);
}

static ssize_t test_write(int fd, const void *buffer, size_t size, off_t offset)
{
    if (fail_write) {
        if (offset == 0)
            return pwrite(fd, buffer, 40, offset);
        errno = ENOSPC;
        return -1;
    }
    return pwrite(fd, buffer, size, offset);
}

static struct dppd_monitor_record fixture(void)
{
    struct dppd_monitor_record record = {.count = 1,
        .recovery_inode = 41,
        .baseline_revision = 7,
        .baseline_attempt = 9,
        .recovery_path = "/test/recovery"};
    record.ports[0].port_id = 3;
    record.ports[0].identity = (struct dppd_recovery_identity){.ifindex = 17,
        .ifname = "owned",
        .boot_id = "6c307404-452c-475d-83b2-e5aaf3ae0102",
        .netns_device = 4,
        .netns_inode = 5};
    return record;
}

/** 校验和正确也不能放过非法状态、保留字段、重复身份或字符串尾部隐藏数据 */
static void test_encoding(const struct dppd_monitor_record *record)
{
    unsigned char bytes[MONITOR_SIZE];
    struct dppd_monitor_record decoded;
    encode(record, bytes);
    assert(decode(bytes, &decoded) == 0 && decoded.baseline_attempt == 9);
    bytes[80] ^= 1;
    assert(decode(bytes, &decoded) == -EBADMSG);
    const size_t offsets[] = {112, MONITOR_HEADER + 100, MONITOR_HEADER + DPPD_STATE_PATH_CAPACITY + 80,
        MONITOR_HEADER + DPPD_STATE_PATH_CAPACITY + MONITOR_PORT};
    for (unsigned int index = 0; index < sizeof(offsets) / sizeof(offsets[0]); ++index) {
        encode(record, bytes);
        bytes[offsets[index]] = 1;
        put(bytes + 28, checksum(bytes), 4);
        assert(decode(bytes, &decoded) == -EBADMSG);
    }
    decoded = *record;
    decoded.count = 2;
    decoded.ports[1] = decoded.ports[0];
    assert(!valid(&decoded));
    decoded.ports[1].port_id++;
    decoded.ports[1].identity.ifindex++;
    strcpy(decoded.ports[1].identity.ifname, "other");
    assert(valid(&decoded));
    decoded.ports[1].identity.netns_inode++;
    assert(!valid(&decoded));
    decoded = *record;
    decoded.ports[0].identity.boot_id[0] = 'z';
    assert(!valid(&decoded));
    decoded = *record;
    decoded.sequence = 2;
    assert(!valid(&decoded));
    decoded.state = DPPD_MONITOR_STOPPED;
    decoded.reason = DPPD_MONITOR_STOP_REQUESTED;
    decoded.error = -ECANCELED;
    assert(valid(&decoded));
    decoded.reason = DPPD_MONITOR_LINK_CHANGED;
    assert(!valid(&decoded));
}

static void test_store(const char *path)
{
    struct dppd_monitor_record record = fixture(), readback;
    struct dppd_monitor_store store = {.fd = -1}, second = {.fd = -1};
    bool locked;
    assert(dppd_monitor_create(&store, path, &record) == 0);
    assert(dppd_monitor_create(&store, path, &record) == -EALREADY);
    assert(dppd_monitor_create(&second, path, &record) == -EEXIST && second.fd == -1);
    assert(dppd_monitor_read(path, &readback, &locked) == 0 && locked);
    assert(readback.state == DPPD_MONITOR_ACTIVE && readback.sequence == 1);
    assert(memcmp(readback.session, store.record.session, sizeof(readback.session)) == 0);
    test_encoding(&readback);
    assert(dppd_monitor_finish(&store, DPPD_MONITOR_LINK_CHANGED, -ESTALE) == 0);
    assert(dppd_monitor_finish(&store, DPPD_MONITOR_STOP_REQUESTED, -ECANCELED) == -EALREADY);
    dppd_monitor_close(&store);
    assert(dppd_monitor_read(path, &readback, &locked) == 0 && !locked);
    assert(readback.state == DPPD_MONITOR_INVALID && readback.sequence == 2 && readback.error == -ESTALE);
    int reader = open(path, O_RDONLY | O_CLOEXEC);
    assert(reader >= 0 && flock(reader, LOCK_SH | LOCK_NB) == 0);
    assert(dppd_monitor_read(path, &readback, &locked) == 0 && !locked);
    close(reader);
    assert(chmod(path, 0640) == 0);
    assert(dppd_monitor_read(path, &readback, &locked) == -ESTALE);
    assert(chmod(path, 0600) == 0);
    assert(truncate(path, MONITOR_SIZE - 1) == 0);
    assert(dppd_monitor_read(path, &readback, &locked) == -EBADMSG);
    assert(unlink(path) == 0);
}

/** 杀死真正持有锁的子进程，旧 ACTIVE 字节保留，但新查询不能把它当作活动写入者 */
static void test_crash(const char *path)
{
    int ready[2];
    assert(pipe(ready) == 0);
    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        close(ready[0]);
        struct dppd_monitor_record record = fixture();
        struct dppd_monitor_store store = {.fd = -1};
        assert(dppd_monitor_create(&store, path, &record) == 0);
        assert(write(ready[1], "r", 1) == 1);
        for (;;)
            pause();
    }
    close(ready[1]);
    char byte;
    assert(read(ready[0], &byte, 1) == 1);
    close(ready[0]);
    struct dppd_monitor_record before, after;
    bool locked;
    assert(dppd_monitor_read(path, &before, &locked) == 0 && locked);
    assert(kill(child, SIGKILL) == 0);
    int status;
    assert(waitpid(child, &status, 0) == child && WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
    assert(dppd_monitor_read(path, &after, &locked) == 0 && !locked);
    assert(memcmp(&before, &after, sizeof(before)) == 0);
    assert(unlink(path) == 0);
}

static void test_failures(const char *path, const char *alias)
{
    struct dppd_monitor_record record = fixture(), readback;
    struct dppd_monitor_store store = {.fd = -1};
    bool locked;
    fail_random = true;
    assert(dppd_monitor_create(&store, path, &record) == -EIO && access(path, F_OK) != 0);
    fail_random = false;
    for (unsigned int sync = 1; sync <= 2; ++sync) {
        syncs = 0;
        fail_sync = sync;
        assert(dppd_monitor_create(&store, path, &record) == -EIO && store.faulted && store.fd == -1);
        assert(dppd_monitor_read(path, &readback, &locked) == 0 && !locked);
        assert(unlink(path) == 0);
    }
    fail_sync = 0;
    for (unsigned int partial = 0; partial <= 1; ++partial) {
        assert(dppd_monitor_create(&store, path, &record) == 0);
        fail_write = partial != 0;
        fail_sync = partial ? 0 : syncs + 1;
        assert(dppd_monitor_finish(&store, DPPD_MONITOR_OBSERVER_LOST, -ENOBUFS) == (partial ? -ENOSPC : -EIO));
        fail_write = false;
        fail_sync = 0;
        assert(dppd_monitor_finish(&store, DPPD_MONITOR_OBSERVER_LOST, -ENOBUFS) == -EUCLEAN);
        dppd_monitor_close(&store);
        if (partial)
            assert(dppd_monitor_read(path, &readback, &locked) == -EBADMSG);
        assert(unlink(path) == 0);
    }
    assert(dppd_monitor_create(&store, path, &record) == 0);
    assert(link(path, alias) == 0);
    assert(dppd_monitor_read(path, &readback, &locked) == -ESTALE);
    assert(unlink(alias) == 0);
    assert(rename(path, alias) == 0);
    int replacement = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    assert(replacement >= 0 && write(replacement, "sentinel", 8) == 8);
    close(replacement);
    assert(dppd_monitor_finish(&store, DPPD_MONITOR_RECORD_REPLACED, -ESTALE) == -ESTALE && store.faulted);
    struct stat info;
    assert(stat(path, &info) == 0 && info.st_size == 8);
    dppd_monitor_close(&store);
    assert(unlink(path) == 0 && symlink(alias, path) == 0);
    assert(dppd_monitor_read(path, &readback, &locked) == -ELOOP);
    assert(unlink(path) == 0 && unlink(alias) == 0);
    assert(mkfifo(path, 0600) == 0);
    assert(dppd_monitor_read(path, &readback, &locked) == -ESTALE);
    assert(unlink(path) == 0);
}

int main(void)
{
    char directory[] = "/tmp/dppd-monitor-unit-XXXXXX", path[256], alias[256];
    assert(mkdtemp(directory) != NULL);
    snprintf(path, sizeof(path), "%s/session", directory);
    snprintf(alias, sizeof(alias), "%s/alias", directory);
    test_store(path);
    test_crash(path);
    test_failures(path, alias);
    assert(rmdir(directory) == 0);
    return 0;
}
