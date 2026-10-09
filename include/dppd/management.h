#ifndef DPPD_MANAGEMENT_H
#define DPPD_MANAGEMENT_H

#include <stdbool.h>
#include <stdint.h>
#include "dppd/control.h"
#include "dppd/link.h"
#include "dppd/stats_values.h"

/*
 * 管理协议版本在请求和响应中显式传递。当前实现不做版本协商：版本不一致时
 * daemon 返回 -EPROTO，避免客户端按错误的结构体布局解释响应。
 */
/**
 * v15 增加失败事件历史分页；v14 增加历史安装耗时分布；v13 增加请求指标与失败分类
 * 因此结构体布局变化必须提升版本，旧客户端会被明确拒绝，而不是错位解释 payload。
 */
#define DPPD_MANAGEMENT_VERSION 15U
#define DPPD_MANAGEMENT_DEFAULT_SOCKET "/tmp/dppd-control.sock"
/* sockaddr_un.sun_path 在 Linux 上通常为 108 字节，最后一字节留给 '\0'。 */
#define DPPD_MANAGEMENT_SOCKET_PATH_MAX 107U
/* 固定小页保证 response union 不因列表操作膨胀为大报文。 */
#define DPPD_MANAGEMENT_RULE_PAGE_SIZE 4U
/*
 * 首版批量命令限制为四条 DROP 创建，保持本地 ABI 报文、SOCK_SEQPACKET 单消息与运维
 * 输出都足够小。该值是协议 ABI 的一部分；提高上限会改变 request/response 的 sizeof，
 * 必须同步提升 DPPD_MANAGEMENT_VERSION，不能只修改 daemon 或 dppctl 一侧。
 */
#define DPPD_MANAGEMENT_BATCH_CREATE_MAX 4U
#define DPPD_MANAGEMENT_BATCH_REMOVE_MAX 4U
/** 协议数组与控制接口采用相同上限，避免协议允许的条数超过实际处理能力 */
#define DPPD_MANAGEMENT_BATCH_UPDATE_MAX DPPD_CONTROL_BATCH_UPDATE_MAX

/* 每个连接只承载一个请求和一个响应，operation 决定 payload union 的有效成员。 */
enum dppd_management_operation {
    DPPD_MANAGEMENT_PING = 1,
    DPPD_MANAGEMENT_RULE_APPLY,
    DPPD_MANAGEMENT_RULE_CREATE_BATCH,
    DPPD_MANAGEMENT_RULE_GET,
    DPPD_MANAGEMENT_RULE_DELETE,
    /* 追加新操作而不改变既有编号，避免同版本诊断时出现命令含义漂移。 */
    DPPD_MANAGEMENT_PORT_GET,
    DPPD_MANAGEMENT_RULE_COUNT_QUERY,
    DPPD_MANAGEMENT_RULE_LIST,
    /* 查询 snapshot 是否启用、是否 dirty 以及磁盘/内存 generation 的差距。 */
    DPPD_MANAGEMENT_PERSISTENCE_STATUS,
    /* 显式保存当前完整 repository，可用于修复 dirty 状态。 */
    DPPD_MANAGEMENT_PERSISTENCE_FLUSH,
    /* 启动重放回滚失败后的隔离状态查看与同进程 handle 删除重试。 */
    DPPD_MANAGEMENT_RECOVERY_STATUS,
    DPPD_MANAGEMENT_RECOVERY_RETRY,
    /* v6：精确 generation 的全有或全无批量删除。 */
    DPPD_MANAGEMENT_RULE_DELETE_BATCH,
    /** v7 新增的整批更新操作，追加在末尾以保持既有操作编号不变 */
    DPPD_MANAGEMENT_RULE_UPDATE_BATCH,
    DPPD_MANAGEMENT_STATS_QUERY,
    /** 健康快照可在恢复隔离期间查询，不执行规则操作、网卡探测或磁盘保存 */
    DPPD_MANAGEMENT_HEALTH_GET,
    /** 按当前版本读取实际后端和安装记录，追加编号以保留旧操作的含义 */
    DPPD_MANAGEMENT_RULE_STATUS,
    /** 只读取设备启动信息和累计校验记录，恢复隔离期间也可使用 */
    DPPD_MANAGEMENT_CAPABILITY_GET,
    /** 校验完整规则而不安装，探测缓存只服务此诊断操作 */
    DPPD_MANAGEMENT_CAPABILITY_PROBE,
    /** 显式放弃某个端口的旧探测结果，不删除网卡规则或账本记录 */
    DPPD_MANAGEMENT_CAPABILITY_CLEAR,
    /** 请求累计值与最近失败只读查询，恢复隔离仍可响应 */
    DPPD_MANAGEMENT_RULE_METRICS,
    /** 只读的后端提交耗时历史，恢复隔离仍可查询，不改变规则或累计值 */
    DPPD_MANAGEMENT_RULE_LATENCY,
    /** 只读最近失败历史，完整分页在恢复隔离期间仍可使用 */
    DPPD_MANAGEMENT_RULE_HISTORY,
};

