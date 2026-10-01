#include <assert.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include "dppd/runtime.h"
#include "dppd/management.h"

int main(void)
{
    struct dppd_runtime runtime = {0};
    struct dppd_control_service control;
    struct dppd_topology topology = {0};
    struct dppd_stats_values values, expected = {0};
    struct dppd_management_request request = {0};
    struct dppd_management_response response;

    runtime.devices.nb_ports = 2;
    runtime.devices.ports[0].port_id = 5;
    runtime.devices.ports[1].port_id = 9;
    runtime.nb_workers = 2;
    for (uint16_t i = 0; i < 2; ++i) {
        runtime.workers[i].queue_id = (uint16_t)(i * 2);
        dppd_stats_init(&runtime.workers[i].stats);
        for (uint16_t j = 0; j < 2; ++j) {
            uint64_t sample = (uint64_t)(1 + i * 2 + j);
            struct dppd_stats_values delta = {0};

#define FILL_FIELD(field) delta.field = sample;
            DPPD_STATS_FIELDS(FILL_FIELD)
#undef FILL_FIELD
            dppd_stats_init(&runtime.workers[i].port_stats[j]);
            dppd_stats_add(&runtime.workers[i].port_stats[j], &delta);
            dppd_stats_add(&runtime.workers[i].stats, &delta);
            dppd_stats_accumulate(&expected, &delta);
        }
    }
    assert(dppd_runtime_stats_query(&runtime, DPPD_STATS_ALL, DPPD_STATS_ALL, &values) == 0);
    assert(memcmp(&values, &expected, sizeof(values)) == 0);
    dppd_runtime_stats_read(&runtime, &values);
    assert(memcmp(&values, &expected, sizeof(values)) == 0);
    assert(dppd_runtime_stats_query(&runtime, 9, 2, &values) == 0);
    assert(values.rx_packets == 4 && values.tx_queue_drops == 4);
    assert(dppd_runtime_stats_query(&runtime, 5, DPPD_STATS_ALL, &values) == 0);
    assert(values.rx_packets == 4);
    assert(dppd_runtime_stats_query(&runtime, DPPD_STATS_ALL, 2, &values) == 0);
    assert(values.rx_packets == 7);
    assert(dppd_runtime_stats_query(&runtime, 0, 0, &values) == -ENOENT);
    assert(dppd_runtime_stats_query(&runtime, 5, 1, &values) == -ENOENT);
    assert(dppd_runtime_stats_query(NULL, 5, 0, &values) == -EINVAL);
    assert(dppd_control_init(&control, &topology, 2, NULL) == 0);
    request.version = DPPD_MANAGEMENT_VERSION;
    request.size = sizeof(request);
    request.operation = DPPD_MANAGEMENT_STATS_QUERY;
    request.payload.stats_query.port_id = 9;
    request.payload.stats_query.queue_id = 2;
    assert(dppd_management_handle(&control, &runtime.devices, &runtime,
                                  &request, &response) == 0);
    assert(response.status == 0 && response.payload.stats.rx_packets == 4);
    request.payload.stats_query.reserved = 1;
    assert(dppd_management_handle(&control, &runtime.devices, &runtime,
                                  &request, &response) == 0);
    assert(response.status == -EINVAL);
    request.payload.stats_query.reserved = 0;
    assert(dppd_management_handle(&control, &runtime.devices, NULL,
                                  &request, &response) == 0);
    assert(response.status == -ENODEV);
    request.version = 7;
    assert(dppd_management_handle(&control, &runtime.devices, &runtime,
                                  &request, &response) == 0);
    assert(response.status == -EPROTO);
    {
        struct dppd_management_server server;
        struct sockaddr_un address = {0};
        char path[100];
        int client = socket(AF_UNIX, SOCK_SEQPACKET, 0);

        assert(client >= 0);
        snprintf(path, sizeof(path), "/tmp/dppd-stats-%ld.sock", (long)getpid());
        assert(dppd_management_start(&server, &control, &runtime.devices,
                                     &runtime, path) == 0);
        address.sun_family = AF_UNIX;
        strcpy(address.sun_path, path);
        assert(connect(client, (struct sockaddr *)&address, sizeof(address)) == 0);
        request.version = DPPD_MANAGEMENT_VERSION;
        assert(send(client, &request, sizeof(request), 0) == sizeof(request));
        assert(dppd_management_poll(&server) == 0);
        assert(recv(client, &response, sizeof(response), 0) == sizeof(response));
        assert(response.status == 0 && response.payload.stats.rx_packets == 4);
        assert(response.payload.stats.tx_queue_drops == 4);
        assert(close(client) == 0);
        dppd_management_stop(&server);
        assert(access(path, F_OK) != 0);
    }
    assert(dppd_control_fini(&control) == 0);
    return 0;
}
