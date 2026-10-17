#include "dppd/config.h"
#include "dppd/management.h"
#include "dppd/ipv6.h"

#include <errno.h>
#include <arpa/inet.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int parse_u32(const char *text, uint32_t min, uint32_t max, uint32_t *value)
{
    char *end = NULL;
    unsigned long parsed;

    if (text == NULL || value == NULL)
        return -1;

    /* 同时检查 errno、完整消费和调用方给定范围，禁止 12abc 这类部分解析 */
    errno = 0;
    parsed = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || parsed < min || parsed > max)
        return -1;

    *value = (uint32_t)parsed;
    return 0;
}

static int parse_ports(const char *text, struct dppd_config *cfg)
{
    char copy[256];
    char *save = NULL;
    char *token;
    uint16_t count = 0;

    if (text == NULL || strlen(text) >= sizeof(copy))
        return -1;

    /* strtok_r 会原地写入分隔符，因此先复制到有界的局部缓冲区 */
    memcpy(copy, text, strlen(text) + 1U);
    token = strtok_r(copy, ",", &save);
    while (token != NULL) {
        uint32_t port;

        if (count == DPPD_MAX_PORTS || parse_u32(token, 0, UINT16_MAX, &port) != 0)
            return -1;
        cfg->ports[count++] = (uint16_t)port;
        token = strtok_r(NULL, ",", &save);
    }

    if (count == 0)
        return -1;
    cfg->nb_ports = count;
    return 0;
}

/** 参数只描述转换策略，不修改 Linux 地址，四段必须完整且不能遗漏端口池边界 */
static int parse_snat(const char *text, struct dppd_snat_config *nat, bool ipv6)
{
    char copy[128];
    if (text == NULL || strlen(text) >= sizeof(copy))
        return -1;
    strcpy(copy, text);
    char *fields[4], *cursor = copy;
    for (unsigned int i = 0; i < 4; ++i) {
        fields[i] = cursor;
        char *comma = strchr(cursor, ',');
        if ((i < 3 && comma == NULL) || (i == 3 && comma != NULL))
            return -1;
        if (comma != NULL) {
            *comma = '\0';
            cursor = comma + 1;
        }
    }
    uint32_t inside, outside, first, last;
    char *dash = strchr(fields[3], '-');
    if (dash == NULL)
        return -1;
    *dash++ = '\0';
    if (parse_u32(fields[0], 0, UINT16_MAX - 1U, &inside) != 0 ||
        parse_u32(fields[1], 0, UINT16_MAX - 1U, &outside) != 0 || inside == outside ||
        inet_pton(ipv6 ? AF_INET6 : AF_INET, fields[2], ipv6 ? (void *)nat->address6 : (void *)&nat->address_be) != 1 ||
        parse_u32(fields[3], 1, UINT16_MAX, &first) != 0 || parse_u32(dash, 1, UINT16_MAX, &last) != 0 || first > last)
        return -1;
    uint32_t address = ntohl(nat->address_be);
    if (ipv6 ? !dppd_ipv6_unicast(nat->address6, false)
             : (address < 0x01000000U || (address >> 24) == 127 || address >= 0xe0000000U))
        return -1;
    nat->enabled = true;
    nat->ipv6 = ipv6;
    nat->inside_port = (uint16_t)inside;
    nat->outside_port = (uint16_t)outside;
    nat->first_port = (uint16_t)first;
    nat->last_port = (uint16_t)last;
    return 0;
}

static bool unicast(uint32_t address_be)
{
    uint32_t address = ntohl(address_be);
    return address >= 0x01000000U && (address >> 24) != 127 && address < 0xe0000000U;
}

static bool overlaps(uint32_t first, uint32_t last, uint32_t other_first, uint32_t other_last)
{
    return first <= other_last && other_first <= last;
}

