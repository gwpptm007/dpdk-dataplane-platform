#include "dppd/software_backend.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <rte_common.h>
/** DPDK 21.11 的实验性 QSBR 头仍含 GNU 可变宏和零长数组声明。 */
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
};

struct dppd_software_rule_object {
    /** metrics 为 NULL 就表示空槽位，无须额外的 occupied 标志。 */
    struct dppd_rule rule;
    struct dppd_software_rule_metrics *metrics;
};

/**
 * 一次发布对应一个完整规则视图。objects 是按 backend capacity 固定分配的柔性数组；
 * count 仅用于容量判断，报文匹配仍遍历槽位并以 metrics != NULL 判定有效性。
 */
struct dppd_software_classifier_snapshot {
    uint32_t count;
    struct dppd_software_rule_object objects[];
};

struct dppd_software_retired_snapshot {
    /** token 在 active 指针切换后取得；check 成功才允许销毁 snapshot。 */
    struct dppd_software_classifier_snapshot *snapshot;
    uint64_t token;
    struct dppd_software_retired_snapshot *next;
};

struct dppd_software_pending_rule {
    /** prepare token 的所有权由 transaction 持有，rollback/finalize 二选一释放。 */
    struct dppd_rule rule;
    /** 仅 commit 完成发布后置位，决定 rollback 是否还要删除 classifier 中的版本。 */
    bool published;
};

/** IPv4 地址和 L4 端口均保持网络字节序；掩码比较不做字节序转换，避免双重转换。 */
static bool masked_equal_u32(uint32_t value, uint32_t expected, uint32_t mask)
{
    return (value & mask) == (expected & mask);
}

static bool masked_equal_u16(uint16_t value, uint16_t expected, uint16_t mask)
{
    return (value & mask) == (expected & mask);
}

static bool rule_matches_packet(const struct dppd_rule *rule,
                                uint16_t ingress_port,
                                const struct dppd_packet *packet)
{
    uint16_t i;

    /** 安装端口属于匹配域的一部分，不能因多个 port 复用规则 ID 而跨端口命中。 */
    if (rule->install_port_id != ingress_port)
        return false;
    for (i = 0; i < rule->nb_matches; ++i) {
        const struct dppd_match *match = &rule->matches[i];

        switch (match->type) {
        case DPPD_MATCH_ETH:
            /** 当前 IR 中 ETH 用来声明报文层次；没有 MAC 字段，因此无需额外比较。 */
            break;
        case DPPD_MATCH_IPV4:
            if (packet->l3_type != DPPD_L3_IPV4 ||
                !masked_equal_u32(packet->ipv4_src_be, match->spec.ipv4.src_be,
                                  match->spec.ipv4.src_mask_be) ||
                !masked_equal_u32(packet->ipv4_dst_be, match->spec.ipv4.dst_be,
                                  match->spec.ipv4.dst_mask_be))
                return false;
            break;
        case DPPD_MATCH_UDP:
        case DPPD_MATCH_TCP:
            if ((match->type == DPPD_MATCH_UDP && packet->l4_type != DPPD_L4_UDP) ||
                (match->type == DPPD_MATCH_TCP && packet->l4_type != DPPD_L4_TCP) ||
                !masked_equal_u16(packet->l4_src_port_be, match->spec.l4.src_be,
                                  match->spec.l4.src_mask_be) ||
                !masked_equal_u16(packet->l4_dst_port_be, match->spec.l4.dst_be,
                                  match->spec.l4.dst_mask_be))
                return false;
            break;
        case DPPD_MATCH_REPRESENTED_PORT:
        default:
            return false;
        }
    }
    return true;
}

static int rule_order(const struct dppd_rule *left, const struct dppd_rule *right)
{
    /**
     * 本软件分类器按 group、priority、ID 依次比较，数值较小的规则先执行
     * 这里只选出一条规则，不模拟硬件的多组跳转；generation 不参与排序
     */
    if (left->group != right->group)
        return left->group < right->group ? -1 : 1;
    if (left->priority != right->priority)
        return left->priority < right->priority ? -1 : 1;
    if (left->id == right->id)
        return 0;
    return left->id < right->id ? -1 : 1;
}

