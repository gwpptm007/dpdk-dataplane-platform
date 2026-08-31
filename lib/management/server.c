#define _GNU_SOURCE
#include "dppd/management.h"
#include "dppd/device.h"

#include <errno.h>
#include <stddef.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

/* 单次主循环最多消费的连接数，防止管理流量饿死统计、信号等控制任务。 */
#define DPPD_MANAGEMENT_POLL_BUDGET 16U
/* accept 后等待首个完整请求的上限；客户端必须采用 connect 后立即 send 的模式。 */
#define DPPD_MANAGEMENT_CLIENT_WAIT_MS 20

/* 统一初始化响应头和全部 padding，避免把栈中的未初始化数据发送给客户端。 */
static void response_header(const struct dppd_management_request *request,
                            struct dppd_management_response *response)
{
    memset(response, 0, sizeof(*response));
    response->version = DPPD_MANAGEMENT_VERSION;
    response->operation = request->operation;
    response->size = sizeof(*response);
    response->request_id = request->request_id;
}

/*
 * 把进程内 device/topology 两份状态合并为不含指针的协议快照。
 * device 记录队列和 offload 能力，topology 记录 representor/switch-domain 语义；
 * 两者缺少任意一份都说明该 port 不属于当前 dppd 管理范围。
 */
static int build_port_info(const struct dppd_device_set *devices,
                           uint16_t port_id,
                           struct dppd_management_port_info *output)
{
    const struct dppd_endpoint *endpoint;
    const struct dppd_port *port;

    if (devices == NULL || output == NULL)
        return -EINVAL;
    port = dppd_devices_find(devices, port_id);
    endpoint = dppd_topology_find(&devices->topology, port_id);
    if (port == NULL || endpoint == NULL)
        return -ENOENT;

    /*
     * response_header 已清零整个响应，此处仍只逐字段赋值，禁止 memcpy 内部结构：
     * dppd_port 的布局含未来可变字段，不能成为管理协议 ABI。
     */
    output->port_id = port->port_id;
    output->peer_port_id = port->peer_port_id;
    output->socket_id = port->socket_id;
    output->endpoint_kind = (uint8_t)endpoint->kind;
    output->configured = port->configured;
    output->started = port->started;
    output->has_switch_domain = endpoint->has_switch_domain;
    output->switch_domain_id = endpoint->switch_domain_id;
    output->switch_port_id = endpoint->switch_port_id;
    output->max_rx_queues = port->capabilities.max_rx_queues;
    output->max_tx_queues = port->capabilities.max_tx_queues;
    output->reta_size = port->capabilities.reta_size;
    memcpy(output->mac, port->mac.addr_bytes, sizeof(output->mac));
    output->rss_offloads = port->capabilities.rss_offloads;
    output->rx_offloads = port->capabilities.rx_offloads;
    output->tx_offloads = port->capabilities.tx_offloads;
    output->device_capabilities = port->capabilities.device_capabilities;
    output->configured_rss_hf = port->configured_rss_hf;
    output->configured_tx_offloads = port->configured_tx_offloads;
    memcpy(output->switch_name, endpoint->switch_name,
           sizeof(output->switch_name));
    memcpy(output->driver_name, endpoint->driver_name,
           sizeof(output->driver_name));
    return 0;
}

static void build_rule_summary(const struct dppd_rule *rule,
                               struct dppd_management_rule_summary *summary)
{
    uint16_t i;

    summary->rule_id = rule->id;
    summary->generation = rule->generation;
    summary->group = rule->group;
    summary->priority = rule->priority;
    summary->nb_matches = rule->nb_matches;
    summary->nb_actions = rule->nb_actions;
    summary->domain = (uint8_t)rule->domain;
    summary->fallback = (uint8_t)rule->fallback;
    summary->install_port_id = rule->install_port_id;
    /*
     * mask 仅表达“包含哪些类型”，不表达顺序和具体参数；完整 canonical rule
     * 仍必须通过 get 读取。enum 范围已由 repository apply 前的 rule 校验保证。
     */
    for (i = 0; i < rule->nb_matches; ++i)
        summary->match_mask |= 1U << rule->matches[i].type;
    for (i = 0; i < rule->nb_actions; ++i)
        summary->action_mask |= 1U << rule->actions[i].type;
}

int dppd_management_handle(struct dppd_control_service *control,
                           const struct dppd_device_set *devices,
                           const struct dppd_management_request *request,
                           struct dppd_management_response *response)
{
    int rc;