/** 同一公网入口不能有重叠规则，同一后端也不能有两种回包无法区分的公网身份 */
int dppd_nat_validate(const struct dppd_snat_config *snat, const struct dppd_dnat_rule *dnat, uint32_t count)
{
    if (count > DPPD_MAX_DNAT_RULES || (count != 0 && dnat == NULL))
        return -EINVAL;
    if (snat != NULL && snat->enabled &&
        ((snat->ipv6 ? !dppd_ipv6_unicast(snat->address6, false) : !unicast(snat->address_be)) ||
            snat->inside_port == snat->outside_port || snat->inside_port == UINT16_MAX ||
            snat->outside_port == UINT16_MAX || snat->first_port == 0 || snat->first_port > snat->last_port))
        return -EINVAL;
    uint32_t reserved[2] = {0};
    for (uint32_t i = 0; i < count; ++i) {
        const struct dppd_dnat_rule *rule = &dnat[i];
        uint32_t last = (uint32_t)rule->private_first + rule->public_last - rule->public_first;
        bool addresses_invalid = rule->ipv6
            ? (!dppd_ipv6_unicast(rule->public_address6, false) || !dppd_ipv6_unicast(rule->private_address6, false) ||
                  memcmp(rule->public_address6, rule->private_address6, 16) == 0)
            : (!unicast(rule->public_address_be) || !unicast(rule->private_address_be) ||
                  rule->public_address_be == rule->private_address_be);
        if (addresses_invalid || (rule->protocol != 6 && rule->protocol != 17) ||
            rule->outside_port == rule->inside_port || rule->outside_port == UINT16_MAX ||
            rule->inside_port == UINT16_MAX || rule->public_first > rule->public_last || last > UINT16_MAX ||
            (rule->public_first == 0 && (rule->public_last != UINT16_MAX || rule->private_first != 0)) ||
            (rule->public_first != 0 && rule->private_first == 0))
            return -EINVAL;
        for (uint32_t j = 0; j < i; ++j) {
            const struct dppd_dnat_rule *other = &dnat[j];
            if (rule->ipv6 != other->ipv6 || rule->protocol != other->protocol)
                continue;
            if ((rule->outside_port == other->outside_port &&
                    (rule->ipv6 ? memcmp(rule->public_address6, other->public_address6, 16) == 0
                                : rule->public_address_be == other->public_address_be) &&
                    overlaps(rule->public_first, rule->public_last, other->public_first, other->public_last)) ||
                (rule->inside_port == other->inside_port &&
                    (rule->ipv6 ? memcmp(rule->private_address6, other->private_address6, 16) == 0
                                : rule->private_address_be == other->private_address_be) &&
                    overlaps(rule->private_first, last, other->private_first,
                        (uint32_t)other->private_first + other->public_last - other->public_first)))
                return -EEXIST;
        }
        if (snat != NULL && snat->enabled && rule->outside_port == snat->outside_port && rule->ipv6 == snat->ipv6 &&
            (rule->ipv6 ? memcmp(rule->public_address6, snat->address6, 16) == 0
                        : rule->public_address_be == snat->address_be) &&
            overlaps(rule->public_first, rule->public_last, snat->first_port, snat->last_port)) {
            uint32_t first = rule->public_first > snat->first_port ? rule->public_first : snat->first_port;
            uint32_t end = rule->public_last < snat->last_port ? rule->public_last : snat->last_port;
            reserved[rule->protocol == 6] += end - first + 1;
        }
    }
    /** SNAT 仍承诺 TCP 和 UDP 都可分配，静态保留不能吃掉任一协议的整个动态池 */
    if (snat != NULL && snat->enabled &&
        (reserved[0] == (uint32_t)snat->last_port - snat->first_port + 1 ||
            reserved[1] == (uint32_t)snat->last_port - snat->first_port + 1))
        return -ENOSPC;
    return 0;
}

/** 单端口与等长端口段用同一种表示，any 只允许配 same，避免截断或取模映射 */
static int parse_port_range(char *text, uint16_t *first, uint16_t *last)
{
    char *dash = strchr(text, '-');
    if (dash != NULL)
        *dash++ = '\0';
    uint32_t start, end;
    if (*text < '0' || *text > '9' || parse_u32(text, 1, UINT16_MAX, &start) != 0 ||
        (dash != NULL && (*dash < '0' || *dash > '9' || parse_u32(dash, 1, UINT16_MAX, &end) != 0)))
        return -1;
    if (dash == NULL)
        end = start;
    *first = (uint16_t)start;
    *last = (uint16_t)end;
    return start <= end ? 0 : -1;
}

