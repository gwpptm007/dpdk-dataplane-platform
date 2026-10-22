#include "dppd/software_backend.h"
#include "dppd/acl.h"
#include "dppd/exact.h"
#include "dppd/software_shared.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <rte_common.h>
/** DPDK 21.11 的实验性 QSBR 头仍含 GNU 可变宏和零长数组声明 */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wvariadic-macros"
#pragma GCC diagnostic ignored "-Wpedantic"
#include <rte_rcu_qsbr.h>
#pragma GCC diagnostic pop

#include "dppd/config.h"
#include "dppd/install_time.h"

/**
 * 将计数器与规则数组分开保存，让同一规则版本在复制快照后继续累计 COUNT
 * 安装新的 generation 会创建新计数器，因此这里的连续统计不表示跨版本继承
 * references 记录引用它的快照数量，包括尚未发布的副本，不是工作线程数量
 * hits 和 bytes 使用原子操作，避免多个工作线程同时累计时覆盖彼此的结果
 */
struct dppd_software_rule_metrics {
    atomic_uint_fast64_t references;
    atomic_uint_fast64_t hits;
    atomic_uint_fast64_t bytes;
    /**
     * 同一版本复制快照时共享安装记录，修改其他规则不会覆盖此版本的原始耗时
     * 以下字段只由控制面在 writer_lock 内访问，工作线程只使用上面的原子计数器
     */
    uint64_t install_duration_ns;
    uint32_t commit_rule_count;
    bool timing_available;
    struct dppd_flow_shared_counts *counts;
    struct dppd_flow_shared_count_lease shared_lease;
    struct dppd_software_shared_counter *shared;
};

struct dppd_software_rule_object {
    /** metrics 为 NULL 就表示空槽位，无须额外的 occupied 标志 */
    struct dppd_rule rule;
    struct dppd_software_rule_metrics *metrics;
};

/**
 * snapshot 表示内存中的一整版软件规则表，让 worker 读取完整、一致的规则，同时允许控制面更新
 *
 * snapshot
 * ├── 有效规则数量 count
 * ├── 规则数组 objects
 * │   ├── 匹配条件、动作等规则内容
 * │   └── metrics 指针 → COUNT 计数器
 * ├── 精确匹配表
 * └── ACL 匹配表
 *
 * 内存布局：[count][capacity 个 objects 槽位][精确匹配表][ACL]，尾部两张表通过辅助函数定位
 * worker 读取 active 取得一个快照，再用同一快照完成候选匹配、动作选择和计数器定位
 *
 * 旧快照 A：worker 正在读取，active 指向 A
 *      │
 *      └── 复制成 B → 修改 B → 重建 B 的匹配表
 *                                  │
 *                         完成后将 active 切换到 B
 *                                  ↓
 *                         后续查询使用新快照 B
 *
 * active 是当前已发布快照的指针，retired 链表保存暂时不能释放的旧快照
 * 已经取得 A 的 worker 继续使用 A，QSBR 确认旧表无人再用后，控制面才回收 A
 * 这样不会在原表上交错修改匹配条件和动作，worker 不会读到更新一半的规则表
 * 发布后的规则内容和匹配索引不再原地修改，metrics 指向的 COUNT 计数器仍可持续更新
 * 此结构保存运行时规则版本，不保存原始报文，也不负责磁盘持久化
 */
struct dppd_software_classifier_snapshot {
    /** 当前有效规则数，槽位容量由 backend->capacity 决定，有效规则不一定连续排列 */
    uint32_t count;
    /** 柔性数组，分配时预留 capacity 个槽位，每项保存规则内容及 metrics 指针 */
    struct dppd_software_rule_object objects[];
};

/** 一个待回收的旧规则表，仍可能被 worker 读取，因此从 active 摘下后先保留 */
struct dppd_software_retired_snapshot {
    /** 旧规则表地址，例如 active 已切换到 B，这里保存之前的 A */
    struct dppd_software_classifier_snapshot *snapshot;
    /** 切换 active 后取得的 QSBR 进度标记，用于检查读者是否已结束对旧表的使用 */
    uint64_t token;
    /** 下一个待回收节点，多次更新可能同时留下多份尚不能释放的旧表 */
    struct dppd_software_retired_snapshot *next;
};

struct dppd_software_pending_rule {
    /** prepare token 的所有权由 transaction 持有，rollback/finalize 二选一释放 */
    struct dppd_rule rule;
    /** 仅 commit 完成发布后置位，决定 rollback 是否还要删除 classifier 中的版本 */
    bool published;
};

static size_t snapshot_size(uint32_t capacity)
{
    return sizeof(struct dppd_software_classifier_snapshot) +
        (size_t)capacity * sizeof(struct dppd_software_rule_object) + dppd_exact_size(capacity) +
        dppd_acl_size(capacity);
}

/** 规则对象按八字节对齐，尾部 ACL 随整张快照分配和回收，不持有另一份活跃指针 */
static struct dppd_exact *snapshot_exact(struct dppd_software_classifier_snapshot *snapshot, uint32_t capacity)
{
    return (struct dppd_exact *)(snapshot->objects + capacity);
}

static struct dppd_acl *snapshot_acl(struct dppd_software_classifier_snapshot *snapshot, uint32_t capacity)
{
    return (struct dppd_acl *)((uint8_t *)snapshot_exact(snapshot, capacity) + dppd_exact_size(capacity));
}

/** 归还一个快照持有的计数器引用，最后一个引用归还时解除共享计数绑定并释放对象，NULL 无操作 */
static void metrics_put(struct dppd_software_rule_metrics *metrics)
{
    /**
     * references 统计持有该对象的快照数量，atomic_fetch_sub 返回减之前的值
     * 例：新旧表 A、B 共用计数器，销毁 A 时 2 → 1，返回 2，不进入释放分支
     * 最后销毁 B 时 1 → 0，返回 1，才释放；这里减的是引用数，不是规则命中次数
     */
    if (metrics != NULL && atomic_fetch_sub_explicit(&metrics->references, 1, memory_order_acq_rel) == 1) {
        if (metrics->shared_lease.owner != 0)
            (void)dppd_flow_shared_counts_unbind(metrics->counts, &metrics->shared_lease);
        free(metrics);
    }
}

/**
 * 销毁 snapshot：逐槽归还计数器引用，再释放包含规则、精确匹配表和 ACL 的整块快照内存
 * capacity 是分配的槽位数，有效规则可能分散其中，因此不能只遍历 count 个槽位
 * 未发布副本可直接销毁；已发布快照须由调用方先通过 QSBR 检查，或等待所有 worker 退出
 * 本函数不等待 worker，也不强制释放仍被其他快照引用的计数器；snapshot 为 NULL 时直接返回
 */
static void snapshot_destroy(struct dppd_software_classifier_snapshot *snapshot, uint32_t capacity)
{
    uint32_t i;

    if (snapshot == NULL)
        return;
    /** 只归还当前表持有的引用，计数器仍被新表使用时保留原来的 hits 和 bytes */
    for (i = 0; i < capacity; ++i)
        metrics_put(snapshot->objects[i].metrics);
    free(snapshot);
}

/**
 * 创建供控制面修改的新规则快照，复制已有规则并共享其计数器，保持未变规则的 COUNT 连续
 * source 为源快照，NULL 表示创建空表；capacity 为槽位容量，复制已有表时须与其容量一致
 * 成功返回尚未发布的副本，分配失败返回 NULL；此处不修改 backend->active
 * ACL 和精确匹配表只初始化为空，调用方完成规则修改后，由 publish_locked 重新构建并发布
 * 副本从创建时就持有计数器引用，后续操作失败也必须通过 snapshot_destroy 归还
 */
