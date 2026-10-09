#ifndef DPPD_CONFIG_H
#define DPPD_CONFIG_H

#include <stdbool.h>
#include <stdint.h>

#define DPPD_MAX_PORTS 16U
#define DPPD_MAX_WORKERS 64U
#define DPPD_MAX_BURST 256U
#define DPPD_CONTROL_SOCKET_PATH_CAPACITY 108U
#define DPPD_STATE_PATH_CAPACITY 4096U
#define DPPD_DEFAULT_RULE_CAPACITY 1024U

struct dppd_config {
    /* ports 按相邻两项组成双向转发对，例如 0,1,2,3 表示 0↔1、2↔3。 */
    uint16_t ports[DPPD_MAX_PORTS];
    uint16_t nb_ports;
    uint16_t nb_queues;
    uint16_t burst_size;
    uint32_t mbufs_per_socket;
    uint16_t mbuf_cache;
    uint32_t stats_period_ms;
    uint32_t duration_s;
    /* desired rule repository 的逻辑容量，不等价于 NIC rte_flow 硬件容量。 */
    uint32_t rule_capacity;
    /* Unix 管理 socket 的文件系统路径，长度包含末尾 '\0'。 */
    char control_socket[DPPD_CONTROL_SOCKET_PATH_CAPACITY];
    /* 为空表示禁用持久化；非空时启动必须成功恢复或创建该 snapshot。 */
    char state_path[DPPD_STATE_PATH_CAPACITY];
    /** 显式启用跨进程安装保护，须与 state_path 配对，默认不创建额外文件 */
    char recovery_path[DPPD_STATE_PATH_CAPACITY];
    bool promiscuous;
    bool enable_pdump;
};

enum dppd_config_result {
    DPPD_CONFIG_OK = 0,
    DPPD_CONFIG_HELP = 1,
    DPPD_CONFIG_ERROR = -1,
};

void dppd_config_defaults(struct dppd_config *cfg);
int dppd_config_parse(int argc, char **argv, struct dppd_config *cfg);
int dppd_config_validate(const struct dppd_config *cfg, char *error, uint32_t error_len);
void dppd_config_print_usage(const char *program);
void dppd_config_dump(const struct dppd_config *cfg);

#endif