static int parse_dnat(const char *text, struct dppd_dnat_rule *rule, bool ipv6)
{
    char copy[192];
    if (text == NULL || strlen(text) >= sizeof(copy))
        return -1;
    strcpy(copy, text);
    char *fields[7], *cursor = copy;
    for (unsigned int i = 0; i < 7; ++i) {
        fields[i] = cursor;
        char *comma = strchr(cursor, ',');
        if ((i < 6 && comma == NULL) || (i == 6 && comma != NULL))
            return -1;
        if (comma != NULL) {
            *comma = '\0';
            cursor = comma + 1;
        }
        if (*fields[i] == '\0')
            return -1;
    }
    uint32_t outside, inside;
    memset(rule, 0, sizeof(*rule));
    if (*fields[0] < '0' || *fields[0] > '9' || *fields[1] < '0' || *fields[1] > '9' ||
        parse_u32(fields[0], 0, UINT16_MAX - 1U, &outside) != 0 ||
        parse_u32(fields[1], 0, UINT16_MAX - 1U, &inside) != 0 ||
        inet_pton(ipv6 ? AF_INET6 : AF_INET, fields[3],
            ipv6 ? (void *)rule->public_address6 : (void *)&rule->public_address_be) != 1 ||
        inet_pton(ipv6 ? AF_INET6 : AF_INET, fields[5],
            ipv6 ? (void *)rule->private_address6 : (void *)&rule->private_address_be) != 1)
        return -1;
    rule->ipv6 = ipv6;
    rule->outside_port = (uint16_t)outside;
    rule->inside_port = (uint16_t)inside;
    rule->protocol = strcmp(fields[2], "tcp") == 0 ? 6 : strcmp(fields[2], "udp") == 0 ? 17 : 0;
    bool all = strcmp(fields[4], "any") == 0;
    if (all)
        rule->public_last = UINT16_MAX;
    else if (parse_port_range(fields[4], &rule->public_first, &rule->public_last) != 0)
        return -1;
    if (strcmp(fields[6], "same") == 0)
        rule->private_first = rule->public_first;
    else {
        uint16_t last;
        if (all || parse_port_range(fields[6], &rule->private_first, &last) != 0 ||
            last - rule->private_first != rule->public_last - rule->public_first)
            return -1;
    }
    return dppd_nat_validate(NULL, rule, 1);
}

void dppd_config_defaults(struct dppd_config *cfg)
{
    /* 先清零确保新增字段有确定默认值，再覆盖可运行的最小双端口配置 */
    memset(cfg, 0, sizeof(*cfg));
    cfg->ports[0] = 0;
    cfg->ports[1] = 1;
    cfg->nb_ports = 2;
    cfg->nb_queues = 1;
    cfg->burst_size = 32;
    cfg->mbufs_per_socket = 32768;
    cfg->mbuf_cache = 256;
    cfg->stats_period_ms = 1000;
    cfg->rule_capacity = DPPD_DEFAULT_RULE_CAPACITY;
    cfg->conntrack_timeout_s = 300;
    /* sizeof 字符串字面量包含末尾 '\0'，复制后路径一定终止 */
    memcpy(cfg->control_socket, DPPD_MANAGEMENT_DEFAULT_SOCKET, sizeof(DPPD_MANAGEMENT_DEFAULT_SOCKET));
    cfg->promiscuous = false;
}