    if (control == NULL || request == NULL || response == NULL)
        return -EINVAL;
    response_header(request, response);
    /*
     * size 必须精确匹配而不是仅检查“至少多大”。当前 payload 是本地 C ABI，
     * 接受更大或更小的结构都会让 union 字段偏移与 daemon 的解释不一致。
     */
    if (request->version != DPPD_MANAGEMENT_VERSION ||
        request->size != sizeof(*request)) {
        response->status = -EPROTO;
        return 0;
    }
    /*
     * 回滚失败时 repository 从未发布，但 backend 可能还有实际 flow。除恢复状态和
     * retry 外一律拒绝，防止“空 desired state”被误认为是可继续运行的正常状态。
     */
    if (control->recovery_state != DPPD_CONTROL_RECOVERY_READY &&
        request->operation != DPPD_MANAGEMENT_RECOVERY_STATUS &&
        request->operation != DPPD_MANAGEMENT_RECOVERY_RETRY) {
        response->status = -EUCLEAN;
        return 0;
    }

    switch (request->operation) {
    case DPPD_MANAGEMENT_PING:
        /* ping 不触碰硬件，仅返回 desired state 的轻量健康信息。 */
        response->payload.pong.repository_generation =
            dppd_rule_repository_generation(&control->rules);
        response->payload.pong.rule_count =
            dppd_rule_repository_count(&control->rules);
        rc = 0;
        break;
    case DPPD_MANAGEMENT_RULE_APPLY:
        /*
         * apply 的原子边界由 control service 保证：硬件事务成功后才发布
         * repository generation，失败时 response.status 直接携带底层 -errno。
         */
        rc = dppd_control_apply(control,
                                request->payload.apply.install_port_id,
                                &request->payload.apply.rule,
                                request->payload.apply.expected_generation,
                                &response->payload.apply);
        break;
    case DPPD_MANAGEMENT_RULE_CREATE_BATCH:
        /*
         * 防止本地 ABI 数组越界；这里不逐条验证规则，也不尝试在协议层拼装部分结果。
         * 规则语义、预检和 actual/desired 的原子边界都属于 control 层，任何错误统一
         * 写入 response.status，客户端以整个请求为失败处理。
         */
        if (request->payload.create_batch.count == 0 ||
            request->payload.create_batch.count >
                DPPD_MANAGEMENT_BATCH_CREATE_MAX) {
            rc = -EINVAL;
            break;
        }
        /*
         * 预先回显 count 只定义成功响应的数组长度；若 control 返回错误，客户端必须先
         * 检查 status，不能据此读取任何单项结果。这样未来扩展 per-item 诊断也不会
         * 改变“批量请求只有一个提交结论”的语义。
         */
        response->payload.create_batch.count = request->payload.create_batch.count;
        rc = dppd_control_create_batch(
            control, request->payload.create_batch.rules,
            request->payload.create_batch.count,
            response->payload.create_batch.rules);
        break;
    case DPPD_MANAGEMENT_RULE_GET:
        /* get 只读取 desired repository，不通过 rte_flow 反查硬件对象。 */
        rc = dppd_rule_repository_get(&control->rules,
                                      request->payload.get.rule_id,
                                      &response->payload.rule);
        break;
    case DPPD_MANAGEMENT_RULE_DELETE:
        /* 删除同样携带 expected_generation，避免旧客户端误删已更新规则。 */
        rc = dppd_control_remove(control,
                                 request->payload.delete_rule.rule_id,
                                 request->payload.delete_rule.expected_generation,
                                 &response->payload.delete_rule.removed,
                                 &response->payload.delete_rule.generation);
        break;
    case DPPD_MANAGEMENT_RULE_DELETE_BATCH:
        /*
         * 与 create_batch 一样，协议层只防御固定数组边界。控制层负责精确 generation
         * 预检、actual 删除补偿与 recovery 隔离；不能在此把单条 delete 循环成伪批量。
         */
        if (request->payload.delete_batch.count < 2 ||
            request->payload.delete_batch.count >
                DPPD_MANAGEMENT_BATCH_REMOVE_MAX) {
            rc = -EINVAL;
            break;
        }
        response->payload.delete_batch.count = request->payload.delete_batch.count;
        rc = dppd_control_remove_batch(
            control, request->payload.delete_batch.rules,
            request->payload.delete_batch.count,
            response->payload.delete_batch.rules);
        break;
    case DPPD_MANAGEMENT_PORT_GET:
        /*
         * 这是启动时能力快照，不在每次查询时调用 PMD；既避免主循环中的设备探测
         * 开销，也保证客户端看到的是 dppd 实际完成配置后的稳定状态。
         */
        rc = build_port_info(devices, request->payload.port_get.port_id,
                             &response->payload.port);
        break;
    case DPPD_MANAGEMENT_RULE_COUNT_QUERY:
        /*
         * COUNT 查询不改变 repository generation，也不重置 PMD counter。
         * hits/bytes 是否可用最终由目标 PMD 的 rte_flow_query() 决定。
         */
        rc = dppd_control_query_count(
            control, request->payload.count_query.rule_id,
            request->payload.count_query.expected_generation,
            &response->payload.count);
        break;
    case DPPD_MANAGEMENT_RULE_LIST: {
        struct dppd_rule rules[DPPD_MANAGEMENT_RULE_PAGE_SIZE];
        struct dppd_management_rule_page *page = &response->payload.rule_page;
        uint32_t count = 0;
        bool has_more = false;
        uint32_t i;

        rc = dppd_rule_repository_list(
            &control->rules, request->payload.list.after_rule_id,
            request->payload.list.expected_repository_generation, rules,
            DPPD_MANAGEMENT_RULE_PAGE_SIZE, &count, &has_more,
            &page->repository_generation);
        if (rc != 0)
            break;
        page->total_count = dppd_rule_repository_count(&control->rules);
        page->count = (uint16_t)count;
        page->has_more = has_more;
        page->next_after_rule_id = request->payload.list.after_rule_id;
        for (i = 0; i < count; ++i) {
            build_rule_summary(&rules[i], &page->rules[i]);
            page->next_after_rule_id = rules[i].id;
        }
        break;
    }
    case DPPD_MANAGEMENT_PERSISTENCE_STATUS:
        /*
         * 状态查询只读取 control service 中已经发布的字段，不触发磁盘 I/O。
         * last_error 保留最近一次落盘失败的负 errno，便于 CLI 给出直接故障原因。
         */
        dppd_control_persistence_status(control,
                                        &response->payload.persistence);
        rc = 0;
        break;
    case DPPD_MANAGEMENT_PERSISTENCE_FLUSH:
        /*
         * flush 不修改硬件或 repository，只把当前完整 desired state 重新落盘。
         * 无论成功与否都填充状态 payload；成功响应可直接展示最新 generation，
         * 失败时 response.status 仍保留底层存储的负 errno。
         */
        rc = dppd_control_persistence_flush(control);
        dppd_control_persistence_status(control,
                                        &response->payload.persistence);
        break;
    case DPPD_MANAGEMENT_RECOVERY_STATUS:
        /* 只读快照，不触碰任何 backend 对象，隔离模式和正常模式均可查询。 */
        dppd_control_recovery_status(control, &response->payload.recovery);
        rc = 0;
        break;
    case DPPD_MANAGEMENT_RECOVERY_RETRY:
        /*
         * retry 只尝试删除本进程仍持有 handle 的遗留对象；成功后由主循环退出，
         * 后续重启重新执行完整 snapshot 恢复，而不在此处发布空 repository。
         */
        rc = dppd_control_reconciliation_retry(control);
        dppd_control_recovery_status(control, &response->payload.recovery);
        break;
    default:
        rc = -ENOTSUP;
        break;
    }
    /* transport 已经成功解析请求，因此业务错误放入响应而不作为本函数返回值。 */
    response->status = rc;
    return 0;
}

