#include "ctrl.h"
#include "port_init.h"
#include "stats.h"
#include "worker.h"
#include "dppd/app.h"
#include <stdio.h>
#include <stdlib.h>

/*
 * main.c 的目标是把程序入口收敛成一条非常清晰的主线：
 * 1. DPDK 模式下先做 EAL 初始化
 * 2. 解析应用自己的参数
 * 3. 初始化端口/队列/mempool
 * 4. 进入 Phase 1 的单 worker 收发闭环
 */

#if DPPD_HAS_DPDK
#include <rte_eal.h>
#endif

#if DPPD_HAS_DPDK
static int dppd_prepare_app_argv(int argc, char **argv, int start_idx, int *out_argc, char ***out_argv)
{
    char **app_argv;
    int app_argc;
    int i;

    if (out_argc == NULL || out_argv == NULL)
        return -1;

    app_argc = 1 + (argc - start_idx);
    app_argv = (char **)calloc((size_t)app_argc + 1U, sizeof(char *));
    if (app_argv == NULL)
        return -1;

    app_argv[0] = argv[0];
    for (i = start_idx; i < argc; ++i)
        app_argv[1 + (i - start_idx)] = argv[i];

    *out_argc = app_argc;
    *out_argv = app_argv;
    return 0;
}
#endif

int main(int argc, char **argv)
{
    struct dppd_app_config cfg;
    int rc = 1;
    int app_argc = argc;
    char **app_argv = argv;
#if DPPD_HAS_DPDK
    int sep_idx = -1;
    int eal_argc = argc;
    int eal_ret;
    int i;

    for (i = 1; i < argc; ++i) {
        if (argv[i][0] == '-' && argv[i][1] == '-' && argv[i][2] == '\0') {
            sep_idx = i;
            eal_argc = i;
            break;
        }
    }

    eal_ret = rte_eal_init(eal_argc, argv);
    if (eal_ret < 0) {
        fprintf(stderr, "[dppd] rte_eal_init failed\n");
        return 1;
    }

    if (sep_idx >= 0) {
        if (dppd_prepare_app_argv(argc, argv, sep_idx + 1, &app_argc, &app_argv) != 0) {
            fprintf(stderr, "[dppd] failed to prepare app argv\n");
            return 1;
        }
    } else if (eal_ret < argc) {
        if (dppd_prepare_app_argv(argc, argv, eal_ret, &app_argc, &app_argv) != 0) {
            fprintf(stderr, "[dppd] failed to prepare app argv\n");
            return 1;
        }
    } else {
        if (dppd_prepare_app_argv(argc, argv, argc, &app_argc, &app_argv) != 0) {
            fprintf(stderr, "[dppd] failed to prepare app argv\n");
            return 1;
        }
    }
#endif

    if (dppd_parse_args(app_argc, app_argv, &cfg) != 0) {
        fprintf(stderr, "[dppd] failed to parse arguments\n");
        goto out;
    }

    dppd_stats_reset();
    dppd_dump_config(&cfg);

    if (dppd_port_init(cfg.port_id, &cfg) != 0) {
        fprintf(stderr, "[dppd] port init failed\n");
        goto out;
    }

    if (dppd_port_start(cfg.port_id) != 0) {
        fprintf(stderr, "[dppd] port start failed\n");
        goto out_stop;
    }

    dppd_worker_poll_once(cfg.port_id, 0);
    dppd_stats_dump();
    rc = 0;

out_stop:
    dppd_port_stop(cfg.port_id);
out:
#if DPPD_HAS_DPDK
    if (app_argv != argv)
        free(app_argv);
#endif
    return rc;
}
