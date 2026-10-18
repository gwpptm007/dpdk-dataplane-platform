#include "native_flow_lock.h"
#include <pthread.h>

static pthread_mutex_t native_lock = PTHREAD_MUTEX_INITIALIZER;

void dppd_native_flow_lock(void)
{
    pthread_mutex_lock(&native_lock);
}

void dppd_native_flow_unlock(void)
{
    pthread_mutex_unlock(&native_lock);
}
