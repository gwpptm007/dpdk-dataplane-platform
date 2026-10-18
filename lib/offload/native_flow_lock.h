#ifndef DPPD_NATIVE_FLOW_LOCK_H
#define DPPD_NATIVE_FLOW_LOCK_H

/** 模板、共享对象和异步队列可能由不同入口操作，同一进程的原生 PMD 调用统一串行 */
void dppd_native_flow_lock(void);
void dppd_native_flow_unlock(void);

#endif