int dppd_config_parse(int argc, char **argv, struct dppd_config *cfg)
{
    static const struct option options[] = {
        {"ports", required_argument, NULL, 'p'},
        {"queues", required_argument, NULL, 'q'},
        {"burst", required_argument, NULL, 'b'},
        {"mbufs", required_argument, NULL, 'm'},
        {"cache", required_argument, NULL, 'c'},
        {"stats-ms", required_argument, NULL, 's'},
        {"duration", required_argument, NULL, 'd'},
        {"promisc", no_argument, NULL, 1000},
        {"enable-pdump", no_argument, NULL, 1001},
        {"control-socket", required_argument, NULL, 1002},
        {"rule-capacity", required_argument, NULL, 1003},
        {"state-path", required_argument, NULL, 1004},
        {"recovery-path", required_argument, NULL, 1005},
        {"tap-owner-cookie", no_argument, NULL, 1006},
        {"recovery-monitor", required_argument, NULL, 1007},
        {"ipv4-routing", required_argument, NULL, 1008},
        {"ip-routing", required_argument, NULL, 1008},
        {"conntrack", required_argument, NULL, 1009},
        {"ct-timeout", required_argument, NULL, 1010},
        {"snat", required_argument, NULL, 1011},
        {"dnat", required_argument, NULL, 1012},
        {"snat6", required_argument, NULL, 1013},
        {"dnat6", required_argument, NULL, 1014},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0},
    };
    int option;

    if (cfg == NULL)
        return DPPD_CONFIG_ERROR;

    /* 每次解析从默认配置开始，避免调用方复用结构体时残留旧参数 */
    dppd_config_defaults(cfg);
    optind = 1;
    while ((option = getopt_long(argc, argv, "p:q:b:m:c:s:d:h", options, NULL)) != -1) {
        uint32_t value;

        switch (option) {
        case 'p':
            if (parse_ports(optarg, cfg) != 0)
                return DPPD_CONFIG_ERROR;
            break;
        case 'q':
            if (parse_u32(optarg, 1, DPPD_MAX_WORKERS, &value) != 0)
                return DPPD_CONFIG_ERROR;
            cfg->nb_queues = (uint16_t)value;
            break;
        case 'b':
            if (parse_u32(optarg, 1, DPPD_MAX_BURST, &value) != 0)
                return DPPD_CONFIG_ERROR;
            cfg->burst_size = (uint16_t)value;
            break;
        case 'm':
            if (parse_u32(optarg, 1024, UINT32_MAX, &cfg->mbufs_per_socket) != 0)
                return DPPD_CONFIG_ERROR;
            break;
        case 'c':
            if (parse_u32(optarg, 0, UINT16_MAX, &value) != 0)
                return DPPD_CONFIG_ERROR;
            cfg->mbuf_cache = (uint16_t)value;
            break;
        case 's':
            if (parse_u32(optarg, 100, UINT32_MAX, &cfg->stats_period_ms) != 0)
                return DPPD_CONFIG_ERROR;
            break;
        case 'd':
            if (parse_u32(optarg, 0, UINT32_MAX, &cfg->duration_s) != 0)
                return DPPD_CONFIG_ERROR;
            break;
        case 1000:
            cfg->promiscuous = true;
            break;
        case 1001:
            cfg->enable_pdump = true;
            break;
        case 1002:
            /* 数组容量检查使用 >=，为 C 字符串末尾 '\0' 留出空间 */
            if (optarg == NULL || optarg[0] == '\0' || strlen(optarg) >= sizeof(cfg->control_socket))
                return DPPD_CONFIG_ERROR;
            memcpy(cfg->control_socket, optarg, strlen(optarg) + 1U);
            break;
        case 1003:
            /* UINT32_MAX 被 repository/backend 用作非法容量哨兵，不能暴露给用户 */
            if (parse_u32(optarg, 1, UINT32_MAX - 1U, &cfg->rule_capacity) != 0)
                return DPPD_CONFIG_ERROR;
            break;
        case 1004:
            /*
             * 空路径没有“启用但无目标”的可靠语义；长度检查同时为末尾 NUL 留位
             * 父目录是否存在、是否可写由启动恢复真实打开文件时判定
             */
            if (optarg == NULL || optarg[0] == '\0' || strlen(optarg) >= sizeof(cfg->state_path))
                return DPPD_CONFIG_ERROR;
            memcpy(cfg->state_path, optarg, strlen(optarg) + 1U);
            break;
        case 1005:
            /** 恢复记录必须有明确路径，启动时再检查文件锁、格式和快照绑定 */
            if (optarg == NULL || optarg[0] == '\0' || strlen(optarg) >= sizeof(cfg->recovery_path))
                return DPPD_CONFIG_ERROR;
            memcpy(cfg->recovery_path, optarg, strlen(optarg) + 1U);
            break;
        case 1006:
            cfg->tap_owner_cookie = true;
            break;
        case 1007:
            if (optarg == NULL || optarg[0] == '\0' || strlen(optarg) >= sizeof(cfg->recovery_monitor))
                return DPPD_CONFIG_ERROR;
            strcpy(cfg->recovery_monitor, optarg);
            break;
        case 1008:
            if (optarg == NULL || *optarg == '\0' || strlen(optarg) >= sizeof(cfg->routing_path))
                return DPPD_CONFIG_ERROR;
            strcpy(cfg->routing_path, optarg);
            break;
        case 1009:
            if (parse_u32(optarg, 1, 65536, &cfg->conntrack_capacity) != 0)
                return DPPD_CONFIG_ERROR;
            break;
        case 1010:
            if (parse_u32(optarg, 1, 86400, &cfg->conntrack_timeout_s) != 0)
                return DPPD_CONFIG_ERROR;
            break;
        case 1011:
        case 1013:
            if (parse_snat(optarg, option == 1013 ? &cfg->snat6 : &cfg->snat, option == 1013) != 0)
                return DPPD_CONFIG_ERROR;
            break;
        case 1012:
        case 1014:
            if (cfg->dnat_count == DPPD_MAX_DNAT_RULES ||
                parse_dnat(optarg, &cfg->dnat[cfg->dnat_count], option == 1014) != 0)
                return DPPD_CONFIG_ERROR;
            cfg->dnat_count++;
            break;
        case 'h':
            return DPPD_CONFIG_HELP;
        default:
            return DPPD_CONFIG_ERROR;
        }
    }

    /* getopt 结束后仍有位置参数，通常表示拼写错误，不能静默忽略 */
    if (optind != argc)
        return DPPD_CONFIG_ERROR;
    if (cfg->tap_owner_cookie && cfg->recovery_path[0] == '\0')
        return DPPD_CONFIG_ERROR;
    if (cfg->recovery_monitor[0] != '\0' &&
        (cfg->recovery_path[0] == '\0' || strcmp(cfg->recovery_monitor, cfg->recovery_path) == 0 ||
            strcmp(cfg->recovery_monitor, cfg->state_path) == 0))
        return DPPD_CONFIG_ERROR;
    if (cfg->recovery_path[0] != '\0' &&
        (cfg->state_path[0] == '\0' || strcmp(cfg->recovery_path, cfg->state_path) == 0))
        return DPPD_CONFIG_ERROR;
    return DPPD_CONFIG_OK;
}

