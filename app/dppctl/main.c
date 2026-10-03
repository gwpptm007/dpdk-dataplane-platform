#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include "dppd/management.h"

static void print_usage(const char *program)
{
    fprintf(stderr, "usage:\n");
    fprintf(stderr, "  %s [--socket PATH] ping\n", program);
    fprintf(stderr, "  %s [--socket PATH] health\n", program);
    fprintf(stderr, "  %s [--socket PATH] ready\n", program);
    fprintf(stderr, "  %s [--socket PATH] persistence-status\n", program);
    fprintf(stderr, "  %s [--socket PATH] persistence-flush\n", program);
    fprintf(stderr, "  %s [--socket PATH] reconcile-status\n", program);
    fprintf(stderr, "  %s [--socket PATH] reconcile-retry\n", program);
    fprintf(stderr, "  %s [--socket PATH] port-show PORT\n", program);
    fprintf(stderr, "  %s [--socket PATH] capability-show PORT\n", program);
    fprintf(stderr, "  %s [--socket PATH] probe-cache-clear PORT\n", program);
    fprintf(stderr, "  %s [--socket PATH] rule-metrics\n", program);
    fprintf(stderr, "  %s [--socket PATH] rule-latency\n", program);
    fprintf(stderr, "  %s [--socket PATH] probe-drop RULE_ID PORT [PRIORITY] [refresh]\n", program);
    fprintf(stderr, "  %s [--socket PATH] probe-filter RULE_ID PORT"
                    " ipv4|udp|tcp SRC_CIDR DST_CIDR SRC_PORT DST_PORT"
                    " drop|queue:N [count] [mark:N] [priority:N] [refresh]\n", program);
    fprintf(stderr, "  %s [--socket PATH] stats [PORT|all [QUEUE|all]]\n", program);
    fprintf(stderr,
            "  %s [--socket PATH] list [AFTER_RULE_ID [REPOSITORY_GENERATION]]\n",
            program);
    fprintf(stderr, "  %s [--socket PATH] get RULE_ID\n", program);
    fprintf(stderr, "  %s [--socket PATH] rule-status RULE_ID [EXPECTED_GENERATION]\n",
            program);
    fprintf(stderr, "  %s [--socket PATH] count RULE_ID EXPECTED_GENERATION\n",
            program);
    fprintf(stderr, "  %s [--socket PATH] delete RULE_ID EXPECTED_GENERATION\n",
            program);
    fprintf(stderr,
            "  %s [--socket PATH] delete-batch RULE_ID GENERATION RULE_ID GENERATION"
            " [RULE_ID GENERATION [RULE_ID GENERATION]]\n", program);
    fprintf(stderr,
            "  %s [--socket PATH] apply-filter RULE_ID PORT EXPECTED_GENERATION"
            " ipv4|udp|tcp SRC_CIDR DST_CIDR SRC_PORT DST_PORT"
            " drop|queue:N [count] [mark:N] [priority:N] [prefer|require|software]\n",
            program);
    fprintf(stderr, "  %s [--socket PATH] apply-drop RULE_ID PORT EXPECTED_GENERATION"
                    " [PRIORITY] [prefer|require|software]\n", program);
    fprintf(stderr, "  %s [--socket PATH] apply-drop-batch PORT RULE_ID RULE_ID"
                    " [RULE_ID [RULE_ID]]\n", program);
    fprintf(stderr, "  %s [--socket PATH] update-drop-batch PORT PRIORITY"
                    " prefer|require|software RULE_ID GENERATION RULE_ID GENERATION"
                    " [RULE_ID GENERATION [RULE_ID GENERATION]]\n", program);
    fprintf(stderr,
            "  %s [--socket PATH] apply-filter-drop RULE_ID PORT EXPECTED_GENERATION"
            " ipv4|udp|tcp SRC_CIDR DST_CIDR SRC_PORT DST_PORT"
            " [PRIORITY] [prefer|require|software]\n",
            program);
    fprintf(stderr, "EXPECTED_GENERATION may be 'any'; use 0 when creating a new rule.\n");
    fprintf(stderr, "Batch update/delete require exact nonzero generations.\n");
    fprintf(stderr, "update-drop-batch replaces each entire rule with ETH/DROP.\n");
    fprintf(stderr, "CIDR or L4 port may be 'any'; ipv4 requires both ports to be 'any'.\n");
}

static int parse_u64(const char *text, uint64_t min, uint64_t max,
                     uint64_t *value)
{
    char *end = NULL;
    unsigned long long parsed;

    /* strtoull 会接受负号并发生无符号转换，因此必须在调用前显式拒绝。 */
    if (text == NULL || value == NULL || text[0] == '-')
        return -EINVAL;
    errno = 0;
    parsed = strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || parsed < min || parsed > max)
        return -EINVAL;
    *value = (uint64_t)parsed;
    return 0;
}

static int parse_expected(const char *text, uint64_t *generation)
{
    /*
     * any 是有意绕过乐观并发保护的显式写法；数值 UINT64_MAX 保留为内部哨兵，
     * 普通 generation 解析不能通过十进制数间接构造该值。
     */
    if (strcmp(text, "any") == 0) {
        *generation = DPPD_RULE_GENERATION_ANY;
        return 0;
    }
    return parse_u64(text, 0, UINT64_MAX - 1U, generation);
}

/*
 * 把 IPv4 CIDR 转成 rule IR 使用的网络字节序地址和掩码。
 * “any”显式表示 address=0/mask=0；普通地址必须带 /0..32，避免调用方误以为
 * 裸地址是精确匹配还是全网段。地址会与掩码相与，统一成规范化网络地址。
 */
static int parse_ipv4_cidr(const char *text, uint32_t *address_be,
                           uint32_t *mask_be)
{
    char copy[INET_ADDRSTRLEN + 4U];
    struct in_addr address;
    char *slash;
    uint64_t prefix;
    uint32_t mask_host;

    if (text == NULL || address_be == NULL || mask_be == NULL)
        return -EINVAL;
    if (strcmp(text, "any") == 0) {
        *address_be = 0;
        *mask_be = 0;
        return 0;
    }
    if (strlen(text) >= sizeof(copy))
        return -EINVAL;
    memcpy(copy, text, strlen(text) + 1U);
    slash = strchr(copy, '/');
    if (slash == NULL || strchr(slash + 1, '/') != NULL)
        return -EINVAL;
    *slash = '\0';
    if (parse_u64(slash + 1, 0, 32, &prefix) != 0 ||
        inet_pton(AF_INET, copy, &address) != 1)
        return -EINVAL;

    mask_host = prefix == 0 ? 0 : UINT32_MAX << (32U - (uint32_t)prefix);
    *mask_be = htonl(mask_host);
    *address_be = address.s_addr & *mask_be;
    return 0;
}

/*
 * L4 端口在 rte_flow item 中使用网络字节序。数字 0 是合法精确值，
 * 所以通配语义必须使用单独的“any”，不能用 0 兼任。
 */
static int parse_l4_port(const char *text, uint16_t *port_be, uint16_t *mask_be)
{
    uint64_t value;

    if (text == NULL || port_be == NULL || mask_be == NULL)
        return -EINVAL;
    if (strcmp(text, "any") == 0) {
        *port_be = 0;
        *mask_be = 0;
        return 0;
    }
    if (parse_u64(text, 0, UINT16_MAX, &value) != 0)
        return -EINVAL;
    *port_be = htons((uint16_t)value);
    *mask_be = htons(UINT16_MAX);
    return 0;
}

/* 统一解析两个规则命令共享的可选 priority/fallback 尾部参数。 */
static int parse_rule_options(int argc, char **argv, int first,
                              struct dppd_rule *rule)
{
    uint64_t value;

