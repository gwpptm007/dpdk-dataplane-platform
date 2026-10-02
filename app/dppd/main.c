#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <rte_cycles.h>
#include <rte_eal.h>
#include "dppd/config.h"
#include "dppd/control.h"
#include "dppd/management.h"
#include "dppd/runtime.h"
#include "dppd/telemetry.h"

static volatile sig_atomic_t stop_signal;

static void handle_signal(int signal_number)
{
    /* 信号处理器只写 sig_atomic_t，实际资源回收全部回到主线程执行。 */
    (void)signal_number;
    stop_signal = 1;
}

static void sleep_control_loop(void)
{
    struct timespec remaining = {
        .tv_sec = 0,
        .tv_nsec = 100000000L,
    };

    /* 被非退出信号打断时睡完剩余时间；退出信号到达后立即返回主循环。 */
    while (nanosleep(&remaining, &remaining) != 0 && errno == EINTR && !stop_signal)
        ;
}

int main(int argc, char **argv)
{
    struct dppd_config config;
    struct dppd_control_service control;
    struct dppd_management_server management;
    struct dppd_runtime runtime;
    uint64_t started_at;
    uint64_t next_stats;
    uint64_t timer_hz;
    int parse_result;
    int eal_consumed;
    int loop_error = 0;
    int rc = EXIT_FAILURE;
    bool recovery_isolation = false;

    /* EAL 必须先消费 -l/-a/--vdev 等参数，后续只解析 -- 后的应用参数。 */
    eal_consumed = rte_eal_init(argc, argv);
    if (eal_consumed < 0) {
        fprintf(stderr, "[dppd] EAL initialization failed\n");
        return EXIT_FAILURE;
    }

    argc -= eal_consumed;
    argv += eal_consumed;
    parse_result = dppd_config_parse(argc, argv, &config);
    if (parse_result == DPPD_CONFIG_HELP) {
        dppd_config_print_usage(argv[0]);
        rc = EXIT_SUCCESS;
        goto cleanup_eal;
    }
    if (parse_result != DPPD_CONFIG_OK) {
        fprintf(stderr, "[dppd] invalid application arguments\n");
        dppd_config_print_usage(argv[0]);
        goto cleanup_eal;
    }

    dppd_config_dump(&config);
    /*
     * 初始化依赖顺序：先发现并启动 ethdev，得到拓扑；control 引用该拓扑；
     * management 再引用 control。goto 清理标签严格按相反顺序释放。
     */
    if (dppd_runtime_init(&runtime, &config) != 0)
        goto cleanup_eal;
    if (dppd_control_init(&control, &runtime.devices.topology,
                          config.rule_capacity, NULL) != 0) {
        fprintf(stderr, "[dppd] control service initialization failed\n");
        goto cleanup_runtime;
    }
    /*
     * worker 只借用 control 持有的 backend；退出时 main 的清理顺序会先停止并 wait 所有
     * worker（使其注销 QSBR reader），再调用 control_fini 释放 snapshot/QSBR。这里不能
     * 绑定临时对象，也不能在 runtime_start 后替换该地址。
     */
    dppd_runtime_set_software_backend(&runtime, &control.software);
    if (config.state_path[0] != '\0') {
        int restore_rc = dppd_control_persistence_restore(&control,
                                                          config.state_path);

        if (restore_rc != 0) {
            if (restore_rc != -EUCLEAN ||
                control.recovery_state !=
                    DPPD_CONTROL_RECOVERY_RECONCILIATION_REQUIRED) {
                fprintf(stderr,
                        "[dppd] rule snapshot restore failed: path=%s error=%s (%d)\n",
                        config.state_path,
                        restore_rc < 0 && restore_rc >= -4095 ?
                            strerror(-restore_rc) : "unknown",
                        restore_rc);
                goto cleanup_control;
            }
            /*
             * transaction 回滚未完成：绝不能启动 worker 或开放普通规则接口，但保留
             * 当前进程的 backend handle，供受限的 reconcile-retry 再次删除。
             */
            recovery_isolation = true;
            fprintf(stderr,
                    "[dppd] snapshot rollback incomplete; entering recovery isolation "
                    "for path=%s\n",
                    config.state_path);
        }
        if (!recovery_isolation)
            printf("[dppd] rule snapshot ready: path=%s generation=%" PRIu64
                   " rules=%u\n",
                   config.state_path,
                   dppd_rule_repository_generation(&control.rules),
                   dppd_rule_repository_count(&control.rules));
    }
    if (dppd_management_start(&management, &control, &runtime.devices,
                               &runtime,
                              config.control_socket) != 0) {
        fprintf(stderr, "[dppd] management socket initialization failed: %s\n",
                config.control_socket);
        goto cleanup_control;
    }
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    if (recovery_isolation) {
        /*
         * 隔离循环不注册 telemetry、不启动 ethdev worker，也不输出正常转发统计。
         * 唯一允许的状态转换是 reconcile-retry 成功后进入 RESTART_REQUIRED。
         */
        while (!stop_signal) {
            loop_error = dppd_management_poll(&management);
            if (loop_error != 0) {
                fprintf(stderr,
                        "[dppd] recovery management processing failed: %d\n",
                        loop_error);
                break;
            }
            if (control.recovery_state ==
                DPPD_CONTROL_RECOVERY_RESTART_REQUIRED) {
                fprintf(stderr,
                        "[dppd] residual flows cleared; exiting for clean snapshot replay\n");
                loop_error = -EUCLEAN;
                break;
            }
            sleep_control_loop();
        }
        goto cleanup_management;
    }
    if (dppd_telemetry_register(&runtime) != 0) {
        fprintf(stderr, "[dppd] telemetry command registration failed\n");
        goto cleanup_management;
    }

    if (dppd_runtime_start(&runtime) != 0)
        goto cleanup_telemetry;

    /* 使用 DPDK 单调 timer cycle，避免系统时间调整影响 duration 与统计周期。 */
    timer_hz = rte_get_timer_hz();
    started_at = rte_get_timer_cycles();
    next_stats = started_at + (timer_hz * config.stats_period_ms) / 1000U;
    while (!stop_signal) {
        const uint64_t now = rte_get_timer_cycles();

        /**
         * 进入恢复隔离后，不再按普通运行时长退出，也不继续周期性输出转发统计
         * 这样用户仍有机会查看残留对象并发起清理，避免定时退出丢失本进程的 handle
         */
        if (!recovery_isolation && config.duration_s != 0 &&
            now - started_at >= timer_hz * config.duration_s)
            break;
        if (!recovery_isolation) {
            loop_error = dppd_devices_poll_links(&runtime.devices);
            if (loop_error != 0) {
                fprintf(stderr, "[dppd] device monitoring failed; stopping workers\n");
                break;
            }
        }
        if (!recovery_isolation && now >= next_stats) {
            dppd_runtime_stats_dump(&runtime);
            next_stats = now + (timer_hz * config.stats_period_ms) / 1000U;
        }
        /*
         * 管理请求只在主线程串行执行，repository 与硬件事务无需额外加锁；
         * poll 内部有连接预算，保证本循环仍能周期性观察 stop_signal。
         */
        loop_error = dppd_management_poll(&management);
        if (loop_error != 0) {
            fprintf(stderr, "[dppd] management request processing failed: %d\n",
                    loop_error);
            break;
        }
        if (control.recovery_state != DPPD_CONTROL_RECOVERY_READY) {
            if (!recovery_isolation) {
                /**
                 * 启动之后发生的补偿失败，也要执行与启动恢复失败相同的隔离约束
                 * 先发送停止请求，再等待已经启动的工作线程结束，之后才处理清理重试
                 * 停止软件转发不代表硬件残留规则停止工作，硬件对象仍要单独清理
                 */
                dppd_runtime_request_stop(&runtime);
                if (dppd_runtime_wait(&runtime) != 0) {
                    loop_error = -EIO;
                    break;
                }
                recovery_isolation = true;
                fprintf(stderr, "[dppd] rule recovery required; workers stopped\n");
            }
            if (control.recovery_state == DPPD_CONTROL_RECOVERY_RESTART_REQUIRED) {
                /**
                 * 残留对象已清理，不在原进程中直接恢复正常服务
                 * 用失败码退出，让操作者或服务管理器重启并从旧快照重新建立完整状态
                 */
                loop_error = -EUCLEAN;
                break;
            }
        }
        sleep_control_loop();
    }

    /* 主循环结束后立即封住入口，防止客户端在 worker 停止窗口继续排队。 */
    dppd_management_stop(&management);
    dppd_runtime_request_stop(&runtime);
    if (dppd_runtime_wait(&runtime) != 0)
        loop_error = -EIO;
    dppd_runtime_stats_dump(&runtime);
    /** 隔离期间即使由用户发信号结束，也不能把这次故障退出报告为正常运行成功 */
    if (loop_error == 0 && !recovery_isolation)
        rc = EXIT_SUCCESS;

cleanup_telemetry:
    /* 标签按“只清理已成功初始化的层”组织，适用于启动失败和正常退出。 */
    dppd_telemetry_unregister_runtime();
cleanup_management:
    /* 先关闭入口，再回收已安装 flow，避免退出阶段接收新事务。 */
    dppd_management_stop(&management);
cleanup_control:
    if (dppd_control_fini(&control) != 0 && rc == EXIT_SUCCESS)
        rc = EXIT_FAILURE;
cleanup_runtime:
    dppd_runtime_destroy(&runtime);
cleanup_eal:
    if (rte_eal_cleanup() != 0 && rc == EXIT_SUCCESS)
        rc = EXIT_FAILURE;
    return rc;
}
