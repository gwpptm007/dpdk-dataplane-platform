#include "rss_conf.h"

struct dppd_rss_conf dppd_default_rss_conf(void)
{
    struct dppd_rss_conf conf;
    conf.rss_hf = 0;
    conf.nb_queues = 1;
    return conf;
}
