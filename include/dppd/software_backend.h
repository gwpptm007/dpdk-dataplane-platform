#ifndef DPPD_SOFTWARE_BACKEND_H
#define DPPD_SOFTWARE_BACKEND_H

#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <pthread.h>

#include "dppd/packet.h"
#include "dppd/transaction.h"

struct dppd_software_classifier_snapshot;
struct dppd_software_retired_snapshot;
struct rte_rcu_qsbr;

/**
 * 软件 classifier 的活跃规则集采用不可变 snapshot：worker 只原子读取 active，
 * 不获取控制面锁；更新线程复制、修改并发布一个新 snapshot。旧 snapshot 交给
 * DPDK QSBR，在所有 worker 都经过一个安全静默点后才释放，因此不会出现 UAF。
 *
 * writer_lock 只串行化控制面发布、查询和回收；它不在报文路径上使用。
 */
struct dppd_software_backend {
    /**
     * 当前供 worker 无锁读取的 immutable snapshot。发布后绝不能原地改写它；
     * 控制面只能通过“复制 -> 修改副本 -> 原子替换”的方式更新。
     */
    _Atomic(struct dppd_software_classifier_snapshot *) active;
    /**
     * 已从 active 摘下、但可能仍被某个 worker 引用的旧 snapshot 链表。仅持有
     * writer_lock 的控制面访问；QSBR 确认安全前不得直接 free。
     */
    struct dppd_software_retired_snapshot *retired;
    /**
     * 每个 worker 以 queue_id 注册为一个 reader。writer 用它判断所有 reader 是否
     * 都越过了发布旧 snapshot 时的安全边界。
     */
    struct rte_rcu_qsbr *qsbr;
    /** 把发布、查询和回收串行化；严禁在报文热路径获取此锁。 */
    pthread_mutex_t writer_lock;
    /** 每个 snapshot 预分配的槽位数，不等于其中当前有效规则数。 */
    uint32_t capacity;
    bool lock_initialized;
    bool qsbr_initialized;
};

/** 单个报文命中软件规则后得到的执行结果；pipeline 据此决定丢弃或写 MARK 元数据。 */
struct dppd_software_decision {
    /** 至少有一条规则命中；未命中时其余字段均不应被 pipeline 使用。 */
    bool matched;
    /** 命中规则包含 DROP；DROP 优先于随后对 mbuf 元数据的处理。 */
    bool drop;
    /** 命中规则包含 MARK，mark_id 才有效。 */
    bool has_mark;
    uint32_t mark_id;
};

/**
 * 创建空 classifier 和 QSBR 回收器；调用时不要求 EAL 已初始化，便于控制面单测。
 * 成功后必须在所有 worker 已退出并注销后调用 fini；capacity 同时限定软件规则数，
 * 因而不能在运行时扩大。
 */
int dppd_software_backend_init(struct dppd_software_backend *backend,
                               uint32_t capacity);
/** 所有工作线程退出并注销后释放活跃表、退役表和回收器；此函数不负责停止线程 */
void dppd_software_backend_fini(struct dppd_software_backend *backend);

/**
 * 对已经通过通用校验的规则检查软件能力，支持入口域的 ETH、IPv4、UDP、TCP 匹配
 * 动作限于 DROP、MARK 和 COUNT；QUEUE、transfer 或 represented port 不支持
 * 该检查用于判断能否降级到软件执行，不代替规则结构和字段合法性的通用校验
 */
bool dppd_software_backend_rule_supported(const struct dppd_rule *rule);
/**
 * 读取活跃软件规则表的对象数，供控制面估算还剩多少临时安装空间
 * 退役快照不计入数量；查询使用控制面锁，不在逐包处理路径上调用
 */
uint32_t dppd_software_backend_count(const struct dppd_software_backend *backend);
/** 把软件规则操作包装成事务回调，让上层统一安排准备、提交和失败补偿 */
struct dppd_transaction_backend dppd_software_transaction_backend(
    struct dppd_software_backend *backend);
/** 整批替换精确旧版本并只发布一次快照；发布前失败保留旧表和计数，新版本计数从零开始 */
int dppd_software_backend_update_batch(
    struct dppd_software_backend *backend, const struct dppd_rule *rules,
    const uint64_t *expected_generations, uint32_t count);
/** 删除指定 ID 和 generation 的活跃版本；成功后旧读者仍可能暂时使用退役快照 */
int dppd_software_backend_remove_version(struct dppd_software_backend *backend,
                                         uint64_t rule_id,
                                         uint64_t generation);
/** 判断当前活跃表是否存在精确版本，不把尚未回收的旧表算作当前安装结果 */
bool dppd_software_backend_contains_version(
    const struct dppd_software_backend *backend,
    uint64_t rule_id, uint64_t generation);
/** 查询指定版本的累计命中数和字节数，不清零；两个数独立采样，并非同一瞬间的快照 */
int dppd_software_backend_query_count(const struct dppd_software_backend *backend,
                                      uint64_t rule_id, uint64_t generation,
                                      uint64_t *hits, uint64_t *bytes);

/**
 * worker 启动后注册并上线；每轮完整的 ingress 扫描结束时报告一次静默点；退出前
 * 下线并注销。queue_id 在 runtime 内与 worker 下标一一对应，因而可直接作为 QSBR
 * reader id。注册失败时 worker 不得调用 decide；注销之前不得释放其运行时上下文。
 */
int dppd_software_backend_worker_register(struct dppd_software_backend *backend,
                                          unsigned int worker_id);
void dppd_software_backend_worker_quiescent(
    struct dppd_software_backend *backend, unsigned int worker_id);
void dppd_software_backend_worker_unregister(
    struct dppd_software_backend *backend, unsigned int worker_id);

/**
 * 使用已解析的有效字段选择一条匹配规则，调用方必须先排除畸形报文并注册读者
 * COUNT 在该规则版本的共享计数器上累加，MARK 和 DROP 只记录到输出结果
 * decision 必须指向有效内存；本函数不发送、不释放报文，也不报告 QSBR 安全点
 * 调用方在不再持有旧快照指针时报告安全点，本项目统一放在一轮端口扫描结束后
 */
void dppd_software_backend_decide(struct dppd_software_backend *backend,
                                  uint16_t ingress_port,
                                  const struct dppd_packet *packet,
                                  struct dppd_software_decision *decision);

#endif
