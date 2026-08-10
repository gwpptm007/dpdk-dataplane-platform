#include <assert.h>
#include <string.h>
#include "dppd/config.h"

int main(void)
{
    struct dppd_config config;
    char error[128];
    char *defaults[] = {"dppd"};
    char *with_state[] = {
        "dppd", "--state-path", "/var/lib/dppd/rules.snapshot",
        "--rule-capacity", "32",
    };
    char *empty_state[] = {"dppd", "--state-path", ""};

    /* 默认必须保持显式禁用，升级现有部署时不会突然读取或创建磁盘状态。 */
    assert(dppd_config_parse(1, defaults, &config) == DPPD_CONFIG_OK);
    assert(config.state_path[0] == '\0');
    assert(dppd_config_validate(&config, error, sizeof(error)) == 0);

    assert(dppd_config_parse(5, with_state, &config) == DPPD_CONFIG_OK);
    assert(strcmp(config.state_path,
                  "/var/lib/dppd/rules.snapshot") == 0);
    assert(config.rule_capacity == 32);
    assert(dppd_config_validate(&config, error, sizeof(error)) == 0);

    /* 空路径既不能表示可靠启用，也不能和默认禁用区分，命令行边界直接拒绝。 */
    assert(dppd_config_parse(3, empty_state, &config) == DPPD_CONFIG_ERROR);
    return 0;
}