static struct dppd_software_classifier_snapshot *snapshot_clone(const struct dppd_software_classifier_snapshot *source,
    uint32_t capacity)
{
    struct dppd_software_classifier_snapshot *copy;
    uint32_t i;

    /** 一次分配快照头、规则槽位、精确匹配表和 ACL 的空间，初始全部清零 */
    copy = calloc(1, snapshot_size(capacity));
    if (copy == NULL)
        return NULL;
    dppd_acl_init(snapshot_acl(copy, capacity), capacity);
    dppd_exact_init(snapshot_exact(copy, capacity), capacity);
    if (source == NULL)
        return copy;
    copy->count = source->count;
    for (i = 0; i < capacity; ++i) {
        /**
         * 结构体赋值复制 rule 内容和 metrics 指针，不复制指针指向的计数器对象
         * 例：A.objects[i].metrics 和新表 B.objects[i].metrics 都指向同一个 M
         * 
         * 旧表 A.objects[i]                 新表 B.objects[i]
            ┌──────────────┐                 ┌──────────────┐
            │ rule 内容    │ ──复制内容─────→ │ rule 内容    │
            │ metrics 指针 │                 │ metrics 指针 │
            └──────┬───────┘                 └──────┬───────┘
                    └──────────→ M ←─────────────────┘
                            同一个计数器
         */
        copy->objects[i] = source->objects[i];
        /** B 也持有 M，引用数例如从 1 增为 2；hits 和 bytes 不清零，也不随复制增加 */
        if (copy->objects[i].metrics != NULL)
            atomic_fetch_add_explicit(&copy->objects[i].metrics->references, 1, memory_order_relaxed);
    }
    return copy;
}

static int snapshot_find(const struct dppd_software_classifier_snapshot *snapshot, uint32_t capacity, uint64_t rule_id,
    uint64_t generation)
{
    uint32_t i;

    /** generation 与 ID 一起定位，防止旧事务误删同 ID 的新版本 */
    for (i = 0; i < capacity; ++i) {
        if (snapshot->objects[i].metrics != NULL && snapshot->objects[i].rule.id == rule_id &&
            snapshot->objects[i].rule.generation == generation)
            return (int)i;
    }
    return -1;
}

/**
 * 检查 retired 链表，销毁已通过 QSBR 安全检查的旧规则表及其链表节点
 *
 * backend 提供待回收链表和读者进度，调用方必须持有 writer_lock，防止控制面同时修改链表
 * check 返回 0 → 保留该旧表，继续检查下一个节点；返回非零 → 从链表摘下并销毁
 * false 表示不等待读者，尚不安全的旧表留待后续发布、查询或显式回收时再检查
 * 无返回值，不修改 active 指向的新表，也不处理报文 mbuf
 */
static void reclaim_locked(struct dppd_software_backend *backend)
{
    /**
     * link 指向保存当前节点地址的位置，*link 才是当前节点指针
     * 初始指向 backend->retired；若保留某节点，再改为指向该节点的 next 字段
     * 因此给 *link 赋值，可以直接修改链表头或前一个节点的 next
     */
    struct dppd_software_retired_snapshot **link = &backend->retired;

    while (*link != NULL) {
        struct dppd_software_retired_snapshot *retired = *link;

        /** token 标识此旧表的回收进度，返回 0 表示仍有相关在线读者未通过对应安全点 */
        if (rte_rcu_qsbr_check(backend->qsbr, retired->token, false) == 0) {
            /** 当前节点继续留在链上，转而检查它的后继，不在这里等读者 */
            link = &retired->next;
            continue;
        }
        /**
         * 安全后让前面的指针绕过当前节点，例如 链表头 → A → Z 改为 链表头 → Z
         * link 所指的位置仍然有效，下一轮从这个位置检查后继，不能再访问即将释放的 A
         */
        *link = retired->next;
        /** 销毁旧规则表并归还它持有的计数器引用，再释放保存 snapshot/token/next 的链表节点 */
        snapshot_destroy(retired->snapshot, backend->capacity);
        free(retired);
    }
}

/**
 * 为 next 中的最终规则构建匹配表，发布新快照，并将旧快照交给 QSBR 回收流程
 * backend 提供容量、active 和回收链表，next 是调用方准备好的私有副本，调用者须持有 writer_lock
 * 返回 0 表示发布成功；负错误码表示发布前失败，active 保持原指向，副本由调用方销毁
 *
 * 副本 B 的规则内容已准备好
 *   → 清空 B 的 ACL / exact 索引 → 遍历有效规则，逐条加入合适的匹配表
 *   → 预先分配旧表回收节点 → active 从 A 切换到 B
 *   → 保存 A 和新取得的 QSBR token → 尝试回收已安全的旧表
 */
static int publish_locked(struct dppd_software_backend *backend, struct dppd_software_classifier_snapshot *next)
{
    struct dppd_software_classifier_snapshot *previous;
    struct dppd_software_retired_snapshot *retired;

    /** 重建的是 B 内部的匹配表，旧表 A 仍供 worker 使用；索引须反映调用方增删改后的最终规则 */
    struct dppd_acl *acl = dppd_acl_init(snapshot_acl(next, backend->capacity), backend->capacity);
    struct dppd_exact *exact = dppd_exact_init(snapshot_exact(next, backend->capacity), backend->capacity);
    for (uint32_t i = 0; i < backend->capacity; ++i) {
        if (next->objects[i].metrics == NULL)
            continue;
        /** 快照自己借用共享对象，实际 flow 已删除时，旧 worker 仍可完成最后一次累计 */
        struct dppd_software_rule_metrics *metrics = next->objects[i].metrics;
        const struct dppd_rule *rule = &next->objects[i].rule;
        if (backend->shared_counts != NULL && metrics->shared_lease.owner == 0) {
            for (uint16_t a = 0; a < rule->nb_actions; ++a) {
                if (rule->actions[a].type != DPPD_ACTION_SHARED_COUNT)
                    continue;
                struct dppd_flow_binding binding = {0};
                int bound =
                    dppd_flow_shared_counts_bind(backend->shared_counts, rule, &binding, &metrics->shared_lease);
                if (bound != 0)
                    return bound;
                metrics->counts = backend->shared_counts;
                metrics->shared = (void *)binding.actions[a];
                break;
            }
        }
        /**
         * 每条有效规则先尝试精确匹配表：
         *   exact_add → 0        → 已加入 exact，处理下一条规则
         *             -ENOTSUP → 改用 acl_add，支持掩码、端口范围等匹配条件
         *             其他错误  → 返回错误，终止本次发布
         *   acl_add   → 0        → 已加入 ACL，处理下一条规则
         *             非 0      → 返回错误，终止本次发布
         * 这是安装规则时的分类，worker 收包时使用 classify 查表，不逐包重新添加规则
         *
         * i + 1 是规则槽位的 tag，0 留给未命中；tag 不是规则 ID，也不是匹配表内部行号
         * 例：B.objects[2] → 加入 exact 或 ACL 时保存 tag = 3
         *     报文匹配返回 3 → B.objects[3 - 1] → 取得该规则的动作和计数器
         */
        int rc = dppd_exact_add(exact, &next->objects[i].rule, i + 1);
        if (rc == -ENOTSUP)
            rc = dppd_acl_add(acl, &next->objects[i].rule, i + 1);
        if (rc != 0)
            return rc;
    }
    retired = calloc(1, sizeof(*retired));
    if (retired == NULL) {
        /** 在发布前保证能够记录旧 snapshot，避免内存紧张时丢失回收所有权 */
        return -ENOMEM;
    }
    /**
     * 原子交换 active 指针：写入 next，同时把交换前的指针返回给 previous
     * 交换前：backend->active → A，next → B，B 的规则和匹配表均已准备好
     * 交换后：backend->active → B，previous → A，next 仍指向 B
     * 本操作只换指针，不复制快照，也不释放 A
     *
     * release 与 worker 读取 active 时的 acquire 配合，使读到 B 的 worker 能看到 B 的初始化结果
     * 已持有 A 的 worker → 继续使用 A → 报告安全点 → QSBR 确认后才能释放 A
     * 从 active 取得 B 的 worker → 使用 B 的规则和匹配表
     */
    previous = atomic_exchange_explicit(&backend->active, next, memory_order_release);
    if (previous == NULL) {
        /** 没有旧快照需要回收，只释放预分配的回收节点，新快照 B 已经发布 */
        free(retired);
        return 0;
    }
    /**
     * 用回收节点 R 保存旧快照 A 的地址，A 本身仍留在内存中供已有读者使用
     * R.snapshot → A；backend->active → B，两者分别管理待回收旧表和当前规则表
     */
    retired->snapshot = previous;
    /**
     * A 退出 active 后再取得 QSBR 回收标记，后续据此确认相关读者已结束此前的访问
     * token 是 QSBR 进度标记，不是时间戳或规则版本；start 不等待，也不释放 A
     */
    retired->token = rte_rcu_qsbr_start(backend->qsbr);
    /**
     * 将回收节点 R 插到链表头，假设原链头为旧节点 X
     * 插入前：backend->retired → X → ...；R.snapshot → A
     * 第一句：R.next → X，接住原来的回收链
     * 第二句：backend->retired → R → X → ...，R 成为新链头
     * 原链为空时 R.next 为 NULL；节点的 next 串起回收记录，snapshot 指向各自的旧快照
     */
    retired->next = backend->retired;
    backend->retired = retired;
    /**
     * 检查整条回收链，各节点分别用自己的 token 判断是否安全，不在这里等待读者
     * 以 R 为例：检查未通过 → 保留 R 和 A，留待以后再检查
     *            检查通过   → 摘下 R → 销毁 A、归还计数器引用 → 释放 R
     * 无论本次是否回收 A，B 都已发布成功，下面返回 0
     */
    reclaim_locked(backend);
    return 0;
}

