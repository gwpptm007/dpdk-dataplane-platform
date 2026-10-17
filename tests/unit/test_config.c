#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "dppd/config.h"

/** 配置的两端端口必须等长，重叠与回包歧义直接拒绝，不能依赖参数顺序猜优先级 */
static void test_dnat_config(void)
{
    struct dppd_config config;
    char error[160];
    char *arguments[] = {"dppd", "--conntrack", "8", "--ipv4-routing", "/tmp/routes", "--dnat",
        "1,0,tcp,203.0.113.9,443,10.0.0.9,8443"};
    assert(dppd_config_parse(7, arguments, &config) == 0 && config.dnat_count == 1);
    assert(dppd_config_validate(&config, error, sizeof(error)) == 0);
    assert(config.dnat[0].public_first == 443 && config.dnat[0].private_first == 8443);
    config.conntrack_capacity = 0;
    assert(dppd_config_validate(&config, error, sizeof(error)) != 0);
    arguments[6] = "1,0,udp,203.0.113.9,1000-1002,10.0.0.9,2000-2002";
    assert(dppd_config_parse(7, arguments, &config) == 0 && config.dnat[0].public_last == 1002);
    config.dnat[0].inside_port = 2;
    assert(dppd_config_validate(&config, error, sizeof(error)) != 0);
    arguments[6] = "1,0,udp,203.0.113.9,any,10.0.0.9,same";
    assert(dppd_config_parse(7, arguments, &config) == 0 && config.dnat[0].public_first == 0 &&
        config.dnat[0].public_last == UINT16_MAX && config.dnat[0].private_first == 0);
    assert(dppd_config_validate(&config, error, sizeof(error)) == 0);
    config.dnat[1] = config.dnat[0];
    config.dnat_count = 2;
    assert(dppd_config_validate(&config, error, sizeof(error)) != 0);
    config.dnat[1].public_address_be++;
    assert(dppd_config_validate(&config, error, sizeof(error)) != 0);
    config.dnat[1].private_address_be++;
    assert(dppd_config_validate(&config, error, sizeof(error)) == 0);
    const char *invalid[] = {"1,0,icmp,203.0.113.9,443,10.0.0.9,8443", "1,0,tcp,203.0.113.9,443,10.0.0.9",
        "1,0,tcp,203.0.113.9,,10.0.0.9,8443", "1,0,tcp,203.0.113.9,443,10.0.0.9,8443,extra",
        "1,0,tcp,224.0.0.1,443,10.0.0.9,8443", "1,0,tcp,203.0.113.9,any,10.0.0.9,8443",
        "1,0,tcp,203.0.113.9,443-444,10.0.0.9,8443", "1,0,tcp,203.0.113.9,0,10.0.0.9,8443",
        "1,0,tcp,203.0.113.9,443,10.0.0.9,0", "1,0,tcp,203.0.113.9,444-443,10.0.0.9,same",
        "1,0,tcp,203.0.113.9,443,203.0.113.9,same", "1,1,tcp,203.0.113.9,443,10.0.0.9,8443",
        "-1,0,tcp,203.0.113.9,443,10.0.0.9,8443"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(*invalid); ++i) {
        arguments[6] = (char *)invalid[i];
        assert(dppd_config_parse(7, arguments, &config) != 0);
    }
    /** 最多保存三十二条，不因重复参数而默默覆盖前一条或写出数组边界 */
    char text[DPPD_MAX_DNAT_RULES + 1][128];
    char *many[5 + 2 * (DPPD_MAX_DNAT_RULES + 1)] = {"dppd", "--conntrack", "8", "--ipv4-routing", "/tmp/routes"};
    for (uint32_t i = 0; i <= DPPD_MAX_DNAT_RULES; ++i) {
        snprintf(text[i], sizeof(text[i]), "1,0,udp,203.0.113.9,%u,10.0.0.9,%u", 1000 + i, 2000 + i);
        many[5 + 2 * i] = "--dnat";
        many[6 + 2 * i] = text[i];
    }
    assert(dppd_config_parse(5 + 2 * DPPD_MAX_DNAT_RULES, many, &config) == 0);
    assert(dppd_config_validate(&config, error, sizeof(error)) == 0);
    assert(dppd_config_parse(7 + 2 * DPPD_MAX_DNAT_RULES, many, &config) != 0);
}

