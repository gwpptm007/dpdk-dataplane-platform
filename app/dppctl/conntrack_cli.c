#include "conntrack_cli.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int number(const char *text, uint64_t *value)
{
    if (*text < '0' || *text > '9')
        return -EINVAL;
    char *end;
    errno = 0;
    *value = strtoull(text, &end, 10);
    return errno == 0 && *end == '\0' ? 0 : -EINVAL;
}

/** 端口编号和协议端口都用十进制，先检查范围再缩窄，避免大整数被截成另一项配置 */
static int port_number(const char *text, uint16_t *value, bool device)
{
    uint64_t parsed;
    if (number(text, &parsed) != 0 || parsed > (device ? UINT16_MAX - 1U : UINT16_MAX))
        return -EINVAL;
    *value = (uint16_t)parsed;
    return 0;
}

static int policy_line(char **words, unsigned int count, struct dppd_conntrack_policy *policy, bool *header)
{
    if (strcmp(words[0], "conntrack") == 0) {
        uint64_t capacity, timeout;
        if (*header || count != 3 || number(words[1], &capacity) != 0 || capacity > 65536 ||
            number(words[2], &timeout) != 0 || timeout > 86400 || (capacity == 0 ? timeout != 0 : timeout == 0))
            return -EINVAL;
        policy->capacity = (uint32_t)capacity;
        policy->timeout_s = (uint32_t)timeout;
        *header = true;
        return 0;
    }
    bool ipv6 = strcmp(words[0], "snat6") == 0 || strcmp(words[0], "dnat6") == 0;
    if (strcmp(words[0], "snat") == 0 || strcmp(words[0], "snat6") == 0) {
        struct dppd_snat_config *nat = ipv6 ? &policy->snat6 : &policy->snat;
        if (count != 6 || nat->enabled || port_number(words[1], &nat->inside_port, true) != 0 ||
            port_number(words[2], &nat->outside_port, true) != 0 || nat->inside_port == nat->outside_port ||
            inet_pton(ipv6 ? AF_INET6 : AF_INET, words[3], ipv6 ? (void *)nat->address6 : (void *)&nat->address_be) != 1 ||
            port_number(words[4], &nat->first_port, false) != 0 || port_number(words[5], &nat->last_port, false) != 0 ||
            nat->first_port == 0 || nat->first_port > nat->last_port)
            return -EINVAL;
        nat->enabled = true;
        nat->ipv6 = ipv6;
        return 0;
    }
    if (strcmp(words[0], "dnat") != 0 && strcmp(words[0], "dnat6") != 0)
        return -EINVAL;
    if (count != 9 || policy->dnat_count == DPPD_MAX_DNAT_RULES)
        return -EINVAL;
    struct dppd_dnat_rule *rule = &policy->dnat[policy->dnat_count];
    rule->ipv6 = ipv6;
    rule->protocol = strcmp(words[1], "tcp") == 0 ? 6 : strcmp(words[1], "udp") == 0 ? 17 : 0;
    if (rule->protocol == 0 || port_number(words[2], &rule->outside_port, true) != 0 ||
        port_number(words[3], &rule->inside_port, true) != 0 || rule->outside_port == rule->inside_port ||
        inet_pton(ipv6 ? AF_INET6 : AF_INET, words[4],
            ipv6 ? (void *)rule->public_address6 : (void *)&rule->public_address_be) != 1 ||
        port_number(words[5], &rule->public_first, false) != 0 ||
        port_number(words[6], &rule->public_last, false) != 0 ||
        inet_pton(ipv6 ? AF_INET6 : AF_INET, words[7],
            ipv6 ? (void *)rule->private_address6 : (void *)&rule->private_address_be) != 1 ||
        port_number(words[8], &rule->private_first, false) != 0 || rule->public_first > rule->public_last ||
        (uint32_t)rule->private_first + rule->public_last - rule->public_first > UINT16_MAX ||
        (rule->public_first == 0 ? rule->public_last != UINT16_MAX || rule->private_first != 0 : rule->private_first == 0))
        return -EINVAL;
    ++policy->dnat_count;
    return 0;
}

