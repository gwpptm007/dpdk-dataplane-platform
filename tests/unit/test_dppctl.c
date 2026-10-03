/**
 * 直接包含 CLI 源码，测试用户实际使用的命令解析逻辑，避免另写一份解析器造成验证失真
 * 临时改名原来的 main，让本文件可以定义自己的测试入口
 * 测试只构造请求结构，不连接管理 socket，也不会真正安装或修改规则
 */
#define main dppctl_program_main
#include "../../app/dppctl/main.c"
#undef main
#include <assert.h>

/** 状态查询默认读当前版本，也可携带精确非零版本，非法参数要在连接服务前被拒绝 */
static void test_status_arguments(void)
{
    struct dppd_management_request request;
    char *args[] = {"rule-status", "100", "any", "extra"};
    const char *invalid[] = {"0", "-1", "wrong", "18446744073709551615",
                             "18446744073709551616"};

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

/** 探测必须生成独立的诊断请求，不能因复用规则构造器而变成真正的安装命令 */
static void test_probe_arguments(void)
{
    struct dppd_management_request request;
    char *profile[] = {"capability-show", "5", "extra"};
    char *drop[] = {"probe-drop", "100", "5", "17", "refresh", "extra"};
    char *filter[] = {"probe-filter", "101", "6", "tcp", "192.0.2.0/24",
                      "any", "any", "80", "queue:1", "count", "mark:7",
                      "priority:19", "refresh"};

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
    test_probe_arguments();

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
    char *args[] = {"update-drop-batch", "5", "20", "prefer",
                    "100", "1", "101", "2", "102", "3", "103", "4"};

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
    return 0;
}
