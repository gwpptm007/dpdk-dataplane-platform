/**
 * 这是测试专用的故障注入库，用来稳定重现平时很难遇到的创建、删除和补偿失败
 * recovery_isolation.py 只向自己启动的独立 daemon 显式加载本库
 * 本库不链接到生产程序，返回的 handle 只是小块测试内存，不会安装网卡规则或处理报文
 *
 * 测试仍运行真实 daemon、工作线程、管理接口和持久化代码，只替换 DPDK flow API 边界
 * 因此可以证明隔离和重启流程正确，不能据此证明某块物理网卡的故障行为
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <rte_errno.h>
#include <rte_flow.h>

/** 用端口号模拟 handle 的归属，删除时可以检查调用方是否使用了正确端口 */
struct test_flow {
    uint16_t port;
};

/** 创建和删除调用次数用于定位注入时机，live 统计仍未释放的测试 handle 数量 */
static unsigned int creates, destroys, live;

/**
 * 由测试进程的环境变量选择场景，未显式指定支持的值时返回零
 * 场景一模拟“新版本创建失败且撤销失败”，场景二模拟“旧版本删除失败且恢复失败”
 * 场景三从第二次创建开始失败，用来验证非空快照启动重放中的恢复隔离和诊断
 */
static int mode(void)
{
    const char *value = getenv("DPPD_TEST_FLOW_FAULT_MODE");

    if (value != NULL && strcmp(value, "create-rollback") == 0)
        return 1;
    if (value != NULL && strcmp(value, "delete-restore") == 0)
        return 2;
    if (value != NULL && strcmp(value, "replay-rollback") == 0)
        return 3;
    return 0;
}

/**
 * 模拟 DPDK 报错方式，同时设置线程错误码、详细错误信息并返回负 errno
 * create 通过空指针报告失败，其调用方还需要读取 rte_errno 才能知道失败原因
 */
static int fail(struct rte_flow_error *error, int code, const char *message)
{
    rte_errno = code;
    if (error != NULL) {
        memset(error, 0, sizeof(*error));
        error->type = RTE_FLOW_ERROR_TYPE_UNSPECIFIED;
        error->message = message;
    }
    fprintf(stderr, "[dppd-test] %s\n", message);
    return -code;
}

/**
 * 测试开启时允许规则进入创建阶段，以便后续精确注入事务错误
 * 这不是硬件能力探测，传入的匹配条件和动作不会被用于任何真实网卡安装
 * 未开启测试场景时返回不支持，避免把没有配置的测试库误当成可用后端
 */
int rte_flow_validate(uint16_t port, const struct rte_flow_attr *attr,
                      const struct rte_flow_item pattern[],
                      const struct rte_flow_action actions[],
                      struct rte_flow_error *error)
{
    (void)port;
    (void)attr;
    (void)pattern;
    (void)actions;
    if (mode() == 0)
        return fail(error, ENOTSUP, "test flow mode is not enabled");
    return 0;
}

/**
 * 第一次和第二次调用创建两条基线旧规则，第三次开始尝试本批新版本
 * 场景一在第四次调用失败，表示第二条新版本没有建成
 * 场景二让两个新版本都建成，在第五次调用失败，表示删除失败后的旧版本恢复没有成功
 * 其他创建调用只分配一个带端口号的测试对象，由 destroy 负责归还
 */
struct rte_flow *rte_flow_create(uint16_t port, const struct rte_flow_attr *attr,
                                const struct rte_flow_item pattern[],
                                const struct rte_flow_action actions[],
                                struct rte_flow_error *error)
{
    struct test_flow *flow;
    int selected = mode();

    (void)attr;
    (void)pattern;
    (void)actions;
    ++creates;
    if (selected == 0 || (selected == 1 && creates == 4) ||
        (selected == 2 && creates == 5) || (selected == 3 && creates == 2)) {
        fail(error, EIO, "injected flow create failure");
        return NULL;
    }
    flow = malloc(sizeof(*flow));
    if (flow == NULL) {
        fail(error, ENOMEM, "test flow allocation failed");
        return NULL;
    }
    flow->port = port;
    ++live;
    return (struct rte_flow *)flow;
}

/**
 * 按删除调用顺序注入错误，失败时保留对象，使恢复重试仍能使用同一个 handle
 * 场景一：第一次删除是撤销新版本，第二次删除是首次清理重试，两次都失败
 * 场景二：第二次删除是删除第二条旧版本，第五次删除是首次清理重试，两次都失败
 * 后面的删除恢复正常，从而验证第二次清理重试能够真正清空全部残留对象
 */
int rte_flow_destroy(uint16_t port, struct rte_flow *handle, struct rte_flow_error *error)
{
    struct test_flow *flow = (struct test_flow *)handle;
    int selected = mode();

    ++destroys;
    /** 重放隔离也保留 handle，第一次回滚和第一次专用重试失败，第二次重试才释放 */
    if (selected == 3 && creates >= 2 && destroys <= 2)
        return fail(error, EFAULT, "injected replay destroy failure");
    /**
     * 创建调用达到第四次才开启删除故障，保证错误确实出现在在线更新期间
     * 重启后只重放两条旧规则，此条件不会满足，因此重放验证可以正常退出
     */
    if (creates >= 4 && ((selected == 1 && (destroys == 1 || destroys == 2)) ||
                         (selected == 2 && (destroys == 2 || destroys == 5))))
        return fail(error, EFAULT, "injected flow destroy failure");
    if (flow == NULL || flow->port != port || live == 0)
        return fail(error, EINVAL, "invalid test flow handle");
    free(flow);
    --live;
    return 0;
}

/**
 * 进程正常执行退出清理时打印剩余 handle 数量，集成脚本要求最终为零
 * 这只检查测试库分配的对象是否泄漏，不代表已经检查真实网卡中的残留资源
 */
__attribute__((destructor)) static void report_handles(void)
{
    fprintf(stderr, "[dppd-test] live-flows=%u\n", live);
}
