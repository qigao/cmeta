#ifndef SERVICE_HOST_CANDIDATE_PUBLICATION_FIXTURE_H
#define SERVICE_HOST_CANDIDATE_PUBLICATION_FIXTURE_H

#include "service_host_fixture.h"

#include <salts/service_host.h>

typedef struct publication_generation_fixture {
    salts_service_host_generation generation;
    salts_configurator_deployment deployments[1];
    salts_configurator_instance instances[1];
    salts_component_dependency dependencies[1];
    size_t activation_order[1];
    salts_service_host_module modules[1];
} publication_generation_fixture;

static salts_service_host_status build_generation(
    publication_generation_fixture *fixture,
    uint64_t id,
    cmeta_plugin_registry *registry,
    cmeta_plugin_ref ref) {
    const salts_service_host_generation_storage storage = {
        fixture->deployments, 1u,
        fixture->instances, 1u,
        fixture->dependencies, 1u,
        fixture->activation_order, 1u,
        fixture->modules, 1u
    };
    const salts_service_host_source source = {
        ref,
        COMPONENT_PROVIDER_EXPORT_ID,
        NULL,
        NULL
    };

    fixture->generation = (salts_service_host_generation){0};
    return salts_service_host_generation_build(
        &fixture->generation,
        id,
        registry,
        &storage,
        NULL, 0u,
        &source, 1u,
        NULL, 0u);
}

static int scope_value(salts_service_host_scope *scope) {
    salts_configurator_service service;
    component_plugin_value value =
        component_plugin_value_bind(NULL, NULL);

    if (salts_service_host_scope_find_service(
            scope,
            component_plugin_value_interface(),
            &service) != SALTS_SERVICE_HOST_OK)
        return -1;
    if (component_plugin_value_borrow_from_object(
            service.object, service.interfaces, &value) != CMETA_OK)
        return -2;
    return component_plugin_value_get(&value);
}

#endif