    if (argc > first) {
        if (parse_u64(argv[first], 0, UINT32_MAX, &value) != 0)
            return -EINVAL;
        rule->priority = (uint32_t)value;
    }
    if (argc > first + 1) {
        if (strcmp(argv[first + 1], "require") == 0)
            rule->fallback = DPPD_FALLBACK_REQUIRE_HARDWARE;
        else if (strcmp(argv[first + 1], "software") == 0)
            rule->fallback = DPPD_FALLBACK_SOFTWARE_ONLY;
        else if (strcmp(argv[first + 1], "prefer") != 0)
            return -EINVAL;
    }
    return argc <= first + 2 ? 0 : -EINVAL;
}

/*
 * 构造过滤规则共享的 pattern。pattern 必须从外层协议向内排列：
 * ETH → IPv4 → UDP/TCP；该顺序既是 rule IR 的规范形式，也是 rte_flow 的要求。
 */
static int build_filter_pattern(struct dppd_rule *rule,
                                const char *protocol,
                                const char *source_cidr,
                                const char *destination_cidr,
                                const char *source_port,
                                const char *destination_port)
{
    struct dppd_match *ipv4;
    struct dppd_match *l4 = NULL;

    rule->nb_matches = 2;
    rule->matches[0].type = DPPD_MATCH_ETH;
    ipv4 = &rule->matches[1];
    ipv4->type = DPPD_MATCH_IPV4;
    if (strcmp(protocol, "udp") == 0) {
        rule->nb_matches = 3;
        l4 = &rule->matches[2];
        l4->type = DPPD_MATCH_UDP;
    } else if (strcmp(protocol, "tcp") == 0) {
        rule->nb_matches = 3;
        l4 = &rule->matches[2];
        l4->type = DPPD_MATCH_TCP;
    } else if (strcmp(protocol, "ipv4") != 0) {
        return -EINVAL;
    }

    if (parse_ipv4_cidr(source_cidr, &ipv4->spec.ipv4.src_be,
                        &ipv4->spec.ipv4.src_mask_be) != 0 ||
        parse_ipv4_cidr(destination_cidr, &ipv4->spec.ipv4.dst_be,
                        &ipv4->spec.ipv4.dst_mask_be) != 0)
        return -EINVAL;
    if (l4 != NULL) {
        if (parse_l4_port(source_port, &l4->spec.l4.src_be,
                          &l4->spec.l4.src_mask_be) != 0 ||
            parse_l4_port(destination_port, &l4->spec.l4.dst_be,
                          &l4->spec.l4.dst_mask_be) != 0)
            return -EINVAL;
    } else if (strcmp(source_port, "any") != 0 ||
               strcmp(destination_port, "any") != 0) {
        /* IPv4 item不表达端口，拒绝看似有效但实际会被忽略的数字参数。 */
        return -EINVAL;
    }
    return 0;
}

static int parse_prefixed_u64(const char *text, const char *prefix,
                              uint64_t max, uint64_t *value)
{
    const size_t prefix_length = strlen(prefix);

    if (strncmp(text, prefix, prefix_length) != 0)
        return -EINVAL;
    return parse_u64(text + prefix_length, 0, max, value);
}

/*
 * apply-filter 的 action 使用紧凑 token，而不是依赖参数位置：
 * fate 必须首先给出 drop 或 queue:N；其后 modifier 可任意排序但不可重复。
 * 最终始终按 MARK → COUNT → fate 生成 canonical action 顺序，确保幂等比较稳定。
 */
static int build_filter_actions(int argc, char **argv, int first,
                                struct dppd_rule *rule)
{
    enum dppd_action_type fate_type;
    uint16_t queue_id = 0;
    uint32_t mark_id = 0;
    uint64_t value;
    bool has_count = false;
    bool has_mark = false;
    bool has_priority = false;
    bool has_fallback = false;
    int index;

    if (argc <= first)
        return -EINVAL;
    if (strcmp(argv[first], "drop") == 0) {
        fate_type = DPPD_ACTION_DROP;
    } else if (parse_prefixed_u64(argv[first], "queue:", UINT16_MAX,
                                  &value) == 0) {
        fate_type = DPPD_ACTION_QUEUE;
        queue_id = (uint16_t)value;
    } else {
        return -EINVAL;
    }

    for (index = first + 1; index < argc; ++index) {
        if (strcmp(argv[index], "count") == 0) {
            if (has_count)
                return -EINVAL;
            has_count = true;
        } else if (parse_prefixed_u64(argv[index], "mark:", UINT32_MAX,
                                      &value) == 0) {
            if (has_mark)
                return -EINVAL;
            has_mark = true;
            mark_id = (uint32_t)value;
        } else if (parse_prefixed_u64(argv[index], "priority:", UINT32_MAX,
                                      &value) == 0) {
            if (has_priority)
                return -EINVAL;
            has_priority = true;
            rule->priority = (uint32_t)value;
        } else if (strcmp(argv[index], "require") == 0 ||
                   strcmp(argv[index], "prefer") == 0 ||
                   strcmp(argv[index], "software") == 0) {
            if (has_fallback)
                return -EINVAL;
            has_fallback = true;
            rule->fallback = strcmp(argv[index], "require") == 0 ?
                DPPD_FALLBACK_REQUIRE_HARDWARE :
                strcmp(argv[index], "software") == 0 ?
                DPPD_FALLBACK_SOFTWARE_ONLY : DPPD_FALLBACK_PREFER_HARDWARE;
        } else {
            return -EINVAL;
        }
    }

    rule->nb_actions = 0;
    if (has_mark) {
        rule->actions[rule->nb_actions].type = DPPD_ACTION_MARK;
        rule->actions[rule->nb_actions].conf.mark_id = mark_id;
        rule->nb_actions++;
    }
    if (has_count) {
        rule->actions[rule->nb_actions].type = DPPD_ACTION_COUNT;
        rule->nb_actions++;
    }
    rule->actions[rule->nb_actions].type = fate_type;
    if (fate_type == DPPD_ACTION_QUEUE)
        rule->actions[rule->nb_actions].conf.queue_id = queue_id;
    rule->nb_actions++;
    return 0;
}

static void initialize_request(struct dppd_management_request *request,
                               enum dppd_management_operation operation)
{
    struct timespec now;

    /* 清零整个结构也会清零 union padding/reserved，确保本地 ABI 报文可重复。 */
    memset(request, 0, sizeof(*request));
    (void)clock_gettime(CLOCK_MONOTONIC, &now);
    request->version = DPPD_MANAGEMENT_VERSION;
    request->operation = operation;
    request->size = sizeof(*request);
    /*
     * 当前客户端一次只保持一个在途请求，不需要全局唯一 ID；PID 与单调时钟
     * 纳秒部分足以发现串线响应。服务端只回显，不把该值作为幂等键。
     */
    request->request_id = ((uint64_t)(uint32_t)getpid() << 32) ^
                          (uint64_t)now.tv_nsec;
}

static int exchange(const char *socket_path,
                    const struct dppd_management_request *request,
                    struct dppd_management_response *response)
{
    struct sockaddr_un address;
    size_t path_length = strlen(socket_path);
    ssize_t bytes;
    int fd;
    int error;

    /* sun_path 是定长数组，必须在 memcpy 前为末尾 '\0' 预留一字节。 */
    if (path_length == 0 || path_length > DPPD_MANAGEMENT_SOCKET_PATH_MAX)
        return -ENAMETOOLONG;
    /* CLI 可阻塞等待一次响应；CLOEXEC 防止调用方扩展子进程逻辑时泄漏连接。 */
    fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -errno;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    memcpy(address.sun_path, socket_path, path_length + 1U);
    if (connect(fd, (const struct sockaddr *)&address, sizeof(address)) != 0) {
        error = -errno;
        close(fd);
        return error;
    }

