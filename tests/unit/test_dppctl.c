/**
 * 直接包含 CLI 源码，测试用户实际使用的命令解析逻辑，避免另写一份解析器造成验证失真
 * 临时改名原来的 main，让本文件可以定义自己的测试入口
 * 测试只构造请求结构，不连接管理 socket，也不会真正安装或修改规则
 */
#define main dppctl_program_main
#include "../../app/dppctl/main.c"
#undef main
#include <assert.h>

/** ACL 命令明确区分端口范围、协议号和旧版本，整批文件任一错误都不能生成部分请求 */
static void test_acl_arguments(void)
{
    struct dppd_management_request request;
    char *args[] = {"acl-set", "10", "3", "0", "20", "udp", "198.18.0.0/24", "10.2.0.0/16", "1000-2000", "80-90",
        "allow", "count", "mark:7"};
    assert(build_request(13, args, &request) == 0);
    assert(request.payload.apply.rule.fallback == DPPD_FALLBACK_SOFTWARE_ONLY);
    assert(request.payload.apply.rule.actions[2].type == DPPD_ACTION_ALLOW);
    assert(request.payload.apply.rule.matches[2].spec.range.src_first_be == htons(1000));
    assert(request.payload.apply.rule.matches[2].spec.range.dst_last_be == htons(90));
    const char *bad[] = {"2000-1000", "-1", "0-65536", "1-", "1-2-3", "any-extra"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(*bad); ++i) {
        args[8] = (char *)bad[i];
        assert(build_request(13, args, &request) == -EINVAL);
    }
    args[8] = "0-65535";
    args[9] = "0";
    assert(build_request(13, args, &request) == 0);
    args[3] = "any";
    assert(build_request(13, args, &request) == -EINVAL);
    args[3] = "0";
    args[5] = "ip:17";
    assert(build_request(13, args, &request) == -EINVAL);
    args[8] = args[9] = "any";
    assert(build_request(13, args, &request) == 0);
    assert(request.payload.apply.rule.matches[2].type == DPPD_MATCH_IP_PROTOCOL);
    assert(request.payload.apply.rule.matches[2].spec.protocol.value == 17);
    char path[] = "/tmp/dppctl-acl-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    const char text[] = "10 3 1 20 udp any any 1000-2000 80-90 allow count\n"
                        "11 3 2 30 ip:17 any any any any drop\n";
    assert(write(fd, text, sizeof(text) - 1) == sizeof(text) - 1);
    char *batch[] = {"acl-update-batch", path};
    assert(build_request(2, batch, &request) == 0);
    assert(request.operation == DPPD_MANAGEMENT_RULE_UPDATE_BATCH && request.payload.update_batch.count == 2);
    assert(request.payload.update_batch.rules[0].expected_generation == 1);
    assert(request.payload.update_batch.rules[1].rule.matches[2].spec.protocol.value == 17);
    assert(write(fd, "\0", 1) == 1);
    assert(build_request(2, batch, &request) == -EINVAL);
    assert(ftruncate(fd, 0) == 0 && lseek(fd, 0, SEEK_SET) == 0);
    assert(write(fd, text, sizeof(text) - 1) == sizeof(text) - 1);
    const char duplicate[] = "10 3 1 20 ipv4 any any any any drop\n";
    assert(write(fd, duplicate, sizeof(duplicate) - 1) == sizeof(duplicate) - 1);
    assert(build_request(2, batch, &request) == -EINVAL);
    close(fd);
    assert(unlink(path) == 0);
    assert(build_request(2, batch, &request) == -ENOENT);
    char *stateful[] = {"acl-set", "12", "0", "0", "10", "udp", "any", "any", "any", "any", "allow", "count", "mark:9",
        "state:new,established"};
    assert(build_request(14, stateful, &request) == 0);
    assert(request.payload.apply.rule.matches[3].type == DPPD_MATCH_CONNECTION_STATE);
    assert(request.payload.apply.rule.matches[3].spec.connection_states == (DPPD_CT_NEW | DPPD_CT_ESTABLISHED));
    stateful[13] = "state:new,,established";
    assert(build_request(14, stateful, &request) == -EINVAL);
    stateful[13] = "state:new,new";
    assert(build_request(14, stateful, &request) == -EINVAL);
    char *query[] = {"conntrack-list", "123", "7"};
    assert(build_request(3, query, &request) == 0 && request.payload.conntrack.expected_revision == 7);
    query[0] = "conntrack-flush";
    assert(build_request(2, query, &request) == 0 && request.payload.conntrack.expected_revision == 123);
    query[1] = "0";
    assert(build_request(2, query, &request) == -EINVAL);
    query[1] = "any";
    assert(build_request(2, query, &request) == -EINVAL);
    query[0] = "nat-status";
    assert(build_request(1, query, &request) == 0 && request.operation == DPPD_MANAGEMENT_NAT_STATUS);
    assert(build_request(2, query, &request) == -EINVAL);
    query[0] = "dnat-list";
    assert(build_request(1, query, &request) == 0 && request.operation == DPPD_MANAGEMENT_DNAT_LIST);
    query[1] = "4";
    assert(build_request(2, query, &request) == 0 && request.payload.conntrack.after_id == 4);
    assert(build_request(3, query, &request) == -EINVAL);
    query[1] = "-1";
    assert(build_request(2, query, &request) == -EINVAL);
}

