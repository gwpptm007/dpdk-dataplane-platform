#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <string.h>
#include "dppd/tap_content.h"

/** 用非零主机位和网络字节序验证规范化，不把结构体自身复制当作编码测试 */
int main(void)
{
    struct dppd_rule rule = {.group = 2, .priority = 9, .nb_matches = 3, .nb_actions = 1};
    rule.matches[0].type = DPPD_MATCH_ETH;
    rule.matches[1].type = DPPD_MATCH_IPV4;
    rule.matches[1].spec.ipv4.src_be = htonl(0xc61201fe);
    rule.matches[1].spec.ipv4.src_mask_be = htonl(0xffffff00);
    rule.matches[1].spec.ipv4.dst_be = htonl(0xc6120203);
    rule.matches[1].spec.ipv4.dst_mask_be = UINT32_MAX;
    rule.matches[2].type = DPPD_MATCH_UDP;
    rule.matches[2].spec.l4.src_be = htons(1234);
    rule.matches[2].spec.l4.src_mask_be = UINT16_MAX;
    rule.matches[2].spec.l4.dst_be = htons(9999);
    rule.actions[0].type = DPPD_ACTION_QUEUE;
    rule.actions[0].conf.queue_id = 3;
    struct dppd_tap_content content, restored;
    uint8_t bytes[DPPD_TAP_CONTENT_SIZE], damaged[DPPD_TAP_CONTENT_SIZE];
    assert(dppd_tap_content_from_rule(&rule, 16, &content) == 0);
    assert(content.priority == 8218 && content.protocol == 3 && content.ip_proto == 17);
    assert(content.ipv4.src_be == htonl(0xc6120100) && content.l4.dst_be == 0);
    dppd_tap_content_encode(&content, bytes);
    assert(bytes[12] == 198 && bytes[13] == 18 && bytes[14] == 1 && bytes[15] == 0);
    assert(bytes[28] == 4 && bytes[29] == 210 && bytes[6] == 3);
    assert(dppd_tap_content_decode(bytes, &restored) == 0 && restored.queue == 3);
    const unsigned int invalid[] = {0, 10, 11, 15, 37, 40, 46, 48, 63};
    for (unsigned int i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        memcpy(damaged, bytes, sizeof(bytes));
        damaged[invalid[i]] ^= 0xff;
        assert(dppd_tap_content_decode(damaged, &restored) == -EBADMSG);
    }
    memset(bytes, 0, sizeof(bytes));
    assert(dppd_tap_content_decode(bytes, &restored) == 0 && restored.version == 0);
    bytes[6] = 1;
    assert(dppd_tap_content_decode(bytes, &restored) == -EBADMSG);
    rule.matches[2].type = DPPD_MATCH_TCP;
    assert(dppd_tap_content_from_rule(&rule, 1, &content) == 0 && content.ip_proto == 6 && content.priority == 8203);
    rule.matches[2].spec.l4.src_mask_be = htons(0xff00);
    assert(dppd_tap_content_from_rule(&rule, 16, &content) == -ENOTSUP);
    rule.nb_matches = 1;
    rule.actions[0].type = DPPD_ACTION_DROP;
    assert(dppd_tap_content_from_rule(&rule, 16, &content) == 0 && content.action == 1 && content.eth_type == 0);
    rule.priority = 4079;
    assert(dppd_tap_content_from_rule(&rule, 16, &content) == -ENOTSUP);
    rule.priority = 4078;
    assert(dppd_tap_content_from_rule(&rule, 16, &content) == 0 && content.priority == 12287);
    assert(dppd_tap_content_from_rule(&rule, UINT16_MAX, &content) == -ENOTSUP);
    rule.group = 16;
    assert(dppd_tap_content_from_rule(&rule, 16, &content) == -ENOTSUP);
    return 0;
}