/**
 * 检查已经通过通用规则校验的规则能否由软件执行，不替代数组长度和字段合法性检查
 * 软件只处理入口域的有限动作，不能把不支持的硬件动作静默改成普通转发
 */
bool dppd_software_backend_rule_supported(const struct dppd_rule *rule)
{
    uint16_t i;

    if (rule == NULL || rule->domain != DPPD_RULE_DOMAIN_INGRESS)
        return false;
    for (i = 0; i < rule->nb_matches; ++i) {
        if (rule->matches[i].type < DPPD_MATCH_ETH || rule->matches[i].type > DPPD_MATCH_IPV6 ||
            rule->matches[i].type == DPPD_MATCH_REPRESENTED_PORT)
            return false;
    }
    for (i = 0; i < rule->nb_actions; ++i) {
        switch (rule->actions[i].type) {
        case DPPD_ACTION_DROP:
        case DPPD_ACTION_MARK:
        case DPPD_ACTION_COUNT:
        case DPPD_ACTION_ALLOW:
            break;
        case DPPD_ACTION_QUEUE:
        case DPPD_ACTION_REPRESENTED_PORT:
        default:
            return false;
        }
    }
    return true;
}

bool dppd_software_backend_rule_supported_managed(const struct dppd_rule *rule)
{
    if (rule == NULL || rule->nb_actions > DPPD_RULE_MAX_ACTIONS)
        return false;
    struct dppd_rule normalized = *rule;
    for (uint16_t i = 0; i < normalized.nb_actions; ++i)
        if (normalized.actions[i].type == DPPD_ACTION_SHARED_COUNT)
            normalized.actions[i].type = DPPD_ACTION_COUNT;
    return dppd_software_backend_rule_supported(&normalized);
}

static bool supported(const struct dppd_software_backend *backend, const struct dppd_rule *rule)
{
    return backend->shared_counts == NULL ? dppd_software_backend_rule_supported(rule)
                                          : dppd_software_backend_rule_supported_managed(rule);
}

int dppd_software_backend_init(struct dppd_software_backend *backend, uint32_t capacity)
{
    struct dppd_software_classifier_snapshot *initial;
    size_t qsbr_size;

    if (backend == NULL || capacity == 0)
        return -EINVAL;
    memset(backend, 0, sizeof(*backend));
    initial = snapshot_clone(NULL, capacity);
    if (initial == NULL)
        return -ENOMEM;
    if (pthread_mutex_init(&backend->writer_lock, NULL) != 0) {
        snapshot_destroy(initial, capacity);
        return -EIO;
    }
    backend->lock_initialized = true;
    /** reader 上限与 runtime 的 queue/worker 编号空间一致，避免动态编号造成越界 */
    qsbr_size = rte_rcu_qsbr_get_memsize(DPPD_MAX_WORKERS);
    /**
     * QSBR 变量只要求 cache-line 对齐；这里刻意不用 rte_zmalloc，使纯控制面单测
     * 无需先启动 EAL。真实 dppd 进程仍在 EAL 初始化后正常使用同一块内存
     */
    qsbr_size = (qsbr_size + RTE_CACHE_LINE_SIZE - 1U) & ~(size_t)(RTE_CACHE_LINE_SIZE - 1U);
    backend->qsbr = aligned_alloc(RTE_CACHE_LINE_SIZE, qsbr_size);
    if (backend->qsbr == NULL || rte_rcu_qsbr_init(backend->qsbr, DPPD_MAX_WORKERS) != 0) {
        free(backend->qsbr);
        (void)pthread_mutex_destroy(&backend->writer_lock);
        snapshot_destroy(initial, capacity);
        memset(backend, 0, sizeof(*backend));
        return -ENOMEM;
    }
    backend->capacity = capacity;
    backend->qsbr_initialized = true;
    atomic_init(&backend->active, initial);
    return 0;
}

void dppd_software_backend_fini(struct dppd_software_backend *backend)
{
    struct dppd_software_retired_snapshot *retired;
    struct dppd_software_classifier_snapshot *active;

    /** fini 不等待 QSBR；调用契约要求 worker 已全部停止，因此此时可直接释放 retired */
    if (backend == NULL)
        return;
    if (backend->lock_initialized)
        (void)pthread_mutex_lock(&backend->writer_lock);
    active = atomic_load_explicit(&backend->active, memory_order_relaxed);
    snapshot_destroy(active, backend->capacity);
    retired = backend->retired;
    while (retired != NULL) {
        struct dppd_software_retired_snapshot *next = retired->next;

        snapshot_destroy(retired->snapshot, backend->capacity);
        free(retired);
        retired = next;
    }
    if (backend->lock_initialized) {
        (void)pthread_mutex_unlock(&backend->writer_lock);
        (void)pthread_mutex_destroy(&backend->writer_lock);
    }
    free(backend->qsbr);
    memset(backend, 0, sizeof(*backend));
}

/** 事务的能力检查阶段只判断目标后端和规则能力，不分配资源，也不发布规则 */
static int transaction_validate(void *context, const struct dppd_transaction_item *item)
{
    struct dppd_software_backend *backend = context;

    if (backend == NULL || item == NULL || item->plan.backend != DPPD_PLAN_BACKEND_SOFTWARE ||
        !supported(backend, &item->rule))
        return -ENOTSUP;
    return 0;
}

/**
 * prepare 只分配可回滚的控制面 token，不让未提交规则暴露给 worker。锁内复查容量和
 * 版本冲突后立即解锁；真正发布留到 commit，以便同一事务的任一 prepare 失败时完全
 * 不改变数据面可见规则集
 * 这里不实际预留规则槽位，多个 prepare 成功不代表它们一定能够全部提交
 * commit 仍需检查当时的容量，批量操作的整体容量预检由上层控制面负责
 */