/** IPv6 前缀和地址族不能混用，非整字节掩码与范围端口完整保留，跟踪状态不能伪造 */
static void test_ipv6_arguments(void)
{
    struct dppd_management_request request;
    char *arguments[] = {"acl6-set", "90", "0", "0", "10", "tcp", "2001:db8:1::ffff/65", "2001:db8:2::7/128",
        "1000-2000", "443", "allow", "count", "state:untracked"};
    assert(build_request(13, arguments, &request) == 0);
    assert(request.payload.apply.rule.matches[1].type == DPPD_MATCH_IPV6);
    assert(request.payload.apply.rule.matches[1].spec.ipv6.src_mask[8] == 0x80);
    assert(request.payload.apply.rule.matches[1].spec.ipv6.src[15] == 0);
    arguments[12] = "state:established";
    assert(build_request(13, arguments, &request) == 0);
    arguments[12] = "state:untracked";
    arguments[6] = "192.0.2.0/24";
    assert(build_request(13, arguments, &request) == -EINVAL);
    char *route[] = {"route6-set", "2001:db8:2::7/65", "1", "fe80::1", "9"};
    assert(build_request(5, route, &request) == 0 && request.operation == DPPD_MANAGEMENT_ROUTE6_SET);
    assert(request.payload.route6.entry.depth == 65 && request.payload.route6.entry.prefix[15] == 0);
    route[1] = "2001:db8::/129";
    assert(build_request(5, route, &request) == -EINVAL);
    char *neighbor[] = {"neighbor6-set", "1", "fe80::1", "02:00:00:00:00:01", "9"};
    assert(build_request(5, neighbor, &request) == 0 && request.operation == DPPD_MANAGEMENT_NEIGHBOR6_SET);
    neighbor[2] = "fe80::1%eth0";
    assert(build_request(5, neighbor, &request) == -EINVAL);
}

/** 路由命令先完整解析再连接服务，错误文件不能被当成空配置发送 */
static void test_routing_arguments(void)
{
    struct dppd_management_request request;
    char *route[] = {"route-set", "10.2.3.4/16", "2", "direct", "7"};
    assert(build_request(5, route, &request) == 0);
    assert(request.operation == DPPD_MANAGEMENT_ROUTE_SET);
    assert(request.payload.route.expected_generation == 7);
    assert(request.payload.route.entry.prefix_be == htonl(0x0a020000));
    assert(request.payload.route.entry.depth == 16 && request.payload.route.entry.port_id == 2);
    route[4] = "any";
    assert(build_request(5, route, &request) == -EINVAL);
    route[4] = "7";
    route[1] = "10.0.0.1/33";
    assert(build_request(5, route, &request) == -EINVAL);
    char *neighbor[] = {"neighbor-set", "2", "192.0.2.1", "02:00:00:00:00:07", "9"};
    assert(build_request(5, neighbor, &request) == 0);
    assert(request.payload.neighbor.entry.mac[5] == 7);
    neighbor[3] = "02:00:00:00:00:777";
    assert(build_request(5, neighbor, &request) == -EINVAL);
    char path[] = "/tmp/dppctl-routing-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    const char text[] = "# complete pair\nroute 10.2.3.4/16 2 192.0.2.1\nneighbor 2 192.0.2.1 02:00:00:00:00:07\n";
    assert(write(fd, text, sizeof(text) - 1) == sizeof(text) - 1);
    char *replace[] = {"routing-replace", path, "9"};
    assert(build_request(3, replace, &request) == 0);
    assert(request.payload.routing_replace.configuration.route_count == 1);
    assert(request.payload.routing_replace.configuration.neighbor_count == 1);
    assert(write(fd, "broken\n", 7) == 7);
    assert(build_request(3, replace, &request) == -EINVAL);
    assert(ftruncate(fd, 0) == 0 && lseek(fd, 0, SEEK_SET) == 0);
    assert(write(fd, "\0", 1) == 1);
    assert(build_request(3, replace, &request) == -EINVAL);
    assert(ftruncate(fd, 65537) == 0);
    assert(build_request(3, replace, &request) != 0);
    close(fd);
    assert(unlink(path) == 0);
    assert(build_request(3, replace, &request) == -ENOENT);
}

/** 状态查询默认读当前版本，也可携带精确非零版本，非法参数要在连接服务前被拒绝 */
static void test_status_arguments(void)
{
    struct dppd_management_request request;
    char *args[] = {"rule-status", "100", "any", "extra"};
    const char *invalid[] = {"0", "-1", "wrong", "18446744073709551615", "18446744073709551616"};

    assert(build_request(2, args, &request) == 0);
    assert(request.operation == DPPD_MANAGEMENT_RULE_STATUS);
    assert(request.payload.rule_status.rule_id == 100);
    assert(request.payload.rule_status.expected_generation == DPPD_RULE_GENERATION_ANY);
    assert(build_request(3, args, &request) == 0);
    assert(request.payload.rule_status.expected_generation == DPPD_RULE_GENERATION_ANY);
    args[2] = "7";
    assert(build_request(3, args, &request) == 0);
    assert(request.payload.rule_status.expected_generation == 7);
    args[2] = "18446744073709551614";
    assert(build_request(3, args, &request) == 0);
    assert(build_request(1, args, &request) == -EINVAL);
    assert(build_request(4, args, &request) == -EINVAL);
    for (unsigned int i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        args[2] = (char *)invalid[i];
        assert(build_request(3, args, &request) == -EINVAL);
    }
    args[1] = "0";
    assert(build_request(2, args, &request) == -EINVAL);
    args[1] = "-1";
    assert(build_request(2, args, &request) == -EINVAL);
}