    /* SOCK_SEQPACKET 保留消息边界；短发送或短响应都视为协议损坏。 */
    bytes = send(fd, request, sizeof(*request), MSG_NOSIGNAL);
    if (bytes != (ssize_t)sizeof(*request)) {
        error = bytes < 0 ? -errno : -EMSGSIZE;
        close(fd);
        return error;
    }
    bytes = recv(fd, response, sizeof(*response), 0);
    if (bytes != (ssize_t)sizeof(*response)) {
        error = bytes < 0 ? -errno : -EMSGSIZE;
        close(fd);
        return error;
    }
    close(fd);
    /*
     * transport 成功并不代表响应属于本请求。四项头字段全部匹配后，调用方才能
     * 按 operation 解释 union；否则返回 -EPROTO，绝不展示可能错位的 payload。
     */
    if (response->version != DPPD_MANAGEMENT_VERSION ||
        response->size != sizeof(*response) ||
        response->operation != request->operation ||
        response->request_id != request->request_id)
        return -EPROTO;
    return 0;
}

static int build_request(int argc, char **argv,
                         struct dppd_management_request *request)
{
    uint64_t value;

    /** 请求指标不接受额外参数，避免用户误以为能按某个 ID 筛选累计值 */
    if (argc == 1 && (strcmp(argv[0], "rule-metrics") == 0 || strcmp(argv[0], "rule-latency") == 0)) {
        initialize_request(request, strcmp(argv[0], "rule-metrics") == 0 ?
            DPPD_MANAGEMENT_RULE_METRICS : DPPD_MANAGEMENT_RULE_LATENCY);
        return 0;
    }
    /**
     * 探测复用正式 apply 的规则构造器，只在临时参数中补上新建版本零
     * 递归只发生一层，返回后改成独立的探测操作，发送到服务端时绝不会成为安装请求
     * refresh 必须在最后，参数数组有固定上限，重复或拼错的动作仍由原构造器拒绝
     */
    if (argc >= 1 && (strcmp(argv[0], "probe-drop") == 0 ||
                      strcmp(argv[0], "probe-filter") == 0)) {
        struct dppd_management_request built;
        char *apply_args[16];
        bool refresh = argc > 1 && strcmp(argv[argc - 1], "refresh") == 0;
        int rule_argc = argc - (refresh ? 1 : 0);
        int rc;

        if (rule_argc < 3 || rule_argc + 1 > (int)(sizeof(apply_args) / sizeof(apply_args[0])))
            return -EINVAL;
        apply_args[0] = strcmp(argv[0], "probe-drop") == 0 ? "apply-drop" : "apply-filter";
        apply_args[1] = argv[1];
        apply_args[2] = argv[2];
        apply_args[3] = "0";
        for (int index = 3; index < rule_argc; ++index)
            apply_args[index + 1] = argv[index];
        rc = build_request(rule_argc + 1, apply_args, &built);
        if (rc != 0)
            return rc;
        initialize_request(request, DPPD_MANAGEMENT_CAPABILITY_PROBE);
        request->payload.probe.install_port_id = built.payload.apply.install_port_id;
        request->payload.probe.rule = built.payload.apply.rule;
        request->payload.probe.refresh = refresh;
        return 0;
    }
    /** 画像查询与清缓存都只选择端口，不接受被忽略的额外参数 */
    if (argc == 2 && (strcmp(argv[0], "capability-show") == 0 ||
                      strcmp(argv[0], "probe-cache-clear") == 0)) {
        initialize_request(request, strcmp(argv[0], "capability-show") == 0 ?
            DPPD_MANAGEMENT_CAPABILITY_GET : DPPD_MANAGEMENT_CAPABILITY_CLEAR);
        if (parse_u64(argv[1], 0, UINT16_MAX, &value) != 0)
            return -EINVAL;
        request->payload.capability.port_id = (uint16_t)value;
        return 0;
    }
    /* 每个分支同时校验命令名和精确参数数量，拒绝被静默忽略的多余参数。 */
    if (argc >= 1 && argc <= 3 && strcmp(argv[0], "stats") == 0) {
        uint64_t selector;

        initialize_request(request, DPPD_MANAGEMENT_STATS_QUERY);
        request->payload.stats_query.port_id = DPPD_STATS_ALL;
        request->payload.stats_query.queue_id = DPPD_STATS_ALL;
        for (int index = 1; index < argc; ++index) {
            if (strcmp(argv[index], "all") == 0)
                continue;
            if (parse_u64(argv[index], 0, UINT16_MAX - 1, &selector) != 0)
                return -EINVAL;
            if (index == 1)
                request->payload.stats_query.port_id = (uint16_t)selector;
            else
                request->payload.stats_query.queue_id = (uint16_t)selector;
        }
        return 0;
    }
    /** 两个命令读取同一个快照，ready 额外把未就绪结果转换成脚本可检查的失败退出码 */
    if (argc == 1 && (strcmp(argv[0], "health") == 0 || strcmp(argv[0], "ready") == 0)) {
        initialize_request(request, DPPD_MANAGEMENT_HEALTH_GET);
        return 0;
    }
    if (argc == 1 && strcmp(argv[0], "ping") == 0) {
        initialize_request(request, DPPD_MANAGEMENT_PING);
        return 0;
    }
    if (argc == 1 && strcmp(argv[0], "persistence-status") == 0) {
        initialize_request(request, DPPD_MANAGEMENT_PERSISTENCE_STATUS);
        return 0;
    }
    if (argc == 1 && strcmp(argv[0], "persistence-flush") == 0) {
        initialize_request(request, DPPD_MANAGEMENT_PERSISTENCE_FLUSH);
        return 0;
    }
    if (argc == 1 && strcmp(argv[0], "reconcile-status") == 0) {
        initialize_request(request, DPPD_MANAGEMENT_RECOVERY_STATUS);
        return 0;
    }
    if (argc == 1 && strcmp(argv[0], "reconcile-retry") == 0) {
        initialize_request(request, DPPD_MANAGEMENT_RECOVERY_RETRY);
        return 0;
    }
    if (argc == 2 && strcmp(argv[0], "port-show") == 0) {
        initialize_request(request, DPPD_MANAGEMENT_PORT_GET);
        if (parse_u64(argv[1], 0, UINT16_MAX, &value) != 0)
            return -EINVAL;
        request->payload.port_get.port_id = (uint16_t)value;
        return 0;
    }
    if (argc >= 1 && argc <= 3 && strcmp(argv[0], "list") == 0) {
        initialize_request(request, DPPD_MANAGEMENT_RULE_LIST);
        request->payload.list.expected_repository_generation =
            DPPD_RULE_GENERATION_ANY;
        if (argc >= 2 &&
            parse_u64(argv[1], 0, UINT64_MAX,
                      &request->payload.list.after_rule_id) != 0)
            return -EINVAL;
        if (argc == 3 &&
            parse_expected(argv[2],
                           &request->payload.list.expected_repository_generation) != 0)
            return -EINVAL;
        return 0;
    }
    if (argc == 2 && strcmp(argv[0], "get") == 0) {
        initialize_request(request, DPPD_MANAGEMENT_RULE_GET);
        if (parse_u64(argv[1], 1, UINT64_MAX, &request->payload.get.rule_id) != 0)
            return -EINVAL;
        return 0;
    }
    /**
     * 默认查询该 ID 当前版本，用户也可带上上次看到的非零版本以检测并发更新
     * 零只用于创建规则，状态查询拒绝零，数值最大值仍保留为内部 ANY 哨兵
     */
    if ((argc == 2 || argc == 3) && strcmp(argv[0], "rule-status") == 0) {
        initialize_request(request, DPPD_MANAGEMENT_RULE_STATUS);
        request->payload.rule_status.expected_generation = DPPD_RULE_GENERATION_ANY;
        if (parse_u64(argv[1], 1, UINT64_MAX,
                      &request->payload.rule_status.rule_id) != 0)
            return -EINVAL;
        if (argc == 3 &&
            (parse_expected(argv[2],
                            &request->payload.rule_status.expected_generation) != 0 ||
             request->payload.rule_status.expected_generation == 0))
            return -EINVAL;
        return 0;
    }
    if (argc == 3 && strcmp(argv[0], "delete") == 0) {
        initialize_request(request, DPPD_MANAGEMENT_RULE_DELETE);
        if (parse_u64(argv[1], 1, UINT64_MAX,
                      &request->payload.delete_rule.rule_id) != 0 ||
            parse_expected(argv[2],
                           &request->payload.delete_rule.expected_generation) != 0)
            return -EINVAL;
        return 0;
    }
    if (argc >= 5 && argc <= 9 && ((argc - 1) % 2) == 0 &&
        strcmp(argv[0], "delete-batch") == 0) {
        uint32_t i;

        /*
         * 批量删除有意不接受 any 或 0：用户必须把 get/list 得到的精确 generation 成对
         * 提供，daemon 才能在删除任何 actual object 前确认整批仍是同一份 desired 视图。
         */
        initialize_request(request, DPPD_MANAGEMENT_RULE_DELETE_BATCH);
        request->payload.delete_batch.count = (uint16_t)((argc - 1) / 2);
        for (i = 0; i < request->payload.delete_batch.count; ++i) {
            struct dppd_control_batch_remove_request *entry =
                &request->payload.delete_batch.rules[i];

            if (parse_u64(argv[i * 2 + 1], 1, UINT64_MAX, &entry->rule_id) != 0 ||
                parse_u64(argv[i * 2 + 2], 1, UINT64_MAX - 1U,
                          &entry->expected_generation) != 0)
                return -EINVAL;
        }
        return 0;
    }
    if (argc == 3 && strcmp(argv[0], "count") == 0) {
        initialize_request(request, DPPD_MANAGEMENT_RULE_COUNT_QUERY);
        if (parse_u64(argv[1], 1, UINT64_MAX,
                      &request->payload.count_query.rule_id) != 0 ||
            parse_expected(argv[2],
                           &request->payload.count_query.expected_generation) != 0)
            return -EINVAL;
        return 0;
    }
    /**
     * 批量更新命令前四项是命令名、端口、优先级和后端策略，后面每两项是一组 ID/旧版本
     * 因此八到十二个参数恰好表达两到四条规则，奇数个尾部参数说明存在不完整的一组
     * 该命令构造完整的 Ethernet DROP 新规则，不会只改优先级而保留旧匹配条件或动作
     */
    if (argc >= 8 && argc <= 12 && (argc - 4) % 2 == 0 &&
        strcmp(argv[0], "update-drop-batch") == 0) {
        struct dppd_rule template = {0};
        uint64_t priority;
        uint32_t i;

        if (parse_u64(argv[1], 0, UINT16_MAX, &value) != 0 ||
            parse_u64(argv[2], 0, UINT32_MAX, &priority) != 0)
            return -EINVAL;
        if (strcmp(argv[3], "prefer") == 0)
            template.fallback = DPPD_FALLBACK_PREFER_HARDWARE;
        else if (strcmp(argv[3], "require") == 0)
            template.fallback = DPPD_FALLBACK_REQUIRE_HARDWARE;
        else if (strcmp(argv[3], "software") == 0)
            template.fallback = DPPD_FALLBACK_SOFTWARE_ONLY;
        else
            return -EINVAL;
        template.domain = DPPD_RULE_DOMAIN_INGRESS;
        template.priority = (uint32_t)priority;
        template.nb_matches = 1;
        template.matches[0].type = DPPD_MATCH_ETH;
        template.nb_actions = 1;
        template.actions[0].type = DPPD_ACTION_DROP;
        /**
         * 所有条目复用同一份端口外的规则模板，再分别填写稳定 ID 和精确旧版本
         * 只生成一个批量管理请求，不能在客户端循环发送单规则请求来冒充原子更新
         */
        initialize_request(request, DPPD_MANAGEMENT_RULE_UPDATE_BATCH);
        request->payload.update_batch.count = (uint16_t)((argc - 4) / 2);
        for (i = 0; i < request->payload.update_batch.count; ++i) {
            struct dppd_control_batch_update_request *entry =
                &request->payload.update_batch.rules[i];
            uint32_t j;

            entry->rule = template;
            entry->install_port_id = (uint16_t)value;
            /**
             * 旧版本必须大于零且不能是保留值 UINT64_MAX，字符串 any 也不会被数字解析接受
             * 用户应使用最近一次 get/list 读到的版本，避免覆盖其他请求刚刚修改过的规则
             */
            if (parse_u64(argv[4 + i * 2], 1, UINT64_MAX, &entry->rule.id) != 0 ||
                parse_u64(argv[5 + i * 2], 1, UINT64_MAX - 1U,
                          &entry->expected_generation) != 0)
                return -EINVAL;
            for (j = 0; j < i; ++j) {
                if (request->payload.update_batch.rules[j].rule.id == entry->rule.id)
                    return -EINVAL;
            }
        }
        return 0;
    }
    if (argc >= 4 && argc <= 6 && strcmp(argv[0], "apply-drop-batch") == 0) {
        uint32_t i;

        /**
         * CLI 首版故意收窄为：同一 ingress port、ETH 匹配、DROP 动作的纯新建。它不是
         * apply-drop 的简单循环，而是一次管理协议请求，daemon 会为所有条目创建同一
         * transaction；任何一条 ID 非法、重复或 backend 失败，均不会留下其它条目。
         */
        initialize_request(request, DPPD_MANAGEMENT_RULE_CREATE_BATCH);
        if (parse_u64(argv[1], 0, UINT16_MAX, &value) != 0)
            return -EINVAL;
        request->payload.create_batch.count = (uint16_t)(argc - 2);
        for (i = 0; i < request->payload.create_batch.count; ++i) {
            struct dppd_control_batch_create_request *entry =
                &request->payload.create_batch.rules[i];

            if (parse_u64(argv[i + 2], 1, UINT64_MAX, &entry->rule.id) != 0)
                return -EINVAL;
            entry->install_port_id = (uint16_t)value;
            /* 0 明确要求“此前不存在”；批量 CLI 不提供 ANY 或 update，以保持可补偿性。 */
            entry->expected_generation = 0;
            entry->rule.domain = DPPD_RULE_DOMAIN_INGRESS;
            /*
             * 优先走 rte_flow；仅 PMD 的无副作用 validate 明确失败且该 ETH/DROP 规则
             * 由软件后端等价支持时，控制层才将本条降级为 software。
             */
            entry->rule.fallback = DPPD_FALLBACK_PREFER_HARDWARE;
            entry->rule.nb_matches = 1;
            entry->rule.matches[0].type = DPPD_MATCH_ETH;
            entry->rule.nb_actions = 1;
            entry->rule.actions[0].type = DPPD_ACTION_DROP;
        }
        return 0;
    }
    if (argc >= 4 && argc <= 6 && strcmp(argv[0], "apply-drop") == 0) {
        struct dppd_rule *rule;

        initialize_request(request, DPPD_MANAGEMENT_RULE_APPLY);
        /*
         * apply-drop 是验证管理闭环的最小规则构造器：匹配所有 Ethernet ingress，
         * fate action 为 DROP。generation 保持 0，由 daemon 在提交成功后统一分配。
         */
        rule = &request->payload.apply.rule;
        if (parse_u64(argv[1], 1, UINT64_MAX, &rule->id) != 0 ||
            parse_u64(argv[2], 0, UINT16_MAX, &value) != 0 ||
            parse_expected(argv[3], &request->payload.apply.expected_generation) != 0)
            return -EINVAL;
        request->payload.apply.install_port_id = (uint16_t)value;
        rule->domain = DPPD_RULE_DOMAIN_INGRESS;
        /* 默认 prefer；仅当硬件 validate 失败且规则可等价执行时才回退软件 backend。 */
        rule->fallback = DPPD_FALLBACK_PREFER_HARDWARE;
        rule->nb_matches = 1;
        rule->matches[0].type = DPPD_MATCH_ETH;
        rule->nb_actions = 1;
        rule->actions[0].type = DPPD_ACTION_DROP;
        return parse_rule_options(argc, argv, 4, rule);
    }
    if (argc >= 9 && argc <= 11 &&
        strcmp(argv[0], "apply-filter-drop") == 0) {
        struct dppd_rule *rule;

        initialize_request(request, DPPD_MANAGEMENT_RULE_APPLY);
        rule = &request->payload.apply.rule;
        if (parse_u64(argv[1], 1, UINT64_MAX, &rule->id) != 0 ||
            parse_u64(argv[2], 0, UINT16_MAX, &value) != 0 ||
            parse_expected(argv[3], &request->payload.apply.expected_generation) != 0)
            return -EINVAL;
        request->payload.apply.install_port_id = (uint16_t)value;

        rule->domain = DPPD_RULE_DOMAIN_INGRESS;
        rule->fallback = DPPD_FALLBACK_PREFER_HARDWARE;
        if (build_filter_pattern(rule, argv[4], argv[5], argv[6],
                                 argv[7], argv[8]) != 0)
            return -EINVAL;
        rule->nb_actions = 1;
        rule->actions[0].type = DPPD_ACTION_DROP;
        return parse_rule_options(argc, argv, 9, rule);
    }
    if (argc >= 10 && strcmp(argv[0], "apply-filter") == 0) {
        struct dppd_rule *rule;

        initialize_request(request, DPPD_MANAGEMENT_RULE_APPLY);
        rule = &request->payload.apply.rule;
        if (parse_u64(argv[1], 1, UINT64_MAX, &rule->id) != 0 ||
            parse_u64(argv[2], 0, UINT16_MAX, &value) != 0 ||
            parse_expected(argv[3], &request->payload.apply.expected_generation) != 0)
            return -EINVAL;
        request->payload.apply.install_port_id = (uint16_t)value;
        rule->domain = DPPD_RULE_DOMAIN_INGRESS;
        rule->fallback = DPPD_FALLBACK_PREFER_HARDWARE;
        if (build_filter_pattern(rule, argv[4], argv[5], argv[6],
                                 argv[7], argv[8]) != 0 ||
            build_filter_actions(argc, argv, 9, rule) != 0)
            return -EINVAL;
        return 0;
    }
    return -EINVAL;
}

