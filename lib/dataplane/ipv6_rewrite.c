#include "dppd/ipv6.h"

#include <errno.h>
#include <string.h>
#include <rte_mbuf.h>

/**
 * 将 bytes 中的网络字节按 16 位字累加到 value，并把高位进位加回低 16 位
 * 奇数字节长度时末字节作为高 8 位、低 8 位补零，返回尚未按位取反的累加结果
 */
static uint32_t sum(uint32_t value, const uint8_t *bytes, uint32_t length)
{
    /**
     * i & 1U 判断奇偶：偶数下标左移 8 位作为高字节，奇数下标不移位作为低字节
     * 例如字节 12 34 → 0x1200 + 0x0034 = 0x1234，按网络字节顺序组成一个 16 位数
     * 若末尾只剩 56，则加入 0x5600，相当于低字节补零，不向原报文写入补齐字节
     */
    for (uint32_t i = 0; i < length; ++i)
        value += (uint32_t)bytes[i] << (i & 1U ? 0 : 8);
    /**
     * 保留低 16 位，再把高位进位加回来，例如 0x12345 → 0x2345 + 1 = 0x2346
     * 相加后仍可能产生进位，需要循环处理，例如 0x1ffff → 0x10000 → 0x0001
     */
    while (value >> 16)
        value = (value & UINT16_MAX) + (value >> 16);
    return value;
}

/**
 * 检查 IPv6 报文中的 TCP、UDP 或 ICMPv6 校验和，只读取 mbuf，不修改报文
 * packet 提供已核对的地址、最终上层协议和头部长度，用于定位数据并构造校验所需的伪首部
 * 返回 0 表示校验通过或协议不在检查范围；负错误码表示读取失败、校验错误或不支持转发
 */
static int transport(const struct rte_mbuf *mbuf, const struct dppd_packet *packet)
{
    /** 解析器已越过 IPv6 扩展头，这里使用最终协议号：TCP 6、UDP 17、ICMPv6 58 */
    uint8_t protocol = packet->ip_protocol;
    if (protocol != 6 && protocol != 17 && protocol != 58)
        return 0;
    /**
     * 报文布局：[二层头][IPv6 基础头 40 字节][扩展头，可选][上层协议头及数据]
     * ipv6_payload_len = 扩展头 + 上层协议头及数据，l3_len = 40 + 扩展头
     * length 扣除扩展头，得到参与校验的上层报文长度；prefix 是它在整包中的起始偏移
     * 例：二层头 14、扩展头 8、IPv6 负载 108 → length = 40 + 108 - 48 = 100，prefix = 14 + 48 = 62
     */
    uint32_t length = 40U + packet->ipv6_payload_len - packet->l3_len;
    uint32_t prefix = packet->l2_len + packet->l3_len;
    /** UDP 和 ICMPv6 先检查各自的特殊限制，TCP 跳过此分支后仍参与下面的校验和计算 */
    if (protocol != 6) {
        uint8_t head[8];
        /** 从上层协议头起点读取：UDP 取完整 8 字节头，ICMPv6 取类型、代码和校验和共 4 字节 */
        const uint8_t *first = rte_pktmbuf_read(mbuf, prefix, protocol == 17 ? 8 : 4, head);
        if (first == NULL)
            return -EINVAL;
        /**
         * UDP 头内偏移  0～1      2～3      4～5       6～7
         * 内容          源端口    目的端口   UDP 长度   校验和
         * first 指向 UDP 头起点，末两字节全零表示未提供校验和，本 IPv6 转发路径拒绝此包
         */
        if (protocol == 17 && first[6] == 0 && first[7] == 0)
            return -EBADMSG;
        /** ICMPv6 首字节是 Type，RS、RA、NS、NA 和 Redirect 属于本链路控制消息，不跨网口转发 */
        if (protocol == 58 && dppd_icmpv6_is_nd(first[0]))
            return -ENOTSUP;
    }
    /**
     * 伪首部参与上层校验和，将数据与源/目的 IP、上层协议和长度一起校验，不额外写入报文
     * 字段：[源 IPv6 16 字节][目的 IPv6 16 字节][上层长度 4 字节][零 3 字节][协议号 1 字节]
     * 内层 sum 累加源地址，外层 sum 继续累加目的地址，再加入最终协议号和上层长度
     * 本路径的 length 不超过 65535，长度高 16 位及保留字段均为零，因此可直接加 length 和 protocol
     * value 此时只是伪首部的累加值，下面还要加入上层协议头和数据，包括报文中已有的校验和字段
     */
    uint32_t value = sum(sum(0, packet->ipv6_src, 16), packet->ipv6_dst, 16) + protocol + length;
    /**
     * 每次最多读取 256 字节参与累加，scratch 仅供跨 mbuf 分段读取时暂存，不限制报文总长度
     * offset 从上层协议头起点计数，prefix + offset 才是整包偏移
     * 例：length 为 600 时分为 256、256、88 字节，offset 依次为 0、256、512
     * 非末块长度为偶数，保持 16 位字边界；只有末块可能需要在求和时补一个零字节
     */
    uint8_t scratch[256];
    for (uint32_t offset = 0; offset < length;) {
        uint32_t size = length - offset > sizeof(scratch) ? sizeof(scratch) : length - offset;
        const uint8_t *bytes = rte_pktmbuf_read(mbuf, prefix + offset, size, scratch);
        if (bytes == NULL)
            return -EINVAL;
        /** 连续时 bytes 指向 mbuf 数据，跨段时指向 scratch，均接着已有 value 累加 */
        value = sum(value, bytes, size);
        offset += size;
    }
    /**
     * 接收校验保留报文中的校验和字段一起求和，连同伪首部折叠进位后应为 0xffff
     * 例如发送方累加值为 0x1234，写入反码 0xedcb，接收方将两者相加得到 0xffff
     */
    return value == UINT16_MAX ? 0 : -EBADMSG;
}