/** 失败历史允许零游标和零版本，最大整数只允许作游标，不能作为显式历史版本 */
static void test_history_arguments(void)
{
    struct dppd_management_request request;
    char *args[] = {"rule-history", "0", "0", "extra"};
    const char *invalid[] = {"-1", "+1", " 1", "", "wrong", "18446744073709551616"};

    assert(build_request(1, args, &request) == 0 && request.operation == DPPD_MANAGEMENT_RULE_HISTORY);
    assert(request.payload.rule_history.after_event_id == 0);
    assert(request.payload.rule_history.expected_revision == DPPD_RULE_HISTORY_REVISION_ANY);
    assert(build_request(2, args, &request) == 0);
    assert(build_request(3, args, &request) == 0 && request.payload.rule_history.expected_revision == 0);
    assert(build_request(4, args, &request) == -EINVAL);
    args[1] = "18446744073709551615";
    args[2] = "18446744073709551614";
    assert(build_request(3, args, &request) == 0);
    assert(request.payload.rule_history.after_event_id == UINT64_MAX);
    args[2] = "18446744073709551615";
    assert(build_request(3, args, &request) == -EINVAL);
    for (unsigned int index = 0; index < sizeof(invalid) / sizeof(invalid[0]); ++index) {
        args[1] = (char *)invalid[index];
        assert(build_request(2, args, &request) == -EINVAL);
        args[1] = "0";
        args[2] = (char *)invalid[index];
        assert(build_request(3, args, &request) == -EINVAL);
    }
}

/** 探测必须生成独立的诊断请求，不能因复用规则构造器而变成真正的安装命令 */
static void test_probe_arguments(void)
{
    struct dppd_management_request request;
    char *profile[] = {"capability-show", "5", "extra"};
    char *drop[] = {"probe-drop", "100", "5", "17", "refresh", "extra"};
    char *filter[] = {"probe-filter", "101", "6", "tcp", "192.0.2.0/24", "any", "any", "80", "queue:1", "count",
        "mark:7", "priority:19", "refresh"};

    assert(build_request(2, profile, &request) == 0);
    assert(request.operation == DPPD_MANAGEMENT_CAPABILITY_GET);
    assert(request.payload.capability.port_id == 5);
    assert(build_request(1, profile, &request) == -EINVAL);
    assert(build_request(3, profile, &request) == -EINVAL);
    profile[0] = "probe-cache-clear";
    assert(build_request(2, profile, &request) == 0);
    assert(request.operation == DPPD_MANAGEMENT_CAPABILITY_CLEAR);
    profile[1] = "65536";
    assert(build_request(2, profile, &request) == -EINVAL);
    profile[1] = "-1";
    assert(build_request(2, profile, &request) == -EINVAL);

    assert(build_request(3, drop, &request) == 0);
    assert(request.operation == DPPD_MANAGEMENT_CAPABILITY_PROBE);
    assert(request.payload.probe.rule.id == 100 && request.payload.probe.install_port_id == 5);
    assert(!request.payload.probe.refresh && request.payload.probe.rule.generation == 0);
    assert(build_request(4, drop, &request) == 0);
    assert(request.payload.probe.rule.priority == 17);
    assert(build_request(5, drop, &request) == 0 && request.payload.probe.refresh);
    assert(build_request(2, drop, &request) == -EINVAL);
    assert(build_request(6, drop, &request) == -EINVAL);
    drop[3] = "refresh";
    assert(build_request(4, drop, &request) == 0 && request.payload.probe.refresh);
    assert(build_request(5, drop, &request) == -EINVAL);
    drop[1] = "0";
    assert(build_request(3, drop, &request) == -EINVAL);
    drop[1] = "100";
    drop[2] = "-1";
    assert(build_request(3, drop, &request) == -EINVAL);

    /** 掩码和队列参数沿用正式规则语义，动作顺序稳定为 MARK、COUNT、QUEUE */
    assert(build_request(13, filter, &request) == 0);
    assert(request.version == DPPD_MANAGEMENT_VERSION);
    assert(request.operation == DPPD_MANAGEMENT_CAPABILITY_PROBE && request.payload.probe.refresh);
    assert(request.payload.probe.install_port_id == 6 && request.payload.probe.rule.id == 101);
    assert(request.payload.probe.rule.nb_matches == 3 && request.payload.probe.rule.priority == 19);
    assert(request.payload.probe.rule.nb_actions == 3);
    assert(request.payload.probe.rule.actions[0].type == DPPD_ACTION_MARK);
    assert(request.payload.probe.rule.actions[0].conf.mark_id == 7);
    assert(request.payload.probe.rule.actions[1].type == DPPD_ACTION_COUNT);
    assert(request.payload.probe.rule.actions[2].type == DPPD_ACTION_QUEUE);
    assert(request.payload.probe.rule.actions[2].conf.queue_id == 1);
    assert(build_request(12, filter, &request) == 0 && !request.payload.probe.refresh);
    assert(build_request(8, filter, &request) == -EINVAL);
    filter[10] = "count";
    assert(build_request(13, filter, &request) == -EINVAL);
    filter[10] = "mark:7";
    filter[9] = "refresh";
    assert(build_request(13, filter, &request) == -EINVAL);
    filter[9] = "count";
    filter[4] = "192.0.2.0/33";
    assert(build_request(13, filter, &request) == -EINVAL);
}