static const char *domain_name(uint8_t domain)
{
    switch ((enum dppd_rule_domain)domain) {
    case DPPD_RULE_DOMAIN_INGRESS:
        return "ingress";
    case DPPD_RULE_DOMAIN_EGRESS:
        return "egress";
    case DPPD_RULE_DOMAIN_TRANSFER:
        return "transfer";
    }
    return "unknown";
}

static const char *fallback_name(uint8_t fallback)
{
    switch ((enum dppd_fallback_policy)fallback) {
    case DPPD_FALLBACK_REQUIRE_HARDWARE:
        return "require-hardware";
    case DPPD_FALLBACK_PREFER_HARDWARE:
        return "prefer-hardware";
    case DPPD_FALLBACK_SOFTWARE_ONLY:
        return "software-only";
    }
    return "unknown";
}

static const char *recovery_state_name(
    enum dppd_control_recovery_state state)
{
    switch (state) {
    case DPPD_CONTROL_RECOVERY_READY:
        return "ready";
    case DPPD_CONTROL_RECOVERY_RECONCILIATION_REQUIRED:
        return "reconciliation-required";
    case DPPD_CONTROL_RECOVERY_RESTART_REQUIRED:
        return "restart-required";
    }
    return "unknown";
}

/**
 * 在一行中输出所有未就绪原因和关键计数，便于人工查看和部署脚本读取
 * live=yes 只说明管理主线程成功响应，ready 则需要线程、端口和规则状态共同满足条件
 */