/** 整策略文件先完整读入再解析，普通文件、大小和行长度都有限制，出错不能发送半份策略 */
int dppctl_conntrack_policy_read(const char *path, struct dppd_conntrack_policy *policy)
{
    if (path == NULL || policy == NULL)
        return -EINVAL;
    memset(policy, 0, sizeof(*policy));
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0)
        return -errno;
    struct stat info;
    int rc = fstat(fd, &info) == 0 ? 0 : -errno;
    const size_t maximum = 16384;
    if (rc == 0 && (!S_ISREG(info.st_mode) || info.st_size > (off_t)maximum))
        rc = -EINVAL;
    char *text = rc == 0 ? malloc(maximum + 1) : NULL;
    if (rc == 0 && text == NULL)
        rc = -ENOMEM;
    size_t used = 0;
    while (rc == 0) {
        ssize_t bytes = read(fd, text + used, maximum + 1 - used);
        if (bytes < 0 && errno == EINTR)
            continue;
        if (bytes <= 0) {
            rc = bytes == 0 ? 0 : -errno;
            break;
        }
        used += (size_t)bytes;
        if (used > maximum)
            rc = -E2BIG;
    }
    close(fd);
    if (rc != 0 || memchr(text, '\0', used) != NULL) {
        free(text);
        return rc != 0 ? rc : -EINVAL;
    }
    text[used] = '\0';
    bool header = false;
    char *lines = NULL;
    for (char *line = strtok_r(text, "\n", &lines); line != NULL; line = strtok_r(NULL, "\n", &lines)) {
        if (strlen(line) > 512) {
            rc = -EINVAL;
            break;
        }
        char *comment = strchr(line, '#');
        if (comment != NULL)
            *comment = '\0';
        char *words[9], *save = NULL, *word = strtok_r(line, " \t\r", &save);
        unsigned int count = 0;
        while (word != NULL && count < 9) {
            words[count++] = word;
            word = strtok_r(NULL, " \t\r", &save);
        }
        if (count == 0)
            continue;
        rc = word != NULL ? -EINVAL : policy_line(words, count, policy, &header);
        if (rc != 0)
            break;
    }
    free(text);
    if (rc == 0 && (!header || (policy->capacity == 0 &&
                                  (policy->snat.enabled || policy->snat6.enabled || policy->dnat_count != 0))))
        rc = -EINVAL;
    return rc;
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
    else if (strcmp(argv[0], "conntrack-policy-show") == 0)
        operation = DPPD_MANAGEMENT_CONNTRACK_POLICY_GET;
    else if (strcmp(argv[0], "conntrack-list") == 0)
        operation = DPPD_MANAGEMENT_CONNTRACK_LIST;
    else if (strcmp(argv[0], "conntrack-flush") == 0)
        operation = DPPD_MANAGEMENT_CONNTRACK_FLUSH;
    else if (strcmp(argv[0], "nat-status") == 0)
        operation = DPPD_MANAGEMENT_NAT_STATUS;
    else if (strcmp(argv[0], "dnat-list") == 0)
        operation = DPPD_MANAGEMENT_DNAT_LIST;
    else if (strcmp(argv[0], "nd6-status") == 0)
        operation = DPPD_MANAGEMENT_ND6_STATUS;
    else if (strcmp(argv[0], "nd6-list") == 0)
        operation = DPPD_MANAGEMENT_ND6_LIST;
    else if (strcmp(argv[0], "nd6-flush") == 0)
        operation = DPPD_MANAGEMENT_ND6_FLUSH;
    else
        return -ENOSYS;
    initialize(request, operation);
    if (operation == DPPD_MANAGEMENT_DNAT_LIST)
        return argc == 1 || (argc == 2 && number(argv[1], &request->payload.conntrack.after_id) == 0) ? 0 : -EINVAL;
    if (operation == DPPD_MANAGEMENT_CONNTRACK_FLUSH || operation == DPPD_MANAGEMENT_ND6_FLUSH) {
        if (argc < 2 || argc > (operation == DPPD_MANAGEMENT_CONNTRACK_FLUSH ? 3 : 2) ||
            number(argv[1], &request->payload.conntrack.expected_revision) != 0 ||
            request->payload.conntrack.expected_revision == 0 ||
            (argc == 3 && (number(argv[2], &request->payload.conntrack.expected_epoch) != 0 ||
                             request->payload.conntrack.expected_epoch == 0)))
            return -EINVAL;
        return 0;
    }
    if (operation == DPPD_MANAGEMENT_CONNTRACK_LIST || operation == DPPD_MANAGEMENT_ND6_LIST) {
        if (argc > (operation == DPPD_MANAGEMENT_CONNTRACK_LIST ? 4 : 3) ||
            (argc >= 2 && number(argv[1], &request->payload.conntrack.after_id) != 0) ||
            (argc >= 3 &&
                (number(argv[2], &request->payload.conntrack.expected_revision) != 0 ||
                    request->payload.conntrack.expected_revision == 0)) ||
            (argc == 4 && (number(argv[3], &request->payload.conntrack.expected_epoch) != 0 ||
                             request->payload.conntrack.expected_epoch == 0)))
            return -EINVAL;
        return 0;
    }
    return argc == 1 ? 0 : -EINVAL;
}