/** 仅接受非零单播 MAC：首字节最低位为 0，且 6 字节不能全为 0 */
static bool valid_mac(const uint8_t *mac)
{
    const uint8_t zero[6] = {0};
    return (mac[0] & 1U) == 0 && memcmp(mac, zero, 6) != 0;
}

/**
 * 重新解析 mbuf，核对调用方提供的头部信息，并检查本实现是否支持转发此 IPv6 报文
 * packet 是调用方已有的解析结果，actual 接收本次解析结果，后续改写以 actual 为依据
 *
 * 返回 0 表示通过；-EINVAL 表示报文损坏或关键解析字段不一致；-ENOTSUP 表示不支持转发
 * 此处只读取报文，传输层校验和由后续 transport 检查
 */
static int validate(const struct rte_mbuf *mbuf, const struct dppd_packet *packet, struct dppd_packet *actual)
{
    if (mbuf == NULL || packet == NULL)
        return -EINVAL;
    int parsed = dppd_packet_parse_mbuf(mbuf, actual);
    /** 核对长度、协议和地址，避免沿用与当前报文字节不一致的解析信息计算改写位置 */
    if (parsed == DPPD_PARSE_MALFORMED || actual->l3_type != DPPD_L3_IPV6 || packet->l3_type != DPPD_L3_IPV6 ||
        packet->packet_len != actual->packet_len || packet->l3_len != actual->l3_len ||
        packet->l4_len != actual->l4_len || packet->l2_len != actual->l2_len ||
        packet->ip_protocol != actual->ip_protocol || memcmp(packet->ipv6_src, actual->ipv6_src, 16) != 0 ||
        memcmp(packet->ipv6_dst, actual->ipv6_dst, 16) != 0)
        return -EINVAL;
    /** 本实现不在此路径处理 IPv6 分片或解析器标记为不支持的报文 */
    if (actual->ipv6_unsupported || actual->l4_type == DPPD_L4_FRAGMENT)
        return -ENOTSUP;
    /** 检查报文本身的源/目的 IP，false 禁止链路本地地址跨网口转发；下一跳仍可使用 fe80::1 */
    if (!dppd_ipv6_unicast(actual->ipv6_src, false) || !dppd_ipv6_unicast(actual->ipv6_dst, false))
        return -ENOTSUP;
    return 0;
}

int dppd_ipv6_transport_validate(const struct rte_mbuf *mbuf, const struct dppd_packet *packet)
{
    struct dppd_packet actual = {0};
    int rc = validate(mbuf, packet, &actual);
    return rc == 0 ? transport(mbuf, &actual) : rc;
}