static void print_health(const struct dppd_management_health *health)
{
    static const struct {
        uint32_t mask;
        const char *name;
    } reasons[] = {
        {DPPD_HEALTH_NO_RUNTIME, "no-runtime"},
        {DPPD_HEALTH_STOP_REQUESTED, "stop-requested"},
        {DPPD_HEALTH_DEVICE_REMOVED, "device-removed"},
        {DPPD_HEALTH_WORKERS_NOT_RUNNING, "workers-not-running"},
        {DPPD_HEALTH_PORTS_NOT_STARTED, "ports-not-started"},
        {DPPD_HEALTH_LINK_DOWN, "link-down"},
        {DPPD_HEALTH_LINK_UNKNOWN, "link-unknown"},
        {DPPD_HEALTH_RECOVERY_REQUIRED, "recovery-required"},
        {DPPD_HEALTH_PERSISTENCE_DIRTY, "persistence-dirty"},
        {DPPD_HEALTH_WORKER_FAILED, "worker-failed"},
    };
    bool first = true;

    printf("health live=yes ready=%s blockers=0x%x reasons=",
           health->ready ? "yes" : "no", health->blockers);
    for (size_t i = 0; i < sizeof(reasons) / sizeof(reasons[0]); ++i) {
        if ((health->blockers & reasons[i].mask) != 0) {
            printf("%s%s", first ? "" : ",", reasons[i].name);
            first = false;
        }
    }
    if (first)
        printf("%s", health->blockers == 0 ? "none" : "unknown");
    printf(" workers=%u/%u workers-failed=%u ports=%u/%u"
           " links-up=%u links-down=%u links-unknown=%u links-unsupported=%u"
           " recovery=%s persistence-enabled=%s persistence-dirty=%s"
           " persisted-generation=%" PRIu64 " current-generation=%" PRIu64
           " rules=%u persistence-error=%d recovery-error=%d\n",
           health->workers_running, health->workers_expected, health->workers_failed,
           health->ports_started, health->ports_expected, health->links_up,
           health->links_down, health->links_unknown, health->links_unsupported,
           recovery_state_name((enum dppd_control_recovery_state)health->recovery_state),
           health->persistence_enabled ? "yes" : "no",
           health->persistence_dirty ? "yes" : "no", health->persisted_generation,
           health->repository_generation, health->rule_count,
           health->persistence_last_error, health->recovery_last_error);
}

static void print_ipv4_value(const char *label, uint32_t address_be,
                             uint32_t mask_be)
{
    struct in_addr address = {.s_addr = address_be};
    struct in_addr mask = {.s_addr = mask_be};
    char address_text[INET_ADDRSTRLEN];
    char mask_text[INET_ADDRSTRLEN];

    if (mask_be == 0) {
        printf(" %s=any", label);
        return;
    }
    /*
     * IR 允许任意 IPv4 mask，不强制连续 CIDR；因此 get 使用 address/mask 原样
     * 输出，而不是尝试转换成可能丢失信息的 prefix length。
     */
    if (inet_ntop(AF_INET, &address, address_text, sizeof(address_text)) == NULL ||
        inet_ntop(AF_INET, &mask, mask_text, sizeof(mask_text)) == NULL) {
        printf(" %s=<invalid>", label);
        return;
    }
    printf(" %s=%s/%s", label, address_text, mask_text);
}