int dppd_config_validate(const struct dppd_config *cfg, char *error, uint32_t error_len)
{
    uint16_t i;
    uint16_t j;

    if (cfg == NULL || error == NULL || error_len == 0)
        return -1;
    if ((cfg->conntrack_capacity != 0 && cfg->routing_path[0] == '\0') ||
        ((cfg->snat.enabled || cfg->snat6.enabled || cfg->dnat_count != 0) && cfg->conntrack_capacity == 0) ||
        cfg->conntrack_capacity > 65536 || cfg->conntrack_timeout_s == 0 || cfg->conntrack_timeout_s > 86400) {
        snprintf(error, error_len,
            "conntrack requires IP routing, a bounded capacity and valid timeout; NAT requires conntrack");
        return -1;
    }
    if ((cfg->snat.enabled && cfg->snat.ipv6) || (cfg->snat6.enabled && !cfg->snat6.ipv6) ||
        dppd_nat_validate(&cfg->snat, cfg->dnat, cfg->dnat_count) != 0 ||
        dppd_nat_validate(&cfg->snat6, cfg->dnat, cfg->dnat_count) != 0) {
        snprintf(error, error_len, "invalid or conflicting NAT rules, or static reservations exhaust the SNAT pool");
        return -1;
    }
    for (uint32_t index = 0; index < cfg->dnat_count; ++index) {
        bool inside = false, outside = false;
        for (uint16_t port = 0; port < cfg->nb_ports && port < DPPD_MAX_PORTS; ++port) {
            inside |= cfg->ports[port] == cfg->dnat[index].inside_port;
            outside |= cfg->ports[port] == cfg->dnat[index].outside_port;
        }
        if (!inside || !outside) {
            snprintf(error, error_len, "DNAT requires configured inside/outside ports");
            return -1;
        }
    }
    for (unsigned int family = 0; family < 2; ++family) {
        const struct dppd_snat_config *nat = family ? &cfg->snat6 : &cfg->snat;
        if (!nat->enabled)
            continue;
        bool inside = false, outside = false;
        for (uint16_t index = 0; index < cfg->nb_ports && index < DPPD_MAX_PORTS; ++index) {
            inside |= cfg->ports[index] == nat->inside_port;
            outside |= cfg->ports[index] == nat->outside_port;
        }
        if (!inside || !outside) {
            snprintf(error, error_len, "SNAT requires configured inside/outside ports");
            return -1;
        }
    }
    if (cfg->recovery_monitor[0] != '\0' &&
        (cfg->recovery_path[0] == '\0' || strcmp(cfg->recovery_monitor, cfg->recovery_path) == 0 ||
            strcmp(cfg->recovery_monitor, cfg->state_path) == 0)) {
        snprintf(error, error_len, "recovery-monitor requires a distinct session file and recovery-path");
        return -1;
    }
    if (cfg->tap_owner_cookie && cfg->recovery_path[0] == '\0') {
        snprintf(error, error_len, "tap-owner-cookie requires recovery-path");
        return -1;
    }

    if (cfg->nb_ports == 0 || cfg->nb_ports > DPPD_MAX_PORTS ||
        (cfg->routing_path[0] == '\0' && (cfg->nb_ports < 2 || (cfg->nb_ports & 1U) != 0))) {
        if (cfg->routing_path[0] != '\0')
            snprintf(error, error_len, "IPv4 routing requires between 1 and %u ports", DPPD_MAX_PORTS);
        else
            snprintf(error, error_len, "ports must contain an even number of entries between 2 and %u", DPPD_MAX_PORTS);
        return -1;
    }
    if (cfg->nb_queues == 0 || cfg->nb_queues > DPPD_MAX_WORKERS) {
        snprintf(error, error_len, "queues must be between 1 and %u", DPPD_MAX_WORKERS);
        return -1;
    }
    if (cfg->burst_size == 0 || cfg->burst_size > DPPD_MAX_BURST) {
        snprintf(error, error_len, "burst must be between 1 and %u", DPPD_MAX_BURST);
        return -1;
    }
    if (cfg->rule_capacity == 0 || cfg->rule_capacity == UINT32_MAX) {
        snprintf(error, error_len, "rule-capacity must be between 1 and %u", UINT32_MAX - 1U);
        return -1;
    }
    if (cfg->control_socket[0] == '\0' || strlen(cfg->control_socket) > DPPD_MANAGEMENT_SOCKET_PATH_MAX) {
        snprintf(error, error_len, "control-socket path must contain 1..%u bytes", DPPD_MANAGEMENT_SOCKET_PATH_MAX);
        return -1;
    }
    /** 没有快照就没有可重放的期望状态，恢复记录也不能覆盖快照自身 */
    if (cfg->recovery_path[0] != '\0' &&
        (cfg->state_path[0] == '\0' || strcmp(cfg->recovery_path, cfg->state_path) == 0)) {
        snprintf(error, error_len, "recovery-path requires a distinct state-path");
        return -1;
    }
    /* 重复 port 会导致多个逻辑端点争用同一 RX/TX queue，启动前直接拒绝 */
    for (i = 0; i < cfg->nb_ports; ++i) {
        for (j = (uint16_t)(i + 1U); j < cfg->nb_ports; ++j) {
            if (cfg->ports[i] == cfg->ports[j]) {
                snprintf(error, error_len, "port %u is listed more than once", cfg->ports[i]);
                return -1;
            }
        }
    }

    error[0] = '\0';
    return 0;
}