int dppd_ipv6_rewrite_flow(struct rte_mbuf **mbuf, const struct dppd_packet *packet,
    const struct dppd_ipv4_forward *forward, const uint8_t source_mac[6], const struct dppd_flow_tuple *translation)
{
    /** mbuf 是调用方指针变量的地址，*mbuf 才是报文对象；指针有效后才能检查两个待写入的 MAC */
    if (mbuf == NULL || *mbuf == NULL || packet == NULL || forward == NULL || source_mac == NULL ||
        !valid_mac(source_mac) || !valid_mac(forward->destination_mac))
        return -EINVAL;
    /** actual 仅保存重新解析的字段，不复制整包；校验失败直接返回，报文仍由调用方持有 */
    struct dppd_packet actual = {0};
    int rc = validate(*mbuf, packet, &actual);
    if (rc != 0)
        return rc;
    /** 仅在需要 NAT 时校验转换目标；普通路由传入 NULL，跳过此分支 */
    if (translation != NULL &&
        (!translation->ipv6 || translation->protocol != actual.ip_protocol ||
            (actual.ip_protocol != 6 && actual.ip_protocol != 17) || !dppd_ipv6_unicast(translation->source6, false) ||
            !dppd_ipv6_unicast(translation->destination6, false)))
        return -EINVAL;
    /**
     * header 暂存将要改写的头部，512 是缓冲区容量，实际读取长度由 size 决定
     * 普通路由只改 MAC 和 Hop Limit，读取二层头加 IPv6 基础头 40 字节即可
     * 无 VLAN 时为 14 + 40 = 54 字节，即使带 IPv6 扩展头也无需在这里一并复制
     * NAT 还要改地址、端口和校验和，因此读取到 TCP 基础头 20 字节或 UDP 头 8 字节末尾
     * 
     * 无 VLAN 时
     * 整包偏移    0～13          14～53
     *  内容        以太网头       IPv6 基础头
     *  长度        14 字节        40 字节
     *              └────── 共读取 54 字节 ──────┘
     */
    uint8_t header[512];
    uint16_t size = actual.l2_len + (translation == NULL ? 40 : actual.l3_len + (actual.ip_protocol == 6 ? 20 : 8));
    /**
     * 读取范围连续 → data 指向 mbuf 内的数据，header 尚未填充，下面再复制到 header
     * 读取范围跨段 → read 已复制到 header，返回 data == header，无需再次复制
     * 两条路径最终都在 header 中准备修改，实际报文稍后确认可写后再统一写回
     */
    const void *data = rte_pktmbuf_read(*mbuf, 0, size, header);
    if (data == NULL)
        return -EINVAL;
    if (data != header)
        memcpy(header, data, size);
    /**
     * IPv6 头内偏移  0～3                 4～5       6           7
     * 内容           版本/流量类别/流标签  负载长度  下一头部    Hop Limit
     * actual.l2_len 跳过二层头，再加 7 定位 Hop Limit；无 VLAN 时整包偏移为 14 + 7 = 21
     * 本次转发需要减 1，值为 0 或 1 时不能继续转发，返回 -ETIMEDOUT 交给 worker 统计并丢弃
     */
    if (header[actual.l2_len + 7] <= 1)
        return -ETIMEDOUT;
    /** 检查 TCP/UDP/ICMPv6 校验和，通过后才准备新的 MAC 和跳数 */
    rc = transport(*mbuf, &actual);
    if (rc != 0)
        return rc;
    /** 目的 MAC 换为下一跳，源 MAC 换为出口，Hop Limit 减 1；此处只修改临时头部 */
    memcpy(header, forward->destination_mac, 6);
    memcpy(header + 6, source_mac, 6);
    /** Hop Limit 类似 IPv4 的 TTL，每次路由转发减 1，限制报文可经过的跳数 */
    header[actual.l2_len + 7]--;
    if (translation != NULL) {
        uint8_t *ip = header + actual.l2_len, *l4 = ip + actual.l3_len;
        unsigned int checksum_offset = actual.ip_protocol == 6 ? 16 : 6;
        uint32_t value = (uint16_t) ~((uint16_t)l4[checksum_offset] << 8 | l4[checksum_offset + 1]);
        uint8_t old[36], updated[36];
        memcpy(old, ip + 8, 32);
        memcpy(old + 32, l4, 4);
        memcpy(ip + 8, translation->source6, 16);
        memcpy(ip + 24, translation->destination6, 16);
        memcpy(l4, &translation->source_port_be, 2);
        memcpy(l4 + 2, &translation->destination_port_be, 2);
        memcpy(updated, ip + 8, 32);
        memcpy(updated + 32, l4, 4);
        /** 地址和端口作为同一组变化增量求和，IPv6 UDP 的结果为零时必须发送全一 */
        for (unsigned int i = 0; i < sizeof(old); i += 2) {
            value += (uint16_t) ~((uint16_t)old[i] << 8 | old[i + 1]);
            value += (uint16_t)((uint16_t)updated[i] << 8 | updated[i + 1]);
        }
        while (value >> 16)
            value = (value & UINT16_MAX) + (value >> 16);
        uint16_t checksum = (uint16_t)~value;
        if (checksum == 0 && actual.ip_protocol == 17)
            checksum = UINT16_MAX;
        l4[checksum_offset] = (uint8_t)(checksum >> 8);
        l4[checksum_offset + 1] = (uint8_t)checksum;
    }
    bool copy = false;
    /**
     * 沿 next 检查整条 mbuf 链，任一段不是直接缓冲区、使用外部缓冲区或引用计数不为 1，就复制整包
     * 例如某段引用计数为 2，直接改写会影响其他使用者，因此需要独立的数据副本
     * copy |= 会保留前面发现的复制要求，后续段可写也不能把它改回 false
     * 分段本身不要求复制；所有段都满足本函数的独占可写条件时，仍可沿原链逐段写回
     */
    for (const struct rte_mbuf *part = *mbuf; part != NULL; part = part->next)
        copy |= !RTE_MBUF_DIRECT(part) || RTE_MBUF_HAS_EXTBUF(part) || rte_mbuf_refcnt_read(part) != 1;
    /**
     * 需要复制时从原 mbuf 所属内存池分配副本，offset 为 0、长度为 UINT32_MAX 表示复制整包数据
     * 否则 writable 直接指向原链；此时原 *mbuf 尚未替换，失败返回后仍由调用方持有
     */
    struct rte_mbuf *writable = copy ? rte_pktmbuf_copy(*mbuf, (*mbuf)->pool, 0, UINT32_MAX) : *mbuf;
    if (writable == NULL)
        return -ENOMEM;
    /** 写入前再次确认目标链满足可写条件，内存池分配出的副本也不能直接假定为普通独占缓冲区 */
    for (struct rte_mbuf *part = writable; part != NULL; part = part->next)
        if (!RTE_MBUF_DIRECT(part) || RTE_MBUF_HAS_EXTBUF(part) || rte_mbuf_refcnt_read(part) != 1) {
            /** 若目标是新副本，只清理副本，原报文仍交由调用方处理 */
            if (copy)
                rte_pktmbuf_free(writable);
            return -ENOTSUP;
        }
    /**
     * 将临时头部沿目标 mbuf 链写回，cursor 指向尚未写出的头部字节，size 表示剩余字节数
     * 每段写入 min(data_len, size)，只覆盖头部范围，后面的报文数据保持原样
     * 例：头部 54 字节，第一段有 20 字节 → 先写 header[0..19]，剩余 34 字节写入后续段
     */
    const uint8_t *cursor = header;
    for (struct rte_mbuf *part = writable; part != NULL && size != 0; part = part->next) {
        uint16_t bytes = part->data_len < size ? part->data_len : size;
        /** mtod 取得当前段的数据起点，返回 uint8_t *，目标不是 rte_mbuf 结构体本身 */
        memcpy(rte_pktmbuf_mtod(part, uint8_t *), cursor, bytes);
        cursor += bytes;
        size -= bytes;
    }
    /** 清除旧的 TX 卸载请求及参数，再填写当前头部长度，避免驱动沿用旧的校验和或分段发送设置 */
    writable->ol_flags &= ~RTE_MBUF_F_TX_OFFLOAD_MASK;
    writable->tx_offload = 0;
    writable->l2_len = actual.l2_len;
    writable->l3_len = actual.l3_len;
    writable->l4_len = actual.l4_len;
    /**
     * 使用副本时释放当前持有的原报文引用，再把调用方的指针替换为已改写的副本
     * worker 传入的是 &rx[i]，因此 *mbuf = writable 会让后续发送使用新的 rx[i]
     * 原数据若仍有其他引用，DPDK 按引用计数保留它，直到最后一个使用者释放
     */
    if (copy) {
        rte_pktmbuf_free(*mbuf);
        *mbuf = writable;
    }
    return 0;
}