static size_t snapshot_size(uint32_t capacity)
{
    return sizeof(struct dppd_software_classifier_snapshot) +
           (size_t)capacity * sizeof(struct dppd_software_rule_object);
}

static void metrics_put(struct dppd_software_rule_metrics *metrics)
{
    /** 最后一个 snapshot 消失才释放统计对象，确保 retired reader 仍可安全累加 COUNT。 */
    if (metrics != NULL &&
        atomic_fetch_sub_explicit(&metrics->references, 1, memory_order_acq_rel) == 1)
        free(metrics);
}

static void snapshot_destroy(struct dppd_software_classifier_snapshot *snapshot,
                             uint32_t capacity)
{
    uint32_t i;

    /**
     * 未发布的私有副本可以直接销毁；已经发布过的快照必须先确认没有读者使用
     * 后者由 QSBR 安全点保证，或由关闭流程等待所有工作线程退出后保证
     */
    if (snapshot == NULL)
        return;
    for (i = 0; i < capacity; ++i)
        metrics_put(snapshot->objects[i].metrics);
    free(snapshot);
}

/**
 * 复制规则数组，并为其中保留的规则版本增加计数器引用，避免复制操作清空统计
 * source 为空时创建空表；否则只复制规则内容，计数器由新旧快照共同持有
 * 返回的副本仍归控制面独占，发布成功之后才允许工作线程读取
 */
static struct dppd_software_classifier_snapshot *snapshot_clone(
    const struct dppd_software_classifier_snapshot *source, uint32_t capacity)
{
    struct dppd_software_classifier_snapshot *copy;
    uint32_t i;

    copy = calloc(1, snapshot_size(capacity));
    if (copy == NULL)
        return NULL;
    if (source == NULL)
        return copy;
    copy->count = source->count;
    for (i = 0; i < capacity; ++i) {
        copy->objects[i] = source->objects[i];
        if (copy->objects[i].metrics != NULL)
            atomic_fetch_add_explicit(&copy->objects[i].metrics->references, 1,
                                      memory_order_relaxed);
    }
    return copy;
}

static int snapshot_find(const struct dppd_software_classifier_snapshot *snapshot,
                         uint32_t capacity, uint64_t rule_id, uint64_t generation)
{
    uint32_t i;

    /** generation 与 ID 一起定位，防止旧事务误删同 ID 的新版本。 */
    for (i = 0; i < capacity; ++i) {
        if (snapshot->objects[i].metrics != NULL && snapshot->objects[i].rule.id == rule_id &&
            snapshot->objects[i].rule.generation == generation)
            return (int)i;
    }
    return -1;
}

/**
 * 尝试回收已经被新 snapshot 替换的旧数组。
 *
 * 本函数绝不等待 worker；若任一 reader 尚未经过对应 QSBR token，就保留旧数组到
 * 下一次控制面操作再检查。这保证规则更新的管理请求不会被慢速数据面卡住。
 */
static void reclaim_locked(struct dppd_software_backend *backend)
{
    /** 保存指向当前链表节点的指针地址，删除首节点和中间节点就能使用同一种写法 */
    struct dppd_software_retired_snapshot **link = &backend->retired;

    while (*link != NULL) {
        struct dppd_software_retired_snapshot *retired = *link;

        /** 控制面不等待 worker；下次规则更新或查询时再尝试回收即可。 */
        if (rte_rcu_qsbr_check(backend->qsbr, retired->token, false) == 0) {
            link = &retired->next;
            continue;
        }
        *link = retired->next;
        snapshot_destroy(retired->snapshot, backend->capacity);
        free(retired);
    }
}

/**
 * 原子切换活跃 classifier，并将旧数组挂入 QSBR 等待队列。
 * 调用者持有 writer_lock；发布顺序遵循“先改数据，后推进 token”，使读侧看到
 * 新指针或在静默点之后安全离开旧指针，二者不会交叉造成悬空访问。
 */
