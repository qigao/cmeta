/* Canonical Salts ServiceHost: the sole Plugin generation and Scope owner.
 * Public installed ABI epoch is selected through service_host_abi.h. */
#ifndef SALTS_SERVICE_HOST_H
#define SALTS_SERVICE_HOST_H

#include <cmeta/service_host_provider_abi.h>
#include <cmeta/configurator.h>
#include <cmeta/plugin.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SALTS_SERVICE_HOST_INDEX_NONE SIZE_MAX
#define SALTS_SERVICE_HOST_MAX_ATTACHED_GENERATIONS 2u
#define SALTS_SERVICE_HOST_GENERATION_INIT {0}
#define SALTS_SERVICE_HOST_RUNTIME_INIT {0}
#define SALTS_SERVICE_HOST_SCOPE_INIT {0}

typedef enum salts_service_host_status {
    SALTS_SERVICE_HOST_OK = 0,
    SALTS_SERVICE_HOST_INVALID_ARGUMENT,
    SALTS_SERVICE_HOST_CAPACITY_EXCEEDED,
    SALTS_SERVICE_HOST_PLUGIN_ERROR,
    SALTS_SERVICE_HOST_PROVIDER_ERROR,
    SALTS_SERVICE_HOST_CONFIGURATOR_ERROR,
    SALTS_SERVICE_HOST_BUSY,
    SALTS_SERVICE_HOST_INVALID_STATE,
    SALTS_SERVICE_HOST_RESOURCE_ERROR
} salts_service_host_status;

typedef enum salts_service_host_generation_state {
    SALTS_SERVICE_HOST_GENERATION_ZERO = 0,
    SALTS_SERVICE_HOST_GENERATION_BUILT,
    SALTS_SERVICE_HOST_GENERATION_PUBLISHED,
    SALTS_SERVICE_HOST_GENERATION_DRAINING,
    SALTS_SERVICE_HOST_GENERATION_STOPPING,
    SALTS_SERVICE_HOST_GENERATION_FAILED,
    SALTS_SERVICE_HOST_GENERATION_DRAINED
} salts_service_host_generation_state;

typedef struct salts_service_host_source {
    cmeta_plugin_ref plugin;
    const char *export_id;
    const cmeta_data_desc *config_data;
    const void *config_value;
} salts_service_host_source;

typedef struct salts_service_host_module {
    cmeta_plugin_ref plugin;
    cmeta_plugin_lease lease;
    const cmeta_plugin_manifest *manifest;
} salts_service_host_module;

typedef struct salts_service_host_generation_storage {
    salts_configurator_deployment *deployments;
    size_t deployment_capacity;

    salts_configurator_instance *instances;
    size_t instance_capacity;

    salts_configurator_dependency *dependencies;
    size_t dependency_capacity;

    size_t *activation_order;
    size_t activation_capacity;

    salts_service_host_module *modules;
    size_t module_capacity;
} salts_service_host_generation_storage;

typedef struct salts_service_host_failure {
    size_t source_index;
    cmeta_plugin_status plugin_status;
    salts_configurator_status configurator_status;
} salts_service_host_failure;

typedef struct salts_service_host_runtime salts_service_host_runtime;

typedef struct salts_service_host_generation {
    uint64_t id;
    cmeta_plugin_registry *registry;
    salts_configurator_context components;

    salts_service_host_generation_storage storage;
    size_t deployment_count;
    /* Prefix length of retained module-owner records. On a partial cleanup
     * failure this shrinks as individual leases settle; only unsettled or
     * malformed ownership records remain in [0, module_count), and every slot
     * in [module_count, storage.module_capacity) is canonical zero. */
    size_t module_count;

    salts_service_host_runtime *runtime_owner;
    size_t active_scopes;

    salts_service_host_generation_state state;
    salts_service_host_failure failure;
} salts_service_host_generation;

typedef struct salts_service_host_scope {
    salts_service_host_runtime *runtime;
    salts_service_host_generation *generation;
    uint64_t generation_id;
    /* Diagnostic owner-address sentinel. A live scope is not copyable/movable.
     * The original object address alone authorizes use/release of its count.
     * This is not a security boundary against arbitrary caller corruption. */
    const struct salts_service_host_scope *owner_address;
    bool live;
} salts_service_host_scope;