/** transfer 规划始终只读，安装必须携带精确账本版本，不能把缺失参数或重复选项默默补全 */
static void test_transfer_arguments(void)
{
    struct dppd_management_request request;
    char *plan[] = {"transfer-plan", "100", "5", "6", "7", "pair", "oneway", "count", "priority:23", "extra"};
    for (unsigned int scenario = 0; scenario < DPPD_TRANSFER_TEMPLATE_COUNT; ++scenario) {
        plan[5] = (char *)dppd_transfer_template_name(scenario);
        assert(build_request(7, plan, &request) == 0);
        assert(request.version == DPPD_MANAGEMENT_VERSION && request.size == sizeof(request));
        assert(request.operation == DPPD_MANAGEMENT_TRANSFER_PLAN);
        assert(request.payload.transfer.first_rule_id == 100 && request.payload.transfer.install_port_id == 5);
        assert(request.payload.transfer.source_port_id == 6 && request.payload.transfer.destination_port_id == 7);
        assert(request.payload.transfer.scenario == scenario && !request.payload.transfer.bidirectional);
        assert(request.payload.transfer.expected_repository_generation == DPPD_RULE_GENERATION_ANY);
        assert(!request.payload.transfer.with_count && request.payload.transfer.priority == 0);
        for (unsigned int i = 0; i < sizeof(request.payload.transfer.reserved); ++i)
            assert(request.payload.transfer.reserved[i] == 0);
    }
    plan[5] = "pair";
    plan[6] = "bidir";
    assert(build_request(9, plan, &request) == 0);
    assert(request.payload.transfer.bidirectional && request.payload.transfer.with_count);
    assert(request.payload.transfer.priority == 23);
    assert(build_request(6, plan, &request) == -EINVAL);
    assert(build_request(10, plan, &request) == -EINVAL);
    plan[7] = "priority:4294967295";
    plan[8] = "count";
    assert(build_request(9, plan, &request) == 0 && request.payload.transfer.priority == UINT32_MAX);
    plan[7] = "count";
    assert(build_request(9, plan, &request) == -EINVAL);
    plan[7] = "priority:1";
    plan[8] = "priority:2";
    assert(build_request(9, plan, &request) == -EINVAL);
    const char *bad_options[] = {"priority:", "priority:-1", "priority:4294967296", "priority:1x", "COUNT", "refresh"};
    for (unsigned int i = 0; i < sizeof(bad_options) / sizeof(*bad_options); ++i) {
        plan[7] = (char *)bad_options[i];
        assert(build_request(8, plan, &request) == -EINVAL);
    }
    plan[7] = "count";
    const char *bad_numbers[] = {"-1", "+1", " 1", "", "1x", "18446744073709551616"};
    for (unsigned int i = 0; i < sizeof(bad_numbers) / sizeof(*bad_numbers); ++i)
        for (unsigned int field = 1; field <= 4; ++field) {
            char *saved = plan[field];
            plan[field] = (char *)bad_numbers[i];
            assert(build_request(7, plan, &request) == -EINVAL);
            plan[field] = saved;
        }
    plan[1] = "0";
    assert(build_request(7, plan, &request) == -EINVAL);
    plan[1] = "18446744073709551614";
    assert(build_request(7, plan, &request) == -EINVAL);
    plan[6] = "oneway";
    assert(build_request(7, plan, &request) == 0);
    plan[1] = "18446744073709551615";
    assert(build_request(7, plan, &request) == -EINVAL);
    plan[1] = "100";
    for (unsigned int field = 2; field <= 4; ++field) {
        char *saved = plan[field];
        plan[field] = "65535";
        assert(build_request(7, plan, &request) == -EINVAL);
        plan[field] = saved;
    }
    plan[4] = "6";
    assert(build_request(7, plan, &request) == -EINVAL);
    plan[4] = "7";
    plan[5] = "unknown";
    assert(build_request(7, plan, &request) == -EINVAL);
    plan[5] = "pair";
    plan[6] = "both";
    assert(build_request(7, plan, &request) == -EINVAL);
    char *apply[] = {"transfer-apply", "100", "5", "6", "7", "uplink-vf", "bidir", "0", "count", "priority:17"};
    assert(build_request(10, apply, &request) == 0 && request.operation == DPPD_MANAGEMENT_TRANSFER_APPLY);
    assert(request.payload.transfer.expected_repository_generation == 0 && request.payload.transfer.with_count);
    assert(request.payload.transfer.scenario == DPPD_TRANSFER_UPLINK_VF && request.payload.transfer.priority == 17);
    assert(build_request(7, apply, &request) == -EINVAL);
    apply[7] = "18446744073709551614";
    assert(build_request(8, apply, &request) == 0);
    assert(request.payload.transfer.expected_repository_generation == UINT64_MAX - 1U);
    const char *bad_versions[] = {"any", "18446744073709551615", "18446744073709551616", "-1", "+1", " 1", ""};
    for (unsigned int i = 0; i < sizeof(bad_versions) / sizeof(*bad_versions); ++i) {
        apply[7] = (char *)bad_versions[i];
        assert(build_request(8, apply, &request) == -EINVAL);
    }
}