static int publish_locked(struct dppd_software_backend *backend,
                          struct dppd_software_classifier_snapshot *next)
{
    struct dppd_software_classifier_snapshot *previous;
    struct dppd_software_retired_snapshot *retired;

    retired = calloc(1, sizeof(*retired));
    if (retired == NULL) {
        /** 在发布前保证能够记录旧 snapshot，避免内存紧张时丢失回收所有权。 */
        return -ENOMEM;
    }
    previous = atomic_exchange_explicit(&backend->active, next, memory_order_release);
    if (previous == NULL) {
        free(retired);
        return 0;
    }
    retired->snapshot = previous;
    retired->token = rte_rcu_qsbr_start(backend->qsbr);
    retired->next = backend->retired;
    backend->retired = retired;
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
        if (rule->matches[i].type == DPPD_MATCH_REPRESENTED_PORT)
            return false;
    }
    for (i = 0; i < rule->nb_actions; ++i) {
        switch (rule->actions[i].type) {
        case DPPD_ACTION_DROP:
        case DPPD_ACTION_MARK:
        case DPPD_ACTION_COUNT:
            break;
        case DPPD_ACTION_QUEUE:
        case DPPD_ACTION_REPRESENTED_PORT:
        default:
            return false;
        }
    }
    return true;
}

