#ifndef SALTS_CONFIGURATOR_ABI_H
#define SALTS_CONFIGURATOR_ABI_H

#include <cmeta/component.h>
#include <cmeta/object_interface.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SALTS_CONFIGURATOR_INDEX_NONE SIZE_MAX
#define SALTS_CONFIGURATOR_PROVIDER_BINDING_ABI_VERSION UINT32_C(2)

#if defined(_WIN32)
#define SALTS_CONFIGURATOR_CALL __cdecl
#else
#define SALTS_CONFIGURATOR_CALL
#endif

typedef enum salts_configurator_status {
    SALTS_CONFIGURATOR_OK = 0,
    SALTS_CONFIGURATOR_INVALID_ARGUMENT,
    SALTS_CONFIGURATOR_INVALID_COMPONENT,
    SALTS_CONFIGURATOR_DUPLICATE_COMPONENT_ID,
    SALTS_CONFIGURATOR_INVALID_SELECTION,
    SALTS_CONFIGURATOR_CONFIG_MISMATCH,
    SALTS_CONFIGURATOR_CAPACITY_EXCEEDED,
    SALTS_CONFIGURATOR_MISSING_PROVIDER,
    SALTS_CONFIGURATOR_AMBIGUOUS_PROVIDER,
    SALTS_CONFIGURATOR_DEPENDENCY_CYCLE,
    SALTS_CONFIGURATOR_INTERFACE_UNAVAILABLE,
    SALTS_CONFIGURATOR_CREATE_FAILED,
    SALTS_CONFIGURATOR_ACTIVATE_FAILED,
    SALTS_CONFIGURATOR_INVALID_STATE
} salts_configurator_status;

typedef struct salts_configurator_dependency salts_configurator_dependency;

/* Failure ownership:
 * A valid partially returned ObjectRef is released when create() fails.
 * On activate() failure, Configurator releases that ObjectRef but does not call
 * deactivate() on the failed instance. Any partial external side effects must
 * either be undone by activate() before returning an error or be covered by
 * the owned ObjectRef's destroy authority. Only a successfully activated
 * instance receives deactivate() (including a subsequent PROVIDES preflight
 * failure), and owned resources must settle exactly once. */
typedef cmeta_status (SALTS_CONFIGURATOR_CALL *salts_configurator_create_fn)(
    void *provider_context,
    const cmeta_data_desc *config_data,
    const void *config_value,
    const salts_configurator_dependency *dependencies,
    size_t dependency_count,
    cmeta_object_ref *out_instance);

typedef cmeta_status (SALTS_CONFIGURATOR_CALL *salts_configurator_activate_fn)(
    void *provider_context,
    const cmeta_object_ref *instance);

typedef void (SALTS_CONFIGURATOR_CALL *salts_configurator_deactivate_fn)(
    void *provider_context,
    const cmeta_object_ref *instance);

typedef struct salts_configurator_provider_binding {
    size_t struct_size;
    uint32_t abi_version;

    const cmeta_component_desc *component;
    void *provider_context;

    /*
     * Provider-authorized Interface projection. Required when component
     * publishes at least one Interface. Dynamic bindings and everything
     * reachable from them remain borrowed under the enclosing module lease.
     */
    const cmeta_object_interface_provider *interfaces;

    salts_configurator_create_fn create;
    salts_configurator_activate_fn activate;
    salts_configurator_deactivate_fn deactivate;
} salts_configurator_provider_binding;

static inline bool salts_configurator_provider_binding_valid(
    const salts_configurator_provider_binding *binding) {
    size_t i;
    bool provides = false;

    if (binding == NULL ||
        binding->struct_size != sizeof(*binding) ||
        binding->abi_version != SALTS_CONFIGURATOR_PROVIDER_BINDING_ABI_VERSION ||
        !cmeta_component_desc_valid(binding->component) ||
        binding->create == NULL ||
        ((binding->activate == NULL) != (binding->deactivate == NULL)))
        return false;

    if (binding->interfaces != NULL &&
        !cmeta_object_interface_provider_valid(binding->interfaces))
        return false;

    for (i = 0u; i < binding->component->capability_count; ++i) {
        if (binding->component->capabilities[i].role ==
            CMETA_COMPONENT_PROVIDES) {
            provides = true;
            break;
        }
    }

    return !provides ||
        cmeta_object_interface_provider_valid(binding->interfaces);
}

struct salts_configurator_dependency {
    size_t consumer_index;
    size_t provider_index;
    const cmeta_interface_desc *interface_desc;
    const cmeta_component_desc *provider_component;
    const cmeta_object_ref *provider_instance;
    const cmeta_object_interface_provider *provider_interfaces;
};

/*
 * ABI-only factory helper. Outputs are published only on one exact match.
 * It performs no registry lookup and owns no lifetime.
 */
static inline salts_configurator_status salts_configurator_dependency_find(
    const salts_configurator_dependency *dependencies,
    size_t dependency_count,
    const cmeta_interface_desc *interface_desc,
    const salts_configurator_dependency **out_dependency) {
    const salts_configurator_dependency *candidate = NULL;
    size_t candidate_count = 0u;
    size_t i;

    if (out_dependency == NULL || !cmeta_interface_desc_valid(interface_desc) ||
        (dependency_count != 0u && dependencies == NULL))
        return SALTS_CONFIGURATOR_INVALID_ARGUMENT;

    for (i = 0u; i < dependency_count; ++i) {
        if (cmeta_interface_desc_equal(
                dependencies[i].interface_desc, interface_desc)) {
            candidate = &dependencies[i];
            ++candidate_count;
        }
    }

    if (candidate_count == 0u)
        return SALTS_CONFIGURATOR_MISSING_PROVIDER;
    if (candidate_count != 1u)
        return SALTS_CONFIGURATOR_AMBIGUOUS_PROVIDER;

    *out_dependency = candidate;
    return SALTS_CONFIGURATOR_OK;
}

#ifdef __cplusplus
}
#endif

#endif /* SALTS_CONFIGURATOR_ABI_H */
