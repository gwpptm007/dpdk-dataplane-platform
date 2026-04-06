#ifndef DPPD_RSS_CONF_H
#define DPPD_RSS_CONF_H
#include <stdint.h>
struct dppd_rss_conf {
    uint64_t rss_hf;
    uint16_t nb_queues;
};
struct dppd_rss_conf dppd_default_rss_conf(void);
#endif