static int transaction_prepare(void *context, const struct dppd_transaction_item *item, uintptr_t *token)
{
    struct dppd_software_backend *backend = context;
    struct dppd_software_pending_rule *pending;
    struct dppd_software_classifier_snapshot *active;
    int found;

    if (backend == NULL || item == NULL || token == NULL || item->rule.generation == 0)
        return -EINVAL;
    pending = calloc(1, sizeof(*pending));
    if (pending == NULL)
        return -ENOMEM;
    (void)pthread_mutex_lock(&backend->writer_lock);
    active = atomic_load_explicit(&backend->active, memory_order_acquire);
    found = snapshot_find(active, backend->capacity, item->rule.id, item->rule.generation);
    if (found >= 0 || active->count >= backend->capacity) {
        (void)pthread_mutex_unlock(&backend->writer_lock);
        free(pending);
        return found >= 0 ? -EEXIST : -ENOSPC;
    }
    (void)pthread_mutex_unlock(&backend->writer_lock);
    pending->rule = item->rule;
    *token = (uintptr_t)pending;
    return 0;
}

/**
 * 控制面提交一条准备好的软件规则：复查当前表，复制快照，加入规则及独立计数器，再发布新表
 * context 是软件后端，token 是 prepare 返回的 pending 地址，规则内容从 pending->rule 取得
 * 此处 token 是事务准备记录，不是 QSBR 回收进度标记；item 在本实现中不再读取
 * 成功返回 0 并设置 pending->published，失败返回负错误码；此函数不会释放 pending
 * 本次切换只发布这一条规则的安装结果，不代表整个跨规则事务同时对报文生效
 * 本函数在安装规则时调用，worker 逐包匹配只读取已发布结果，不会每包复制规则表
 */
static int transaction_commit(void *context, const struct dppd_transaction_item *item, uintptr_t token)
{
    struct dppd_software_backend *backend = context;
    struct dppd_software_pending_rule *pending = (struct dppd_software_pending_rule *)token;
    struct dppd_software_classifier_snapshot *active;
    struct dppd_software_classifier_snapshot *next;
    struct dppd_software_rule_metrics *metrics;
    uint32_t i;
    struct dppd_install_timer timer;

    /** commit 必须再次检查：prepare 与 commit 之间可能已有另一控制请求完成发布 */
    (void)item;
    if (backend == NULL || pending == NULL)
        return -EINVAL;
    (void)pthread_mutex_lock(&backend->writer_lock);
    active = atomic_load_explicit(&backend->active, memory_order_acquire);
    if (snapshot_find(active, backend->capacity, pending->rule.id, pending->rule.generation) >= 0 ||
        active->count >= backend->capacity) {
        (void)pthread_mutex_unlock(&backend->writer_lock);
        return -EEXIST;
    }
    dppd_install_timer_start(&timer);
    /**
     * 例：旧表只有 R1，本次准备新增 R2，先复制整张表，不修改已发布的 A
     *
     * 复制前：backend->active → A：[R1]，pending->rule 保存 R2
     * 复制后：backend->active → A：[R1]
     *         next            → B：[R1]    尚未发布，供控制面修改
     */
    next = snapshot_clone(active, backend->capacity);
    if (next == NULL) {
        (void)pthread_mutex_unlock(&backend->writer_lock);
        return -ENOMEM;
    }
    /**
     * 例：A 原有 R1，副本 B 已与 A 共享 R1 的计数器；本次新增 R2，需要另建计数器
     * 新安装版本从 hits = 0、bytes = 0 开始，不继承同 ID 旧版本的统计
     */
    metrics = calloc(1, sizeof(*metrics));
    if (metrics == NULL) {
        snapshot_destroy(next, backend->capacity);
        (void)pthread_mutex_unlock(&backend->writer_lock);
        return -ENOMEM;
    }
    /**
     * R2 新计数器的初始状态：
     *   references = 1 → 只有副本 B 持有，不表示已经命中一个包
     *   hits       = 0 → 尚未匹配过报文
     *   bytes      = 0 → 尚未累计报文字节
     */
    atomic_init(&metrics->references, 1);
    atomic_init(&metrics->hits, 0);
    atomic_init(&metrics->bytes, 0);
    /** metrics 为 NULL 表示空槽位，将 R2 内容和新计数器一同放入 B，再增加有效规则数 */
    for (i = 0; i < backend->capacity; ++i) {
        if (next->objects[i].metrics == NULL) {
            next->objects[i].rule = pending->rule;
            next->objects[i].metrics = metrics;
            next->count++;
            break;
        }
    }
    /**
     * 加入 R2 后，规则与计数器的关系：
     *
     * A：[R1] ────────────→ R1 原计数器
     *                          ↑
     * B：[R1] ─────────────────┘
     *    [R2] ────────────→ R2 新计数器
     *
     * 此时 R1 的计数器由 A、B 共同持有，引用数为 2；R2 的计数器只由 B 持有，引用数为 1
     * 发布前：backend->active → A，worker 还看不到 B 中新增的 R2
     * publish_locked 返回
     *   非 0 → active 仍指向 A → 销毁未发布的 B → 解锁 → 返回发布错误
     *          R1 计数器引用 2 → 1，A 继续使用；R2 计数器引用 1 → 0，随之释放
     *   为 0 → active 已指向 B，A 已交给 QSBR 回收流程
     *        → 记录安装耗时 → 标记 pending 已发布 → 解锁 → 返回成功
     * 失败时 B 从未交给 worker，可直接销毁；成功时 B 已由 active 持有，不能在此销毁
     */
    int publish_error = publish_locked(backend, next);
    if (publish_error != 0) {
        snapshot_destroy(next, backend->capacity);
        (void)pthread_mutex_unlock(&backend->writer_lock);
        return publish_error;
    }
    /**
     * 成功发布后完成计时，耗时包括副本分配、指针发布和当次可完成的退役回收
     * 查询同样持有 writer_lock，所以读不到尚未填完的记录，逐包匹配不读取这些字段
     */
    metrics->timing_available = dppd_install_timer_finish(&timer, &metrics->install_duration_ns);
    metrics->commit_rule_count = 1;
    /** 历史在已有控制面锁内更新，零纳秒是有效测量，时钟未知单独计数 */
    dppd_rule_latency_record(&backend->latency_single, metrics->timing_available, metrics->install_duration_ns, 1);
    /**
     * published 记录本条规则已经安装，供后续回滚判断是否需要删除该规则版本
     * 真正发布发生在前面的 active 指针交换，此标志不负责让 worker 切换规则
     * pending 留给事务的 finalize 或 rollback 释放，本函数只解锁并返回
     */
    pending->published = true;
    (void)pthread_mutex_unlock(&backend->writer_lock);
    return 0;
}

/**
 * 撤销一条软件规则的安装，token 是 prepare 创建的 pending 地址，返回删除结果或参数错误
 * pending->published
 *   false → 规则未发布 → 只释放 pending → 返回 0
 *   true  → 按 id + generation 删除已发布版本 → 释放 pending → 返回删除结果
 * commit_was_attempted 只说明调用过 commit，不能说明发布成功，本后端以 published 为准
 * 删除也要创建并发布新快照，可能因内存不足失败；即使删除失败也会释放 pending
 * 上层须检查返回值，不能把准备记录已释放当作规则已撤回
 */
