#ifndef SALTS_SERVICE_HOST_RUNTIME_FAILURE_ADAPTER_H
#define SALTS_SERVICE_HOST_RUNTIME_FAILURE_ADAPTER_H

#include <thread.h>

void salts_service_host_test_mutex_init(cmeta_mutex_t *mutex);
void salts_service_host_test_mutex_destroy(cmeta_mutex_t *mutex);
void salts_service_host_test_mutex_lock(cmeta_mutex_t *mutex);
void salts_service_host_test_mutex_unlock(cmeta_mutex_t *mutex);

#endif
