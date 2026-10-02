#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <rte_ethdev.h>

int rte_eth_link_get_nowait(uint16_t port_id, struct rte_eth_link *link)
{
    const char *trigger = getenv("DPPD_TEST_LINK_FAULT_FILE");
    int (*real_query)(uint16_t, struct rte_eth_link *) = NULL;
    void *symbol;

    if (trigger != NULL && access(trigger, F_OK) == 0) {
        const char *error = getenv("DPPD_TEST_LINK_ERROR");

        return error != NULL && strcmp(error, "ENODEV") == 0 ? -ENODEV : -EIO;
    }
    symbol = dlsym(RTLD_NEXT, "rte_eth_link_get_nowait");
    _Static_assert(sizeof(real_query) == sizeof(symbol), "function pointer size");
    memcpy(&real_query, &symbol, sizeof(real_query));
    return real_query == NULL ? -ENOSYS : real_query(port_id, link);
}
