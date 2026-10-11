#ifndef DPPD_MONITOR_CHANNEL_H
#define DPPD_MONITOR_CHANNEL_H

#include "dppd/recovery_monitor.h"

#define DPPD_MONITOR_CLIENT_LIMIT 8U
#define DPPD_MONITOR_REPLY_MS 1000U

/** 所有描述符先设为 -1，少量慢客户端不能阻塞接口事件读取 */
struct dppd_monitor_server {
    int fd;
    int clients[DPPD_MONITOR_CLIENT_LIMIT];
    uint64_t expires[DPPD_MONITOR_CLIENT_LIMIT];
};

/** 主线程持有会话文件，任何一次握手失败都会锁存，不能靠后续成功自动恢复 */
struct dppd_monitor_client {
    int fd, error;
    char path[DPPD_STATE_PATH_CAPACITY];
    struct dppd_monitor_record record;
};

int dppd_monitor_server_open(struct dppd_monitor_server *server, const struct dppd_monitor_record *record);
/** 回复前调用 check 排空已到达事件，校验失败时不向客户端发送成功 */
int dppd_monitor_server_poll(struct dppd_monitor_server *server, const struct dppd_monitor_record *record,
    int (*check)(void *context), void *context);
void dppd_monitor_server_close(struct dppd_monitor_server *server);
/** 核对完整端口集合、恢复文件起点和新鲜回复后，才把会话编号写入恢复记录 */
int dppd_monitor_client_open(struct dppd_monitor_client *client, const char *path, struct dppd_recovery_guard *guard,
    const struct dppd_recovery_record *ports);
int dppd_monitor_client_check(struct dppd_monitor_client *client);
int dppd_monitor_client_identity(struct dppd_monitor_client *client, uint16_t port,
    const struct dppd_recovery_identity *identity);
void dppd_monitor_client_close(struct dppd_monitor_client *client);

#endif
