#include "csum.h"

uint16_t dppd_ipv4_csum16(const void *buf, uint32_t len)
{
    const uint8_t *p = (const uint8_t *)buf;
    uint32_t sum = 0;
    uint32_t i;

    for (i = 0; i + 1 < len; i += 2)
        sum += ((uint32_t)p[i] << 8) | p[i + 1];

    if (len & 1U)
        sum += ((uint32_t)p[len - 1] << 8);

    while (sum >> 16)
        sum = (sum & 0xffffU) + (sum >> 16);

    return (uint16_t)(~sum & 0xffffU);
}