static void print_rule_detail(const struct dppd_rule *rule)
{
    uint16_t i;

    printf("rule id=%" PRIu64 " generation=%" PRIu64
           " install-port=%u domain=%s fallback=%s group=%u priority=%u\n",
           rule->id, rule->generation, rule->install_port_id,
           domain_name((uint8_t)rule->domain),
           fallback_name((uint8_t)rule->fallback), rule->group, rule->priority);
    for (i = 0; i < rule->nb_matches; ++i) {
        const struct dppd_match *match = &rule->matches[i];

        printf("  match[%u] ", i);
        switch (match->type) {
        case DPPD_MATCH_ETH:
            printf("eth");
            break;
        case DPPD_MATCH_IPV4:
            printf("ipv4");
            print_ipv4_value("src", match->spec.ipv4.src_be,
                             match->spec.ipv4.src_mask_be);
            print_ipv4_value("dst", match->spec.ipv4.dst_be,
                             match->spec.ipv4.dst_mask_be);
            break;
        case DPPD_MATCH_UDP:
        case DPPD_MATCH_TCP:
            printf("%s", match->type == DPPD_MATCH_UDP ? "udp" : "tcp");
            if (match->spec.l4.src_mask_be == 0)
                printf(" src=any");
            else
                printf(" src=%u/mask:0x%04x", ntohs(match->spec.l4.src_be),
                       ntohs(match->spec.l4.src_mask_be));
            if (match->spec.l4.dst_mask_be == 0)
                printf(" dst=any");
            else
                printf(" dst=%u/mask:0x%04x", ntohs(match->spec.l4.dst_be),
                       ntohs(match->spec.l4.dst_mask_be));
            break;
        case DPPD_MATCH_REPRESENTED_PORT:
            printf("represented-port port=%u", match->spec.ethdev_port_id);
            break;
        default:
            printf("unknown type=%d", match->type);
            break;
        }
        printf("\n");
    }
    for (i = 0; i < rule->nb_actions; ++i) {
        const struct dppd_action *action = &rule->actions[i];

        printf("  action[%u] ", i);
        switch (action->type) {
        case DPPD_ACTION_DROP:
            printf("drop");
            break;
        case DPPD_ACTION_QUEUE:
            printf("queue index=%u", action->conf.queue_id);
            break;
        case DPPD_ACTION_MARK:
            printf("mark id=%u", action->conf.mark_id);
            break;
        case DPPD_ACTION_COUNT:
            printf("count");
            break;
        case DPPD_ACTION_REPRESENTED_PORT:
            printf("represented-port port=%u", action->conf.ethdev_port_id);
            break;
        default:
            printf("unknown type=%d", action->type);
            break;
        }
        printf("\n");
    }
}

/** 探测回答的是当前能否校验完整规则，校验失败本身仍是一份成功取得的诊断结果 */
static const char *probe_status_name(enum dppd_flow_probe_status status)
{
    switch (status) {
    case DPPD_FLOW_PROBE_SUPPORTED:
        return "supported";
    case DPPD_FLOW_PROBE_UNSUPPORTED:
        return "unsupported";
    case DPPD_FLOW_PROBE_REJECTED:
        return "rejected";
    case DPPD_FLOW_PROBE_UNAVAILABLE:
        return "unavailable";
    }
    return "unknown";
}

/** 启动能力与历史校验分行显示，不把观察过的元素集合当成任意组合的支持承诺 */
static void print_capability(const struct dppd_management_capability_profile *profile)
{
    const struct dppd_management_port_info *port = &profile->port;
    const struct dppd_capability_identity *identity = &profile->identity;
    const struct dppd_flow_probe_statistics *probes = &profile->probes;

    printf("capability port=%u device=%s driver=%s dpdk=%s firmware=%s firmware-error=%d"
           " socket=%d kind=%s configured=%s started=%s\n",
           port->port_id, identity->device_name_known ? identity->device_name : "unknown",
           port->driver_name, identity->dpdk_version[0] != '\0' ? identity->dpdk_version : "unknown",
           identity->firmware_known ? identity->firmware_version : "unknown", identity->firmware_error,
           port->socket_id, port->endpoint_kind == DPPD_ENDPOINT_REPRESENTOR ? "representor" : "ethdev",
           port->configured ? "yes" : "no", port->started ? "yes" : "no");
    printf("  queues=%u max-rx=%u max-tx=%u rx-desc=%u rx-desc-min=%u rx-desc-max=%u"
           " rx-desc-align=%u tx-desc=%u tx-desc-min=%u tx-desc-max=%u tx-desc-align=%u"
           " rss-capabilities=0x%" PRIx64 " rss-configured=0x%" PRIx64
           " rx-offloads=0x%" PRIx64 " tx-offloads=0x%" PRIx64
           " tx-configured=0x%" PRIx64 " device-capabilities=0x%" PRIx64 "\n",
           identity->configured_queues, port->max_rx_queues, port->max_tx_queues,
           identity->configured_rx_desc, identity->rx_desc_min, identity->rx_desc_max,
           identity->rx_desc_align, identity->configured_tx_desc, identity->tx_desc_min,
           identity->tx_desc_max, identity->tx_desc_align, port->rss_offloads,
           port->configured_rss_hf, port->rx_offloads, port->tx_offloads,
           port->configured_tx_offloads, port->device_capabilities);
    printf("  flow-probes epoch=%" PRIu64 " validations=%" PRIu64 " supported=%" PRIu64
           " unsupported=%" PRIu64 " failed=%" PRIu64 " hits=%" PRIu64 " misses=%" PRIu64
           " invalidations=%" PRIu64 " cache-entries=%u cache-capacity=%u ttl-ms=%" PRIu64
           " observed-domains=0x%x observed-items=0x%x observed-actions=0x%x\n",
           probes->epoch, probes->validations, probes->supported, probes->unsupported, probes->failed,
           probes->cache_hits, probes->cache_misses, probes->invalidations, probes->cache_entries,
           probes->cache_capacity, profile->cache_ttl_ns / UINT64_C(1000000),
           probes->observed_domains, probes->observed_items, probes->observed_actions);
}

