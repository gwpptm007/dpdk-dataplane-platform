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

int main(void)
{
    struct dppd_rule_repository repository;
    struct dppd_persisted_snapshot snapshot;
    struct dppd_rule_apply_result result;
    struct dppd_rule expected10;
    struct dppd_rule expected20;
    struct dppd_rule rule;
    struct stat metadata;
    char path[128];
    uint8_t byte;
    int fd;

    snprintf(path, sizeof(path), "/tmp/dppd-persistence-%ld.bin",
             (long)getpid());
    unlink(path);
    assert(dppd_rule_repository_init(&repository, 4) == 0);

    /* 写入顺序故意与 ID 顺序相反，快照必须仍按稳定 ID 排序。 */
    rule = make_rule(20, 200);
    assert(dppd_rule_repository_apply(&repository, &rule, 0, &result) == 0);
    rule = make_rule(10, 100);
    assert(dppd_rule_repository_apply(&repository, &rule, 0, &result) == 0);
    assert(dppd_rule_repository_get(&repository, 10, &expected10) == 0);
    assert(dppd_rule_repository_get(&repository, 20, &expected20) == 0);

    assert(dppd_persistence_save(path, &repository) == 0);
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
    assert(snapshot.rules[0].matches[1].spec.ipv4.src_be ==
           expected10.matches[1].spec.ipv4.src_be);
    assert(snapshot.rules[0].matches[2].spec.l4.dst_be == htons(443));
    dppd_persisted_snapshot_destroy(&snapshot);

    /*
     * v1 缺少 install_port_id，不能安全猜测目标端口。即使 magic 正确也必须返回
     * 明确的版本不支持，而不是把旧记录按 v2 布局错位解码。
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
     * 内容交给后续硬件重放阶段。
     */
    fd = open(path, O_RDWR | O_CLOEXEC);
    assert(fd >= 0);
    assert(pread(fd, &byte, 1, 64) == 1);
    byte ^= 0x5aU;
    assert(pwrite(fd, &byte, 1, 64) == 1);
    assert(fsync(fd) == 0);
    close(fd);
    assert(dppd_persistence_load(path, &snapshot) == -EBADMSG);

    /* 截断文件同样必须稳定返回格式损坏，而不是部分恢复规则。 */
    assert(dppd_persistence_save(path, &repository) == 0);
    assert(truncate(path, 20) == 0);
    assert(dppd_persistence_load(path, &snapshot) == -EBADMSG);

    /* 空 repository 也必须形成合法、可校验的 header-only snapshot。 */
    dppd_rule_repository_destroy(&repository);
    assert(dppd_rule_repository_init(&repository, 4) == 0);
    assert(dppd_persistence_save(path, &repository) == 0);
    assert(dppd_persistence_load(path, &snapshot) == 0);
    assert(snapshot.count == 0);
    assert(snapshot.repository_generation == 0);
    assert(snapshot.rules == NULL);
    dppd_persisted_snapshot_destroy(&snapshot);

    unlink(path);
    dppd_rule_repository_destroy(&repository);
    return 0;
}