static int transaction_rollback(void *context, const struct dppd_transaction_item *item, uintptr_t token,
    bool commit_was_attempted)
{
    struct dppd_software_backend *backend = context;
    struct dppd_software_pending_rule *pending = (struct dppd_software_pending_rule *)token;
    int rc = 0;

    (void)item;
    (void)commit_was_attempted;
    if (backend == NULL || pending == NULL)
        return -EINVAL;
    /** commit 尚未发布时只需释放 token；已发布时删除同一 ID+generation 的精确版本 */
    if (pending->published)
        rc = dppd_software_backend_remove_version(backend, pending->rule.id, pending->rule.generation);
    free(pending);
    return rc;
}

/**
 * 释放事务准备记录，不修改已经发布的规则，也不释放规则的计数器
 * prepare 创建 pending → commit 将规则内容复制进快照 → finalize 释放 pending
 * 此后 active → 已发布快照 → 规则和计数器仍在，worker 继续使用
 * 上层 dppd_transaction_finalize 随后清零条目 token，并将事务标记为 FINALIZED
 * 正常提交完成后使用此入口；进入隔离并放弃继续回滚时也可用它清理准备记录
 * 因此调用 finalize 本身不能作为业务事务成功的判断依据
 */
static void transaction_finalize(void *context, const struct dppd_transaction_item *item, uintptr_t token)
{
    (void)context;
    (void)item;
    free((struct dppd_software_pending_rule *)token);
}

/**
 * 返回软件后端的事务回调表，context 保存后端地址，供上层调用各阶段函数时传回
 * transaction.c：backend->commit(context, item, token) → 本文件 transaction_commit
 * 同样通过 prepare、rollback、finalize 回调完成准备、撤销和临时资源清理
 */
struct dppd_transaction_backend dppd_software_transaction_backend(struct dppd_software_backend *backend)
{
    const struct dppd_transaction_backend operations = {
        /**
         * .成员名 = 初始值 是 C 的指定初始化语法，左侧名称来自 dppd_transaction_backend 的定义
         * .context = backend          → 将后端对象指针存入 operations.context
         * .commit = transaction_commit → 将函数地址存入 operations.commit，此处不会执行函数
         * 调用 operations.commit(...) 时，才会执行 transaction_commit(...)
         * 按成员名指定初值，无须依照结构体成员顺序；未指定的成员按零初始化规则处理
         */
        .context = backend,
        .validate = transaction_validate,
        .prepare = transaction_prepare,
        .commit = transaction_commit,
        .rollback = transaction_rollback,
        .finalize = transaction_finalize,
    };
    return operations;
}

/**
 * 整批替换在私有副本内完成，规则槽位复用，未更新规则保留原计数器
 * 全部检查和分配成功后仅切换一次指针，失败时丢弃副本而不改变活跃表
 */
int dppd_software_backend_update_batch(struct dppd_software_backend *backend, const struct dppd_rule *rules,
    const uint64_t *expected_generations, uint32_t count)
{
    struct dppd_software_classifier_snapshot *active, *next = NULL;
    uint32_t i;
    int rc = 0;
    struct dppd_install_timer timer;

    if (backend == NULL || !backend->lock_initialized || rules == NULL || expected_generations == NULL || count == 0 ||
        count > backend->capacity)
        return -EINVAL;
    (void)pthread_mutex_lock(&backend->writer_lock);
    active = atomic_load_explicit(&backend->active, memory_order_acquire);
    for (i = 0; i < count; ++i) {
        char error[128];
        uint32_t j;

        if (rules[i].id == 0 || rules[i].generation == 0 || rules[i].generation == UINT64_MAX ||
            dppd_rule_validate(&rules[i], error, sizeof(error)) != 0) {
            rc = -EINVAL;
            goto out;
        }
        if (!supported(backend, &rules[i])) {
            rc = -ENOTSUP;
            goto out;
        }
        for (j = 0; j < i; ++j) {
            if (rules[j].id == rules[i].id) {
                rc = -EEXIST;
                goto out;
            }
        }
        if (expected_generations[i] == 0 || expected_generations[i] == UINT64_MAX ||
            rules[i].generation <= expected_generations[i] ||
            snapshot_find(active, backend->capacity, rules[i].id, expected_generations[i]) < 0) {
            rc = -ESTALE;
            goto out;
        }
        if (snapshot_find(active, backend->capacity, rules[i].id, rules[i].generation) >= 0) {
            rc = -EEXIST;
            goto out;
        }
    }
    /** 全批条件检查结束才开始计时，不把无效请求的检查耗时记录为安装耗时 */
    dppd_install_timer_start(&timer);
    next = snapshot_clone(active, backend->capacity);
    if (next == NULL) {
        rc = -ENOMEM;
        goto out;
    }
    for (i = 0; i < count; ++i) {
        int index = snapshot_find(next, backend->capacity, rules[i].id, expected_generations[i]);
        struct dppd_software_rule_metrics *metrics = calloc(1, sizeof(*metrics));

        if (metrics == NULL) {
            rc = -ENOMEM;
            goto out;
        }
        atomic_init(&metrics->references, 1);
        atomic_init(&metrics->hits, 0);
        atomic_init(&metrics->bytes, 0);
        metrics_put(next->objects[index].metrics);
        next->objects[index].rule = rules[i];
        next->objects[index].metrics = metrics;
    }
    rc = publish_locked(backend, next);
    if (rc == 0) {
        uint64_t duration_ns;
        bool timing_available = dppd_install_timer_finish(&timer, &duration_ns);

        /** 整批只有一次指针发布，因此只增加一个样本，同时记下这一批涉及的规则数 */
        dppd_rule_latency_record(&backend->latency_batch, timing_available, duration_ns, count);

        /**
         * 一次发布同时替换整个批次，因此各新版本共享整批耗时，不能伪装成各自的单条耗时
         * 这里只修改新分配的记录，未更新版本仍共享原来的记录，失败路径完全不改旧表
         */
        for (i = 0; i < count; ++i) {
            int index = snapshot_find(next, backend->capacity, rules[i].id, rules[i].generation);
            struct dppd_software_rule_metrics *metrics = next->objects[index].metrics;

            metrics->install_duration_ns = duration_ns;
            metrics->timing_available = timing_available;
            metrics->commit_rule_count = count;
        }
        next = NULL;
    }
out:
    snapshot_destroy(next, backend->capacity);
    (void)pthread_mutex_unlock(&backend->writer_lock);
    return rc;
}

