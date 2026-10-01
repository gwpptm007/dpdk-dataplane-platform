#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include "dppd/device.h"
#include "dppd/management.h"

static int fake_validate(uint16_t port_id, const struct dppd_rule *rule,
                         struct dppd_flow_error *error)
{
    (void)port_id;
    (void)rule;
    (void)error;
    return 0;
}

static int fake_create(uint16_t port_id, const struct dppd_rule *rule,
                       struct dppd_flow_handle *handle,
                       struct dppd_flow_error *error)
{
    (void)error;
    memset(handle, 0, sizeof(*handle));
    handle->rule_id = rule->id;
    handle->rule_generation = rule->generation;
    handle->port_id = port_id;
    handle->flow = (struct rte_flow *)(uintptr_t)(rule->id + 1U);
    for (uint16_t i = 0; i < rule->nb_actions; ++i) {
        if (rule->actions[i].type == DPPD_ACTION_COUNT)
            handle->has_count = true;
    }
    return 0;
}

static int fake_remove(struct dppd_flow_handle *handle,
                       struct dppd_flow_error *error)
{
    (void)error;
    memset(handle, 0, sizeof(*handle));
    return 0;
}

static int fake_query_count(const struct dppd_flow_handle *handle,
                            uint64_t *hits,
                            uint64_t *bytes,
                            struct dppd_flow_error *error)
{
    (void)handle;
    (void)error;
    *hits = 88;
    *bytes = 4096;
    return 0;
}

static void initialize_request(struct dppd_management_request *request,
                               enum dppd_management_operation operation)
{
    memset(request, 0, sizeof(*request));
    request->version = DPPD_MANAGEMENT_VERSION;
    request->operation = operation;
    request->size = sizeof(*request);
    request->request_id = 1234;
}

static void make_drop_request(struct dppd_management_request *request)
{
    struct dppd_rule *rule;

    initialize_request(request, DPPD_MANAGEMENT_RULE_APPLY);
    request->payload.apply.install_port_id = 5;
    request->payload.apply.expected_generation = 0;
    rule = &request->payload.apply.rule;
    rule->id = 100;
    rule->domain = DPPD_RULE_DOMAIN_INGRESS;
    rule->fallback = DPPD_FALLBACK_PREFER_HARDWARE;
    rule->nb_matches = 1;
    rule->matches[0].type = DPPD_MATCH_ETH;
    rule->nb_actions = 2;
    rule->actions[0].type = DPPD_ACTION_COUNT;
    rule->actions[1].type = DPPD_ACTION_DROP;
}

static void test_socket_round_trip(struct dppd_management_server *server,
                                   struct dppd_control_service *control,
                                   const struct dppd_device_set *devices)
{
    struct dppd_management_request request;
    struct dppd_management_response response;
    struct sockaddr_un address;
    struct stat metadata;
    uint8_t oversized[sizeof(request) + 1U];
    char path[108];
    ssize_t bytes;
    int client;

    /* PID 隔离并行测试实例，stop 后再断言路径确实被回收。 */
    snprintf(path, sizeof(path), "/tmp/dppd-management-%ld.sock", (long)getpid());
    assert(dppd_management_start(server, control, devices, NULL, path) == 0);
    assert(stat(path, &metadata) == 0);
    assert((metadata.st_mode & 0777) == 0600);

    client = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    assert(client >= 0);
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    memcpy(address.sun_path, path, strlen(path) + 1U);
    assert(connect(client, (const struct sockaddr *)&address, sizeof(address)) == 0);
    initialize_request(&request, DPPD_MANAGEMENT_PING);
    assert(send(client, &request, sizeof(request), 0) == (ssize_t)sizeof(request));

    /* 单进程测试先发送再轮询，覆盖 daemon 实际使用的 socket 路径。 */
    assert(dppd_management_poll(server) == 0);
    bytes = recv(client, &response, sizeof(response), 0);
    assert(bytes == (ssize_t)sizeof(response));
    assert(response.status == 0 && response.request_id == request.request_id);
    assert(response.payload.pong.rule_count == 0);
    close(client);

    /*
     * 构造比协议结构多一个字节的 SEQPACKET，验证服务端利用 MSG_TRUNC 取得原始
     * 消息长度并返回 EMSGSIZE，而不是把合法前缀误认为完整请求。
     */
    client = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    assert(client >= 0);
    assert(connect(client, (const struct sockaddr *)&address, sizeof(address)) == 0);
    memset(oversized, 0, sizeof(oversized));
    memcpy(oversized, &request, sizeof(request));
    assert(send(client, oversized, sizeof(oversized), 0) ==
           (ssize_t)sizeof(oversized));
    assert(dppd_management_poll(server) == 0);
    bytes = recv(client, &response, sizeof(response), 0);
    assert(bytes == (ssize_t)sizeof(response));
    assert(response.status == -EMSGSIZE);
    close(client);

    dppd_management_stop(server);
    assert(access(path, F_OK) != 0 && errno == ENOENT);
}

