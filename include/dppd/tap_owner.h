#ifndef DPPD_TAP_OWNER_H
#define DPPD_TAP_OWNER_H

#include <stdbool.h>
#include <stdint.h>

/**
 * 与仓库中的 DPDK 21.11.9 TAP 补丁共用的私有动作协议，不属于 DPDK 标准接口
 * 独立动作让未适配驱动明确拒绝，不能把需要标识的请求悄悄变成普通规则
 */
#define DPPD_TAP_ACTION_OWNER_V1 (-0x44505001)
#define DPPD_TAP_COOKIE_SIZE 16U

struct dppd_tap_owner_action {
    uint32_t version;
    uint32_t size;
    uint8_t cookie[DPPD_TAP_COOKIE_SIZE];
};

/** 全零保留给未启用原生标识的旧路径，随机生成器不得把它当作有效标识 */
static inline bool dppd_tap_cookie_present(const uint8_t cookie[DPPD_TAP_COOKIE_SIZE])
{
    uint8_t bits = 0;
    for (unsigned int index = 0; index < DPPD_TAP_COOKIE_SIZE; ++index)
        bits |= cookie[index];
    return bits != 0;
}

#endif
