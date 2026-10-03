#ifndef DPPD_TELEMETRY_H
#define DPPD_TELEMETRY_H

struct dppd_runtime;
struct dppd_control_service;

/** 由管理线程绑定一个运行实例，先建立规则副本，再开放 telemetry 的只读查询 */
int dppd_telemetry_register(const struct dppd_runtime *runtime,
                            const struct dppd_control_service *control);
/** 管理请求处理完成后更新副本，查询线程不直接访问规则仓库或驱动对象 */
int dppd_telemetry_publish_rules(const struct dppd_control_service *control);
/** 先停止查询对实例的借用，再释放规则副本，调用方随后才能销毁 control 和 runtime */
void dppd_telemetry_unregister_runtime(void);

#endif