static void print_response(const struct dppd_management_response *response)
{
    /* 仅在 main 完成协议头和 status 校验后进入这里，union 成员才可安全解释。 */
    switch (response->operation) {
    case DPPD_MANAGEMENT_RULE_LATENCY:
        /** 均值和分位数的可用性单独展示，未知时钟不能被当作零耗时样本 */
        for (unsigned int scope = 0; scope < DPPD_RULE_LATENCY_SCOPE_COUNT; ++scope) {
            const struct dppd_rule_latency_summary *summary = &response->payload.rule_latency.scopes[scope];
            const struct dppd_rule_latency_histogram *histogram = &summary->histogram;

            printf("rule-latency scope=%s samples=%" PRIu64 " unavailable=%" PRIu64
                   " rules=%" PRIu64 " total-ns=%" PRIu64 " min-ns=%" PRIu64 " max-ns=%" PRIu64
                   " mean-available=%s mean-ns=%" PRIu64 " quantiles-available=%s"
                   " p50-upper-ns=%" PRIu64 " p95-upper-ns=%" PRIu64 " p99-upper-ns=%" PRIu64
                   " total-saturated=%s counters-saturated=%s\n",
                   dppd_rule_latency_scope_name(scope), histogram->samples, histogram->unavailable,
                   histogram->rules, histogram->total_ns, histogram->min_ns, histogram->max_ns,
                   summary->mean_available ? "yes" : "no", summary->mean_ns,
                   summary->quantiles_available ? "yes" : "no", summary->p50_upper_ns,
                   summary->p95_upper_ns, summary->p99_upper_ns,
                   histogram->total_saturated ? "yes" : "no", histogram->counters_saturated ? "yes" : "no");
            /** 区间采用左开右闭，第一段包含零，最后一段的最大整数表示开放上界 */
            for (unsigned int bucket = 0; bucket < DPPD_RULE_LATENCY_BUCKETS; ++bucket)
                printf("  bucket scope=%s index=%u upper-ns=%" PRIu64 " samples=%" PRIu64 "\n",
                       dppd_rule_latency_scope_name(scope), bucket,
                       dppd_rule_latency_bucket_upper(bucket), histogram->buckets[bucket]);
        }
        break;
    case DPPD_MANAGEMENT_RULE_METRICS: {
        const struct dppd_rule_metrics *metrics = &response->payload.rule_metrics;
        const struct dppd_rule_failure_event *last = &metrics->last;

        printf("rule-metrics ");
#define PRINT_RULE_METRIC(field) printf(#field "=%" PRIu64 " ", metrics->field);
        DPPD_RULE_METRIC_FIELDS(PRINT_RULE_METRIC)
#undef PRINT_RULE_METRIC
        printf("\n  failures ");
        for (unsigned int kind = DPPD_RULE_FAILURE_INPUT; kind < DPPD_RULE_FAILURE_KIND_COUNT; ++kind)
            printf("failed_%s=%" PRIu64 " ", dppd_rule_failure_kind_name(kind), metrics->failures[kind]);
        printf("\n  last-failure sequence=%" PRIu64 " operation=%s stage=%s kind=%s"
               " rule=%" PRIu64 " generation=%" PRIu64 " transaction=%" PRIu64 " rules=%u port=",
               last->sequence, dppd_rule_operation_name(last->operation),
               dppd_rule_failure_stage_name(last->stage), dppd_rule_failure_kind_name(last->kind),
               last->rule_id, last->generation, last->transaction_id, last->rule_count);
        if (last->port_known)
            printf("%u", last->install_port_id);
        else
            printf("unknown");
        printf(" backend=%s cause-error=%d response-error=%d compensation-error=%d"
               " compensation-rule=%" PRIu64 " last-applied=%s\n",
               !last->backend_known ? "unknown" :
               last->backend == DPPD_PLAN_BACKEND_SOFTWARE ? "software" : "rte_flow",
               last->cause_code, last->response_code, last->compensation_code,
               last->compensation_rule_id, last->applied ? "yes" : "no");
        break;
    }
    case DPPD_MANAGEMENT_CAPABILITY_GET:
    case DPPD_MANAGEMENT_CAPABILITY_CLEAR:
        print_capability(&response->payload.capability);
        break;
    case DPPD_MANAGEMENT_CAPABILITY_PROBE: {
        const struct dppd_flow_probe_result *probe = &response->payload.probe;

        printf("flow-probe rule=%" PRIu64 " port=%u hardware=%s software=%s cached=%s"
               " hardware-error=%d epoch=%" PRIu64 " age-ms=",
               probe->rule_id, probe->install_port_id, probe_status_name(probe->status),
               probe->software_equivalent ? "yes" : "no", probe->cached ? "yes" : "no",
               probe->validation_code, probe->epoch);
        if (probe->age_available)
            printf("%" PRIu64 "\n", probe->age_ns / UINT64_C(1000000));
        else
            printf("unknown\n");
        if (probe->validation_code != 0 && probe->message[0] != '\0')
            printf("  detail: %s\n", probe->message);
        break;
    }
    case DPPD_MANAGEMENT_HEALTH_GET:
        print_health(&response->payload.health);
        break;
    case DPPD_MANAGEMENT_STATS_QUERY:
#define PRINT_STATS_FIELD(field) printf(#field "=%" PRIu64 " ", response->payload.stats.field);
        DPPD_STATS_FIELDS(PRINT_STATS_FIELD)
#undef PRINT_STATS_FIELD
        printf("\n");
        break;
    case DPPD_MANAGEMENT_PING:
        printf("pong version=%u generation=%" PRIu64 " rules=%u\n",
               response->version,
               response->payload.pong.repository_generation,
               response->payload.pong.rule_count);
        break;
    case DPPD_MANAGEMENT_PORT_GET:
        printf("port=%u peer=%u kind=%s driver=%s socket=%d state=%s/%s link=%s "
               "queues(rx=%u,tx=%u) reta=%u mac=%02x:%02x:%02x:%02x:%02x:%02x\n",
               response->payload.port.port_id,
               response->payload.port.peer_port_id,
               response->payload.port.endpoint_kind == DPPD_ENDPOINT_REPRESENTOR ?
                   "representor" : "ethdev",
               response->payload.port.driver_name,
               response->payload.port.socket_id,
               response->payload.port.configured ? "configured" : "unconfigured",
               response->payload.port.started ? "started" : "stopped",
               dppd_link_state_name(response->payload.port.link_state),
               response->payload.port.max_rx_queues,
               response->payload.port.max_tx_queues,
               response->payload.port.reta_size,
               response->payload.port.mac[0], response->payload.port.mac[1],
               response->payload.port.mac[2], response->payload.port.mac[3],
               response->payload.port.mac[4], response->payload.port.mac[5]);
        printf("rss-cap=0x%" PRIx64 " rss-enabled=0x%" PRIx64
               " rx-offloads=0x%" PRIx64 " tx-offloads=0x%" PRIx64
               " tx-enabled=0x%" PRIx64 " dev-cap=0x%" PRIx64 "\n",
               response->payload.port.rss_offloads,
               response->payload.port.configured_rss_hf,
               response->payload.port.rx_offloads,
               response->payload.port.tx_offloads,
               response->payload.port.configured_tx_offloads,
               response->payload.port.device_capabilities);
        if (response->payload.port.has_switch_domain)
            printf("switch-domain=%u switch-port=%u switch=%s\n",
                   response->payload.port.switch_domain_id,
                   response->payload.port.switch_port_id,
                   response->payload.port.switch_name);
        else
            printf("switch-domain=none\n");
        break;
    case DPPD_MANAGEMENT_RULE_GET:
        print_rule_detail(&response->payload.rule);
        break;
    case DPPD_MANAGEMENT_RULE_APPLY:
        printf("applied status=%d generation=%" PRIu64 " transaction=%" PRIu64
               " backend=%d reason=%d\n",
               response->payload.apply.status, response->payload.apply.generation,
               response->payload.apply.transaction_id,
               response->payload.apply.plan.backend,
               response->payload.apply.plan.reason);
        break;
    case DPPD_MANAGEMENT_RULE_CREATE_BATCH: {
        uint16_t i;

        printf("batch-applied count=%u\n", response->payload.create_batch.count);
        for (i = 0; i < response->payload.create_batch.count; ++i) {
            const struct dppd_control_apply_result *result =
                &response->payload.create_batch.rules[i];

            printf("  generation=%" PRIu64 " transaction=%" PRIu64
                   " backend=%d reason=%d\n",
                   result->generation, result->transaction_id,
                   result->plan.backend, result->plan.reason);
        }
        break;
    }
    case DPPD_MANAGEMENT_RULE_UPDATE_BATCH: {
        /**
         * 主程序已确认整个请求成功后才进入此分支，下面的每行结果对应一个输入条目
         * 同时输出规则 ID、新版本和事务编号，便于核对这一批是否属于同一次完整更新
         */
        uint16_t i;

        printf("batch-updated count=%u\n", response->payload.update_batch.count);
        for (i = 0; i < response->payload.update_batch.count; ++i) {
            const struct dppd_control_apply_result *result =
                &response->payload.update_batch.rules[i];

            printf("  rule=%" PRIu64 " generation=%" PRIu64 " transaction=%" PRIu64
                   " backend=%d reason=%d\n",
                   result->plan.rule_id, result->generation, result->transaction_id,
                   result->plan.backend, result->plan.reason);
        }
        break;
    }
    case DPPD_MANAGEMENT_RULE_DELETE:
        printf("deleted=%s generation=%" PRIu64 "\n",
               response->payload.delete_rule.removed ? "yes" : "no",
               response->payload.delete_rule.generation);
        break;
    case DPPD_MANAGEMENT_RULE_DELETE_BATCH: {
        uint16_t i;

        printf("batch-deleted count=%u\n", response->payload.delete_batch.count);
        for (i = 0; i < response->payload.delete_batch.count; ++i) {
            const struct dppd_control_batch_remove_result *result =
                &response->payload.delete_batch.rules[i];

            printf("  rule=%" PRIu64 " generation=%" PRIu64 "\n",
                   result->rule_id, result->generation);
        }
        break;
    }
    case DPPD_MANAGEMENT_RULE_STATUS: {
        const struct dppd_control_rule_status *status = &response->payload.rule_status;
        const struct dppd_rule_install_info *installed = &status->installation;

        /** 未配置 COUNT 也能查询状态，耗时未知时明确显示 unknown，避免把零当成有效测量 */
        printf("rule-status rule=%" PRIu64 " generation=%" PRIu64
               " port=%u backend=%s fallback=%s count=%s install-scope=%s"
               " commit-rules=%u install-ns=",
               installed->rule_id, installed->generation, installed->install_port_id,
               status->backend == DPPD_PLAN_BACKEND_RTE_FLOW ? "rte_flow" : "software",
               fallback_name((uint8_t)status->fallback), installed->has_count ? "yes" : "no",
               installed->commit_rule_count > 1 ? "batch" : "rule",
               installed->commit_rule_count);
        if (installed->timing_available)
            printf("%" PRIu64, installed->install_duration_ns);
        else
            printf("unknown");
        printf(" persistence=%s dirty=%s persisted-generation=%" PRIu64
               " repository-generation=%" PRIu64 " last-error=%d\n",
               status->persistence.enabled ? "enabled" : "disabled",
               status->persistence.dirty ? "yes" : "no",
               status->persistence.persisted_generation,
               status->persistence.current_generation, status->persistence.last_error);
        break;
    }
    case DPPD_MANAGEMENT_RULE_COUNT_QUERY:
        printf("count rule=%" PRIu64 " generation=%" PRIu64 " hits=%" PRIu64
               " bytes=%" PRIu64 "\n",
               response->payload.count.rule_id,
               response->payload.count.generation,
               response->payload.count.hits,
               response->payload.count.bytes);
        break;
    case DPPD_MANAGEMENT_RULE_LIST: {
        const struct dppd_management_rule_page *page =
            &response->payload.rule_page;
        uint16_t i;

        printf("repository-generation=%" PRIu64 " total=%u page-count=%u"
               " more=%s next=%" PRIu64 "\n",
               page->repository_generation, page->total_count, page->count,
               page->has_more ? "yes" : "no", page->next_after_rule_id);
        for (i = 0; i < page->count; ++i) {
            const struct dppd_management_rule_summary *rule = &page->rules[i];

            printf("  id=%" PRIu64 " generation=%" PRIu64 " install-port=%u"
                   " domain=%s fallback=%s group=%u priority=%u"
                   " matches=%u mask=0x%x actions=%u mask=0x%x\n",
                   rule->rule_id, rule->generation, rule->install_port_id,
                   domain_name(rule->domain),
                   fallback_name(rule->fallback), rule->group, rule->priority,
                   rule->nb_matches, rule->match_mask, rule->nb_actions,
                   rule->action_mask);
        }
        if (page->has_more)
            printf("next-command: list %" PRIu64 " %" PRIu64 "\n",
                   page->next_after_rule_id, page->repository_generation);
        break;
    }
    case DPPD_MANAGEMENT_PERSISTENCE_STATUS:
    case DPPD_MANAGEMENT_PERSISTENCE_FLUSH:
        printf("persistence enabled=%s dirty=%s persisted-generation=%" PRIu64
               " current-generation=%" PRIu64 " last-error=%d\n",
               response->payload.persistence.enabled ? "yes" : "no",
               response->payload.persistence.dirty ? "yes" : "no",
               response->payload.persistence.persisted_generation,
               response->payload.persistence.current_generation,
               response->payload.persistence.last_error);
        break;
    case DPPD_MANAGEMENT_RECOVERY_STATUS:
    case DPPD_MANAGEMENT_RECOVERY_RETRY:
        printf("recovery state=%s residual-objects=%u last-error=%d\n",
               recovery_state_name(response->payload.recovery.state),
               response->payload.recovery.residual_objects,
               response->payload.recovery.last_error);
        break;
    default:
        break;
    }
}