/** 先修改私有副本中的整个批次，任何分配或编译失败都不会改变活跃规则及计数 */
int dppd_software_backend_change_batch(struct dppd_software_backend *backend,
    const struct dppd_software_change *changes, uint32_t count)
{
    struct dppd_software_classifier_snapshot *active, *next = NULL;
    struct dppd_install_timer timer;
    uint64_t final_count;
    int slots[DPPD_SOFTWARE_CHANGE_BATCH_MAX];
    uint32_t i;
    int rc = 0;

    if (backend == NULL || !backend->lock_initialized || changes == NULL || count == 0 ||
        count > DPPD_SOFTWARE_CHANGE_BATCH_MAX)
        return -EINVAL;
    pthread_mutex_lock(&backend->writer_lock);
    active = atomic_load_explicit(&backend->active, memory_order_acquire);
    final_count = active->count;
    for (i = 0; i < count; ++i) {
        const struct dppd_software_change *change = &changes[i];
        const struct dppd_rule *rule = change->replacement;
        char error[128];
        unsigned int found = 0;
        slots[i] = -1;
        if (change->id == 0 || change->id == UINT64_MAX || change->expected_generation == UINT64_MAX ||
            (rule == NULL && change->expected_generation == 0)) {
            rc = -EINVAL;
            goto done;
        }
        for (uint32_t previous = 0; previous < i; ++previous)
            if (changes[previous].id == change->id) {
                rc = -EEXIST;
                goto done;
            }
        for (uint32_t slot = 0; slot < backend->capacity; ++slot)
            if (active->objects[slot].metrics != NULL && active->objects[slot].rule.id == change->id) {
                ++found;
                slots[i] = (int)slot;
            }
        if (found > 1) {
            rc = -EUCLEAN;
            goto done;
        }
        if (change->expected_generation == 0 && found != 0) {
            rc = -EEXIST;
            goto done;
        }
        if (change->expected_generation != 0 &&
            (found == 0 || active->objects[slots[i]].rule.generation != change->expected_generation)) {
            rc = -ESTALE;
            goto done;
        }
        if (rule != NULL) {
            if (rule->id != change->id || rule->generation == UINT64_MAX ||
                rule->generation <= change->expected_generation ||
                dppd_rule_validate(rule, error, sizeof(error)) != 0) {
                rc = -EINVAL;
                goto done;
            }
            if (!supported(backend, rule)) {
                rc = -ENOTSUP;
                goto done;
            }
        }
        if (change->expected_generation == 0)
            ++final_count;
        else if (rule == NULL)
            --final_count;
    }
    if (final_count > backend->capacity) {
        rc = -ENOSPC;
        goto done;
    }
    dppd_install_timer_start(&timer);
    next = snapshot_clone(active, backend->capacity);
    if (next == NULL) {
        rc = -ENOMEM;
        goto done;
    }
    /** 先在副本清除所有被替换或删除的槽位，满表也能用本批腾出的槽位创建其他 ID */
    for (i = 0; i < count; ++i)
        if (slots[i] >= 0) {
            struct dppd_software_rule_object *object = &next->objects[slots[i]];
            metrics_put(object->metrics);
            memset(object, 0, sizeof(*object));
            --next->count;
        }
    for (i = 0; i < count; ++i) {
        struct dppd_software_rule_metrics *metrics;
        if (changes[i].replacement == NULL)
            continue;
        metrics = calloc(1, sizeof(*metrics));
        if (metrics == NULL) {
            rc = -ENOMEM;
            goto done;
        }
        atomic_init(&metrics->references, 1);
        atomic_init(&metrics->hits, 0);
        atomic_init(&metrics->bytes, 0);
        for (uint32_t slot = 0; slot < backend->capacity; ++slot)
            if (next->objects[slot].metrics == NULL) {
                next->objects[slot].rule = *changes[i].replacement;
                next->objects[slot].metrics = metrics;
                slots[i] = (int)slot;
                ++next->count;
                break;
            }
    }
    rc = publish_locked(backend, next);
    if (rc == 0) {
        uint64_t duration = 0;
        bool available = dppd_install_timer_finish(&timer, &duration);
        dppd_rule_latency_record(&backend->latency_batch, available, duration, count);
        for (i = 0; i < count; ++i)
            if (changes[i].replacement != NULL) {
                struct dppd_software_rule_metrics *metrics = next->objects[slots[i]].metrics;
                metrics->install_duration_ns = duration;
                metrics->timing_available = available;
                metrics->commit_rule_count = count;
            }
        next = NULL;
    }
done:
    snapshot_destroy(next, backend->capacity);
    pthread_mutex_unlock(&backend->writer_lock);
    return rc;
}

/** 恢复先准备完整副本，退出先准备空副本，都只编译并发布一次，避免按小批重复编译大表 */
static int replace_snapshot(struct dppd_software_backend *backend, const struct dppd_rule *rules, uint32_t count,
    bool require_empty)
{
    struct dppd_software_classifier_snapshot *next = NULL;
    struct dppd_install_timer timer;
    int rc = 0;
    if (backend == NULL || !backend->lock_initialized || count > backend->capacity || (count != 0 && rules == NULL))
        return -EINVAL;
    pthread_mutex_lock(&backend->writer_lock);
    struct dppd_software_classifier_snapshot *active = atomic_load_explicit(&backend->active, memory_order_acquire);
    if (require_empty && active->count != 0) {
        rc = -EBUSY;
        goto done;
    }
    if (count == 0 && active->count == 0)
        goto done;
    for (uint32_t i = 0; i < count; ++i) {
        char error[128];
        if (rules[i].id == 0 || rules[i].id == UINT64_MAX || rules[i].generation == 0 ||
            rules[i].generation == UINT64_MAX || (i != 0 && rules[i - 1U].id >= rules[i].id) ||
            dppd_rule_validate(&rules[i], error, sizeof(error)) != 0) {
            rc = -EINVAL;
            goto done;
        }
        if (!supported(backend, &rules[i])) {
            rc = -ENOTSUP;
            goto done;
        }
    }
    dppd_install_timer_start(&timer);
    next = snapshot_clone(NULL, backend->capacity);
    if (next == NULL) {
        rc = -ENOMEM;
        goto done;
    }
    for (uint32_t i = 0; i < count; ++i) {
        struct dppd_software_rule_metrics *metrics = calloc(1, sizeof(*metrics));
        if (metrics == NULL) {
            rc = -ENOMEM;
            goto done;
        }
        atomic_init(&metrics->references, 1);
        atomic_init(&metrics->hits, 0);
        atomic_init(&metrics->bytes, 0);
        next->objects[i] = (struct dppd_software_rule_object){rules[i], metrics};
        ++next->count;
    }
    rc = publish_locked(backend, next);
    if (rc == 0) {
        uint64_t duration = 0;
        bool available = dppd_install_timer_finish(&timer, &duration);
        if (count != 0) {
            dppd_rule_latency_record(&backend->latency_batch, available, duration, count);
            for (uint32_t i = 0; i < count; ++i) {
                next->objects[i].metrics->install_duration_ns = duration;
                next->objects[i].metrics->timing_available = available;
                next->objects[i].metrics->commit_rule_count = count;
            }
        }
        next = NULL;
    }
done:
    snapshot_destroy(next, backend->capacity);
    pthread_mutex_unlock(&backend->writer_lock);
    return rc;
}

int dppd_software_backend_restore(struct dppd_software_backend *backend, const struct dppd_rule *rules, uint32_t count)
{
    return replace_snapshot(backend, rules, count, true);
}

int dppd_software_backend_clear(struct dppd_software_backend *backend)
{
    return replace_snapshot(backend, NULL, 0, false);
}

/** 只复制历史计数，和已有控制面写者串行，逐包读者无需参与此锁 */
void dppd_software_backend_latency(const struct dppd_software_backend *backend,
    struct dppd_rule_latency_histogram *single, struct dppd_rule_latency_histogram *batch)
{
    if (single == NULL || batch == NULL)
        return;
    memset(single, 0, sizeof(*single));
    memset(batch, 0, sizeof(*batch));
    if (backend == NULL || !backend->lock_initialized)
        return;
    (void)pthread_mutex_lock((pthread_mutex_t *)&backend->writer_lock);
    *single = backend->latency_single;
    *batch = backend->latency_batch;
    (void)pthread_mutex_unlock((pthread_mutex_t *)&backend->writer_lock);
}

/**
 * 按 rule_id 和 generation 删除一个规则版本，返回 0 表示新快照发布成功，负值表示失败
 * backend 提供当前快照和写锁；版本必须同时匹配，避免删除同 ID 的其他版本
 *
 * 例：当前 B 含 R1、R2，本次撤销 R2
 * B：[R1, R2] → 复制出 C：[R1, R2] → 清空 C 中 R2 的槽位并归还其计数器引用
 * 发布成功：active → C：[R1]，旧 B 交给 QSBR，已有读者仍可完成对 B 的访问
 * 发布失败：销毁 C，active 仍指向 B：[R1, R2]，调用方收到删除错误
 * 回滚通过发布新版本撤销规则，不把 active 直接切回某个历史快照
 */