/** 共享身份必须完整且精确，读操作可选 any，删除和规则引用不能绕过版本检查 */
static void test_shared_arguments(void)
{
    struct dppd_management_request request;
    char *create[] = {"shared-count-create", "77", "0", "transfer", "extra"};
    assert(build_request(4, create, &request) == 0 && request.operation == DPPD_MANAGEMENT_SHARED_COUNT_CREATE);
    assert(request.payload.shared_count_create.id == 77 && request.payload.shared_count_create.generation == 0 &&
        request.payload.shared_count_create.domain == DPPD_RULE_DOMAIN_TRANSFER);
    assert(build_request(5, create, &request) == -EINVAL);
    char *query[] = {"shared-count-query", "77", "any", "extra"};
    assert(build_request(3, query, &request) == 0 && request.payload.shared_count.expected == UINT64_MAX);
    query[0] = "shared-count-delete";
    assert(build_request(3, query, &request) == -EINVAL);
    query[2] = "8";
    assert(build_request(3, query, &request) == 0 && request.operation == DPPD_MANAGEMENT_SHARED_COUNT_DELETE);
    query[0] = "shared-count-get";
    assert(build_request(3, query, &request) == 0 && request.operation == DPPD_MANAGEMENT_SHARED_COUNT_GET);
    assert(build_request(4, query, &request) == -EINVAL);
    const char *bad[] = {"", "0", "any", "-1", "+1", " 1", "1 ", "1x", "18446744073709551615", "18446744073709551616"};
    for (unsigned int i = 0; i < sizeof(bad) / sizeof(*bad); ++i) {
        create[1] = (char *)bad[i];
        assert(build_request(4, create, &request) == -EINVAL);
        query[2] = (char *)bad[i];
        assert(build_request(3, query, &request) == (strcmp(bad[i], "any") == 0 ? 0 : -EINVAL));
    }
    char *list[] = {"shared-count-list", "0", "0", "extra"};
    assert(build_request(1, list, &request) == 0 && request.payload.shared_count_list.expected_revision == UINT64_MAX);
    assert(build_request(3, list, &request) == 0 && request.payload.shared_count_list.expected_revision == 0);
    assert(build_request(4, list, &request) == -EINVAL);
    char *plan[] = {"transfer-plan", "100", "0", "0", "2", "pair", "bidir", "shared-count:77:8", "age:5", "priority:1"};
    assert(build_request(10, plan, &request) == 0 && !request.payload.transfer.with_count);
    assert(request.payload.transfer.shared_count_id == 77 && request.payload.transfer.shared_count_generation == 8 &&
        request.payload.transfer.age_timeout_seconds == 5);
    const char *invalid[] = {"shared-count:", "shared-count:1", "shared-count:0:8", "shared-count:77:0",
        "shared-count:77:any", "shared-count:77:8:9", "shared-count:+77:8", "shared-count:77:-1",
        "shared-count:18446744073709551615:8", "shared-count:77:18446744073709551615"};
    for (unsigned int i = 0; i < sizeof(invalid) / sizeof(*invalid); ++i) {
        plan[7] = (char *)invalid[i];
        assert(build_request(8, plan, &request) == -EINVAL);
    }
    plan[7] = "shared-count:77:8";
    plan[8] = "count";
    assert(build_request(9, plan, &request) == -EINVAL);
    plan[7] = "count";
    plan[8] = "shared-count:77:8";
    assert(build_request(9, plan, &request) == -EINVAL);
    plan[7] = "shared-count:77:8";
    assert(build_request(9, plan, &request) == -EINVAL);
}

/** AGE 仅扩展 transfer 入口，秒数、组合选项和查询参数都必须完整校验 */
static void test_age_arguments(void)
{
    struct dppd_management_request request;
    char *plan[] = {"transfer-plan", "100", "5", "6", "7", "pair", "bidir", "age:1", "count", "priority:23", "extra"};
    assert(build_request(7, plan, &request) == 0 && request.payload.transfer.age_timeout_seconds == 0);
    assert(build_request(8, plan, &request) == 0 && request.operation == DPPD_MANAGEMENT_TRANSFER_PLAN);
    assert(request.payload.transfer.age_timeout_seconds == 1 && !request.payload.transfer.with_count);
    assert(build_request(10, plan, &request) == 0);
    assert(request.payload.transfer.age_timeout_seconds == 1 && request.payload.transfer.with_count &&
        request.payload.transfer.priority == 23 && request.payload.transfer.bidirectional);
    plan[7] = "count";
    plan[8] = "priority:23";
    plan[9] = "age:16777215";
    assert(build_request(10, plan, &request) == 0);
    assert(request.payload.transfer.age_timeout_seconds == 0xffffffU && request.payload.transfer.with_count);
    assert(build_request(11, plan, &request) == -EINVAL);

    const char *invalid[] = {"age:", "age:0", "age:16777216", "age:4294967296", "age:18446744073709551616", "age:-1",
        "age:+1", "age: 1", "age:1 ", "age:1x", "age:0x10", "age:1.5", "AGE:1", "age:any"};
    for (unsigned int i = 0; i < sizeof(invalid) / sizeof(*invalid); ++i) {
        plan[7] = (char *)invalid[i];
        assert(build_request(8, plan, &request) == -EINVAL);
    }
    plan[7] = "age:1";
    plan[8] = "age:2";
    assert(build_request(9, plan, &request) == -EINVAL);
    plan[8] = "count";
    plan[9] = "age:2";
    assert(build_request(10, plan, &request) == -EINVAL);
    plan[9] = "count";
    assert(build_request(10, plan, &request) == -EINVAL);

    char *apply[] = {"transfer-apply", "100", "5", "6", "7", "uplink-vf", "bidir", "7", "age:16777215", "count",
        "priority:19", "extra"};
    assert(build_request(11, apply, &request) == 0 && request.operation == DPPD_MANAGEMENT_TRANSFER_APPLY);
    assert(request.payload.transfer.age_timeout_seconds == 0xffffffU && request.payload.transfer.with_count);
    assert(request.payload.transfer.expected_repository_generation == 7 && request.payload.transfer.priority == 19);
    assert(build_request(12, apply, &request) == -EINVAL);
    for (unsigned int i = 0; i < sizeof(invalid) / sizeof(*invalid); ++i) {
        apply[8] = (char *)invalid[i];
        assert(build_request(9, apply, &request) == -EINVAL);
    }
    apply[8] = "age:1";
    apply[9] = "age:2";
    assert(build_request(10, apply, &request) == -EINVAL);
    apply[8] = "count";
    apply[9] = "priority:19";
    apply[10] = "age:1";
    assert(build_request(11, apply, &request) == 0 && request.payload.transfer.age_timeout_seconds == 1);

    char *status[] = {"age-status", "extra"};
    memset(&request, 0xa5, sizeof(request));
    assert(build_request(1, status, &request) == 0 && request.operation == DPPD_MANAGEMENT_AGE_STATUS);
    assert(request.version == DPPD_MANAGEMENT_VERSION && request.size == sizeof(request));
    const uint8_t *payload = (const uint8_t *)&request.payload;
    for (unsigned int i = 0; i < sizeof(request.payload); ++i)
        assert(payload[i] == 0);
    assert(build_request(2, status, &request) == -EINVAL);

    char *query[] = {"rule-age", "100", "any", "extra"};
    assert(build_request(3, query, &request) == 0 && request.operation == DPPD_MANAGEMENT_RULE_AGE_QUERY);
    assert(request.payload.count_query.rule_id == 100 &&
        request.payload.count_query.expected_generation == DPPD_RULE_GENERATION_ANY);
    query[2] = "7";
    assert(build_request(3, query, &request) == 0 && request.payload.count_query.expected_generation == 7);
    query[2] = "18446744073709551614";
    assert(
        build_request(3, query, &request) == 0 && request.payload.count_query.expected_generation == UINT64_MAX - 1U);
    assert(build_request(1, query, &request) == -EINVAL);
    assert(build_request(2, query, &request) == -EINVAL);
    assert(build_request(4, query, &request) == -EINVAL);
    const char *bad_generation[] = {"-1", "", "wrong", "18446744073709551615", "18446744073709551616"};
    for (unsigned int i = 0; i < sizeof(bad_generation) / sizeof(*bad_generation); ++i) {
        query[2] = (char *)bad_generation[i];
        assert(build_request(3, query, &request) == -EINVAL);
    }
    const char *bad_id[] = {"0", "-1", "", "wrong", "18446744073709551616"};
    query[2] = "any";
    for (unsigned int i = 0; i < sizeof(bad_id) / sizeof(*bad_id); ++i) {
        query[1] = (char *)bad_id[i];
        assert(build_request(3, query, &request) == -EINVAL);
    }
}

