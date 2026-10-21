#ifndef SALTS_INSTALLED_DSO_SCENARIO_H
#define SALTS_INSTALLED_DSO_SCENARIO_H
#include "service_host_fixture.h"
#include <salts/configurator.h>
#include <salts/service_host.h>
#include <salts/component_runtime_abi.h>
#include <stdio.h>
#include <stdint.h>

typedef struct installed_generation {
    salts_service_host_generation generation;
    salts_configurator_deployment deployments[1];
    salts_configurator_instance instances[1];
    salts_component_dependency dependencies[1];
    size_t activation_order[1];
    salts_service_host_module modules[1];
} installed_generation;

#define INSTALLED_CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "installed SDK DSO consumer %s:%d: %s\n", \
            __FILE__, __LINE__, #condition); return 1; \
} } while (0)

static salts_service_host_status installed_build(
    installed_generation *item, uint64_t id,
    cmeta_plugin_registry *registry, cmeta_plugin_ref plugin,
    const char *export_id) {
    const salts_service_host_generation_storage storage = {
        item->deployments, 1u,
        item->instances, 1u,
        item->dependencies, 1u,
        item->activation_order, 1u,
        item->modules, 1u
    };
    const salts_service_host_source source = {plugin, export_id, NULL, NULL};
    return salts_service_host_generation_build(
        &item->generation, id, registry, &storage,
        NULL, 0u, &source, 1u, NULL, 0u);
}
static int installed_scope_value(salts_service_host_scope *scope) {
    salts_configurator_service service;
    service_host_provider_value value = service_host_provider_value_bind(NULL, NULL);
    if (salts_service_host_scope_find_service(
            scope, service_host_provider_value_interface(), &service) !=
        SALTS_SERVICE_HOST_OK)
        return -1;
    if (service_host_provider_value_borrow_from_object(
            service.object, service.interfaces, &value) != CMETA_OK)
        return -2;
    return service_host_provider_value_get(&value);
}
static int installed_dso_run(int argc, char **argv) {
    const cmeta_plugin_registry_config config = {2u};
    cmeta_plugin_registry registry = {0};
    cmeta_plugin_ref first = {0}, second = {0};
    installed_generation live = {0}, pending = {0};
    salts_service_host_runtime runtime = SALTS_SERVICE_HOST_RUNTIME_INIT;
    salts_service_host_scope original = SALTS_SERVICE_HOST_SCOPE_INIT;
    salts_service_host_scope successor = SALTS_SERVICE_HOST_SCOPE_INIT;
    salts_service_host_generation *previous = NULL;
    cmeta_plugin_lifecycle_info info;
    bool quiet = false;
    INSTALLED_CHECK(argc == 3 && argv[1] && argv[2]);
    INSTALLED_CHECK(salts_configurator_abi_version() ==
                    SALTS_COMPONENT_RUNTIME_ABI_VERSION);
    INSTALLED_CHECK(salts_service_host_abi_version() ==
                    SALTS_COMPONENT_RUNTIME_ABI_VERSION);
    salts_service_host_abi_v1_0_required();
    INSTALLED_CHECK(salts_configurator_abi_context_size() ==
                    sizeof(salts_configurator_context));
    INSTALLED_CHECK(salts_service_host_abi_scope_size() ==
                    sizeof(salts_service_host_scope));
    INSTALLED_CHECK(cmeta_plugin_registry_init(&registry, &config) ==
                    CMETA_PLUGIN_OK);
    const cmeta_plugin_status first_load =
        cmeta_plugin_registry_load(&registry, argv[1], &first);
    if (first_load != CMETA_PLUGIN_OK)
        fprintf(stderr, "installed Provider N load: %s\n",
                cmeta_plugin_status_string(first_load));
    INSTALLED_CHECK(first_load == CMETA_PLUGIN_OK);
    const cmeta_plugin_status next_load =
        cmeta_plugin_registry_load(&registry, argv[2], &second);
    if (next_load != CMETA_PLUGIN_OK)
        fprintf(stderr, "installed Provider N+1 load: %s\n",
                cmeta_plugin_status_string(next_load));
    INSTALLED_CHECK(next_load == CMETA_PLUGIN_OK);
    INSTALLED_CHECK(cmeta_plugin_registry_start(&registry, first) == CMETA_PLUGIN_OK);
    INSTALLED_CHECK(cmeta_plugin_registry_start(&registry, second) == CMETA_PLUGIN_OK);
    INSTALLED_CHECK(installed_build(
        &live, UINT64_C(1), &registry, first, COMPONENT_PROVIDER_EXPORT_ID)
        == SALTS_SERVICE_HOST_OK);
    INSTALLED_CHECK(salts_service_host_runtime_init(&runtime) == SALTS_SERVICE_HOST_OK);
    INSTALLED_CHECK(salts_service_host_runtime_publish(
        &runtime, &live.generation, &previous) == SALTS_SERVICE_HOST_OK);
    INSTALLED_CHECK(previous == NULL);
    INSTALLED_CHECK(salts_service_host_scope_acquire(
        &runtime, &original) == SALTS_SERVICE_HOST_OK);
    INSTALLED_CHECK(installed_scope_value(&original) == COMPONENT_PROVIDER_VALUE);
    INSTALLED_CHECK(salts_service_host_scope_generation_id(&original) == UINT64_C(1));
    salts_service_host_scope copy = original;
    INSTALLED_CHECK(salts_service_host_scope_release(&copy) ==
                    SALTS_SERVICE_HOST_INVALID_STATE);
    INSTALLED_CHECK(installed_scope_value(&original) == COMPONENT_PROVIDER_VALUE);
    /* R0 legacy export belongs to a valid DSO, but cannot be dynamically
     * borrowed under FR strict ABI: fail without withdrawing N or leaking. */
    INSTALLED_CHECK(installed_build(
        &pending, UINT64_C(2), &registry, second,
        COMPONENT_PROVIDER_LEGACY_EXPORT_ID) == SALTS_SERVICE_HOST_PROVIDER_ERROR);
    INSTALLED_CHECK(pending.generation.state == SALTS_SERVICE_HOST_GENERATION_FAILED);
    INSTALLED_CHECK(cmeta_plugin_registry_get_lifecycle(
        &registry, second, &info) == CMETA_PLUGIN_OK && info.active_leases == 0u);
    previous = &live.generation;
    INSTALLED_CHECK(salts_service_host_runtime_publish(
        &runtime, &pending.generation, &previous) == SALTS_SERVICE_HOST_INVALID_STATE);
    INSTALLED_CHECK(previous == &live.generation);
    INSTALLED_CHECK(installed_scope_value(&original) == COMPONENT_PROVIDER_VALUE);
    INSTALLED_CHECK(installed_build(
        &pending, UINT64_C(3), &registry, second,
        COMPONENT_PROVIDER_EXPORT_ID) == SALTS_SERVICE_HOST_OK);
    INSTALLED_CHECK(cmeta_plugin_registry_get_lifecycle(
        &registry, second, &info) == CMETA_PLUGIN_OK && info.active_leases == 1u);
    INSTALLED_CHECK(salts_service_host_runtime_publish(
        &runtime, &pending.generation, &previous) == SALTS_SERVICE_HOST_OK);
    INSTALLED_CHECK(previous == &live.generation);
    INSTALLED_CHECK(salts_service_host_scope_acquire(
        &runtime, &successor) == SALTS_SERVICE_HOST_OK);
    INSTALLED_CHECK(salts_service_host_scope_generation_id(&successor) == UINT64_C(3));
    INSTALLED_CHECK(installed_scope_value(&successor) == COMPONENT_PROVIDER_VALUE);
    INSTALLED_CHECK(installed_scope_value(&original) == COMPONENT_PROVIDER_VALUE);
    INSTALLED_CHECK(salts_service_host_generation_drain(
        &runtime, &live.generation) == SALTS_SERVICE_HOST_BUSY);
    INSTALLED_CHECK(salts_service_host_scope_release(&original) ==
                    SALTS_SERVICE_HOST_OK);
    INSTALLED_CHECK(salts_service_host_generation_drain(
        &runtime, &live.generation) == SALTS_SERVICE_HOST_OK);
    INSTALLED_CHECK(cmeta_plugin_registry_get_lifecycle(
        &registry, first, &info) == CMETA_PLUGIN_OK && info.active_leases == 0u);
    INSTALLED_CHECK(salts_service_host_runtime_close(
        &runtime, &previous) == SALTS_SERVICE_HOST_OK);
    INSTALLED_CHECK(previous == &pending.generation);
    INSTALLED_CHECK(salts_service_host_generation_drain(
        &runtime, &pending.generation) == SALTS_SERVICE_HOST_BUSY);
    INSTALLED_CHECK(salts_service_host_scope_release(&successor) ==
                    SALTS_SERVICE_HOST_OK);
    INSTALLED_CHECK(salts_service_host_generation_drain(
        &runtime, &pending.generation) == SALTS_SERVICE_HOST_OK);
    INSTALLED_CHECK(salts_service_host_runtime_destroy(&runtime) ==
                    SALTS_SERVICE_HOST_OK);
    INSTALLED_CHECK(cmeta_plugin_registry_request_stop(
        &registry, first) == CMETA_PLUGIN_OK);
    INSTALLED_CHECK(cmeta_plugin_registry_poll_quiescent(
        &registry, first, &quiet) == CMETA_PLUGIN_OK && quiet);
    INSTALLED_CHECK(cmeta_plugin_registry_unload(&registry, first) == CMETA_PLUGIN_OK);
    quiet = false;
    INSTALLED_CHECK(cmeta_plugin_registry_request_stop(
        &registry, second) == CMETA_PLUGIN_OK);
    INSTALLED_CHECK(cmeta_plugin_registry_poll_quiescent(
        &registry, second, &quiet) == CMETA_PLUGIN_OK && quiet);
    INSTALLED_CHECK(cmeta_plugin_registry_unload(&registry, second) == CMETA_PLUGIN_OK);
    INSTALLED_CHECK(cmeta_plugin_registry_destroy(&registry) == CMETA_PLUGIN_OK);
    puts("Installed ServiceHost FR0 Provider DSOs: N survives bad N+1, "
         "BUSY drain, ownership and two real unloads PASS");
    return 0;
}
#endif /* SALTS_INSTALLED_DSO_SCENARIO_H */