static void print_tuple(const char *label, const struct dppd_flow_tuple *tuple)
{
    char source[INET6_ADDRSTRLEN], destination[INET6_ADDRSTRLEN];
    inet_ntop(tuple->ipv6 ? AF_INET6 : AF_INET,
        tuple->ipv6 ? (const void *)tuple->source6 : (const void *)&tuple->source_be, source, sizeof(source));
    inet_ntop(tuple->ipv6 ? AF_INET6 : AF_INET,
        tuple->ipv6 ? (const void *)tuple->destination6 : (const void *)&tuple->destination_be, destination,
        sizeof(destination));
    if (tuple->ipv6) {
        printf(" %s=[%s]:%u->[%s]:%u", label, source, ntohs(tuple->source_port_be), destination,
            ntohs(tuple->destination_port_be));
        return;
    }
    printf(" %s=%s:%u->%s:%u", label, source, ntohs(tuple->source_port_be), destination,
        ntohs(tuple->destination_port_be));
}

/** 动态缓存单独查询，不混入持久静态邻居；修订号只属于本次进程的易失状态 */
static bool nd6_print(const struct dppd_management_response *response)
{
    if (response->operation < DPPD_MANAGEMENT_ND6_STATUS || response->operation > DPPD_MANAGEMENT_ND6_FLUSH)
        return false;
    if (response->operation == DPPD_MANAGEMENT_ND6_STATUS) {
        const struct dppd_nd6_status *status = &response->payload.nd6;
        printf("nd6 enabled=%s revision=%" PRIu64 " active=%u pending=%u local-count=%u reachable-ms=%u"
               " solicitations=%" PRIu64 " advertisements=%" PRIu64 " invalid=%" PRIu64 " tx-errors=%" PRIu64
               " queue-drops=%" PRIu64 " resolved=%" PRIu64 " failed=%" PRIu64 " persistent=no\n",
            status->enabled ? "yes" : "no", status->revision, status->active, status->pending, status->local_count,
            status->reachable_ms, status->solicitations, status->advertisements, status->invalid, status->tx_errors,
            status->queue_drops, status->resolved, status->failed);
        for (uint32_t i = 0; i < status->local_count; ++i) {
            char address[INET6_ADDRSTRLEN];
            const struct dppd_nd6_local *local = &status->locals[i].configuration;
            inet_ntop(AF_INET6, local->address, address, sizeof(address));
            printf("local port=%u address=%s state=%s vlan-depth=%u vlan=%u/%u\n", local->port_id, address,
                status->locals[i].state == DPPD_ND6_PREFERRED       ? "preferred"
                    : status->locals[i].state == DPPD_ND6_DUPLICATE ? "duplicate"
                                                                    : "tentative",
                local->vlan_depth, local->vlan[0], local->vlan[1]);
        }
    } else if (response->operation == DPPD_MANAGEMENT_ND6_LIST) {
        const struct dppd_nd6_page *page = &response->payload.dynamic_neighbors;
        static const char *states[] = {"none", "incomplete", "reachable", "stale", "delay", "probe", "failed"};
        printf("nd6 revision=%" PRIu64 " count=%u next-id=%" PRIu64 " more=%s\n", page->revision, page->count,
            page->next_id, page->more ? "yes" : "no");
        for (uint32_t i = 0; i < page->count; ++i) {
            const struct dppd_nd6_entry *entry = &page->entries[i];
            char address[INET6_ADDRSTRLEN];
            inet_ntop(AF_INET6, entry->address, address, sizeof(address));
            printf("neighbor id=%" PRIu64 " port=%u address=%s mac=%02x:%02x:%02x:%02x:%02x:%02x state=%s probes=%u"
                   " changed-ms=%" PRIu64 " due-ms=%" PRIu64 " used-ms=%" PRIu64 "\n",
                entry->id, entry->port_id, address, entry->mac[0], entry->mac[1], entry->mac[2], entry->mac[3],
                entry->mac[4], entry->mac[5], entry->state <= DPPD_ND6_FAILED ? states[entry->state] : "unknown",
                entry->probes, entry->changed_ms, entry->due_ms, entry->used_ms);
        }
    } else
        puts("nd6 cache flushed");
    return true;
}

