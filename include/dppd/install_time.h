#ifndef DPPD_INSTALL_TIME_H
#define DPPD_INSTALL_TIME_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

/**
 * 只在控制面提交规则时使用的短时计时器，逐包转发路径不调用时钟
 * 使用单调时钟，调整系统日期不会把安装耗时变成负数
 * 读时钟失败只会使统计不可用，不影响规则安装本身的成功或失败
 */
struct dppd_install_timer {
    struct timespec started;
    bool available;
};

/** 在真正的后端提交开始前采样，准备和能力校验阶段不计入此次耗时 */
static inline void dppd_install_timer_start(struct dppd_install_timer *timer)
{
    timer->available = clock_gettime(CLOCK_MONOTONIC, &timer->started) == 0;
}

/**
 * 后端提交成功后计算纳秒差值，返回值说明这次测量是否有效
 * 借位处理跨秒情况，拒绝倒退或超出整数范围的结果，失败时统一输出零
 */
static inline bool dppd_install_timer_finish(const struct dppd_install_timer *timer,
                                             uint64_t *duration_ns)
{
    struct timespec ended;
    uint64_t seconds;
    long nanoseconds;

    *duration_ns = 0;
    if (!timer->available || clock_gettime(CLOCK_MONOTONIC, &ended) != 0)
        return false;
    if (ended.tv_sec < timer->started.tv_sec ||
        (ended.tv_sec == timer->started.tv_sec &&
         ended.tv_nsec < timer->started.tv_nsec))
        return false;
    seconds = (uint64_t)ended.tv_sec - (uint64_t)timer->started.tv_sec;
    nanoseconds = ended.tv_nsec - timer->started.tv_nsec;
    if (nanoseconds < 0) {
        seconds--;
        nanoseconds += 1000000000L;
    }
    if (seconds > (UINT64_MAX - (uint64_t)nanoseconds) / UINT64_C(1000000000))
        return false;
    *duration_ns = seconds * UINT64_C(1000000000) + (uint64_t)nanoseconds;
    return true;
}

#endif