/** 捕获两个输出流，既检查明确报错，也防止坏响应先打印一半成功图再停止 */
static void check_transfer_print(const struct dppd_management_response *response, bool recognized, bool valid)
{
    FILE *output = tmpfile(), *errors = tmpfile();
    char text[2048], error[256];
    assert(output != NULL && errors != NULL);
    assert(fflush(stdout) == 0 && fflush(stderr) == 0);
    int saved_output = dup(STDOUT_FILENO), saved_errors = dup(STDERR_FILENO);
    assert(saved_output >= 0 && saved_errors >= 0);
    assert(dup2(fileno(output), STDOUT_FILENO) >= 0 && dup2(fileno(errors), STDERR_FILENO) >= 0);
    int handled = dppctl_transfer_print(response);
    assert(fflush(stdout) == 0 && fflush(stderr) == 0);
    assert(dup2(saved_output, STDOUT_FILENO) >= 0 && dup2(saved_errors, STDERR_FILENO) >= 0);
    assert(close(saved_output) == 0 && close(saved_errors) == 0);
    assert(fseek(output, 0, SEEK_SET) == 0 && fseek(errors, 0, SEEK_SET) == 0);
    size_t text_size = fread(text, 1, sizeof(text) - 1U, output);
    size_t error_size = fread(error, 1, sizeof(error) - 1U, errors);
    assert(!ferror(output) && !ferror(errors));
    text[text_size] = error[error_size] = '\0';
    assert(fclose(output) == 0 && fclose(errors) == 0);
    assert(handled == (recognized ? valid ? 1 : -EPROTO : 0));
    if (!recognized)
        assert(text_size == 0 && error_size == 0);
    else if (valid) {
        assert(strstr(text, "transfer=") != NULL && strstr(text, "edge=0 id=100") != NULL);
        assert(strstr(text, "source=5 destination=6") != NULL && error_size == 0);
    } else
        assert(text_size == 0 && strstr(error, "malformed transfer response") != NULL);
}

