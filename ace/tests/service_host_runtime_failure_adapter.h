#ifndef SALTS_SERVICE_HOST_RUNTIME_FAILURE_ADAPTER_H
#define SALTS_SERVICE_HOST_RUNTIME_FAILURE_ADAPTER_H

#include <threads/threads.h>

void salts_service_host_test_mutex_init(sync_mutex_t *mutex);
void salts_service_host_test_mutex_destroy(sync_mutex_t *mutex);
void salts_service_host_test_mutex_lock(sync_mutex_t *mutex);
void salts_service_host_test_mutex_unlock(sync_mutex_t *mutex);

#endif