/*
 * 当前协议只用于同机、同版本的 dppd/dppctl。SOCK_SEQPACKET 保留消息边界，
 * version 与 size 用于拒绝不兼容客户端；后续跨版本协议不能直接复用 C 结构体 ABI。
 * 这里包含 enum、bool、对齐填充及嵌套 rule/plan，因此不能假定不同编译器、架构或
 * 大小端之间具有相同布局。reserved 字段必须发送为 0，为兼容扩展预留空间。
 */
struct dppd_management_request {
    uint16_t version;      /* 必须等于 DPPD_MANAGEMENT_VERSION。 */
    uint16_t operation;    /* enum dppd_management_operation。 */
    uint32_t size;         /* 必须等于发送方 sizeof(*request)。 */
    uint64_t request_id;   /* 客户端生成；响应原样回显，用于请求/响应配对。 */
    union {
        struct {
            uint16_t port_id;
            uint16_t queue_id;
            uint32_t reserved;
        } stats_query;
        struct {
            /* 规则安装到哪个 DPDK ethdev port；它不是 PCI BDF 或 Linux ifindex。 */
            uint16_t install_port_id;
            uint16_t reserved;
            uint32_t reserved2;
            /*
             * 新建规则通常传 0；更新传客户端上次读取到的 generation；
             * DPPD_RULE_GENERATION_ANY 表示调用方明确放弃并发冲突检查。
             */
            uint64_t expected_generation;
            /* 客户端不负责填写 generation，daemon 在事务提交时分配。 */
            struct dppd_rule rule;
        } apply;
        /**
         * 固定小数组：只承载 create，不混入 update/delete。每个 entry 自带 install_port_id
         * 和 expected_generation；后者必须为 0，daemon 以此拒绝任何可能具有覆盖语义的
         * 请求，保证整个数组是一组纯新建操作。
         */
        struct {
            uint16_t count;
            uint8_t reserved[6];
            struct dppd_control_batch_create_request
                rules[DPPD_MANAGEMENT_BATCH_CREATE_MAX];
        } create_batch;
        /**
         * 删除批次必须包含 2–4 个不同的 (rule_id, expected_generation) 精确条件；
         * 不支持 ANY 或“缺失即成功”，以避免误删更新后的规则被当作批量幂等成功。
         */
        struct {
            uint16_t count;
            uint8_t reserved[6];
            struct dppd_control_batch_remove_request
                rules[DPPD_MANAGEMENT_BATCH_REMOVE_MAX];
        } delete_batch;
        /**
         * 一次请求携带两到四条完整的新规则，而不是对旧规则某个字段做局部修改
         * 每条独立指定安装端口和精确旧版本；count 表示数组前面实际使用的条数
         * reserved 必须全零，数组剩余位置不参与本次更新
         */
        struct {
            uint16_t count;
            uint8_t reserved[6];
            struct dppd_control_batch_update_request
                rules[DPPD_MANAGEMENT_BATCH_UPDATE_MAX];
        } update_batch;
        struct {
            /* 稳定规则 ID；0 是无效 ID。 */
            uint64_t rule_id;
        } get;
        struct {
            uint64_t rule_id;
            /* 与 apply 相同的乐观并发条件。 */
            uint64_t expected_generation;
        } delete_rule;
        struct {
            /* 查询的是 EAL 探测后的 ethdev port id。 */
            uint16_t port_id;
            uint8_t reserved[6];
        } port_get;
        /** 画像查询和清缓存共用端口选择，保留字节必须为零 */
        struct {
            uint16_t port_id;
            uint8_t reserved[6];
        } capability;
        /** 不要求规则已经存在，generation 不参与探测，refresh 可强制重新询问驱动 */
        struct {
            uint16_t install_port_id;
            bool refresh;
            uint8_t reserved[5];
            struct dppd_rule rule;
        } probe;
        struct {
            uint64_t rule_id;
            /* 查询也检查 generation，防止把更新后新规则的计数返回给旧客户端。 */
            uint64_t expected_generation;
        } count_query;
        /** 不传 ANY 时只接受账本当前的精确版本，避免把新对象的状态返回给旧客户端 */
        struct {
            uint64_t rule_id;
            uint64_t expected_generation;
        } rule_status;
        struct {
            /* 返回 rule id 严格大于该值的记录；第一页使用 0。 */
            uint64_t after_rule_id;
            /*
             * 第一页使用 ANY；后续页必须使用首个响应的 repository_generation，
             * 防止分页期间规则变化后返回混合快照。
             */
            uint64_t expected_repository_generation;
        } list;
        /** 游标是失败 ID 而不是操作序号，省略版本时使用独立历史 ANY 保留值 */
        struct {
            uint64_t after_event_id;
            uint64_t expected_revision;
        } rule_history;
    } payload;
};