/** 数组长度和动作类型来自服务端响应，必须在使用末动作下标之前完成完整检查 */
static void test_transfer_print(void)
{
    struct dppd_management_response response = {.operation = DPPD_MANAGEMENT_TRANSFER_PLAN};
    struct dppd_transfer_graph *graph = &response.payload.transfer.graph;
    graph->count = 1;
    graph->switch_domain_id = 42;
    graph->rules[0] = (struct dppd_rule){.id = 100,
        .nb_matches = 2,
        .nb_actions = 1,
        .matches = {{.type = DPPD_MATCH_REPRESENTED_PORT, .spec.ethdev_port_id = 5}, {.type = DPPD_MATCH_ETH}},
        .actions = {{.type = DPPD_ACTION_REPRESENTED_PORT, .conf.ethdev_port_id = 6}}};
    struct dppd_management_response valid = response;
    check_transfer_print(&response, true, true);
    response.operation = DPPD_MANAGEMENT_TRANSFER_APPLY;
    graph->count = 2;
    graph->rules[1] = graph->rules[0];
    graph->rules[1].id = 101;
    graph->rules[1].nb_actions = 2;
    graph->rules[1].actions[1] = graph->rules[1].actions[0];
    graph->rules[1].actions[0].type = DPPD_ACTION_COUNT;
    memset(response.payload.transfer.message, 'x', sizeof(response.payload.transfer.message));
    check_transfer_print(&response, true, true);
    const uint16_t invalid_counts[] = {0, DPPD_TRANSFER_MAX_EDGES + 1U, UINT16_MAX};
    for (unsigned int i = 0; i < sizeof(invalid_counts) / sizeof(*invalid_counts); ++i) {
        response = valid;
        graph->count = invalid_counts[i];
        check_transfer_print(&response, true, false);
    }
    const uint16_t invalid_actions[] = {0, 3, DPPD_RULE_MAX_ACTIONS, DPPD_RULE_MAX_ACTIONS + 1U, UINT16_MAX};
    for (unsigned int i = 0; i < sizeof(invalid_actions) / sizeof(*invalid_actions); ++i) {
        response = valid;
        graph->rules[0].nb_actions = invalid_actions[i];
        check_transfer_print(&response, true, false);
    }
    const uint16_t invalid_matches[] = {0, 1, 3, DPPD_RULE_MAX_ITEMS, DPPD_RULE_MAX_ITEMS + 1U, UINT16_MAX};
    for (unsigned int i = 0; i < sizeof(invalid_matches) / sizeof(*invalid_matches); ++i) {
        response = valid;
        graph->rules[0].nb_matches = invalid_matches[i];
        check_transfer_print(&response, true, false);
    }
    response = valid;
    graph->rules[0].matches[0].type = DPPD_MATCH_ETH;
    check_transfer_print(&response, true, false);
    response = valid;
    graph->rules[0].matches[1].type = DPPD_MATCH_IPV4;
    check_transfer_print(&response, true, false);
    response = valid;
    graph->rules[0].actions[0].type = DPPD_ACTION_DROP;
    check_transfer_print(&response, true, false);
    response = valid;
    graph->rules[0].nb_actions = 2;
    graph->rules[0].actions[1] = graph->rules[0].actions[0];
    graph->rules[0].actions[0].type = DPPD_ACTION_MARK;
    check_transfer_print(&response, true, false);
    response = valid;
    graph->count = 2;
    graph->rules[1] = graph->rules[0];
    graph->rules[1].nb_actions = UINT16_MAX;
    check_transfer_print(&response, true, false);
    response.operation = DPPD_MANAGEMENT_PING;
    check_transfer_print(&response, false, false);
}

/**
 * 前半部分确认两到四条规则都能正确映射到请求字段
 * 后半部分逐项替换参数，确认不完整分组、无效旧版本、重复 ID 和越界数值都被拒绝
 */