int main(void)
{
    const struct dppd_flow_api api = {
        .validate = fake_validate,
        .create = fake_create,
        .remove = fake_remove,
        .query_count = fake_query_count,
    };
    struct dppd_topology topology;
    struct dppd_device_set devices;
    struct dppd_control_service control;
    struct dppd_management_server server;
    struct dppd_management_request request;
    struct dppd_management_response response;
    char state_directory[128];
    char state_path[160];

    memset(&topology, 0, sizeof(topology));
    topology.nb_endpoints = 1;
    topology.endpoints[0].ethdev_port_id = 5;
    topology.endpoints[0].kind = DPPD_ENDPOINT_REPRESENTOR;
    topology.endpoints[0].has_switch_domain = true;
    topology.endpoints[0].switch_domain_id = 9;
    topology.endpoints[0].switch_port_id = 12;
    memcpy(topology.endpoints[0].driver_name, "fake_pmd", sizeof("fake_pmd"));
    memcpy(topology.endpoints[0].switch_name, "fake_switch",
           sizeof("fake_switch"));
    memset(&devices, 0, sizeof(devices));
    devices.nb_ports = 1;
    devices.topology = topology;
    devices.ports[0].port_id = 5;
    devices.ports[0].peer_port_id = 6;
    devices.ports[0].socket_id = 1;
    devices.ports[0].configured = true;
    devices.ports[0].started = true;
    devices.ports[0].capabilities.max_rx_queues = 8;
    devices.ports[0].capabilities.max_tx_queues = 4;
    devices.ports[0].capabilities.reta_size = 128;
    devices.ports[0].capabilities.rss_offloads = 0x11;
    devices.ports[0].configured_rss_hf = 0x1;
    assert(dppd_control_init(&control, &topology, 4, &api) == 0);

    /* 先覆盖真实 socket framing/权限/清理，再直接测试各 operation 的业务映射。 */
    test_socket_round_trip(&server, &control, &devices);

    initialize_request(&request, DPPD_MANAGEMENT_PING);
    request.version++;
    assert(dppd_management_handle(&control, &devices, NULL, &request, &response) == 0);
    assert(response.status == -EPROTO);

    /*
     * daemon 尚未绑定 snapshot path 时也必须返回结构化状态，而不是 ENOTSUP。
     * 这样部署脚本可以区分“功能未启用”和“已启用但落盘失败”。
     */
    initialize_request(&request, DPPD_MANAGEMENT_PERSISTENCE_STATUS);
    assert(dppd_management_handle(&control, &devices, NULL, &request, &response) == 0);
    assert(response.status == 0);
    assert(!response.payload.persistence.enabled);
    assert(!response.payload.persistence.dirty);
    assert(response.payload.persistence.current_generation == 0);
    initialize_request(&request, DPPD_MANAGEMENT_PERSISTENCE_FLUSH);
    assert(dppd_management_handle(&control, &devices, NULL, &request, &response) == 0);
    assert(response.status == -EINVAL);

    /*
     * 在任何 mutation 前绑定一个新的 state path。这样后续管理操作同时覆盖
     * management → control → snapshot 的真实写路径，而不需要伪造内部状态。
     */
    snprintf(state_directory, sizeof(state_directory),
             "/tmp/dppd-management-state-%ld", (long)getpid());
    snprintf(state_path, sizeof(state_path), "%s/rules.bin", state_directory);
    unlink(state_path);
    rmdir(state_directory);
    assert(mkdir(state_directory, 0700) == 0);
    assert(dppd_control_persistence_attach(&control, state_path) == 0);
    initialize_request(&request, DPPD_MANAGEMENT_PERSISTENCE_STATUS);
    assert(dppd_management_handle(&control, &devices, NULL, &request, &response) == 0);
    assert(response.status == 0);
    assert(response.payload.persistence.enabled);
    assert(!response.payload.persistence.dirty);
    assert(response.payload.persistence.persisted_generation == 0);
    assert(response.payload.persistence.current_generation == 0);

    /* 正常模式也可查询恢复状态；此时没有 residual backend 对象。 */
    initialize_request(&request, DPPD_MANAGEMENT_RECOVERY_STATUS);
    assert(dppd_management_handle(&control, &devices, NULL, &request, &response) == 0);
    assert(response.status == 0);
    assert(response.payload.recovery.state == DPPD_CONTROL_RECOVERY_READY);
    assert(response.payload.recovery.residual_objects == 0);

    /* apply 成功后 desired generation 从 0 发布为 1。 */
    make_drop_request(&request);
    assert(dppd_management_handle(&control, &devices, NULL, &request, &response) == 0);
    assert(response.status == 0);
    assert(response.payload.apply.status == DPPD_RULE_CREATED);
    assert(response.payload.apply.generation == 1);

    initialize_request(&request, DPPD_MANAGEMENT_RULE_LIST);
    request.payload.list.expected_repository_generation =
        DPPD_RULE_GENERATION_ANY;
    assert(dppd_management_handle(&control, &devices, NULL, &request, &response) == 0);
    assert(response.status == 0);
    assert(response.payload.rule_page.repository_generation == 1);
    assert(response.payload.rule_page.total_count == 1);
    assert(response.payload.rule_page.count == 1);
    assert(!response.payload.rule_page.has_more);
    assert(response.payload.rule_page.rules[0].rule_id == 100);
    assert(response.payload.rule_page.rules[0].install_port_id == 5);
    assert((response.payload.rule_page.rules[0].action_mask &
            (1U << DPPD_ACTION_COUNT)) != 0);

    request.payload.list.expected_repository_generation = 0;
    assert(dppd_management_handle(&control, &devices, NULL, &request, &response) == 0);
    assert(response.status == -ESTALE);

    initialize_request(&request, DPPD_MANAGEMENT_RULE_GET);
    request.payload.get.rule_id = 100;
    assert(dppd_management_handle(&control, &devices, NULL, &request, &response) == 0);
    assert(response.status == 0);
    assert(response.payload.rule.id == 100 && response.payload.rule.generation == 1);
    assert(response.payload.rule.install_port_id == 5);

    initialize_request(&request, DPPD_MANAGEMENT_RULE_COUNT_QUERY);
    request.payload.count_query.rule_id = 100;
    request.payload.count_query.expected_generation = 1;
    assert(dppd_management_handle(&control, &devices, NULL, &request, &response) == 0);
    assert(response.status == 0);
    assert(response.payload.count.rule_id == 100);
    assert(response.payload.count.generation == 1);
    assert(response.payload.count.hits == 88);
    assert(response.payload.count.bytes == 4096);

    /* 删除 generation=1 的规则后，全局 repository generation 单调增加到 2。 */
    initialize_request(&request, DPPD_MANAGEMENT_RULE_DELETE);
    request.payload.delete_rule.rule_id = 100;
    request.payload.delete_rule.expected_generation = 1;
    assert(dppd_management_handle(&control, &devices, NULL, &request, &response) == 0);
    assert(response.status == 0 && response.payload.delete_rule.removed);
    assert(response.payload.delete_rule.generation == 2);

    /* port-show 合并 device 能力与 topology 身份，并保持协议结构不含进程内指针。 */
    initialize_request(&request, DPPD_MANAGEMENT_PORT_GET);
    request.payload.port_get.port_id = 5;
    assert(dppd_management_handle(&control, &devices, NULL, &request, &response) == 0);
    assert(response.status == 0);
    assert(response.payload.port.port_id == 5);
    assert(response.payload.port.peer_port_id == 6);
    assert(response.payload.port.endpoint_kind == DPPD_ENDPOINT_REPRESENTOR);
    assert(response.payload.port.has_switch_domain);
    assert(response.payload.port.switch_domain_id == 9);
    assert(response.payload.port.max_rx_queues == 8);
    assert(response.payload.port.max_tx_queues == 4);
    assert(response.payload.port.reta_size == 128);
    assert(response.payload.port.rss_offloads == 0x11);
    assert(response.payload.port.configured_rss_hf == 0x1);
    assert(strcmp(response.payload.port.driver_name, "fake_pmd") == 0);

    request.payload.port_get.port_id = 99;
    assert(dppd_management_handle(&control, &devices, NULL, &request, &response) == 0);
    assert(response.status == -ENOENT);

    /*
     * flush 即使在 clean 状态也重新写入完整 snapshot，可用于运维人员在处理
     * EUCLEAN 后显式确认持久化；成功响应直接返回刷新后的状态。
     */
    initialize_request(&request, DPPD_MANAGEMENT_PERSISTENCE_FLUSH);
    assert(dppd_management_handle(&control, &devices, NULL, &request, &response) == 0);
    assert(response.status == 0);
    assert(response.payload.persistence.enabled);
    assert(!response.payload.persistence.dirty);
    assert(response.payload.persistence.persisted_generation == 2);
    assert(response.payload.persistence.current_generation == 2);
    assert(response.payload.persistence.last_error == 0);

    /*
     * 协议 v5 的首版批量创建入口：同一请求创建两条规则并返回同一个 transaction id。这里
     * 不经真实 UNIX socket，而是直接调用 dispatch，以定位验证“定长 ABI request →
     * control 批量事务 → 定长 response”的字段映射；socket 边界已在前面的 round-trip
     * 用例覆盖。generation 继续从已存在的两条规则之后连续分配。
     */
    initialize_request(&request, DPPD_MANAGEMENT_RULE_CREATE_BATCH);
    request.payload.create_batch.count = 2;
    for (uint16_t i = 0; i < request.payload.create_batch.count; ++i) {
        struct dppd_control_batch_create_request *entry =
            &request.payload.create_batch.rules[i];

        entry->install_port_id = 5;
        entry->expected_generation = 0;
        entry->rule.id = 200 + i;
        entry->rule.domain = DPPD_RULE_DOMAIN_INGRESS;
        entry->rule.fallback = DPPD_FALLBACK_PREFER_HARDWARE;
        entry->rule.nb_matches = 1;
        entry->rule.matches[0].type = DPPD_MATCH_ETH;
        entry->rule.nb_actions = 1;
        entry->rule.actions[0].type = DPPD_ACTION_DROP;
    }
    assert(dppd_management_handle(&control, &devices, NULL, &request, &response) == 0);
    assert(response.status == 0 && response.payload.create_batch.count == 2);
    assert(response.payload.create_batch.rules[0].generation == 3);
    assert(response.payload.create_batch.rules[1].generation == 4);
    assert(response.payload.create_batch.rules[0].transaction_id ==
           response.payload.create_batch.rules[1].transaction_id);

    /**
     * 复用刚刚创建的两条规则构造一笔更新请求，旧版本从仓库读取而不是随意填写
     * 下面先验证协议边界，再验证整批成功的结果顺序和事务编号，最后检查旧请求重放
     */
    initialize_request(&request, DPPD_MANAGEMENT_RULE_UPDATE_BATCH);
    request.payload.update_batch.count = 2;
    for (uint16_t i = 0; i < 2; ++i) {
        struct dppd_control_batch_update_request *entry =
            &request.payload.update_batch.rules[i];

        assert(dppd_rule_repository_get(&control.rules, 200 + i, &entry->rule) == 0);
        entry->install_port_id = 5;
        entry->expected_generation = entry->rule.generation;
        entry->rule.priority = 20 + i;
    }
    /**
     * v7 服务端不接受 v6 请求，也不能按越界 count 读取固定数组
     * 保留位非零和旧版本错误都应在修改规则前拒绝，失败响应不携带有效结果数量
     */
    request.version = 6;
    assert(dppd_management_handle(&control, &devices, NULL, &request, &response) == 0);
    assert(response.status == -EPROTO);
    request.version = DPPD_MANAGEMENT_VERSION;
    for (uint16_t count = 0; count <= 5; ++count) {
        if (count >= 2 && count <= 4)
            continue;
        request.payload.update_batch.count = count;
        assert(dppd_management_handle(&control, &devices, NULL, &request, &response) == 0);
        assert(response.status == -EINVAL && response.payload.update_batch.count == 0);
    }
    request.payload.update_batch.count = 2;
    request.payload.update_batch.reserved[0] = 1;
    assert(dppd_management_handle(&control, &devices, NULL, &request, &response) == 0);
    assert(response.status == -EINVAL);
    request.payload.update_batch.reserved[0] = 0;
    request.payload.update_batch.rules[1].expected_generation = 0;
    assert(dppd_management_handle(&control, &devices, NULL, &request, &response) == 0);
    assert(response.status == -ESTALE && control.rules.generation == 4);
    assert(response.payload.update_batch.count == 0);
    request.payload.update_batch.rules[1].expected_generation = 4;
    assert(dppd_management_handle(&control, &devices, NULL, &request, &response) == 0);
    assert(response.status == 0 && response.payload.update_batch.count == 2);
    assert(response.payload.update_batch.rules[0].generation == 5);
    assert(response.payload.update_batch.rules[1].generation == 6);
    assert(response.payload.update_batch.rules[0].transaction_id ==
           response.payload.update_batch.rules[1].transaction_id);
    assert(dppd_management_handle(&control, &devices, NULL, &request, &response) == 0);
    assert(response.status == -ESTALE && response.payload.update_batch.count == 0);

    /** 更新成功后精确删除必须使用新版本，不能继续用创建时的旧版本删除已经更新的规则 */
    initialize_request(&request, DPPD_MANAGEMENT_RULE_DELETE_BATCH);
    request.payload.delete_batch.count = 2;
    request.payload.delete_batch.rules[0].rule_id = 200;
    request.payload.delete_batch.rules[0].expected_generation = 5;
    request.payload.delete_batch.rules[1].rule_id = 201;
    request.payload.delete_batch.rules[1].expected_generation = 6;
    assert(dppd_management_handle(&control, &devices, NULL, &request, &response) == 0);
    assert(response.status == 0 && response.payload.delete_batch.count == 2);
    assert(response.payload.delete_batch.rules[0].rule_id == 200 &&
           response.payload.delete_batch.rules[0].generation == 7);
    assert(response.payload.delete_batch.rules[1].rule_id == 201 &&
           response.payload.delete_batch.rules[1].generation == 8);

    /*
     * 协议层必须只放行 recovery status/retry。这里直接注入隔离状态，专注验证
     * dispatch 边界；实际 residual handle 的重试行为由 control service 单测覆盖。
     * 尤其要确认普通 PING 也被拒绝：否则自动化探活可能把“实际对象未对账完成”误判
     * 为可安全接收新管理请求的实例。
     */
    control.recovery_state =
        DPPD_CONTROL_RECOVERY_RECONCILIATION_REQUIRED;
    control.recovery_last_error = -EFAULT;
    initialize_request(&request, DPPD_MANAGEMENT_PING);
    assert(dppd_management_handle(&control, &devices, NULL, &request, &response) == 0);
    assert(response.status == -EUCLEAN);
    initialize_request(&request, DPPD_MANAGEMENT_RECOVERY_STATUS);
    assert(dppd_management_handle(&control, &devices, NULL, &request, &response) == 0);
    assert(response.status == 0);
    assert(response.payload.recovery.state ==
           DPPD_CONTROL_RECOVERY_RECONCILIATION_REQUIRED);
    assert(response.payload.recovery.last_error == -EFAULT);
    initialize_request(&request, DPPD_MANAGEMENT_RECOVERY_RETRY);
    assert(dppd_management_handle(&control, &devices, NULL, &request, &response) == 0);
    assert(response.status == 0);
    assert(response.payload.recovery.state ==
           DPPD_CONTROL_RECOVERY_RESTART_REQUIRED);

    assert(dppd_control_fini(&control) == 0);
    assert(unlink(state_path) == 0);
    assert(rmdir(state_directory) == 0);
    return 0;
}
