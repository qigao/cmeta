#include "service_host_publication_fixture.h"
#include <salts/component_plugin.h>
#include "tinytest.h"

/* The candidate and legacy hosts each own a DIFFERENT generation/context.
 * The ONE existing Plugin registry owns the common, real loaded provider DSO.
 * Prove neither host releases the other's module lease or borrowed scope. */
typedef struct legacy_publication_bundle {
    salts_component_plugin_generation generation;
    salts_component_deployment deployments[1];
    salts_component_instance instances[1];
    salts_component_dependency dependencies[1];
    size_t order[1];
    salts_component_plugin_module modules[1];
} legacy_publication_bundle;

static salts_component_plugin_status build_legacy_generation(
    legacy_publication_bundle *bundle, uint64_t id,
    cmeta_plugin_registry *registry, cmeta_plugin_ref ref) {
    const salts_component_plugin_generation_storage storage = {
        bundle->deployments, 1u,
        bundle->instances, 1u,
        bundle->dependencies, 1u,
        bundle->order, 1u,
        bundle->modules, 1u
    };
    const salts_component_plugin_source source = {
        ref, COMPONENT_PROVIDER_EXPORT_ID, NULL, NULL
    };
    return salts_component_plugin_generation_build(
        &bundle->generation, id, registry, &storage,
        NULL, 0u, &source, 1u, NULL, 0u);
}

static int legacy_scope_value(salts_component_plugin_scope *scope) {
    salts_component_service service;
    component_plugin_value value = component_plugin_value_bind(NULL, NULL);
    if (salts_component_plugin_scope_find_service(
            scope, component_plugin_value_interface(), &service) != SALTS_COMPONENT_PLUGIN_OK)
        return -1;
    if (component_plugin_value_borrow_from_object(
            service.object, service.interfaces, &value) != CMETA_OK)
        return -2;
    return component_plugin_value_get(&value);
}

suite("ComponentPlugin to ServiceHost DSO coexistence") {
    it("keeps two independent graph generations pinned until each scope releases") {
        const cmeta_plugin_registry_config config = {1};
        cmeta_plugin_registry registry = {0};
        cmeta_plugin_ref ref = {0};
        cmeta_plugin_lifecycle_info info = {0};
        legacy_publication_bundle legacy = {0};
        publication_generation_fixture candidate = {0};
        salts_component_plugin_runtime original_host = SALTS_COMPONENT_PLUGIN_RUNTIME_INIT;
        salts_service_host_runtime candidate_host = SALTS_SERVICE_HOST_RUNTIME_INIT;
        salts_component_plugin_scope original_scope = SALTS_COMPONENT_PLUGIN_SCOPE_INIT;
        salts_service_host_scope candidate_scope = SALTS_SERVICE_HOST_SCOPE_INIT;
        salts_component_plugin_generation *original_retired = NULL;
        salts_service_host_generation *candidate_retired = NULL;
        bool quiet = false;

        check_equal(cmeta_plugin_registry_init(&registry, &config), CMETA_PLUGIN_OK);
        check_equal(cmeta_plugin_registry_load(
            &registry, COMPONENT_PROVIDER_PLUGIN_PATH, &ref), CMETA_PLUGIN_OK);
        check_equal(cmeta_plugin_registry_start(&registry, ref), CMETA_PLUGIN_OK);

        check_equal(build_legacy_generation(
            &legacy, UINT64_C(4), &registry, ref), SALTS_COMPONENT_PLUGIN_OK);
        check_equal(build_generation(
            &candidate, UINT64_C(5), &registry, ref), SALTS_SERVICE_HOST_OK);
        check_equal(cmeta_plugin_registry_get_lifecycle(
            &registry, ref, &info), CMETA_PLUGIN_OK);
        check_equal(info.active_leases, (size_t)2u);

        check_equal(salts_component_plugin_runtime_init(
            &original_host), SALTS_COMPONENT_PLUGIN_OK);
        check_equal(salts_service_host_runtime_init(
            &candidate_host), SALTS_SERVICE_HOST_OK);
        check_equal(salts_component_plugin_runtime_publish(
            &original_host, &legacy.generation, &original_retired), SALTS_COMPONENT_PLUGIN_OK);
        check_null(original_retired);
        check_equal(salts_service_host_runtime_publish(
            &candidate_host, &candidate.generation, &candidate_retired), SALTS_SERVICE_HOST_OK);
        check_null(candidate_retired);
        check_equal(salts_component_plugin_scope_acquire(
            &original_host, &original_scope), SALTS_COMPONENT_PLUGIN_OK);
        check_equal(salts_service_host_scope_acquire(
            &candidate_host, &candidate_scope), SALTS_SERVICE_HOST_OK);
        check_equal(legacy_scope_value(&original_scope), COMPONENT_PROVIDER_VALUE);
        check_equal(scope_value(&candidate_scope), COMPONENT_PROVIDER_VALUE);

        check_equal(salts_component_plugin_runtime_close(
            &original_host, &original_retired), SALTS_COMPONENT_PLUGIN_OK);
        check_true(original_retired == &legacy.generation);
        check_equal(salts_service_host_runtime_close(
            &candidate_host, &candidate_retired), SALTS_SERVICE_HOST_OK);
        check_true(candidate_retired == &candidate.generation);
        check_equal(salts_component_plugin_generation_drain(
            &original_host, original_retired), SALTS_COMPONENT_PLUGIN_BUSY);
        check_equal(salts_service_host_generation_drain(
            &candidate_host, candidate_retired), SALTS_SERVICE_HOST_BUSY);
        check_equal(cmeta_plugin_registry_unload(&registry, ref), CMETA_PLUGIN_BUSY);

        check_equal(salts_component_plugin_scope_release(
            &original_scope), SALTS_COMPONENT_PLUGIN_OK);
        check_equal(salts_component_plugin_generation_drain(
            &original_host, original_retired), SALTS_COMPONENT_PLUGIN_OK);
        check_equal(cmeta_plugin_registry_get_lifecycle(
            &registry, ref, &info), CMETA_PLUGIN_OK);
        check_equal(info.active_leases, (size_t)1u);
        check_equal(scope_value(&candidate_scope), COMPONENT_PROVIDER_VALUE);
        check_equal(cmeta_plugin_registry_unload(&registry, ref), CMETA_PLUGIN_BUSY);

        check_equal(salts_service_host_scope_release(
            &candidate_scope), SALTS_SERVICE_HOST_OK);
        check_equal(salts_service_host_generation_drain(
            &candidate_host, candidate_retired), SALTS_SERVICE_HOST_OK);
        check_equal(cmeta_plugin_registry_get_lifecycle(
            &registry, ref, &info), CMETA_PLUGIN_OK);
        check_equal(info.active_leases, (size_t)0u);
        check_equal(salts_component_plugin_runtime_destroy(
            &original_host), SALTS_COMPONENT_PLUGIN_OK);
        check_equal(salts_service_host_runtime_destroy(
            &candidate_host), SALTS_SERVICE_HOST_OK);
        check_equal(cmeta_plugin_registry_request_stop(&registry, ref), CMETA_PLUGIN_OK);
        check_equal(cmeta_plugin_registry_poll_quiescent(
            &registry, ref, &quiet), CMETA_PLUGIN_OK);
        check_true(quiet);
        check_equal(cmeta_plugin_registry_unload(&registry, ref), CMETA_PLUGIN_OK);
        check_equal(cmeta_plugin_registry_destroy(&registry), CMETA_PLUGIN_OK);
    }
}