int dppd_software_backend_remove_version(struct dppd_software_backend *backend, uint64_t rule_id, uint64_t generation)
{
    struct dppd_software_classifier_snapshot *active;
    struct dppd_software_classifier_snapshot *next;
    int index;

    if (backend == NULL || rule_id == 0 || generation == 0)
        return -EINVAL;
    (void)pthread_mutex_lock(&backend->writer_lock);
    active = atomic_load_explicit(&backend->active, memory_order_acquire);
    index = snapshot_find(active, backend->capacity, rule_id, generation);
    if (index < 0) {
        (void)pthread_mutex_unlock(&backend->writer_lock);
        return -ENOENT;
    }
    /** 删除也经由新 snapshot 发布；旧 snapshot 仍保留被删除规则直到 QSBR 放行 */
    next = snapshot_clone(active, backend->capacity);
    if (next == NULL) {
        (void)pthread_mutex_unlock(&backend->writer_lock);
        return -ENOMEM;
    }
    /** 只放弃副本持有的计数器引用，旧快照仍持有自己的引用，不能在此强行释放 */
    metrics_put(next->objects[index].metrics);
    memset(&next->objects[index], 0, sizeof(next->objects[index]));
    next->count--;
    int publish_error = publish_locked(backend, next);
    if (publish_error != 0) {
        snapshot_destroy(next, backend->capacity);
        (void)pthread_mutex_unlock(&backend->writer_lock);
        return publish_error;
    }
    (void)pthread_mutex_unlock(&backend->writer_lock);
    return 0;
}

/** 只回收已经没有旧读者的快照，配置删除共享对象前需要归还这些快照的借用 */
void dppd_software_backend_reclaim(struct dppd_software_backend *backend)
{
    if (backend == NULL || !backend->lock_initialized)
        return;
    pthread_mutex_lock(&backend->writer_lock);
    reclaim_locked(backend);
    pthread_mutex_unlock(&backend->writer_lock);
}

/** 返回活跃规则数，退役快照不占新的逻辑容量，管理侧查询不会给逐包路径加锁 */
uint32_t dppd_software_backend_count(const struct dppd_software_backend *backend)
{
    struct dppd_software_classifier_snapshot *active;
    uint32_t count;

    if (backend == NULL || !backend->lock_initialized)
        return 0;
    (void)pthread_mutex_lock((pthread_mutex_t *)&backend->writer_lock);
    active = atomic_load_explicit(&backend->active, memory_order_acquire);
    count = active->count;
    (void)pthread_mutex_unlock((pthread_mutex_t *)&backend->writer_lock);
    return count;
}

/** 查询活跃表是否包含精确版本，并顺便回收已安全退役的快照，不把退役表视为当前规则 */
bool dppd_software_backend_contains_version(const struct dppd_software_backend *backend, uint64_t rule_id,
    uint64_t generation)
{
    struct dppd_software_classifier_snapshot *active;
    bool found;

    if (backend == NULL || rule_id == 0 || generation == 0)
        return false;
    (void)pthread_mutex_lock((pthread_mutex_t *)&backend->writer_lock);
    active = atomic_load_explicit(&backend->active, memory_order_acquire);
    found = snapshot_find(active, backend->capacity, rule_id, generation) >= 0;
    reclaim_locked((struct dppd_software_backend *)backend);
    (void)pthread_mutex_unlock((pthread_mutex_t *)&backend->writer_lock);
    return found;
}

/**
 * 读取活跃版本的规则字段和提交耗时，不清零计数，也不推进退役快照回收
 * 持锁期间快照不会被另一个控制请求替换并释放，退出前只复制不含指针的结果
 */
int dppd_software_backend_install_info(const struct dppd_software_backend *backend, uint64_t rule_id,
    uint64_t generation, struct dppd_rule_install_info *info)
{
    const struct dppd_software_classifier_snapshot *active;
    const struct dppd_software_rule_object *object;
    uint16_t action;
    int index;

    if (backend == NULL || !backend->lock_initialized || rule_id == 0 || generation == 0 || info == NULL)
        return -EINVAL;
    memset(info, 0, sizeof(*info));
    (void)pthread_mutex_lock((pthread_mutex_t *)&backend->writer_lock);
    active = atomic_load_explicit(&backend->active, memory_order_acquire);
    index = snapshot_find(active, backend->capacity, rule_id, generation);
    if (index < 0) {
        (void)pthread_mutex_unlock((pthread_mutex_t *)&backend->writer_lock);
        return -ENOENT;
    }
    object = &active->objects[index];
    info->rule_id = object->rule.id;
    info->generation = object->rule.generation;
    info->install_port_id = object->rule.install_port_id;
    info->install_duration_ns = object->metrics->install_duration_ns;
    info->timing_available = object->metrics->timing_available;
    info->commit_rule_count = object->metrics->commit_rule_count;
    for (action = 0; action < object->rule.nb_actions; ++action) {
        if (object->rule.actions[action].type == DPPD_ACTION_COUNT)
            info->has_count = true;
    }
    (void)pthread_mutex_unlock((pthread_mutex_t *)&backend->writer_lock);
    return 0;
}

/**
 * 查询指定版本的 COUNT，不清零统计；版本不存在与规则没有 COUNT 动作分别返回错误
 * 控制面锁保护快照的存活时间，工作线程仍可以同时累计计数
 * hits 和 bytes 分别读取，因此繁忙时两者不保证对应完全相同的采样瞬间
 */
int dppd_software_backend_query_count(const struct dppd_software_backend *backend, uint64_t rule_id,
    uint64_t generation, uint64_t *hits, uint64_t *bytes)
{
    struct dppd_software_classifier_snapshot *active;
    int index;
    uint16_t action;

    if (backend == NULL || hits == NULL || bytes == NULL)
        return -EINVAL;
    (void)pthread_mutex_lock((pthread_mutex_t *)&backend->writer_lock);
    active = atomic_load_explicit(&backend->active, memory_order_acquire);
    index = snapshot_find(active, backend->capacity, rule_id, generation);
    if (index < 0) {
        reclaim_locked((struct dppd_software_backend *)backend);
        (void)pthread_mutex_unlock((pthread_mutex_t *)&backend->writer_lock);
        return -ENOENT;
    }
    for (action = 0; action < active->objects[index].rule.nb_actions; ++action) {
        if (active->objects[index].rule.actions[action].type == DPPD_ACTION_COUNT) {
            *hits = atomic_load_explicit(&active->objects[index].metrics->hits, memory_order_relaxed);
            *bytes = atomic_load_explicit(&active->objects[index].metrics->bytes, memory_order_relaxed);
            reclaim_locked((struct dppd_software_backend *)backend);
            (void)pthread_mutex_unlock((pthread_mutex_t *)&backend->writer_lock);
            return 0;
        }
    }
    reclaim_locked((struct dppd_software_backend *)backend);
    (void)pthread_mutex_unlock((pthread_mutex_t *)&backend->writer_lock);
    return -ENODATA;
}

int dppd_software_backend_worker_register(struct dppd_software_backend *backend, unsigned int worker_id)
{
    int rc;

    /** worker_id 必须稳定且唯一；同一 reader 重复注册会破坏 QSBR 进度判断 */
    if (backend == NULL || !backend->qsbr_initialized || worker_id >= DPPD_MAX_WORKERS)
        return -EINVAL;
    rc = rte_rcu_qsbr_thread_register(backend->qsbr, worker_id);
    if (rc != 0)
        return rc;
    rte_rcu_qsbr_thread_online(backend->qsbr, worker_id);
    return 0;
}

/**
 * 工作线程确认不再使用先前读取的规则指针后报告安全点，允许控制面推进回收
 * 此函数只报告进度，不直接释放快照；即使本轮没有收到报文也应报告
 * backend->qsbr 跟踪本后端的读者，worker_id 指定本次报告者；完整 A/B 示例见 software_backend.h
 */