int main(void)
{
    /** 指标命令只接受命令名本身，不能默默忽略用户追加的规则 ID */
    struct dppd_management_request metrics_request;
    char *metrics_args[] = {"rule-metrics", "123"};

    assert(build_request(1, metrics_args, &metrics_request) == 0);
    assert(metrics_request.operation == DPPD_MANAGEMENT_RULE_METRICS);
    assert(metrics_request.version == DPPD_MANAGEMENT_VERSION);
    assert(build_request(2, metrics_args, &metrics_request) == -EINVAL);
    /** 历史查询也是独立只读操作，必须拒绝没有定义的额外参数 */
    metrics_args[0] = "rule-latency";
    assert(build_request(1, metrics_args, &metrics_request) == 0);
    assert(metrics_request.operation == DPPD_MANAGEMENT_RULE_LATENCY);
    assert(build_request(2, metrics_args, &metrics_request) == -EINVAL);
    struct dppd_management_request request;
    char *probe[] = {"health", "unexpected"};
    char *stats[] = {"stats", "all", "2"};

    test_status_arguments();
    test_acl_arguments();
    test_routing_arguments();
    test_ipv6_arguments();
    test_probe_arguments();
    test_history_arguments();
    test_transfer_arguments();
    test_age_arguments();
    test_shared_arguments();
    test_transfer_print();

    /** 健康与就绪命令都使用无参数的状态查询，额外参数必须明确拒绝 */
    assert(build_request(1, probe, &request) == 0);
    assert(request.operation == DPPD_MANAGEMENT_HEALTH_GET);
    assert(build_request(2, probe, &request) == -EINVAL);
    probe[0] = "ready";
    assert(build_request(1, probe, &request) == 0);
    assert(request.operation == DPPD_MANAGEMENT_HEALTH_GET);
    assert(build_request(2, probe, &request) == -EINVAL);
    assert(build_request(1, stats, &request) == 0);
    assert(request.payload.stats_query.port_id == DPPD_STATS_ALL);
    assert(request.payload.stats_query.queue_id == DPPD_STATS_ALL);
    assert(build_request(3, stats, &request) == 0);
    assert(request.payload.stats_query.queue_id == 2);
    stats[1] = "5";
    assert(build_request(2, stats, &request) == 0);
    assert(request.payload.stats_query.port_id == 5);
    assert(request.payload.stats_query.queue_id == DPPD_STATS_ALL);
    stats[1] = "65535";
    assert(build_request(2, stats, &request) == -EINVAL);
    stats[1] = "-1";
    assert(build_request(2, stats, &request) == -EINVAL);
    stats[1] = "wrong";
    assert(build_request(2, stats, &request) == -EINVAL);
    char *args[] = {"update-drop-batch", "5", "20", "prefer", "100", "1", "101", "2", "102", "3", "103", "4"};

    /** 四个固定参数之后，每增加一组 ID 和旧版本，就多表达一条待更新规则 */
    for (int count = 8; count <= 12; count += 2) {
        assert(build_request(count, args, &request) == 0);
        assert(request.version == DPPD_MANAGEMENT_VERSION);
        assert(request.operation == DPPD_MANAGEMENT_RULE_UPDATE_BATCH);
        assert(request.payload.update_batch.count == (count - 4) / 2);
        assert(request.payload.update_batch.rules[1].rule.id == 101);
        assert(request.payload.update_batch.rules[1].expected_generation == 2);
        assert(request.payload.update_batch.rules[1].install_port_id == 5);
        assert(request.payload.update_batch.rules[1].rule.priority == 20);
    }
    /** 六个参数只够一条规则，九个参数会留下半组 ID/版本，两种输入都不允许 */
    assert(build_request(6, args, &request) == -EINVAL);
    assert(build_request(9, args, &request) == -EINVAL);
    /** 精确旧版本既不能是 any，也不能是零或被保留为 ANY 的最大无符号整数 */
    args[5] = "any";
    assert(build_request(8, args, &request) == -EINVAL);
    args[5] = "0";
    assert(build_request(8, args, &request) == -EINVAL);
    args[5] = "18446744073709551615";
    assert(build_request(8, args, &request) == -EINVAL);
    /** 每次只修改一个条件，避免前一个非法参数掩盖当前要验证的重复 ID 或数值边界 */
    args[5] = "1";
    args[6] = "100";
    assert(build_request(8, args, &request) == -EINVAL);
    args[6] = "101";
    args[1] = "65536";
    assert(build_request(8, args, &request) == -EINVAL);
    args[1] = "5";
    args[2] = "4294967296";
    assert(build_request(8, args, &request) == -EINVAL);
    args[2] = "20";
    /** 合法策略要映射到正确枚举，拼错的策略不能被悄悄当作默认 prefer */
    args[3] = "software";
    assert(build_request(8, args, &request) == 0);
    assert(request.payload.update_batch.rules[0].rule.fallback == DPPD_FALLBACK_SOFTWARE_ONLY);
    args[3] = "require";
    assert(build_request(8, args, &request) == 0);
    assert(request.payload.update_batch.rules[0].rule.fallback == DPPD_FALLBACK_REQUIRE_HARDWARE);
    args[3] = "invalid";
    assert(build_request(8, args, &request) == -EINVAL);
    char *nd[] = {"nd6-list", "8", "9"};
    assert(build_request(3, nd, &request) == 0 && request.operation == DPPD_MANAGEMENT_ND6_LIST);
    assert(request.payload.conntrack.after_id == 8 && request.payload.conntrack.expected_revision == 9);
    nd[0] = "nd6-flush";
    assert(build_request(2, nd, &request) == 0 && request.operation == DPPD_MANAGEMENT_ND6_FLUSH);
    nd[1] = "0";
    assert(build_request(2, nd, &request) == -EINVAL);
    /** 设备复位使用精确事件代次，零是有效初始代次，any 和溢出都不能授权操作 */
    char *device[] = {"device-status", "0"};
    assert(build_request(1, device, &request) == 0 && request.operation == DPPD_MANAGEMENT_DEVICE_STATUS);
    assert(build_request(2, device, &request) == -EINVAL);
    device[0] = "device-reset";
    assert(build_request(1, device, &request) == -EINVAL);
    assert(build_request(2, device, &request) == 0 && request.operation == DPPD_MANAGEMENT_DEVICE_RESET &&
        request.payload.device_reset.expected_sequence == 0);
    device[1] = "17";
    assert(build_request(2, device, &request) == 0 && request.payload.device_reset.expected_sequence == 17);
    device[1] = "any";
    assert(build_request(2, device, &request) == -EINVAL);
    device[1] = "18446744073709551615";
    assert(build_request(2, device, &request) == -EINVAL);
    device[1] = "-1";
    assert(build_request(2, device, &request) == -EINVAL);
    /** 新管理命令必须显式区分新建与精确版本更新，不能借用 any 绕过冲突检查 */
    char *flow[] = {"flow-create", "1", "0", "apply-drop", "123", "7", "0"};
    assert(build_request(7, flow, &request) == 0 && request.operation == DPPD_MANAGEMENT_FLOW_CREATE);
    assert(request.payload.flow_change.rules[0].table_key == 1);
    assert(request.payload.flow_change.rules[0].rule.install_port_id == 7);
    flow[0] = "flow-update";
    assert(build_request(7, flow, &request) == -EINVAL);
    flow[6] = "9";
    assert(build_request(7, flow, &request) == 0 && request.payload.flow_change.expected[0] == 9);
    flow[6] = "any";
    assert(build_request(7, flow, &request) == -EINVAL);
    /** 配置文件整批解析，坏行、嵌入零字节或超出条数都不能发送已解析的前缀 */
    char configuration_path[] = "/tmp/dppctl-configuration-XXXXXX";
    int configuration_fd = mkstemp(configuration_path);
    assert(configuration_fd >= 0);
    const char configuration[] = "count-add 800 7 ingress\n"
                                 "template-add 2 64 1 apply-drop 1 7 0 shared:800:0\n";
    assert(write(configuration_fd, configuration, sizeof(configuration) - 1U) == sizeof(configuration) - 1U);
    char *configure[] = {"flow-config-apply", "0", configuration_path};
    assert(build_request(3, configure, &request) == 0 && request.payload.flow_config_apply.count == 2);
    assert(request.payload.flow_config_apply.changes[1].spec.table.example.actions[0].type == DPPD_ACTION_SHARED_COUNT);
    assert(write(configuration_fd, "\0", 1) == 1);
    assert(build_request(3, configure, &request) == -EINVAL);
    assert(ftruncate(configuration_fd, 0) == 0 && lseek(configuration_fd, 0, SEEK_SET) == 0);
    for (unsigned int i = 0; i < 9; ++i)
        assert(write(configuration_fd, "template-delete 2\n", 18) == 18);
    assert(build_request(3, configure, &request) == -E2BIG);
    close(configuration_fd);
    assert(unlink(configuration_path) == 0);
    return 0;
}