/* The runtime, registry, generation bundles and their borrowed storage must
 * remain alive and address-stable until all scopes are released and attached
 * generations are drained. Do not copy/move a live scope; a copied scope is
 * rejected without changing its generation's admission count. One original
 * owner releases it exactly once. Services borrowed from it expire at release.
 * The owner-address field is part of the canonical ServiceHost epoch-2
 * layout. It is not compatible with any earlier unpublished prototype layout;
 * consumers must use the headers and library from one immutable SDK.
 *
 * After init, publish/close/acquire/release/drain synchronize runtime admission
 * and generation counts. Each scope is used by one thread at a time; provider
 * services retain their own concurrency contracts. Build/discard and reuse of
 * a generation require exclusive access to that bundle. Never inspect mutable
 * runtime/generation fields concurrently with these operations.
 *
 * Init/destroy require exclusive access to the runtime, including exclusion of
 * concurrent API calls. Destroy returns BUSY until close and drain complete. */

struct salts_service_host_runtime {
    void *lock;
    salts_service_host_generation *current;
    uint64_t last_generation_id;
    size_t active_scopes;
    size_t attached_generations;
    bool initialized;
};

const char *salts_service_host_status_string(
    salts_service_host_status status);

salts_service_host_status salts_service_host_generation_build(
    salts_service_host_generation *generation,
    uint64_t generation_id,
    cmeta_plugin_registry *registry,
    const salts_service_host_generation_storage *storage,
    const salts_configurator_deployment *static_deployments,
    size_t static_deployment_count,
    const salts_service_host_source *dynamic_sources,
    size_t dynamic_source_count,
    const salts_configurator_selection *selections,
    size_t selection_count);

salts_service_host_status salts_service_host_generation_discard(
    salts_service_host_generation *generation);

/* Retry cleanup for an unpublished FAILED generation after its owner has
 * repaired the underlying lease/ownership fault. Requires exclusive access,
 * no runtime owner or active scopes, and a ZERO, fully rolled-back FAILED, or
 * STOPPED Configurator context with activation_count == 0.
 *
 * Only remaining authoritative Plugin leases are retried; this function never
 * starts/stops a live graph, loads/unloads a DSO, or retries automatically.
 * On failure the generation stays FAILED and unsettled module records/storage
 * remain owned by the caller. On success ownership is settled and the
 * generation becomes DRAINED/reusable. Attached generations use generation_drain. */
salts_service_host_status salts_service_host_generation_recover(
    salts_service_host_generation *generation);

/* Initialize a zero-initialized runtime. RESOURCE_ERROR means the platform
 * mutex could not be created; the runtime remains zero and may be retried.
 * NULL or an already initialized runtime returns INVALID_ARGUMENT. */
salts_service_host_status salts_service_host_runtime_init(
    salts_service_host_runtime *runtime);

salts_service_host_status salts_service_host_runtime_destroy(
    salts_service_host_runtime *runtime);

salts_service_host_status salts_service_host_runtime_publish(
    salts_service_host_runtime *runtime,
    salts_service_host_generation *generation,
    salts_service_host_generation **out_previous);

/* Withdraws the currently published generation from admission and returns
 * it to the caller for draining; existing scope holders remain valid. A later
 * explicit publish() may re-open admission if generation IDs remain strictly
 * increasing and the two-attached-generation bound is satisfied. This is
 * distinct from final runtime_destroy(). */
salts_service_host_status salts_service_host_runtime_close(
    salts_service_host_runtime *runtime,
    salts_service_host_generation **out_previous);

salts_service_host_status salts_service_host_scope_acquire(
    salts_service_host_runtime *runtime,
    salts_service_host_scope *scope);

salts_service_host_status salts_service_host_scope_release(
    salts_service_host_scope *scope);

uint64_t salts_service_host_scope_generation_id(
    const salts_service_host_scope *scope);

salts_service_host_status salts_service_host_scope_find_service(
    const salts_service_host_scope *scope,
    const cmeta_interface_desc *interface_desc,
    salts_configurator_service *out_service);

salts_service_host_status salts_service_host_scope_find_service_from(
    const salts_service_host_scope *scope,
    const char *provider_component_id,
    const cmeta_interface_desc *interface_desc,
    salts_configurator_service *out_service);

salts_service_host_status salts_service_host_generation_drain(
    salts_service_host_runtime *runtime,
    salts_service_host_generation *generation);

const salts_service_host_failure *salts_service_host_generation_failure(
    const salts_service_host_generation *generation);

#ifdef __cplusplus
}
#endif

#endif /* SALTS_SERVICE_HOST_H */