int dppd_management_start(struct dppd_management_server *server,
                          struct dppd_control_service *control,
                          const struct dppd_device_set *devices,
                          const char *socket_path)
{
    struct sockaddr_un address;
    size_t path_length;
    int fd;

    if (server == NULL || control == NULL || devices == NULL ||
        socket_path == NULL)
        return -EINVAL;
    path_length = strlen(socket_path);
    if (path_length == 0 || path_length > DPPD_MANAGEMENT_SOCKET_PATH_MAX)
        return -ENAMETOOLONG;
    /* 先建立明确的未启动状态，保证后续任一步失败都不会留下可误关闭的 fd。 */
    memset(server, 0, sizeof(*server));
    server->socket_fd = -1;

    /*
     * SOCK_SEQPACKET 让一次 send 对应一次 recv，避免自行设计流式 framing；
     * NONBLOCK 防止管理客户端卡住主循环；CLOEXEC 防止未来启动子进程时泄漏 fd。
     */
    fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -errno;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    memcpy(address.sun_path, socket_path, path_length + 1U);
    /*
     * 不在 bind 前无条件 unlink：配置错误或另一个存活 daemon 使用同一路径时，
     * 擅自删除会绕过单实例保护。异常退出留下的路径需由部署层确认后清理。
     */
    if (bind(fd, (const struct sockaddr *)&address, sizeof(address)) != 0) {
        const int error = -errno;
        close(fd);
        return error;
    }
    /* socket 默认仅允许启动 dppd 的用户访问，避免未授权规则变更。 */
    if (chmod(socket_path, S_IRUSR | S_IWUSR) != 0 || listen(fd, 16) != 0) {
        const int error = -errno;
        close(fd);
        unlink(socket_path);
        return error;
    }
    /* 只有 bind/chmod/listen 全部成功后才发布 started 状态。 */
    server->socket_fd = fd;
    server->control = control;
    server->devices = devices;
    memcpy(server->socket_path, socket_path, path_length + 1U);
    server->started = true;
    printf("[dppd] management socket=%s\n", socket_path);
    return 0;
}