int main(void)
{
    test_dnat_config();
    struct dppd_config config;
    char error[128];
    char *defaults[] = {"dppd"};
    char *with_state[] = {
        "dppd",
        "--state-path",
        "/var/lib/dppd/rules.snapshot",
        "--rule-capacity",
        "32",
    };
    char *empty_state[] = {"dppd", "--state-path", ""};
    char *with_recovery[] = {"dppd", "--state-path", "/tmp/state", "--recovery-path", "/tmp/recovery"};
    char *no_state[] = {"dppd", "--recovery-path", "/tmp/recovery"};
    char *owner_only[] = {"dppd", "--tap-owner-cookie"};
    char *monitor_only[] = {"dppd", "--recovery-monitor", "/tmp/session"};
    char *with_monitor[] = {"dppd", "--state-path", "/tmp/state", "--recovery-path", "/tmp/recovery",
        "--recovery-monitor", "/tmp/session"};
    char *with_owner[] = {"dppd", "--tap-owner-cookie", "--state-path", "/tmp/state", "--recovery-path",
        "/tmp/recovery"};

    /* 默认必须保持显式禁用，升级现有部署时不会突然读取或创建磁盘状态 */
    assert(dppd_config_parse(1, defaults, &config) == DPPD_CONFIG_OK);
    assert(config.state_path[0] == '\0');
    assert(config.recovery_path[0] == '\0');
    assert(!config.tap_owner_cookie);
    assert(config.recovery_monitor[0] == '\0');
    assert(config.routing_path[0] == '\0');
    assert(dppd_config_validate(&config, error, sizeof(error)) == 0);

    assert(dppd_config_parse(5, with_state, &config) == DPPD_CONFIG_OK);
    assert(strcmp(config.state_path, "/var/lib/dppd/rules.snapshot") == 0);
    assert(config.rule_capacity == 32);
    assert(dppd_config_validate(&config, error, sizeof(error)) == 0);

    /* 空路径既不能表示可靠启用，也不能和默认禁用区分，命令行边界直接拒绝 */
    assert(dppd_config_parse(3, empty_state, &config) == DPPD_CONFIG_ERROR);
    /** 恢复保护显式启用，必须绑定独立快照路径，不能覆盖快照或缺失恢复来源 */
    assert(dppd_config_parse(5, with_recovery, &config) == DPPD_CONFIG_OK);
    assert(strcmp(config.recovery_path, "/tmp/recovery") == 0);
    assert(dppd_config_validate(&config, error, sizeof(error)) == 0);
    assert(dppd_config_parse(3, no_state, &config) == DPPD_CONFIG_ERROR);
    with_recovery[4] = "/tmp/state";
    assert(dppd_config_parse(5, with_recovery, &config) == DPPD_CONFIG_ERROR);
    assert(dppd_config_parse(3, monitor_only, &config) == DPPD_CONFIG_ERROR);
    assert(dppd_config_parse(7, with_monitor, &config) == DPPD_CONFIG_OK);
    assert(strcmp(config.recovery_monitor, "/tmp/session") == 0);
    assert(dppd_config_validate(&config, error, sizeof(error)) == 0);
    with_monitor[6] = "/tmp/recovery";
    assert(dppd_config_parse(7, with_monitor, &config) == DPPD_CONFIG_ERROR);
    with_monitor[6] = "";
    assert(dppd_config_parse(7, with_monitor, &config) == DPPD_CONFIG_ERROR);
    dppd_config_defaults(&config);
    strcpy(config.recovery_monitor, "/tmp/session");
    assert(dppd_config_validate(&config, error, sizeof(error)) != 0);
    /** 原生标识必须有落盘目标，直接构造配置也不能绕过这个约束 */
    assert(dppd_config_parse(2, owner_only, &config) == DPPD_CONFIG_ERROR);
    dppd_config_defaults(&config);
    config.tap_owner_cookie = true;
    assert(dppd_config_validate(&config, error, sizeof(error)) != 0);
    assert(dppd_config_parse(6, with_owner, &config) == DPPD_CONFIG_OK);
    assert(config.tap_owner_cookie && dppd_config_validate(&config, error, sizeof(error)) == 0);
    with_recovery[4] = "";
    assert(dppd_config_parse(5, with_recovery, &config) == DPPD_CONFIG_ERROR);
    char *routing[] = {"dppd", "--ipv4-routing", "/tmp/routes", "--ports", "0,2,7"};
    assert(dppd_config_parse(5, routing, &config) == DPPD_CONFIG_OK);
    assert(dppd_config_validate(&config, error, sizeof(error)) == 0 && config.nb_ports == 3);
    config.routing_path[0] = '\0';
    assert(dppd_config_validate(&config, error, sizeof(error)) != 0);
    routing[2] = "";
    assert(dppd_config_parse(5, routing, &config) == DPPD_CONFIG_ERROR);
    /** 只有显式三层模式允许跟踪，NAT 还必须引用两个已经配置的不同端口 */
    char *tracking[] = {"dppd", "--conntrack", "128", "--ct-timeout", "2", "--ipv4-routing", "/tmp/routes", "--snat",
        "0,1,203.0.113.1,40000-40001"};
    assert(dppd_config_parse(9, tracking, &config) == DPPD_CONFIG_OK);
    assert(dppd_config_validate(&config, error, sizeof(error)) == 0);
    assert(config.conntrack_capacity == 128 && config.conntrack_timeout_s == 2 && config.snat.first_port == 40000);
    config.routing_path[0] = '\0';
    assert(dppd_config_validate(&config, error, sizeof(error)) != 0);
    tracking[8] = "0,2,203.0.113.1,40000-40001";
    assert(dppd_config_parse(9, tracking, &config) == DPPD_CONFIG_OK);
    assert(dppd_config_validate(&config, error, sizeof(error)) != 0);
    tracking[8] = "0,1,203.0.113.1,40001-40000";
    assert(dppd_config_parse(9, tracking, &config) == DPPD_CONFIG_ERROR);
    tracking[8] = "0,1,224.0.0.1,40000-40001";
    assert(dppd_config_parse(9, tracking, &config) == DPPD_CONFIG_ERROR);
    tracking[8] = "0,0,203.0.113.1,40000-40001";
    assert(dppd_config_parse(9, tracking, &config) == DPPD_CONFIG_ERROR);
    return 0;
}
