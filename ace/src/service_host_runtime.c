/* Canonical ServiceHost runtime synchronization and resource admission.
 * Plugin loading and lifecycle authority remain in CMeta::Plugin. */
#include <cmeta/service_host.h>
#include <threads/threads.h>

#include <string.h>

/* Keep platform resource admission separate from graph/Plugin dependencies so
 * its failure path can be exercised with a test-owned platform adapter. */
salts_service_host_status salts_service_host_runtime_init(
    salts_service_host_runtime *runtime) {
    if (runtime == NULL || runtime->initialized)
        return SALTS_SERVICE_HOST_INVALID_ARGUMENT;

    memset(runtime, 0, sizeof(*runtime));
    sync_mutex_init((sync_mutex_t *)&runtime->lock);
    if (runtime->lock == NULL)
        return SALTS_SERVICE_HOST_RESOURCE_ERROR;
    runtime->initialized = true;
    return SALTS_SERVICE_HOST_OK;
}

salts_service_host_status salts_service_host_runtime_destroy(
    salts_service_host_runtime *runtime) {
    if (runtime == NULL || !runtime->initialized)
        return SALTS_SERVICE_HOST_INVALID_ARGUMENT;

    sync_mutex_lock((sync_mutex_t *)&runtime->lock);
    if (runtime->current != NULL ||
        runtime->active_scopes != 0u ||
        runtime->attached_generations != 0u) {
        sync_mutex_unlock((sync_mutex_t *)&runtime->lock);
        return SALTS_SERVICE_HOST_BUSY;
    }
    sync_mutex_unlock((sync_mutex_t *)&runtime->lock);

    sync_mutex_destroy((sync_mutex_t *)&runtime->lock);
    memset(runtime, 0, sizeof(*runtime));
    return SALTS_SERVICE_HOST_OK;
}