/** 完整查询按文件语法列出策略，各版本放在注释行，复制保存后仍能被整策略命令读取 */
static void policy_print(const struct dppd_flow_conntrack_view *view)
{
    const struct dppd_conntrack_policy *policy = &view->policy;
    printf("# generation=%" PRIu64 " revision=%" PRIu64 " durable-revision=%" PRIu64
           " session-epoch=%" PRIu64 " session-revision=%" PRIu64 "\nconntrack %u %u\n",
        view->generation, view->revision, view->durable_revision, view->session_epoch, view->session_revision,
        policy->capacity, policy->timeout_s);
    for (unsigned int family = 0; family < 2; ++family) {
        const struct dppd_snat_config *nat = family == 0 ? &policy->snat : &policy->snat6;
        if (!nat->enabled)
            continue;
        char address[INET6_ADDRSTRLEN] = {0};
        inet_ntop(family == 0 ? AF_INET : AF_INET6,
            family == 0 ? (const void *)&nat->address_be : (const void *)nat->address6, address, sizeof(address));
        printf("%s %u %u %s %u %u\n", family == 0 ? "snat" : "snat6", nat->inside_port, nat->outside_port,
            address, nat->first_port, nat->last_port);
    }
    for (uint32_t i = 0; i < policy->dnat_count && i < DPPD_MAX_DNAT_RULES; ++i) {
        const struct dppd_dnat_rule *rule = &policy->dnat[i];
        char public[INET6_ADDRSTRLEN] = {0}, private[INET6_ADDRSTRLEN] = {0};
        inet_ntop(rule->ipv6 ? AF_INET6 : AF_INET,
            rule->ipv6 ? (const void *)rule->public_address6 : (const void *)&rule->public_address_be, public,
            sizeof(public));
        inet_ntop(rule->ipv6 ? AF_INET6 : AF_INET,
            rule->ipv6 ? (const void *)rule->private_address6 : (const void *)&rule->private_address_be, private,
            sizeof(private));
        printf("%s %s %u %u %s %u %u %s %u\n", rule->ipv6 ? "dnat6" : "dnat", rule->protocol == 6 ? "tcp" : "udp",
            rule->outside_port, rule->inside_port, public, rule->public_first, rule->public_last, private,
            rule->private_first);
    }
}