void dppd_software_backend_worker_quiescent(struct dppd_software_backend *backend, unsigned int worker_id)
{
    if (backend != NULL && backend->qsbr_initialized && worker_id < DPPD_MAX_WORKERS)
        rte_rcu_qsbr_quiescent(backend->qsbr, worker_id);
}

/** 先下线再注销，使回收器不再等待已退出的线程；调用前必须停止使用所有快照指针 */
void dppd_software_backend_worker_unregister(struct dppd_software_backend *backend, unsigned int worker_id)
{
    if (backend == NULL || !backend->qsbr_initialized || worker_id >= DPPD_MAX_WORKERS)
        return;
    rte_rcu_qsbr_thread_offline(backend->qsbr, worker_id);
    (void)rte_rcu_qsbr_thread_unregister(backend->qsbr, worker_id);
}

/**
 * 比较候选规则优先顺序，返回 true 表示 a 排在 b 前面
 * group → priority → id → tag：前项相等才比较下一项，数值小的优先
 * 精确规则与 ACL 规则使用同一排序，匹配方式不决定优先级
 */
static bool rule_before(const struct dppd_rule *a, uint32_t a_tag, const struct dppd_rule *b, uint32_t b_tag)
{
    if (a->group != b->group)
        return a->group < b->group;
    if (a->priority != b->priority)
        return a->priority < b->priority;
    if (a->id != b->id)
        return a->id < b->id;
    return a_tag < b_tag;
}

/** 将批次中的次数和字节增量原子加到各规则统计，清空有效项数供后续复用 */
void dppd_software_count_batch_flush(struct dppd_software_count_batch *batch)
{
    if (batch == NULL)
        return;
    for (uint16_t i = 0; i < batch->count; ++i) {
        atomic_fetch_add_explicit(&batch->entries[i].metrics->hits, batch->entries[i].hits, memory_order_relaxed);
        atomic_fetch_add_explicit(&batch->entries[i].metrics->bytes, batch->entries[i].bytes, memory_order_relaxed);
    }
    batch->count = 0;
}

/**
 * 记录一次规则命中和本包字节数，无返回值
 * batch 为空时直接原子累加，否则按 metrics 对象合并到批次，减少共享统计写入
 * metrics 指针区分规则版本，避免把相同 ID 的新旧版本统计混在一起
 * 示例：同一对象先后命中 60 和 100 字节 → 有效项 1，hits 为 2，bytes 为 160
 */
static void count_packet(struct dppd_software_rule_metrics *metrics, uint32_t bytes,
    struct dppd_software_count_batch *batch)
{
    if (batch == NULL) {
        atomic_fetch_add_explicit(&metrics->hits, 1, memory_order_relaxed);
        atomic_fetch_add_explicit(&metrics->bytes, bytes, memory_order_relaxed);
        return;
    }
    /** count 是批次内有效统计对象数，同一对象重复命中只增加该项的次数和字节数 */
    for (uint16_t i = 0; i < batch->count; ++i)
        if (batch->entries[i].metrics == metrics) {
            batch->entries[i].hits++;
            batch->entries[i].bytes += bytes;
            return;
        }
    /** 新增对象前若没有空位则先提交，32 是对象容量，不要求凑满 32 个包 */
    if (batch->count == DPPD_SOFTWARE_COUNT_BATCH_SIZE)
        dppd_software_count_batch_flush(batch);
    batch->entries[batch->count].metrics = metrics;
    batch->entries[batch->count].hits = 1;
    batch->entries[batch->count++].bytes = bytes;
}

/**
 * 匹配当前报文，生成软件规则结果并按需累计 COUNT 统计
 * 读取规则表 → ACL/精确匹配 → 选定一条规则
 *   未命中 → matched = false，返回
 *     命中 → DROP/MARK 写入 decision，COUNT 累计命中次数和字节数
 */
void dppd_software_backend_decide_counted(struct dppd_software_backend *backend, uint16_t ingress_port,
    const struct dppd_packet *packet, struct dppd_software_decision *decision, struct dppd_software_count_batch *batch)
{
    struct dppd_software_classifier_snapshot *active;
    struct dppd_software_rule_object *selected = NULL;
    memset(decision, 0, sizeof(*decision));
    if (backend == NULL || packet == NULL)
        return;
    /**
     * backend->active → 本包的规则快照 active → ACL/精确匹配各自返回候选，0 表示未命中
     *
     * 读到这里可继续追查：active 中的规则是谁提前准备并发布进去的
     * 报文处理：process_batch → routed_decide → pipeline → 本函数读取 active，匹配当前包
     * 规则配置：control.c/dppd_control_apply → control_apply_impl → transaction_run
     *           → 软件 transaction_commit → publish_locked 构建匹配表并更新 backend->active
     * 两条流程通过同一个 backend->active 衔接；转去读配置入口是在追查规则来源，并非本函数调用它
     * 回到本函数后，继续用取得的同一快照查询下面的 ACL 和 exact 表
     *
     * 每包只取一次 active，两类匹配共用同一版本，不获取控制面写锁
     * 调用线程须已注册上线 QSBR，防止读取期间旧规则表被回收
     * acquire 与发布端 release 配对，保证读到新指针时规则内容也已可见
     */
    active = atomic_load_explicit(&backend->active, memory_order_acquire);
    /**
     * ACL 支持掩码、端口范围等条件；exact 对完整地址和端口组合做哈希查找
     * 各自返回本表优先命中的规则 tag，0 表示未命中，选定候选后再执行动作
     */
    uint32_t tag = dppd_acl_classify(snapshot_acl(active, backend->capacity), ingress_port, packet);
    uint32_t exact_tag = dppd_exact_classify(snapshot_exact(active, backend->capacity), ingress_port, packet);
    /**
     * ACL tag + exact_tag → 选定一个 tag
     * 单个命中 → 直接选用；两个都命中 → 按 rule_before 排序
     * tag = 0 → 无规则；tag > 0 → active->objects[tag - 1]，数组下标从 0 开始
     */
    if (exact_tag != 0 &&
        (tag == 0 || rule_before(&active->objects[exact_tag - 1].rule, exact_tag, &active->objects[tag - 1].rule, tag)))
        tag = exact_tag;
    if (tag != 0)
        selected = &active->objects[tag - 1];
    if (selected != NULL) {
        uint16_t action;

        /**
         * 只执行选中规则的动作，DROP/MARK 写入软件决策，COUNT 累计命中统计
         * R1 示例：[DROP, COUNT] → matched = true、drop = true，并累计本包次数和字节数
         * DROP 不退出循环，也不释放报文；释放由 worker.c/process_batch 按最终决策执行
         */
        decision->matched = true;
        for (action = 0; action < selected->rule.nb_actions; ++action) {
            const struct dppd_action *source = &selected->rule.actions[action];

            if (source->type == DPPD_ACTION_DROP)
                decision->drop = true;
            else if (source->type == DPPD_ACTION_MARK) {
                decision->has_mark = true;
                decision->mark_id = source->conf.mark_id;
            } else if (source->type == DPPD_ACTION_SHARED_COUNT && !packet->neighbor_retry) {
                atomic_fetch_add_explicit(&selected->metrics->shared->hits, 1, memory_order_relaxed);
                atomic_fetch_add_explicit(&selected->metrics->shared->bytes, packet->packet_len, memory_order_relaxed);
            } else if (source->type == DPPD_ACTION_COUNT && !packet->neighbor_retry)
                count_packet(selected->metrics, packet->packet_len, batch);
        }
    }
}

void dppd_software_backend_decide(struct dppd_software_backend *backend, uint16_t ingress_port,
    const struct dppd_packet *packet, struct dppd_software_decision *decision)
{
    dppd_software_backend_decide_counted(backend, ingress_port, packet, decision, NULL);
}