void dppd_config_print_usage(const char *program)
{
    printf("usage: %s [EAL options] -- [application options]\n", program);
    printf("  --ports 0,1[,2,3]  adjacent entries form bidirectional port pairs\n");
    printf("  --queues N          one worker and one RX/TX queue per port for each N\n");
    printf("  --burst N           burst size, 1..%u\n", DPPD_MAX_BURST);
    printf("  --mbufs N           mbufs allocated for each NUMA socket\n");
    printf("  --cache N           per-lcore mempool cache size\n");
    printf("  --stats-ms N        statistics interval in milliseconds\n");
    printf("  --duration N        stop after N seconds; zero means run until signalled\n");
    printf("  --promisc           enable promiscuous mode\n");
    printf("  --enable-pdump      initialize the packet capture service\n");
    printf("  --control-socket P  management socket path (default %s)\n", DPPD_MANAGEMENT_DEFAULT_SOCKET);
    printf("  --rule-capacity N   maximum number of desired rules (default %u)\n", DPPD_DEFAULT_RULE_CAPACITY);
    printf("  --state-path P      enable durable rule snapshot and startup replay\n");
    printf("  --ipv4-routing P    enable IPv4 routing with a durable route and static-neighbor snapshot\n");
    printf("  --ip-routing P      enable dual-stack routing; ipv4-routing remains a compatible alias\n");
    printf("  --conntrack N       track IPv4/IPv6 TCP/UDP sessions, capacity 1..65536, requires IP routing\n");
    printf("  --ct-timeout N      established idle timeout in seconds (default 300)\n");
    printf("  --snat I,O,IP,A-B   translate inside I to outside O using public IPv4 and TCP/UDP port pool\n");
    printf("  --dnat O,I,PROTO,PUBLIC,PORTS,PRIVATE,PORTS  repeatable static TCP/UDP destination translation\n");
    printf("  --snat6 I,O,IP6,A-B  IPv6 source translation with a separate TCP/UDP port pool\n");
    printf("  --dnat6 O,I,PROTO,PUBLIC6,PORTS,PRIVATE6,PORTS  static IPv6 destination translation\n");
    printf("                      PORTS accepts N or A-B; public any requires private same\n");
    printf("  --recovery-path P   guard hardware replay after an unclean exit (requires state-path)\n");
    printf("  --tap-owner-cookie require native TAP owner cookie (patched driver and recovery-path)\n");
    printf("  --recovery-monitor P require a live monitor session before hardware installation\n");
}