bool dppctl_conntrack_print(const struct dppd_management_response *response)
{
    if (nd6_print(response))
        return true;
    if (response->operation == DPPD_MANAGEMENT_CONNTRACK_POLICY_GET) {
        policy_print(&response->payload.conntrack_policy);
        return true;
    }
    if (response->operation < DPPD_MANAGEMENT_CONNTRACK_STATUS || response->operation > DPPD_MANAGEMENT_DNAT_LIST)
        return false;
    if (response->operation == DPPD_MANAGEMENT_DNAT_LIST) {
        const struct dppd_dnat_page *page = &response->payload.dnat;
        printf("dnat count=%u next-id=%u more=%s\n", page->count, page->next_id,
            page->more ? "yes" : "no");
        for (uint32_t i = 0; i < page->count; ++i) {
            const struct dppd_dnat_rule *rule = &page->entries[i].rule;
            char public[INET6_ADDRSTRLEN], private[INET6_ADDRSTRLEN];
            inet_ntop(rule->ipv6 ? AF_INET6 : AF_INET,
                rule->ipv6 ? (const void *)rule->public_address6 : (const void *)&rule->public_address_be, public,
                sizeof(public));
            inet_ntop(rule->ipv6 ? AF_INET6 : AF_INET,
                rule->ipv6 ? (const void *)rule->private_address6 : (const void *)&rule->private_address_be, private,
                sizeof(private));
            printf("dnat id=%u protocol=%s outside=%u inside=%u public=%s%s%s:%u-%u private=%s%s%s:%u-%u family=%s\n",
                page->entries[i].id, rule->protocol == 6 ? "tcp" : "udp", rule->outside_port, rule->inside_port,
                rule->ipv6 ? "[" : "", public, rule->ipv6 ? "]" : "", rule->public_first, rule->public_last,
                rule->ipv6 ? "[" : "", private, rule->ipv6 ? "]" : "", rule->private_first,
                rule->private_first + rule->public_last - rule->public_first, rule->ipv6 ? "ipv6" : "ipv4");
        }
    } else if (response->operation == DPPD_MANAGEMENT_CONNTRACK_LIST) {
        const struct dppd_connection_page *page = &response->payload.connections;
        printf("conntrack epoch=%" PRIu64 " revision=%" PRIu64 " count=%u next-id=%" PRIu64 " more=%s\n",
            page->epoch, page->revision, page->count, page->next_id, page->more ? "yes" : "no");
        static const char *states[] = {"none", "syn", "syn-ack", "open", "fin", "closed"};
        for (uint32_t i = 0; i < page->count; ++i) {
            const struct dppd_connection_view *entry = &page->entries[i];
            printf("session id=%" PRIu64 " protocol=%u state=%s tcp=%s ingress=%u egress=%u nat=%s", entry->id,
                entry->original.protocol, entry->seen_reply ? "established" : "new",
                entry->tcp_state <= DPPD_TCP_CLOSED ? states[entry->tcp_state] : "unknown", entry->ingress,
                entry->egress, entry->nat ? "yes" : "no");
            print_tuple("original", &entry->original);
            print_tuple("translated", &entry->translated);
            printf(" family=%s kind=%s dnat-rule=%u", entry->original.ipv6 ? "ipv6" : "ipv4",
                entry->dnat_rule_id != 0 ? "dnat"
                    : entry->nat         ? "snat"
                                         : "none",
                entry->dnat_rule_id);
            printf(" packets=%" PRIu64 "/%" PRIu64 " bytes=%" PRIu64 "/%" PRIu64 " last-seen-ms=%" PRIu64
                   " expires-ms=%" PRIu64 "\n",
                entry->packets[0], entry->packets[1], entry->bytes[0], entry->bytes[1], entry->last_seen_ms,
                entry->expires_ms);
        }
    } else {
        const struct dppd_conntrack_status *status = &response->payload.conntrack;
        printf("conntrack enabled=%s epoch=%" PRIu64 " revision=%" PRIu64 " active=%u capacity=%u peak=%u timeout=%u"
               " created=%" PRIu64 " expired=%" PRIu64 " flushed=%" PRIu64 " invalid=%" PRIu64 " table-full=%" PRIu64
               " pool-full=%" PRIu64 "\n",
            status->enabled ? "yes" : "no", status->epoch, status->revision, status->active, status->capacity, status->peak,
            status->timeout_s, status->created, status->expired, status->flushed, status->invalid, status->table_full,
            status->pool_full);
        char address[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &status->snat.address_be, address, sizeof(address));
        printf("nat enabled=%s snat=%s inside=%u outside=%u address=%s ports=%u-%u dnat-rules=%u"
               " sessions-persistent=no\n",
            status->snat.enabled || status->snat6.enabled || status->dnat_count != 0 ? "yes" : "no",
            status->snat.enabled ? "yes" : "no", status->snat.inside_port, status->snat.outside_port, address,
            status->snat.first_port, status->snat.last_port, status->dnat_count);
        char address6[INET6_ADDRSTRLEN];
        inet_ntop(AF_INET6, status->snat6.address6, address6, sizeof(address6));
        printf("nat6 snat=%s inside=%u outside=%u address=%s ports=%u-%u sessions-persistent=no\n",
            status->snat6.enabled ? "yes" : "no", status->snat6.inside_port, status->snat6.outside_port, address6,
            status->snat6.first_port, status->snat6.last_port);
    }
    return true;
}
