#ifndef DPPD_INSTALL_INFO_H
#define DPPD_INSTALL_INFO_H

#include <stdbool.h>
#include <stdint.h>

/**
 * 已安装规则版本的控制面记录，不包含驱动指针，可以复制到管理响应中
 * 这些字段描述本进程成功提交的对象，不证明网卡内部如何执行，也不代表报文已经命中
 * 耗时不写入规则快照，重启重放会为重新安装的对象生成新的记录
 */
struct dppd_rule_install_info {
    uint64_t rule_id;
    uint64_t generation;
    /** 后端提交阶段的耗时，单位为纳秒，不包含规划、能力校验或保存磁盘快照 */
    uint64_t install_duration_ns;
    uint16_t install_port_id;
    /** 是否配置了 COUNT 动作，状态查询不会读取或清零计数器 */
    bool has_count;
    /** 时钟不可用时为 false，此时零耗时表示未知，不能理解为瞬间完成 */
    bool timing_available;
    /**
     * 本次提交一起安装的规则数，单条安装为一，纯软件批量更新为整批条数
     * 整批中的规则共享同一次提交耗时，不能将它们相加或当作单条耗时
     */
    uint32_t commit_rule_count;
};

#endif