/*
 * 管理协议专用的端口快照。这里不直接暴露 rte_eth_dev_info：DPDK 结构包含指针，
 * 不能跨进程传递；同时不同 DPDK 版本的字段布局也不稳定。
 *
 * capability 是 PMD 在启动时报告的静态上限；configured_* 是 dppd 结合配置后
 * 实际启用的子集。它们不能替代具体规则的 rte_flow_validate()。
 */
struct dppd_management_port_info {
    uint16_t port_id;
    uint16_t peer_port_id;
    int32_t socket_id;
    uint8_t endpoint_kind;
    bool configured;
    bool started;
    bool has_switch_domain;
    uint16_t switch_domain_id;
    uint16_t switch_port_id;
    uint16_t max_rx_queues;
    uint16_t max_tx_queues;
    uint16_t reta_size;
    uint8_t mac[6];
    uint64_t rss_offloads;
    uint64_t rx_offloads;
    uint64_t tx_offloads;
    uint64_t device_capabilities;
    uint64_t configured_rss_hf;
    uint64_t configured_tx_offloads;
    uint8_t link_state;
    char switch_name[64];
    char driver_name[64];
};

/* list 只返回运维摘要；完整 match/action 使用 get 获取，避免列表报文过大。 */
struct dppd_management_rule_summary {
    uint64_t rule_id;
    uint64_t generation;
    uint32_t group;
    uint32_t priority;
    uint32_t match_mask;
    uint32_t action_mask;
    uint16_t nb_matches;
    uint16_t nb_actions;
    uint8_t domain;
    uint8_t fallback;
    uint16_t install_port_id;
    uint8_t reserved[4];
};

struct dppd_management_rule_page {
    /* 本页对应的 repository 全局修订号，下一页请求必须原样携带。 */
    uint64_t repository_generation;
    /* 下一页 after_rule_id；本页为空时保留请求 cursor。 */
    uint64_t next_after_rule_id;
    uint32_t total_count;
    uint16_t count;
    bool has_more;
    uint8_t reserved;
    struct dppd_management_rule_summary rules[DPPD_MANAGEMENT_RULE_PAGE_SIZE];
};

/**
 * 每一位表示当前无法宣布就绪的一个原因，多种问题可同时保留，不能只报告第一个
 * 初始不支持链路查询的端口单独计数，它本身不会禁止既有的软件转发
 */
enum dppd_management_health_blocker {
    DPPD_HEALTH_NO_RUNTIME = 1U << 0,
    DPPD_HEALTH_STOP_REQUESTED = 1U << 1,
    DPPD_HEALTH_DEVICE_REMOVED = 1U << 2,
    DPPD_HEALTH_WORKERS_NOT_RUNNING = 1U << 3,
    DPPD_HEALTH_PORTS_NOT_STARTED = 1U << 4,
    DPPD_HEALTH_LINK_DOWN = 1U << 5,
    DPPD_HEALTH_LINK_UNKNOWN = 1U << 6,
    DPPD_HEALTH_RECOVERY_REQUIRED = 1U << 7,
    DPPD_HEALTH_PERSISTENCE_DIRTY = 1U << 8,
    DPPD_HEALTH_WORKER_FAILED = 1U << 9,
};

/**
 * 管理主线程读取自己持有的控制状态和工作线程发布的原子状态，组成无指针的快照
 * ready 只说明当前配置、线程、已知链路与策略状态允许运行，不承诺端到端网络可达
 * 查询成功即可证明主线程能够响应，客户端将此表示为 live=yes
 */
struct dppd_management_health {
    bool ready;
    uint8_t recovery_state;
    bool persistence_enabled;
    bool persistence_dirty;
    uint32_t blockers;
    uint16_t workers_expected;
    uint16_t workers_running;
    uint16_t workers_failed;
    uint16_t ports_expected;
    uint16_t ports_started;
    uint16_t links_up;
    uint16_t links_down;
    uint16_t links_unknown;
    /** 不支持查询表示链路未经验证，不能把此数值归入 links_up */
    uint16_t links_unsupported;
    uint16_t reserved;
    uint32_t rule_count;
    uint64_t repository_generation;
    uint64_t persisted_generation;
    int32_t persistence_last_error;
    int32_t recovery_last_error;
};

/** 将启动快照与本进程的规则校验记录一起呈现，未知能力不能伪装成驱动已经支持 */
struct dppd_management_capability_profile {
    struct dppd_management_port_info port;
    struct dppd_capability_identity identity;
    struct dppd_flow_probe_statistics probes;
    uint64_t cache_ttl_ns;
};