int dppd_software_backend_init(struct dppd_software_backend *backend,
                               uint32_t capacity)
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
    /** reader 上限与 runtime 的 queue/worker 编号空间一致，避免动态编号造成越界。 */
    qsbr_size = rte_rcu_qsbr_get_memsize(DPPD_MAX_WORKERS);
    /**
     * QSBR 变量只要求 cache-line 对齐；这里刻意不用 rte_zmalloc，使纯控制面单测
     * 无需先启动 EAL。真实 dppd 进程仍在 EAL 初始化后正常使用同一块内存。
     */
    qsbr_size = (qsbr_size + RTE_CACHE_LINE_SIZE - 1U) &
                ~(size_t)(RTE_CACHE_LINE_SIZE - 1U);
    backend->qsbr = aligned_alloc(RTE_CACHE_LINE_SIZE, qsbr_size);
    if (backend->qsbr == NULL ||
        rte_rcu_qsbr_init(backend->qsbr, DPPD_MAX_WORKERS) != 0) {
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

    /** fini 不等待 QSBR；调用契约要求 worker 已全部停止，因此此时可直接释放 retired。 */
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
static int transaction_validate(void *context,
                                const struct dppd_transaction_item *item)
{
    struct dppd_software_backend *backend = context;

    if (backend == NULL || item == NULL ||
        item->plan.backend != DPPD_PLAN_BACKEND_SOFTWARE ||
        !dppd_software_backend_rule_supported(&item->rule))
        return -ENOTSUP;
    return 0;
}

/**
 * prepare 只分配可回滚的控制面 token，不让未提交规则暴露给 worker。锁内复查容量和
 * 版本冲突后立即解锁；真正发布留到 commit，以便同一事务的任一 prepare 失败时完全
 * 不改变数据面可见规则集。
 * 这里不实际预留规则槽位，多个 prepare 成功不代表它们一定能够全部提交
 * commit 仍需检查当时的容量，批量操作的整体容量预检由上层控制面负责
 */
static int transaction_prepare(void *context,
                               const struct dppd_transaction_item *item,
                               uintptr_t *token)
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
 * 在私有快照副本中加入一个精确版本，然后一次切换 active 指针让它可见
 * 本次切换只发布这一条规则的安装结果，不代表整个跨规则事务同时对报文生效
 */
static int transaction_commit(void *context,
                              const struct dppd_transaction_item *item,
                              uintptr_t token)
{
    struct dppd_software_backend *backend = context;
    struct dppd_software_pending_rule *pending =
        (struct dppd_software_pending_rule *)token;
    struct dppd_software_classifier_snapshot *active;
    struct dppd_software_classifier_snapshot *next;
    struct dppd_software_rule_metrics *metrics;
    uint32_t i;
    struct dppd_install_timer timer;

    /** commit 必须再次检查：prepare 与 commit 之间可能已有另一控制请求完成发布。 */
    (void)item;
    if (backend == NULL || pending == NULL)
        return -EINVAL;
    (void)pthread_mutex_lock(&backend->writer_lock);
    active = atomic_load_explicit(&backend->active, memory_order_acquire);
    if (snapshot_find(active, backend->capacity, pending->rule.id,
                      pending->rule.generation) >= 0 ||
        active->count >= backend->capacity) {
        (void)pthread_mutex_unlock(&backend->writer_lock);
        return -EEXIST;
    }
    dppd_install_timer_start(&timer);
    /** 永不修改 active；即使只有一条规则变更也复制整张表，换取 worker 无锁读取。 */
    next = snapshot_clone(active, backend->capacity);
    if (next == NULL) {
        (void)pthread_mutex_unlock(&backend->writer_lock);
        return -ENOMEM;
    }
    /** 新安装版本使用独立计数器，从零开始累计，不继承同 ID 旧版本的计数 */
    metrics = calloc(1, sizeof(*metrics));
    if (metrics == NULL) {
        snapshot_destroy(next, backend->capacity);
        (void)pthread_mutex_unlock(&backend->writer_lock);
        return -ENOMEM;
    }
    atomic_init(&metrics->references, 1);
    atomic_init(&metrics->hits, 0);
    atomic_init(&metrics->bytes, 0);
    for (i = 0; i < backend->capacity; ++i) {
        if (next->objects[i].metrics == NULL) {
            next->objects[i].rule = pending->rule;
            next->objects[i].metrics = metrics;
            next->count++;
            break;
        }
    }
    if (publish_locked(backend, next) != 0) {
        snapshot_destroy(next, backend->capacity);
        (void)pthread_mutex_unlock(&backend->writer_lock);
        return -ENOMEM;
    }
    /**
     * 成功发布后完成计时，耗时包括副本分配、指针发布和当次可完成的退役回收
     * 查询同样持有 writer_lock，所以读不到尚未填完的记录，逐包匹配不读取这些字段
     */
    metrics->timing_available = dppd_install_timer_finish(
        &timer, &metrics->install_duration_ns);
    metrics->commit_rule_count = 1;
    pending->published = true;
    (void)pthread_mutex_unlock(&backend->writer_lock);
    return 0;
}

/**
 * 尚未发布时只释放准备记录；已经发布时还要删除对应的规则版本
 * 删除需要创建快照，也可能因内存不足失败，返回值必须交给上层处理
 * 无论删除是否成功都会释放准备记录，残留规则由上层恢复隔离流程接管
 */
static int transaction_rollback(void *context,
                                const struct dppd_transaction_item *item,
                                uintptr_t token, bool commit_was_attempted)
{
    struct dppd_software_backend *backend = context;
    struct dppd_software_pending_rule *pending =
        (struct dppd_software_pending_rule *)token;
    int rc = 0;

    (void)item;
    (void)commit_was_attempted;
    if (backend == NULL || pending == NULL)
        return -EINVAL;
    /** commit 尚未发布时只需释放 token；已发布时删除同一 ID+generation 的精确版本。 */
    if (pending->published)
        rc = dppd_software_backend_remove_version(backend, pending->rule.id,
                                                  pending->rule.generation);
    free(pending);
    return rc;
}

/**
 * 释放事务准备记录，不修改已经发布的规则，也不释放规则的计数器
 * 正常提交完成后使用此入口；进入隔离并放弃继续回滚时也可用它清理准备记录
 * 因此调用 finalize 本身不能作为业务事务成功的判断依据
 */
static void transaction_finalize(void *context,
                                 const struct dppd_transaction_item *item,
                                 uintptr_t token)
{
    (void)context;
    (void)item;
    free((struct dppd_software_pending_rule *)token);
}

struct dppd_transaction_backend dppd_software_transaction_backend(
    struct dppd_software_backend *backend)
{
    const struct dppd_transaction_backend operations = {
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
int dppd_software_backend_update_batch(
    struct dppd_software_backend *backend, const struct dppd_rule *rules,
    const uint64_t *expected_generations, uint32_t count)
{
    struct dppd_software_classifier_snapshot *active, *next = NULL;
    uint32_t i;
    int rc = 0;
    struct dppd_install_timer timer;

    if (backend == NULL || !backend->lock_initialized || rules == NULL ||
        expected_generations == NULL || count == 0 || count > backend->capacity)
        return -EINVAL;
    (void)pthread_mutex_lock(&backend->writer_lock);
    active = atomic_load_explicit(&backend->active, memory_order_acquire);
    for (i = 0; i < count; ++i) {
        char error[128];
        uint32_t j;

        if (rules[i].id == 0 || rules[i].generation == 0 ||
            rules[i].generation == UINT64_MAX ||
            dppd_rule_validate(&rules[i], error, sizeof(error)) != 0) {
            rc = -EINVAL;
            goto out;
        }
        if (!dppd_software_backend_rule_supported(&rules[i])) {
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
            snapshot_find(active, backend->capacity, rules[i].id,
                          expected_generations[i]) < 0) {
            rc = -ESTALE;
            goto out;
        }
        if (snapshot_find(active, backend->capacity, rules[i].id,
                          rules[i].generation) >= 0) {
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
        int index = snapshot_find(next, backend->capacity, rules[i].id,
                                  expected_generations[i]);
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

        /**
         * 一次发布同时替换整个批次，因此各新版本共享整批耗时，不能伪装成各自的单条耗时
         * 这里只修改新分配的记录，未更新版本仍共享原来的记录，失败路径完全不改旧表
         */
        for (i = 0; i < count; ++i) {
            int index = snapshot_find(next, backend->capacity, rules[i].id,
                                      rules[i].generation);
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

/** 按 ID 和 generation 精确删除；新表不再包含该版本，旧读者可继续使用退役快照 */
int dppd_software_backend_remove_version(struct dppd_software_backend *backend,
                                         uint64_t rule_id, uint64_t generation)
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
    /** 删除也经由新 snapshot 发布；旧 snapshot 仍保留被删除规则直到 QSBR 放行。 */
    next = snapshot_clone(active, backend->capacity);
    if (next == NULL) {
        (void)pthread_mutex_unlock(&backend->writer_lock);
        return -ENOMEM;
    }
    /** 只放弃副本持有的计数器引用，旧快照仍持有自己的引用，不能在此强行释放 */
    metrics_put(next->objects[index].metrics);
    memset(&next->objects[index], 0, sizeof(next->objects[index]));
    next->count--;
    if (publish_locked(backend, next) != 0) {
        snapshot_destroy(next, backend->capacity);
        (void)pthread_mutex_unlock(&backend->writer_lock);
        return -ENOMEM;
    }
    (void)pthread_mutex_unlock(&backend->writer_lock);
    return 0;
}

/**
 * 返回当前活跃软件规则表中的对象数量，供批量更新检查还剩多少临时槽位
 * 已退役但尚未释放的快照不参与计数，因为它们只是供尚未离开的读者继续使用
 *
 * 查询期间持有控制面的写锁，防止读取 active 后，该快照又被另一次更新回收
 * 这个锁只用于管理侧查询，不会让逐包匹配路径增加锁操作
 */
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
bool dppd_software_backend_contains_version(
    const struct dppd_software_backend *backend,
    uint64_t rule_id, uint64_t generation)
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
int dppd_software_backend_install_info(
    const struct dppd_software_backend *backend, uint64_t rule_id,
    uint64_t generation, struct dppd_rule_install_info *info)
{
    const struct dppd_software_classifier_snapshot *active;
    const struct dppd_software_rule_object *object;
    uint16_t action;
    int index;

    if (backend == NULL || !backend->lock_initialized || rule_id == 0 ||
        generation == 0 || info == NULL)
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
int dppd_software_backend_query_count(const struct dppd_software_backend *backend,
                                      uint64_t rule_id, uint64_t generation,
                                      uint64_t *hits, uint64_t *bytes)
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
            *hits = atomic_load_explicit(&active->objects[index].metrics->hits,
                                         memory_order_relaxed);
            *bytes = atomic_load_explicit(&active->objects[index].metrics->bytes,
                                          memory_order_relaxed);
            reclaim_locked((struct dppd_software_backend *)backend);
            (void)pthread_mutex_unlock((pthread_mutex_t *)&backend->writer_lock);
            return 0;
        }
    }
    reclaim_locked((struct dppd_software_backend *)backend);
    (void)pthread_mutex_unlock((pthread_mutex_t *)&backend->writer_lock);
    return -ENODATA;
}

int dppd_software_backend_worker_register(struct dppd_software_backend *backend,
                                          unsigned int worker_id)
{
    int rc;

    /** worker_id 必须稳定且唯一；同一 reader 重复注册会破坏 QSBR 进度判断。 */
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
 */
void dppd_software_backend_worker_quiescent(
    struct dppd_software_backend *backend, unsigned int worker_id)
{
    if (backend != NULL && backend->qsbr_initialized && worker_id < DPPD_MAX_WORKERS)
        rte_rcu_qsbr_quiescent(backend->qsbr, worker_id);
}

/** 先下线再注销，使回收器不再等待已退出的线程；调用前必须停止使用所有快照指针 */
void dppd_software_backend_worker_unregister(
    struct dppd_software_backend *backend, unsigned int worker_id)
{
    if (backend == NULL || !backend->qsbr_initialized || worker_id >= DPPD_MAX_WORKERS)
        return;
    rte_rcu_qsbr_thread_offline(backend->qsbr, worker_id);
    (void)rte_rcu_qsbr_thread_unregister(backend->qsbr, worker_id);
}

void dppd_software_backend_decide(struct dppd_software_backend *backend,
                                  uint16_t ingress_port,
                                  const struct dppd_packet *packet,
                                  struct dppd_software_decision *decision)
{
    struct dppd_software_classifier_snapshot *active;
    struct dppd_software_rule_object *selected = NULL;
    uint32_t i;

    memset(decision, 0, sizeof(*decision));
    if (backend == NULL || packet == NULL)
        return;
    /**
     * 调用线程必须已经注册并上线，QSBR 在读取期间保护旧快照不被回收
     * 每个报文只读取一次 active，后续遍历使用同一张表，不获取控制面写锁
     * acquire 与发布端的 release 配合，保证读到新指针时也能读到完整规则内容
     */
    active = atomic_load_explicit(&backend->active, memory_order_acquire);
    for (i = 0; i < backend->capacity; ++i) {
        struct dppd_software_rule_object *object = &active->objects[i];

        if (object->metrics == NULL ||
            !rule_matches_packet(&object->rule, ingress_port, packet))
            continue;
        if (selected == NULL || rule_order(&object->rule, &selected->rule) < 0)
            selected = object;
    }
    if (selected != NULL) {
        uint16_t action;

        /**
         * 只执行最终选中的规则，不把所有命中规则的动作叠加
         * DROP 只设置结果标志，不提前退出动作循环，所以同一规则的 COUNT 仍会累计
         * 实际释放报文或写入 MARK 由外层工作线程完成
         */
        decision->matched = true;
        for (action = 0; action < selected->rule.nb_actions; ++action) {
            const struct dppd_action *source = &selected->rule.actions[action];

            if (source->type == DPPD_ACTION_DROP)
                decision->drop = true;
            else if (source->type == DPPD_ACTION_MARK) {
                decision->has_mark = true;
                decision->mark_id = source->conf.mark_id;
            } else if (source->type == DPPD_ACTION_COUNT) {
                atomic_fetch_add_explicit(&selected->metrics->hits, 1,
                                          memory_order_relaxed);
                atomic_fetch_add_explicit(&selected->metrics->bytes, packet->packet_len,
                                          memory_order_relaxed);
            }
        }
    }
}
