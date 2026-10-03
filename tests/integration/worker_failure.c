/**
 * 此测试程序直接使用正式 daemon 入口，只在链接时替换规则读者注册这个边界
 * 验证远程启动已经成功，但工作线程随后初始化失败时，主循环仍会发现并失败退出
 * 包装函数不链接到正式 dppd，也不安装，只有显式测试环境才注入故障
 */
#define main dppd_program_main
#include "../../app/dppd/main.c"
#undef main

int __real_dppd_software_backend_worker_register(struct dppd_software_backend *backend,
                                                 unsigned int worker_id);
int __wrap_dppd_software_backend_worker_register(struct dppd_software_backend *backend,
                                                 unsigned int worker_id);

/** 只让第一条队列注册失败，其他队列继续走真实接口，以便检查主线程是否会停止它们 */
int __wrap_dppd_software_backend_worker_register(struct dppd_software_backend *backend,
                                                 unsigned int worker_id)
{
    const char *mode = getenv("DPPD_TEST_WORKER_FAILURE");

    if (worker_id == 0 && mode != NULL && strcmp(mode, "registration") == 0) {
        fprintf(stderr, "[worker-fixture] queue=0 registration failed\n");
        return -EINVAL;
    }
    return __real_dppd_software_backend_worker_register(backend, worker_id);
}

/** 使用正式入口完成配置、启动、监控和清理，测试不会自行模仿进程退出逻辑 */
int main(int argc, char **argv)
{
    return dppd_program_main(argc, argv);
}