void dppd_config_dump(const struct dppd_config *cfg)
{
    uint16_t i;

    printf("[dppd] ports=");
    for (i = 0; i < cfg->nb_ports; ++i)
        printf("%s%u", i == 0 ? "" : ",", cfg->ports[i]);
    printf(" queues=%u burst=%u mbufs/socket=%u cache=%u stats=%ums duration=%us"
           " rules=%u control=%s state=%s recovery=%s monitor=%s tap-owner-cookie=%s promisc=%s pdump=%s\n",
        cfg->nb_queues, cfg->burst_size, cfg->mbufs_per_socket, cfg->mbuf_cache, cfg->stats_period_ms, cfg->duration_s,
        cfg->rule_capacity, cfg->control_socket, cfg->state_path[0] == '\0' ? "disabled" : cfg->state_path,
        cfg->recovery_path[0] == '\0' ? "disabled" : cfg->recovery_path,
        cfg->recovery_monitor[0] == '\0' ? "disabled" : cfg->recovery_monitor, cfg->tap_owner_cookie ? "on" : "off",
        cfg->promiscuous ? "on" : "off", cfg->enable_pdump ? "on" : "off");
    if (cfg->routing_path[0] != '\0')
        printf("[dppd] forwarding=ipv4-routing routing-path=%s\n", cfg->routing_path);
    if (cfg->conntrack_capacity != 0)
        printf("[dppd] conntrack capacity=%u timeout=%us snat=%s snat6=%s dnat-rules=%u\n", cfg->conntrack_capacity,
            cfg->conntrack_timeout_s, cfg->snat.enabled ? "on" : "off", cfg->snat6.enabled ? "on" : "off",
            cfg->dnat_count);
}
