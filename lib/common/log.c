#include "log.h"
#include <stdarg.h>
#include <stdio.h>

void dppd_log_info(const char *fmt, ...)
{
    va_list ap;

    printf("[dppd] ");
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
}
