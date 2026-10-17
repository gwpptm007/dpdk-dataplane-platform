#define _GNU_SOURCE
#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "dppd/persistence.h"

#define SNAPSHOT_HEADER_SIZE 48U
#define SNAPSHOT_V1_RECORD_SIZE 260U
#define SNAPSHOT_V2_RECORD_SIZE 264U
#define SNAPSHOT_V3_RECORD_SIZE 648U
#define SNAPSHOT_CHECKSUM_OFFSET 40U

static void put_u32_le(uint8_t *output, uint32_t value)
{
    output[0] = (uint8_t)value;
    output[1] = (uint8_t)(value >> 8);
    output[2] = (uint8_t)(value >> 16);
    output[3] = (uint8_t)(value >> 24);
}

static void put_u64_le(uint8_t *output, uint64_t value)
{
    uint8_t i;

    for (i = 0; i < 8; ++i)
        output[i] = (uint8_t)(value >> (8U * i));
}

static uint32_t crc32_update(uint32_t crc, const uint8_t *data, size_t length)
{
    size_t i;

    for (i = 0; i < length; ++i) {
        uint8_t bit;

        crc ^= data[i];
        for (bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
    }
    return crc;
}

static void write_all_or_die(int fd, const uint8_t *data, size_t length)
{
    while (length != 0) {
        ssize_t written = write(fd, data, length);

        assert(written > 0);
        data += written;
        length -= (size_t)written;
    }
}

/*
 * 手工构造最小合法 v1 文件，避免用当前 v2 写入器伪造兼容性测试。v1 的 rule
 * header 只有 36 字节，match/action 区紧随其后，故能检验迁移读取是否真的
 * 采用旧布局而非仅接受 version 字段
 */
static void write_v1_snapshot(const char *path)
{
    static const uint8_t magic[8] = {
        'D',
        'P',
        'P',
        'R',
        'U',
        'L',
        'E',
        '\0',
    };
    uint8_t header[SNAPSHOT_HEADER_SIZE];
    uint8_t record[SNAPSHOT_V1_RECORD_SIZE];
    uint32_t checksum;
    int fd;

    memset(header, 0, sizeof(header));
    memset(record, 0, sizeof(record));
    memcpy(header, magic, sizeof(magic));
    put_u32_le(header + 8, 1);
    put_u32_le(header + 12, SNAPSHOT_HEADER_SIZE);
    put_u32_le(header + 16, SNAPSHOT_V1_RECORD_SIZE);
    put_u32_le(header + 20, 1);
    put_u64_le(header + 24, 9);
    put_u64_le(header + 32, sizeof(record));

    put_u64_le(record, 77);
    put_u64_le(record + 8, 4);
    put_u32_le(record + 16, DPPD_RULE_DOMAIN_INGRESS);
    put_u32_le(record + 20, DPPD_FALLBACK_REQUIRE_HARDWARE);
    put_u32_le(record + 24, 2);
    put_u32_le(record + 28, 8);
    record[32] = 1; /* 一个 ETH match，无额外字段 */
    record[34] = 1; /* 一个 DROP action，无额外字段 */
    put_u32_le(record + 36, DPPD_MATCH_ETH);
    put_u32_le(record + 36 + 8 * 20, DPPD_ACTION_DROP);

    checksum = crc32_update(UINT32_MAX, header, sizeof(header));
    checksum = crc32_update(checksum, record, sizeof(record)) ^ UINT32_MAX;
    put_u32_le(header + SNAPSHOT_CHECKSUM_OFFSET, checksum);
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    assert(fd >= 0);
    write_all_or_die(fd, header, sizeof(header));
    write_all_or_die(fd, record, sizeof(record));
    assert(fsync(fd) == 0);
    assert(close(fd) == 0);
}

static struct dppd_rule make_rule(uint64_t id, uint32_t mark_id)
{
    struct dppd_rule rule;

    memset(&rule, 0, sizeof(rule));
    rule.id = id;
    rule.install_port_id = id == 10 ? 5 : 7;
    rule.domain = DPPD_RULE_DOMAIN_INGRESS;
    rule.fallback = DPPD_FALLBACK_REQUIRE_HARDWARE;
    rule.group = 3;
    rule.priority = (uint32_t)id;
    rule.nb_matches = 3;
    rule.matches[0].type = DPPD_MATCH_ETH;
    rule.matches[1].type = DPPD_MATCH_IPV4;
    rule.matches[1].spec.ipv4.src_be = htonl(0x0a010000U);
    rule.matches[1].spec.ipv4.src_mask_be = htonl(0xffff0000U);
    rule.matches[1].spec.ipv4.dst_be = htonl(0xc0a80101U);
    rule.matches[1].spec.ipv4.dst_mask_be = htonl(UINT32_MAX);
    rule.matches[2].type = DPPD_MATCH_TCP;
    rule.matches[2].spec.l4.src_mask_be = 0;
    rule.matches[2].spec.l4.dst_be = htons(443);
    rule.matches[2].spec.l4.dst_mask_be = htons(UINT16_MAX);
    rule.nb_actions = 3;
    rule.actions[0].type = DPPD_ACTION_MARK;
    rule.actions[0].conf.mark_id = mark_id;
    rule.actions[1].type = DPPD_ACTION_COUNT;
    rule.actions[2].type = DPPD_ACTION_DROP;
    return rule;
}

/** 手写各版本布局和校验和，避免用新写入器冒充旧格式兼容样本 */
static void write_action_snapshot(const char *path, uint32_t version, uint32_t action_type, uint32_t parameter)
{
    uint8_t header[SNAPSHOT_HEADER_SIZE] = {'D', 'P', 'P', 'R', 'U', 'L', 'E', '\0'};
    uint8_t record[SNAPSHOT_V3_RECORD_SIZE] = {0};
    uint32_t rule_header_size = version == 1 ? 36U : 40U;
    uint32_t match_size = version >= 3 ? 68U : 20U;
    uint32_t record_size = version == 1 ? SNAPSHOT_V1_RECORD_SIZE
        : version == 2                  ? SNAPSHOT_V2_RECORD_SIZE
                                        : SNAPSHOT_V3_RECORD_SIZE;
    uint8_t *action = record + rule_header_size + 8U * match_size;
    bool terminal = action_type == DPPD_ACTION_ALLOW || action_type == DPPD_ACTION_DROP;
    int fd;

    put_u32_le(header + 8, version);
    put_u32_le(header + 12, sizeof(header));
    put_u32_le(header + 16, record_size);
    put_u32_le(header + 20, 1);
    put_u64_le(header + 24, 9);
    put_u64_le(header + 32, record_size);
    put_u64_le(record, 77);
    put_u64_le(record + 8, 4);
    put_u32_le(record + 16, DPPD_RULE_DOMAIN_INGRESS);
    put_u32_le(record + 20, DPPD_FALLBACK_REQUIRE_HARDWARE);
    record[32] = version >= 3 ? 2 : 1;
    record[34] = terminal ? 1 : 2;
    if (version != 1)
        record[36] = 13;
    put_u32_le(record + rule_header_size, DPPD_MATCH_ETH);
    if (version >= 3) {
        uint8_t *ipv6 = record + rule_header_size + match_size;
        put_u32_le(ipv6, DPPD_MATCH_IPV6);
        ipv6[4] = 0x20;
        ipv6[5] = 0x01;
        ipv6[19] = 1;
        memset(ipv6 + 20, 0xff, 16);
        ipv6[36] = 0x20;
        ipv6[37] = 0x01;
        ipv6[51] = 2;
        memset(ipv6 + 52, 0xff, 16);
    }
    put_u32_le(action, action_type);
    put_u32_le(action + 4, parameter);
    if (!terminal)
        put_u32_le(action + 8, DPPD_ACTION_DROP);
    uint32_t checksum = crc32_update(UINT32_MAX, header, sizeof(header));
    checksum = crc32_update(checksum, record, record_size) ^ UINT32_MAX;
    put_u32_le(header + SNAPSHOT_CHECKSUM_OFFSET, checksum);
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    assert(fd >= 0);
    write_all_or_die(fd, header, sizeof(header));
    write_all_or_die(fd, record, record_size);
    assert(close(fd) == 0);
}

/** AGE 配置完整往返，普通规则保持旧版本，旧解码器不能把 AGE 当成 ALLOW */
static void age_snapshot_compatibility(void)
{
    struct dppd_persisted_snapshot snapshot;
    char path[128];
    snprintf(path, sizeof(path), "/tmp/dppd-age-persistence-%ld.bin", (long)getpid());
    assert(DPPD_ACTION_ALLOW == 5 && DPPD_ACTION_AGE == 6);

    for (uint32_t version = 1; version <= 4; ++version) {
        write_action_snapshot(path, version, DPPD_ACTION_ALLOW, 0);
        int rc = version == 1 ? dppd_persistence_load_v1_for_migration(path, &snapshot)
                              : dppd_persistence_load(path, &snapshot);
        assert(rc == 0 && snapshot.count == 1 && snapshot.repository_generation == 9);
        assert(snapshot.rules[0].actions[0].type == DPPD_ACTION_ALLOW && snapshot.rules[0].nb_actions == 1);
        assert(snapshot.rules[0].install_port_id == (version == 1 ? 0 : 13));
        if (version >= 3) {
            assert(snapshot.rules[0].matches[1].type == DPPD_MATCH_IPV6);
            assert(snapshot.rules[0].matches[1].spec.ipv6.src[15] == 1);
            assert(snapshot.rules[0].matches[1].spec.ipv6.dst[15] == 2);
        }
        dppd_persisted_snapshot_destroy(&snapshot);

        write_action_snapshot(path, version, DPPD_ACTION_AGE, 12345);
        rc = version == 1 ? dppd_persistence_load_v1_for_migration(path, &snapshot)
                          : dppd_persistence_load(path, &snapshot);
        assert(rc == (version == 4 ? 0 : -EBADMSG));
        if (rc == 0) {
            assert(snapshot.rules[0].actions[0].type == DPPD_ACTION_AGE);
            assert(snapshot.rules[0].actions[0].conf.age_timeout_seconds == 12345);
            dppd_persisted_snapshot_destroy(&snapshot);
        } else {
            assert(snapshot.rules == NULL && snapshot.count == 0);
        }
        write_action_snapshot(path, version, 255, 1);
        rc = version == 1 ? dppd_persistence_load_v1_for_migration(path, &snapshot)
                          : dppd_persistence_load(path, &snapshot);
        assert(rc == -EBADMSG && snapshot.rules == NULL && snapshot.count == 0);
    }
    for (uint32_t invalid = 0; invalid <= 1; ++invalid) {
        write_action_snapshot(path, 4, DPPD_ACTION_AGE, invalid == 0 ? 0 : 0x1000000U);
        assert(dppd_persistence_load(path, &snapshot) == -EBADMSG && snapshot.rules == NULL);
    }
    const uint32_t timeouts[] = {1, 0xffffffU};
    for (uint32_t index = 0; index < sizeof(timeouts) / sizeof(timeouts[0]); ++index) {
        uint32_t timeout = timeouts[index];
        struct dppd_rule_repository repository;
        struct dppd_rule_apply_result applied;
        struct dppd_rule rule = make_rule(10, 123), expected;
        assert(dppd_rule_repository_init(&repository, 4) == 0);
        rule.nb_actions = 4;
        rule.actions[2].type = DPPD_ACTION_AGE;
        rule.actions[2].conf.age_timeout_seconds = timeout;
        rule.actions[3].type = DPPD_ACTION_DROP;
        assert(dppd_rule_repository_apply(&repository, &rule, 0, &applied) == 0);
        assert(dppd_rule_repository_get(&repository, rule.id, &expected) == 0);
        rule = make_rule(20, 456);
        rule.nb_matches = 2;
        rule.matches[1].type = DPPD_MATCH_IPV6;
        memset(&rule.matches[1].spec, 0, sizeof(rule.matches[1].spec));
        rule.matches[1].spec.ipv6.src[0] = 0x20;
        rule.matches[1].spec.ipv6.src[15] = 3;
        assert(dppd_rule_repository_apply(&repository, &rule, 0, &applied) == 0);
        assert(dppd_persistence_save(path, &repository) == 0);
        uint8_t header[SNAPSHOT_HEADER_SIZE];
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        assert(fd >= 0 && read(fd, header, sizeof(header)) == (ssize_t)sizeof(header) && header[8] == 4);
        assert(close(fd) == 0);
        assert(dppd_persistence_load(path, &snapshot) == 0 && snapshot.count == 2);
        assert(dppd_rule_equal(&snapshot.rules[0], &expected));
        assert(snapshot.rules[0].actions[2].conf.age_timeout_seconds == timeout);
        assert(snapshot.rules[1].matches[1].spec.ipv6.src[15] == 3);
        dppd_persisted_snapshot_destroy(&snapshot);
        dppd_rule_repository_destroy(&repository);
    }
    assert(unlink(path) == 0);
}

int main(void)
{
    struct dppd_rule_repository repository;
    struct dppd_rule_repository migrated_repository;
    struct dppd_persisted_snapshot snapshot;
    struct dppd_rule_apply_result result;
    struct dppd_rule expected10;
    struct dppd_rule expected20;
    struct dppd_rule rule;
    struct stat metadata;
    char path[128];
    char v1_path[128];
    char migrated_path[128];
    uint8_t byte;
    int fd;

    snprintf(path, sizeof(path), "/tmp/dppd-persistence-%ld.bin", (long)getpid());
    snprintf(v1_path, sizeof(v1_path), "/tmp/dppd-persistence-v1-%ld.bin", (long)getpid());
    snprintf(migrated_path, sizeof(migrated_path), "/tmp/dppd-persistence-v2-%ld.bin", (long)getpid());
    unlink(path);
    unlink(v1_path);
    unlink(migrated_path);
    memset(&migrated_repository, 0, sizeof(migrated_repository));
    assert(dppd_rule_repository_init(&repository, 4) == 0);

    /* 写入顺序故意与 ID 顺序相反，快照必须仍按稳定 ID 排序 */
    rule = make_rule(20, 200);
    assert(dppd_rule_repository_apply(&repository, &rule, 0, &result) == 0);
    rule = make_rule(10, 100);
    assert(dppd_rule_repository_apply(&repository, &rule, 0, &result) == 0);
    assert(dppd_rule_repository_get(&repository, 10, &expected10) == 0);
    assert(dppd_rule_repository_get(&repository, 20, &expected20) == 0);

    assert(dppd_persistence_save(path, &repository) == 0);

    /* v1 只能由迁移专用接口读取；正常启动入口仍必须拒绝该版本 */
    write_v1_snapshot(v1_path);
    assert(dppd_persistence_load(v1_path, &snapshot) == -EPROTONOSUPPORT);
    assert(dppd_persistence_load_v1_for_migration(v1_path, &snapshot) == 0);
    assert(snapshot.count == 1 && snapshot.repository_generation == 9);
    assert(snapshot.rules[0].id == 77 && snapshot.rules[0].generation == 4);
    assert(snapshot.rules[0].install_port_id == 0);

    /* 模拟工具的显式端口映射，restore/save 必须保留旧代际并写成 v2 */
    snapshot.rules[0].install_port_id = 13;
    assert(dppd_rule_repository_init(&migrated_repository, snapshot.count) == 0);
    assert(dppd_rule_repository_restore(&migrated_repository, snapshot.rules, snapshot.count,
               snapshot.repository_generation) == 0);
    assert(dppd_persistence_save(migrated_path, &migrated_repository) == 0);
    dppd_persisted_snapshot_destroy(&snapshot);
    assert(dppd_persistence_load(migrated_path, &snapshot) == 0);
    assert(snapshot.count == 1 && snapshot.repository_generation == 9);
    assert(snapshot.rules[0].id == 77 && snapshot.rules[0].generation == 4);
    assert(snapshot.rules[0].install_port_id == 13);
    dppd_persisted_snapshot_destroy(&snapshot);
    dppd_rule_repository_destroy(&migrated_repository);
    assert(stat(path, &metadata) == 0);
    assert(S_ISREG(metadata.st_mode));
    assert((metadata.st_mode & 0777) == 0600);
    assert(dppd_persistence_load(path, &snapshot) == 0);
    assert(snapshot.repository_generation == 2);
    assert(snapshot.count == 2);
    assert(snapshot.rules[0].id == 10 && snapshot.rules[1].id == 20);
    assert(snapshot.rules[0].generation == expected10.generation);
    assert(snapshot.rules[1].generation == expected20.generation);
    assert(snapshot.rules[0].install_port_id == 5);
    assert(snapshot.rules[1].install_port_id == 7);
    assert(dppd_rule_equal(&snapshot.rules[0], &expected10));
    assert(dppd_rule_equal(&snapshot.rules[1], &expected20));
    assert(snapshot.rules[0].matches[1].spec.ipv4.src_be == expected10.matches[1].spec.ipv4.src_be);
    assert(snapshot.rules[0].matches[2].spec.l4.dst_be == htons(443));
    dppd_persisted_snapshot_destroy(&snapshot);

    /*
     * v1 缺少 install_port_id，不能安全猜测目标端口。即使 magic 正确也必须返回
     * 明确的版本不支持，而不是把旧记录按 v2 布局错位解码
     */
    fd = open(path, O_RDWR | O_CLOEXEC);
    assert(fd >= 0);
    byte = 1;
    assert(pwrite(fd, &byte, 1, 8) == 1);
    assert(fsync(fd) == 0);
    close(fd);
    assert(dppd_persistence_load(path, &snapshot) == -EPROTONOSUPPORT);
    assert(dppd_persistence_save(path, &repository) == 0);

    /*
     * 修改 payload 的一个字节但保持文件长度不变，必须由 CRC 拒绝，不能把损坏
     * 内容交给后续硬件重放阶段
     */
    fd = open(path, O_RDWR | O_CLOEXEC);
    assert(fd >= 0);
    assert(pread(fd, &byte, 1, 64) == 1);
    byte ^= 0x5aU;
    assert(pwrite(fd, &byte, 1, 64) == 1);
    assert(fsync(fd) == 0);
    close(fd);
    assert(dppd_persistence_load(path, &snapshot) == -EBADMSG);

    /* 截断文件同样必须稳定返回格式损坏，而不是部分恢复规则 */
    assert(dppd_persistence_save(path, &repository) == 0);
    assert(truncate(path, 20) == 0);
    assert(dppd_persistence_load(path, &snapshot) == -EBADMSG);

    /* 空 repository 也必须形成合法、可校验的 header-only snapshot */
    dppd_rule_repository_destroy(&repository);
    assert(dppd_rule_repository_init(&repository, 4) == 0);
    assert(dppd_persistence_save(path, &repository) == 0);
    assert(dppd_persistence_load(path, &snapshot) == 0);
    assert(snapshot.count == 0);
    assert(snapshot.repository_generation == 0);
    assert(snapshot.rules == NULL);
    dppd_persisted_snapshot_destroy(&snapshot);

    unlink(path);
    unlink(v1_path);
    unlink(migrated_path);
    dppd_rule_repository_destroy(&repository);
    age_snapshot_compatibility();
    return 0;
}
