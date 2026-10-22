#ifndef DPPD_FLOW_REQUEST_IO_H
#define DPPD_FLOW_REQUEST_IO_H

#include "dppd/flow_request.h"

#define DPPD_REQUEST_CLIENTS 32U
#define DPPD_REQUEST_RECEIPTS 128U
#define DPPD_REQUEST_BYTES 8192U

struct dppd_flow_request_record {
    struct dppd_flow_request_result result;
    uint32_t length;
    uint8_t contents[DPPD_REQUEST_BYTES];
};

/** 客户端水位不随回执回收而删除，旧键永远不会被误认成新的写请求 */
struct dppd_flow_request_ledger {
    struct dppd_flow_request_key clients[DPPD_REQUEST_CLIENTS];
    struct dppd_flow_request_record receipts[DPPD_REQUEST_RECEIPTS];
    uint32_t cursor;
};

int dppd_flow_request_encode(const struct dppd_flow_request *request, uint8_t *data, uint32_t *length);
int dppd_flow_request_ledger_validate(const struct dppd_flow_request_ledger *ledger, uint64_t revision);

#endif
