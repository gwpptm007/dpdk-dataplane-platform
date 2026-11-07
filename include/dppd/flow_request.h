#ifndef DPPD_FLOW_REQUEST_H
#define DPPD_FLOW_REQUEST_H

#include "dppd/flow_state.h"

enum dppd_flow_request_operation {
    DPPD_FLOW_REQUEST_CREATE = 1,
    DPPD_FLOW_REQUEST_UPDATE,
    DPPD_FLOW_REQUEST_REMOVE,
    DPPD_FLOW_REQUEST_CONFIGURE,
    DPPD_FLOW_REQUEST_ROUTING_REPLACE,
    DPPD_FLOW_REQUEST_CONNTRACK_REPLACE,
    DPPD_FLOW_REQUEST_ND6_REPLACE
};

enum dppd_flow_request_phase { DPPD_FLOW_REQUEST_PENDING = 1, DPPD_FLOW_REQUEST_DONE };

/** 客户端标识由调用者长期保存，每次新请求递增序号，重试必须使用原键和原内容 */
struct dppd_flow_request_key {
    uint64_t client, sequence;
};

struct dppd_flow_request {
    struct dppd_flow_request_key key;
    enum dppd_flow_request_operation operation;
    uint32_t queue, count;
    uint64_t expected_revision;
    union {
        struct {
            struct dppd_flow_state_rule rules[4];
            uint64_t expected[4];
        } change;
        struct dppd_flow_remove_request remove[4];
        struct dppd_flow_config_change configuration[8];
        /** 一次请求替换完整双栈表，expected_revision 使用整个 managed 配置的版本 */
        struct dppd_ipv4_configuration routing;
        /** 清空连接必须由调用方明确选择，旧启动或旧会话快照都不能触发清空 */
        struct {
            struct dppd_conntrack_policy policy;
            bool reset_sessions;
            uint64_t expected_session_epoch, expected_session_revision;
        } conntrack;
        /** 调用 ND 替换前必须暂停报文线程，明确允许后才释放旧邻居和等待报文 */
        struct {
            struct dppd_nd6_policy policy;
            bool reset_neighbors;
            uint64_t expected_neighbor_epoch, expected_neighbor_revision;
        } nd6;
    } payload;
};

struct dppd_flow_request_result {
    struct dppd_flow_request_key key;
    enum dppd_flow_request_operation operation;
    enum dppd_flow_request_phase phase;
    int status;
    uint32_t count;
    bool applied, durable;
    uint64_t revision, ids[8], generations[8];
};

/** 同键同内容只查询原请求，不再次安装；同键不同内容返回 EEXIST，过期序号返回 ESTALE */
int dppd_flow_store_request(struct dppd_flow_store *store, const struct dppd_flow_request *request, uint64_t now_ns,
    struct dppd_flow_request_result *result);
/** 查询不会消费回执，进程重启后仍可按原键查询，终态与策略可靠保存后才标记 durable */
int dppd_flow_store_request_get(struct dppd_flow_store *store, struct dppd_flow_request_key key,
    struct dppd_flow_request_result *result);
/** 停机前只取消还没执行的持久任务，完成结果继续由正常 process 路径原子保存 */
int dppd_flow_store_requests_cancel(struct dppd_flow_store *store);

#endif
