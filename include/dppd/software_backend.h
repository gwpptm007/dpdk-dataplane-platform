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
void dppd_software_backend_fini(struct dppd_software_backend *backend);

/**
 * 仅接受已经实现且与硬件语义等价的 ingress/ETH/IPv4/UDP/TCP + DROP/MARK/COUNT。
 * 它是 PREFER 降级的准入检查，不是“尽力匹配”：包含 QUEUE、transfer 或 represented
 * port 的规则会返回 false，避免软件路径悄悄改变转发语义。
 */
bool dppd_software_backend_rule_supported(const struct dppd_rule *rule);
struct dppd_transaction_backend dppd_software_transaction_backend(
    struct dppd_software_backend *backend);
int dppd_software_backend_remove_version(struct dppd_software_backend *backend,
                                         uint64_t rule_id,
                                         uint64_t generation);
bool dppd_software_backend_contains_version(
    const struct dppd_software_backend *backend,
    uint64_t rule_id, uint64_t generation);
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
 * 对一个已成功解析的 ingress packet 求最高优先级的匹配规则。COUNT 在 snapshot
 * 共享的原子计数器上累加；MARK 只写入 decision，由 worker 映射到 mbuf metadata。
 * 本函数不报告 QSBR 静默点：调用者必须在确认本轮不再持有任何 snapshot 指针时统一
 * 报告，不能在逐包处理中提前报告安全。
 */
void dppd_software_backend_decide(struct dppd_software_backend *backend,
                                  uint16_t ingress_port,
                                  const struct dppd_packet *packet,
                                  struct dppd_software_decision *decision);

#endif
