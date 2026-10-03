/**
 * 直接包含 CLI 源码，测试用户实际使用的命令解析逻辑，避免另写一份解析器造成验证失真
 * 临时改名原来的 main，让本文件可以定义自己的测试入口
 * 测试只构造请求结构，不连接管理 socket，也不会真正安装或修改规则
 */
#define main dppctl_program_main
#include "../../app/dppctl/main.c"
#undef main
#include <assert.h>

/**
 * 前半部分确认两到四条规则都能正确映射到请求字段
 * 后半部分逐项替换参数，确认不完整分组、无效旧版本、重复 ID 和越界数值都被拒绝
 */
int main(void)
{
    struct dppd_management_request request;
    char *probe[] = {"health", "unexpected"};
    char *stats[] = {"stats", "all", "2"};

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