/**
 * 按路由结果改写 IPv6 报文的源/目的 MAC，并将 Hop Limit 减 1
 *
 * mbuf 为报文指针的地址，必要时复制报文并更新此指针；packet 为已解析的报文信息
 * forward 提供下一跳目的 MAC，source_mac 为出口网口 MAC
 * 委托 rewrite_flow 完成校验和改写，translation 传 NULL 表示不转换 IP 地址和端口
 *
 * 普通路由改写范围：
 *   二层：目的 MAC 换为下一跳 MAC，源 MAC 换为出口网卡 MAC
 *   三层：Hop Limit 减 1，源/目的 IPv6 地址保持不变
 *   四层：TCP/UDP 端口、校验和及业务数据保持不变，前面只验证校验和
 * Hop Limit 不参与上层校验和计算，IPv6 基础头也没有头部校验和，因此递减跳数无需重算校验和
 * 例：发往 2001:db8:2::20 的 UDP 包，经下一跳 fe80::1 转发，目的 IP 仍为前者，目的 MAC 改为后者的 MAC
 *
 * NAT 路径由调用方直接向 rewrite_flow 提供 translation，才进一步修改 IP、端口并更新 TCP/UDP 校验和
 * mbuf 的头部长度和 TX 卸载标志属于管理信息，更新这些字段不会增加线上报文字节
 *
 * 返回 0 表示改写成功，负错误码表示失败，由调用方处理丢弃；本函数不发送报文
 */
int dppd_ipv6_rewrite(struct rte_mbuf **mbuf, const struct dppd_packet *packet, const struct dppd_ipv4_forward *forward,
    const uint8_t source_mac[6])
{
    return dppd_ipv6_rewrite_flow(mbuf, packet, forward, source_mac, NULL);
}
