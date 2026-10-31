/* Canonical Salts Configurator: sole static Component dependency/lifecycle graph.
 * No Plugin generation or module lease is owned by this interface. */
#ifndef SALTS_CONFIGURATOR_H
#define SALTS_CONFIGURATOR_H

#include <salts/configurator_abi.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum salts_configurator_phase {
    SALTS_CONFIGURATOR_PHASE_NONE = 0,
    SALTS_CONFIGURATOR_PHASE_INIT,
    SALTS_CONFIGURATOR_PHASE_RESOLVE,
    SALTS_CONFIGURATOR_PHASE_DEPENDENCY,
    SALTS_CONFIGURATOR_PHASE_PROVIDE,
    SALTS_CONFIGURATOR_PHASE_CREATE,
    SALTS_CONFIGURATOR_PHASE_ACTIVATE
} salts_configurator_phase;

typedef enum salts_configurator_context_state {
    SALTS_CONFIGURATOR_CONTEXT_ZERO = 0,
    SALTS_CONFIGURATOR_CONTEXT_READY,
    SALTS_CONFIGURATOR_CONTEXT_RESOLVED,
    SALTS_CONFIGURATOR_CONTEXT_ACTIVE,
    SALTS_CONFIGURATOR_CONTEXT_FAILED,
    SALTS_CONFIGURATOR_CONTEXT_STOPPED,
    SALTS_CONFIGURATOR_CONTEXT_STARTING,
    SALTS_CONFIGURATOR_CONTEXT_STOPPING
} salts_configurator_context_state;

typedef struct salts_configurator_selection {
    const char *consumer_component_id;
    const cmeta_interface_desc *requirement;
    const char *provider_component_id;
} salts_configurator_selection;

/*
 * Host/deployment-owned configuration. Configuration is borrowed until
 * create() returns; providers that need it afterwards must copy/retain it
 * under their own semantics. This applies to both config_data and config_value;
 * the runtime does not read either construction input after create() returns.
 * Provider/component metadata is a separate borrow and remains alive until the
 * context is stopped (or failed construction/rollback is fully settled).
 */
typedef struct salts_configurator_deployment {
    const salts_configurator_provider_binding *provider;
    const cmeta_data_desc *config_data;
    const void *config_value;
} salts_configurator_deployment;

typedef struct salts_configurator_instance {
    cmeta_object_ref object;
    size_t dependency_offset;
    size_t dependency_count;
    bool active;
} salts_configurator_instance;

typedef struct salts_configurator_failure {
    salts_configurator_phase phase;
    size_t component_index;
    size_t dependency_index;
    cmeta_status provider_status;
} salts_configurator_failure;

typedef struct salts_configurator_service {
    const cmeta_component_desc *component;
    const cmeta_object_ref *object;
    const cmeta_object_interface_provider *interfaces;
} salts_configurator_service;

typedef struct salts_configurator_context {
    const salts_configurator_deployment *deployments;
    size_t deployment_count;

    const salts_configurator_selection *selections;
    size_t selection_count;

    salts_configurator_instance *instances;
    size_t instance_capacity;

    salts_configurator_dependency *dependencies;
    size_t dependency_capacity;
    size_t dependency_count;

    size_t *activation_order;
    size_t activation_capacity;
    size_t activation_count;

    salts_configurator_context_state state;
    salts_configurator_failure failure;
} salts_configurator_context;

/* Required on first use, in both C11 and C++17:
 * salts_configurator_context context = SALTS_CONFIGURATOR_CONTEXT_INIT;
 * Do not copy a context with live instances. Calls on one context require
 * external serialization; callback reentry is not concurrent-call support.
 * During create/activate/rollback callbacks the state is STARTING; during
 * normal deactivate/destroy it is STOPPING. Reentrant init/resolve/start/stop
 * and service lookup return INVALID_STATE without changing ownership. */
#define SALTS_CONFIGURATOR_CONTEXT_INIT {0}

const char *salts_configurator_status_string(salts_configurator_status status);

/* First admission requires an explicitly zero-initialized context.
 * Reinitialization from READY, RESOLVED or ACTIVE is rejected without
 * modifying the original context or any owned ObjectRef. A STOPPED context
 * (after successful stop) or FAILED context (after rollback) may be reused
 * exclusively once all borrowed service views have expired.
 *
 * Caller-owned provider/storage arrays must remain alive while a context
 * is READY, RESOLVED or ACTIVE. `instance_capacity` is the actual backing
 * capacity, not the active deployment count: on successful init every slot in
 * `instances[0..instance_capacity)` is reset, and unused tail slots remain
 * scratch storage with no live ObjectRefs. Fully settled INIT/RESOLVE failures,
 * completed rollback, and successful stop detach those borrowed addresses
 * before returning, so the caller may reclaim the arrays. If rollback cannot prove
 * that all ObjectRefs settled, FAILED retains the addresses and re-init is
 * rejected; the caller must preserve that backing storage.
 *
 * This contract applies to the canonical Configurator API: explicitly
 * initialize contexts before first use; never pass indeterminate automatic
 * storage or reset a context while it retains ObjectRefs. */
salts_configurator_status salts_configurator_context_init(
    salts_configurator_context *context,
    const salts_configurator_deployment *deployments,
    size_t deployment_count,
    const salts_configurator_selection *selections,
    size_t selection_count,
    salts_configurator_instance *instances,
    size_t instance_capacity,
    salts_configurator_dependency *dependencies,
    size_t dependency_capacity,
    size_t *activation_order,
    size_t activation_capacity);

salts_configurator_status salts_configurator_context_resolve(
    salts_configurator_context *context);

salts_configurator_status salts_configurator_context_start(
    salts_configurator_context *context);

salts_configurator_status salts_configurator_context_stop(
    salts_configurator_context *context);

/*
 * Control-plane service lookup. The returned view borrows the ACTIVE context.
 * Bind the exact typed Interface once and retain the appropriate outer scope;
 * do not route hot calls through repeated lookup.
 */
salts_configurator_status salts_configurator_context_find_service(
    const salts_configurator_context *context,
    const cmeta_interface_desc *interface_desc,
    salts_configurator_service *out_service);

/*
 * Explicit root/service selection for contexts that intentionally contain
 * multiple providers of the same Interface. No ranking or fallback occurs.
 */
salts_configurator_status salts_configurator_context_find_service_from(
    const salts_configurator_context *context,
    const char *provider_component_id,
    const cmeta_interface_desc *interface_desc,
    salts_configurator_service *out_service);

const salts_configurator_failure *salts_configurator_context_failure(
    const salts_configurator_context *context);

#ifdef __cplusplus
}
#endif

#endif /* SALTS_CONFIGURATOR_H */
