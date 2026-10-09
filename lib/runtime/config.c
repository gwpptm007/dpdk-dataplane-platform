#include "dppd/config.h"
#include "dppd/management.h"

#include <errno.h>
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

    /* 同时检查 errno、完整消费和调用方给定范围，禁止 12abc 这类部分解析。 */
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

    /* strtok_r 会原地写入分隔符，因此先复制到有界的局部缓冲区。 */
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

void dppd_config_defaults(struct dppd_config *cfg)
{
    /* 先清零确保新增字段有确定默认值，再覆盖可运行的最小双端口配置。 */
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
    /* sizeof 字符串字面量包含末尾 '\0'，复制后路径一定终止。 */
    memcpy(cfg->control_socket, DPPD_MANAGEMENT_DEFAULT_SOCKET,
           sizeof(DPPD_MANAGEMENT_DEFAULT_SOCKET));
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
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0},
    };
    int option;

    if (cfg == NULL)
        return DPPD_CONFIG_ERROR;

    /* 每次解析从默认配置开始，避免调用方复用结构体时残留旧参数。 */
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
            /* 数组容量检查使用 >=，为 C 字符串末尾 '\0' 留出空间。 */
            if (optarg == NULL || optarg[0] == '\0' ||
                strlen(optarg) >= sizeof(cfg->control_socket))
                return DPPD_CONFIG_ERROR;
            memcpy(cfg->control_socket, optarg, strlen(optarg) + 1U);
            break;
        case 1003:
            /* UINT32_MAX 被 repository/backend 用作非法容量哨兵，不能暴露给用户。 */
            if (parse_u32(optarg, 1, UINT32_MAX - 1U,
                          &cfg->rule_capacity) != 0)
                return DPPD_CONFIG_ERROR;
            break;
        case 1004:
            /*
             * 空路径没有“启用但无目标”的可靠语义；长度检查同时为末尾 NUL 留位。
             * 父目录是否存在、是否可写由启动恢复真实打开文件时判定。
             */
            if (optarg == NULL || optarg[0] == '\0' ||
                strlen(optarg) >= sizeof(cfg->state_path))
                return DPPD_CONFIG_ERROR;
            memcpy(cfg->state_path, optarg, strlen(optarg) + 1U);
            break;
        case 1005:
            /** 恢复记录必须有明确路径，启动时再检查文件锁、格式和快照绑定 */
            if (optarg == NULL || optarg[0] == '\0' || strlen(optarg) >= sizeof(cfg->recovery_path))
                return DPPD_CONFIG_ERROR;
            memcpy(cfg->recovery_path, optarg, strlen(optarg) + 1U);
            break;
        case 'h':
            return DPPD_CONFIG_HELP;
        default:
            return DPPD_CONFIG_ERROR;
        }
    }

    /* getopt 结束后仍有位置参数，通常表示拼写错误，不能静默忽略。 */
    if (optind != argc)
        return DPPD_CONFIG_ERROR;
    if (cfg->recovery_path[0] != '\0' && (cfg->state_path[0] == '\0' ||
        strcmp(cfg->recovery_path, cfg->state_path) == 0))
        return DPPD_CONFIG_ERROR;
    return DPPD_CONFIG_OK;
}

int dppd_config_validate(const struct dppd_config *cfg, char *error, uint32_t error_len)
{
    uint16_t i;
    uint16_t j;

    if (cfg == NULL || error == NULL || error_len == 0)
        return -1;

    if (cfg->nb_ports < 2 || cfg->nb_ports > DPPD_MAX_PORTS || (cfg->nb_ports & 1U) != 0) {
        snprintf(error, error_len, "ports must contain an even number of entries between 2 and %u",
                 DPPD_MAX_PORTS);
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
        snprintf(error, error_len, "rule-capacity must be between 1 and %u",
                 UINT32_MAX - 1U);
        return -1;
    }
    if (cfg->control_socket[0] == '\0' ||
        strlen(cfg->control_socket) > DPPD_MANAGEMENT_SOCKET_PATH_MAX) {
        snprintf(error, error_len, "control-socket path must contain 1..%u bytes",
                 DPPD_MANAGEMENT_SOCKET_PATH_MAX);
        return -1;
    }
    /** 没有快照就没有可重放的期望状态，恢复记录也不能覆盖快照自身 */
    if (cfg->recovery_path[0] != '\0' && (cfg->state_path[0] == '\0' ||
        strcmp(cfg->recovery_path, cfg->state_path) == 0)) {
        snprintf(error, error_len, "recovery-path requires a distinct state-path");
        return -1;
    }
    /* 重复 port 会导致多个逻辑端点争用同一 RX/TX queue，启动前直接拒绝。 */
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
    printf("  --control-socket P  management socket path (default %s)\n",
           DPPD_MANAGEMENT_DEFAULT_SOCKET);
    printf("  --rule-capacity N   maximum number of desired rules (default %u)\n",
           DPPD_DEFAULT_RULE_CAPACITY);
    printf("  --state-path P      enable durable rule snapshot and startup replay\n");
    printf("  --recovery-path P   guard hardware replay after an unclean exit (requires state-path)\n");
}

void dppd_config_dump(const struct dppd_config *cfg)
{
    uint16_t i;

    printf("[dppd] ports=");
    for (i = 0; i < cfg->nb_ports; ++i)
        printf("%s%u", i == 0 ? "" : ",", cfg->ports[i]);
    printf(" queues=%u burst=%u mbufs/socket=%u cache=%u stats=%ums duration=%us"
           " rules=%u control=%s state=%s recovery=%s promisc=%s pdump=%s\n",
           cfg->nb_queues,
           cfg->burst_size,
           cfg->mbufs_per_socket,
           cfg->mbuf_cache,
           cfg->stats_period_ms,
           cfg->duration_s,
           cfg->rule_capacity,
           cfg->control_socket,
           cfg->state_path[0] == '\0' ? "disabled" : cfg->state_path,
           cfg->recovery_path[0] == '\0' ? "disabled" : cfg->recovery_path,
           cfg->promiscuous ? "on" : "off",
           cfg->enable_pdump ? "on" : "off");
}