int main(int argc, char **argv)
{
    const char *socket_path = DPPD_MANAGEMENT_DEFAULT_SOCKET;
    struct dppd_management_request request;
    struct dppd_management_response response;
    bool require_ready;
    int argument = 1;
    int rc;

    /* 当前只支持全局 --socket 前缀，剩余 argv 完整交给命令构造器校验。 */
    if (argc >= 3 && strcmp(argv[1], "--socket") == 0) {
        socket_path = argv[2];
        argument = 3;
    }
    rc = build_request(argc - argument, argv + argument, &request);
    if (rc != 0) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }
    /** 参数校验已通过，此时可以安全读取命令名，health 查询不会因 ready=no 自行失败 */
    require_ready = request.operation == DPPD_MANAGEMENT_HEALTH_GET &&
                    strcmp(argv[argument], "ready") == 0;
    rc = exchange(socket_path, &request, &response);
    if (rc != 0) {
        fprintf(stderr, "dppctl: transport failed: %s (%d)\n", strerror(-rc), rc);
        return EXIT_FAILURE;
    }
    /* transport 错误与 daemon 业务错误分开报告，便于脚本区分连接和规则失败。 */
    if (response.status != 0) {
        int status = response.status;

        fprintf(stderr, "dppctl: request failed: %s (%d)\n",
                status < 0 && status >= -4095 ? strerror(-status) : "unknown error",
                status);
        /*
         * EUCLEAN 与普通“完全未执行”错误不同：硬件和内存 repository 可能已经提交，
         * 只是 snapshot 保存失败。apply 可以幂等重试，但 delete 已生效后用旧
         * generation 重试会返回 ESTALE，因此统一引导操作者先显式 flush 再核对状态。
         */
        if (status == -EUCLEAN &&
            (response.operation == DPPD_MANAGEMENT_RULE_APPLY ||
             response.operation == DPPD_MANAGEMENT_RULE_CREATE_BATCH ||
             response.operation == DPPD_MANAGEMENT_RULE_DELETE)) {
            fprintf(stderr,
                    "dppctl: operation may already be active, but the snapshot "
                    "is dirty; fix storage, run persistence-flush, then verify "
                    "with persistence-status and get/list\n");
        }
        if (status == -EUCLEAN) {
            fprintf(stderr,
                    "dppctl: daemon may be in recovery isolation; run "
                    "reconcile-status and, after fixing the PMD/device issue, "
                    "reconcile-retry\n");
        }
        return EXIT_FAILURE;
    }
    print_response(&response);
    /** 先打印明确原因，再给就绪探针返回非零，避免脚本只能得到一个没有解释的失败码 */
    if (require_ready && !response.payload.health.ready)
        return EXIT_FAILURE;
    return EXIT_SUCCESS;
}
