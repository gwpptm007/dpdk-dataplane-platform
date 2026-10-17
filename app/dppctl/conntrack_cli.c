#include "conntrack_cli.h"

#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int number(const char *text, uint64_t *value)
{
    if (*text < '0' || *text > '9')
        return -EINVAL;
    char *end;
    errno = 0;
    *value = strtoull(text, &end, 10);
    return errno == 0 && *end == '\0' ? 0 : -EINVAL;
}

/** 查询可从当前集合开始，清空必须携带精确修订号，不能误删查询后新出现的会话 */
int dppctl_conntrack_build(int argc, char **argv, struct dppd_management_request *request,
    void (*initialize)(struct dppd_management_request *, enum dppd_management_operation))
{
    if (argc == 0)
        return -ENOSYS;
    enum dppd_management_operation operation;
    if (strcmp(argv[0], "conntrack-status") == 0)
        operation = DPPD_MANAGEMENT_CONNTRACK_STATUS;
    else if (strcmp(argv[0], "conntrack-list") == 0)
        operation = DPPD_MANAGEMENT_CONNTRACK_LIST;
    else if (strcmp(argv[0], "conntrack-flush") == 0)
        operation = DPPD_MANAGEMENT_CONNTRACK_FLUSH;
    else if (strcmp(argv[0], "nat-status") == 0)
        operation = DPPD_MANAGEMENT_NAT_STATUS;
    else
        return -ENOSYS;
    initialize(request, operation);
    if (operation == DPPD_MANAGEMENT_CONNTRACK_FLUSH)
        return argc == 2 && number(argv[1], &request->payload.conntrack.expected_revision) == 0 &&
                request->payload.conntrack.expected_revision != 0
            ? 0
            : -EINVAL;
    if (operation == DPPD_MANAGEMENT_CONNTRACK_LIST) {
        if (argc > 3 || (argc >= 2 && number(argv[1], &request->payload.conntrack.after_id) != 0) ||
            (argc == 3 &&
                (number(argv[2], &request->payload.conntrack.expected_revision) != 0 ||
                    request->payload.conntrack.expected_revision == 0)))
            return -EINVAL;
        return 0;
    }
    return argc == 1 ? 0 : -EINVAL;
}

static void print_tuple(const char *label, const struct dppd_flow_tuple *tuple)
{
    char source[INET_ADDRSTRLEN], destination[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &tuple->source_be, source, sizeof(source));
    inet_ntop(AF_INET, &tuple->destination_be, destination, sizeof(destination));
    printf(" %s=%s:%u->%s:%u", label, source, ntohs(tuple->source_port_be), destination,
        ntohs(tuple->destination_port_be));
}

bool dppctl_conntrack_print(const struct dppd_management_response *response)
{
    if (response->operation < DPPD_MANAGEMENT_CONNTRACK_STATUS || response->operation > DPPD_MANAGEMENT_NAT_STATUS)
        return false;
    if (response->operation == DPPD_MANAGEMENT_CONNTRACK_LIST) {
        const struct dppd_connection_page *page = &response->payload.connections;
        printf("conntrack revision=%" PRIu64 " count=%u next-id=%" PRIu64 " more=%s\n", page->revision, page->count,
            page->next_id, page->more ? "yes" : "no");
        static const char *states[] = {"none", "syn", "syn-ack", "open", "fin", "closed"};
        for (uint32_t i = 0; i < page->count; ++i) {
            const struct dppd_connection_view *entry = &page->entries[i];
            printf("session id=%" PRIu64 " protocol=%u state=%s tcp=%s ingress=%u egress=%u nat=%s", entry->id,
                entry->original.protocol, entry->seen_reply ? "established" : "new",
                entry->tcp_state <= DPPD_TCP_CLOSED ? states[entry->tcp_state] : "unknown", entry->ingress,
                entry->egress, entry->nat ? "yes" : "no");
            print_tuple("original", &entry->original);
            print_tuple("translated", &entry->translated);
            printf(" packets=%" PRIu64 "/%" PRIu64 " bytes=%" PRIu64 "/%" PRIu64 " last-seen-ms=%" PRIu64
                   " expires-ms=%" PRIu64 "\n",
                entry->packets[0], entry->packets[1], entry->bytes[0], entry->bytes[1], entry->last_seen_ms,
                entry->expires_ms);
        }
    } else {
        const struct dppd_conntrack_status *status = &response->payload.conntrack;
        printf("conntrack enabled=%s revision=%" PRIu64 " active=%u capacity=%u peak=%u timeout=%u"
               " created=%" PRIu64 " expired=%" PRIu64 " flushed=%" PRIu64 " invalid=%" PRIu64 " table-full=%" PRIu64
               " pool-full=%" PRIu64 "\n",
            status->enabled ? "yes" : "no", status->revision, status->active, status->capacity, status->peak,
            status->timeout_s, status->created, status->expired, status->flushed, status->invalid, status->table_full,
            status->pool_full);
        char address[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &status->snat.address_be, address, sizeof(address));
        printf("nat enabled=%s inside=%u outside=%u address=%s ports=%u-%u sessions-persistent=no\n",
            status->snat.enabled ? "yes" : "no", status->snat.inside_port, status->snat.outside_port, address,
            status->snat.first_port, status->snat.last_port);
    }
    return true;
}