struct dppd_management_response {
    uint16_t version;      /* daemon 实际使用的协议版本。 */
    uint16_t operation;    /* 回显请求 operation。 */
    uint32_t size;         /* daemon 侧 sizeof(*response)。 */
    uint64_t request_id;   /* 回显请求 ID。 */
    /* 0 表示业务成功；负值使用 Linux -errno 约定，例如 -ENOENT/-ESTALE。 */
    int32_t status;
    uint32_t reserved;
    union {
        struct {
            /* repository 的全局单调 generation，可用于快速判断 desired state 是否变化。 */
            uint64_t repository_generation;
            /* 当前 desired repository 中的规则数，不代表 NIC 剩余容量。 */
            uint32_t rule_count;
            uint32_t reserved;
        } pong;
        /* 包含创建/更新/幂等结果、最终 generation、事务 ID 和规划决策。 */
        struct dppd_control_apply_result apply;
        /**
         * 与请求顺序一一对应的批量创建结果，所有成功条目应共享 transaction_id。任一
         * 条目失败时 status 为负值且调用方不得把数组中残留字段解释为部分成功结果；
         * 控制层已将本批 actual/desired 全部回滚或进入 recovery 隔离。
         */
        struct {
            uint16_t count;
            uint8_t reserved[6];
            struct dppd_control_apply_result
                rules[DPPD_MANAGEMENT_BATCH_CREATE_MAX];
        } create_batch;
        /**
         * 与 delete_batch 输入顺序一一对应。仅 response.status 为 0 时才可读取，
         * generation 是每次 repository 删除后单调递增的全局修订号。
         */
        struct {
            uint16_t count;
            uint8_t reserved[6];
            struct dppd_control_batch_remove_result
                rules[DPPD_MANAGEMENT_BATCH_REMOVE_MAX];
        } delete_batch;
        /**
         * 只有整个响应的 status 为零时，这组结果才有效
         * 每条结果按请求顺序给出新版本和实际后端，所有结果共享同一个 transaction_id
         * 失败响应不会提供可当作部分成功使用的回执，客户端应根据错误查询相关状态
         */
        struct {
            uint16_t count;
            uint8_t reserved[6];
            struct dppd_control_apply_result
                rules[DPPD_MANAGEMENT_BATCH_UPDATE_MAX];
        } update_batch;
        /* get 成功时返回 repository 中保存的完整 canonical rule。 */
        struct dppd_rule rule;
        struct {
            /* 删除操作完成后的 repository 全局 generation。 */
            uint64_t generation;
            /* 规则原本不存在时为 false；这仍是幂等成功而不是 ENOENT。 */
            bool removed;
            uint8_t reserved[7];
        } delete_rule;
        struct dppd_management_port_info port;
        struct dppd_control_count_result count;
        struct dppd_management_rule_page rule_page;
        struct dppd_control_persistence_status persistence;
        struct dppd_control_recovery_status recovery;
        struct dppd_stats_values stats;
        struct dppd_management_health health;
        struct dppd_control_rule_status rule_status;
        struct dppd_management_capability_profile capability;
        struct dppd_flow_probe_result probe;
        struct dppd_rule_metrics rule_metrics;
        struct dppd_rule_latency_report rule_latency;
        struct dppd_rule_history_page rule_history;
    } payload;
};

struct dppd_device_set;
struct dppd_runtime;

struct dppd_management_server {
    int socket_fd;                         /* 非阻塞监听 socket；未启动时为 -1。 */
    /* control 的所有权仍属于 dppd main，management 只保存非拥有型指针。 */
    struct dppd_control_service *control;
    /* device_set 同样由 runtime 持有，必须比 management server 生命周期更长。 */
    const struct dppd_device_set *devices;
    const struct dppd_runtime *runtime;
    /* 保存 bind 成功的路径，stop 时只删除本实例创建的 socket。 */
    char socket_path[DPPD_MANAGEMENT_SOCKET_PATH_MAX + 1U];
    bool started;
};

/*
 * 纯协议分发函数，不执行 socket I/O，便于单元测试覆盖业务映射。
 * 函数自身参数错误通过返回值报告；已收到请求后的协议/业务错误写入 response->status。
 */
int dppd_management_handle(struct dppd_control_service *control,
                           const struct dppd_device_set *devices,
                           const struct dppd_runtime *runtime,
                           const struct dppd_management_request *request,
                           struct dppd_management_response *response);
int dppd_management_start(struct dppd_management_server *server,
                          struct dppd_control_service *control,
                          const struct dppd_device_set *devices,
                          const struct dppd_runtime *runtime,
                          const char *socket_path);
int dppd_management_poll(struct dppd_management_server *server);
/* stop 可重复调用；成功启动过时会关闭 fd 并删除对应文件系统路径。 */
void dppd_management_stop(struct dppd_management_server *server);

#endif