int dppd_management_poll(struct dppd_management_server *server)
{
    uint32_t handled = 0;

    if (server == NULL || !server->started)
        return -EINVAL;

    /* 每轮设置预算，避免持续连接让统计、信号和退出处理长期得不到调度。 */
    while (handled < DPPD_MANAGEMENT_POLL_BUDGET) {
        struct dppd_management_request request;
        struct dppd_management_response response;
        ssize_t received;
        struct pollfd ready;
        int client;
        int poll_rc;

        /* 客户端 fd 同样保持非阻塞并禁止跨 exec 继承。 */
        client = accept4(server->socket_fd, NULL, NULL,
                         SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (client < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return 0;
            if (errno == EINTR)
                continue;
            return -errno;
        }
        handled++;

        /*
         * 监听 socket 可读只表示连接已排队，不保证请求数据已经到达。这里最多等待
         * 20ms，既消除 connect/send 竞态，也避免异常客户端卡住 daemon 主循环。
         */
        ready.fd = client;
        ready.events = POLLIN;
        ready.revents = 0;
        do {
            poll_rc = poll(&ready, 1, DPPD_MANAGEMENT_CLIENT_WAIT_MS);
        } while (poll_rc < 0 && errno == EINTR);
        if (poll_rc <= 0 || (ready.revents & POLLIN) == 0) {
            close(client);
            if (poll_rc < 0)
                return -errno;
            continue;
        }

        /*
         * 每个连接只收一个固定大小请求。SEQPACKET 消息过短表示客户端结构不兼容；
         * 消息过长时 recv 返回缓冲区大小并截断，因此 version/size 校验仍会拒绝它。
         */
        memset(&request, 0, sizeof(request));
        /* MSG_TRUNC 让过长数据包返回原始长度，从而不会把被截断前缀误当成合法请求。 */
        received = recv(client, &request, sizeof(request), MSG_TRUNC);
        if (received == (ssize_t)sizeof(request)) {
            (void)dppd_management_handle(server->control, server->devices,
                                         &request, &response);
        } else {
            memset(&response, 0, sizeof(response));
            response.version = DPPD_MANAGEMENT_VERSION;
            response.size = sizeof(response);
            response.status = received < 0 ? -errno : -EMSGSIZE;
        }
        /*
         * 客户端提前退出属于连接级事件，不应终止 daemon；MSG_NOSIGNAL 避免
         * 对已关闭 peer 发送时触发 SIGPIPE。响应很小，SEQPACKET 要么整包发送要么失败。
         */
        if (send(client, &response, sizeof(response), MSG_NOSIGNAL) < 0 &&
            errno != EPIPE && errno != ECONNRESET) {
            const int error = -errno;
            close(client);
            return error;
        }
        close(client);
    }
    return 0;
}

void dppd_management_stop(struct dppd_management_server *server)
{
    if (server == NULL || !server->started)
        return;
    /*
     * 先关闭监听 fd，再删除路径，新的客户端不会在退出窗口建立有效连接。
     * started 检查使该函数可安全用于正常退出和多层 goto 清理路径。
     */
    close(server->socket_fd);
    unlink(server->socket_path);
    memset(server, 0, sizeof(*server));
    server->socket_fd = -1;
}
