#include "component_plugin_fixture.h"
#include "tinytest.h"

#include <salts/service_host.h>
#include <cmeta/object_scope.h>

/* This callback observes the outer Plugin lease during lexical ObjectRef
 * destruction; cmeta_cleanup itself never acquires or extends that lease. */
typedef struct test_lease_observer {
    cmeta_plugin_registry *registry;
    cmeta_plugin_ref ref;
    unsigned destroys;
    size_t active_leases_at_destroy;
} test_lease_observer;

static void test_observed_object_destroy(void *context, void *object) {
    test_lease_observer *observer = (test_lease_observer *)context;
    cmeta_plugin_lifecycle_info info;
    (void)object;
    ++observer->destroys;
    if (cmeta_plugin_registry_get_lifecycle(
            observer->registry, observer->ref, &info) == CMETA_PLUGIN_OK)
        observer->active_leases_at_destroy = info.active_leases;
}


static cmeta_plugin_registry registry;
static cmeta_plugin_ref ref;
static salts_service_host_generation generation;
static salts_configurator_deployment deployments[1];
static salts_configurator_instance instances[1];
static salts_component_dependency dependencies[1];
static size_t activation_order[1];
static salts_service_host_module modules[1];

suite("ComponentPlugin candidate generation") {
    before_each() {
        const cmeta_plugin_registry_config config = {1};

        generation = (salts_service_host_generation){0};

        check_equal(cmeta_plugin_registry_init(&registry, &config),
                    CMETA_PLUGIN_OK);
        check_equal(cmeta_plugin_registry_load(
            &registry, COMPONENT_PROVIDER_PLUGIN_PATH, &ref),
            CMETA_PLUGIN_OK);
        check_equal(cmeta_plugin_registry_start(&registry, ref),
                    CMETA_PLUGIN_OK);
    }

    after_each() {
        cmeta_plugin_lifecycle_info info;
        bool quiet = false;

        if (generation.state == SALTS_SERVICE_HOST_GENERATION_BUILT)
            check_equal(salts_service_host_generation_discard(
                &generation), SALTS_SERVICE_HOST_OK);

        check_equal(cmeta_plugin_registry_get_lifecycle(
            &registry, ref, &info), CMETA_PLUGIN_OK);
        if (info.state == CMETA_PLUGIN_LIFECYCLE_STARTED)
            check_equal(cmeta_plugin_registry_request_stop(
                &registry, ref), CMETA_PLUGIN_OK);

        check_equal(cmeta_plugin_registry_poll_quiescent(
            &registry, ref, &quiet), CMETA_PLUGIN_OK);
        check_true(quiet);
        check_equal(cmeta_plugin_registry_unload(
            &registry, ref), CMETA_PLUGIN_OK);
        check_equal(cmeta_plugin_registry_destroy(&registry), CMETA_PLUGIN_OK);
    }

    it("rejects module capacity before acquiring any Plugin lease") {
        const salts_service_host_generation_storage storage = {
            deployments, 1u,
            instances, 1u,
            dependencies, 1u,
            activation_order, 1u,
            NULL, 0u
        };
        const salts_service_host_source source = {
            ref,
            COMPONENT_PROVIDER_EXPORT_ID,
            NULL,
            NULL
        };
        cmeta_plugin_lifecycle_info info;

        check_equal(salts_service_host_generation_build(
            &generation,
            UINT64_C(1),
            &registry,
            &storage,
            NULL, 0u,
            &source, 1u,
            NULL, 0u),
            SALTS_SERVICE_HOST_CAPACITY_EXCEEDED);

        check_equal(cmeta_plugin_registry_get_lifecycle(
            &registry, ref, &info), CMETA_PLUGIN_OK);
        check_equal(info.active_leases, (size_t)0u);
    }

    it("rejects an ABI-only real DSO service and unwinds its Plugin lease") {
        const salts_service_host_generation_storage storage = {
            deployments, 1u,
            instances, 1u,
            dependencies, 1u,
            activation_order, 1u,
            modules, 1u
        };
        const salts_service_host_source legacy = {
            ref, COMPONENT_PROVIDER_LEGACY_EXPORT_ID, NULL, NULL
        };
        const salts_service_host_source exact = {
            ref, COMPONENT_PROVIDER_EXPORT_ID, NULL, NULL
        };
        cmeta_plugin_lifecycle_info info;

        check_equal(salts_service_host_generation_build(
            &generation, UINT64_C(1), &registry, &storage,
            NULL, 0u, &legacy, 1u, NULL, 0u),
            SALTS_SERVICE_HOST_PROVIDER_ERROR);
        check_equal(generation.state, SALTS_SERVICE_HOST_GENERATION_FAILED);
        check_equal(generation.failure.source_index, (size_t)0u);
        check_equal(cmeta_plugin_registry_get_lifecycle(
            &registry, ref, &info), CMETA_PLUGIN_OK);
        check_equal(info.active_leases, (size_t)0u);

        /* Failure must not publish a graph or poison this candidate bundle. */
        check_equal(salts_service_host_generation_build(
            &generation, UINT64_C(2), &registry, &storage,
            NULL, 0u, &exact, 1u, NULL, 0u),
            SALTS_SERVICE_HOST_OK);
        check_equal(salts_service_host_generation_discard(
            &generation), SALTS_SERVICE_HOST_OK);
        check_equal(cmeta_plugin_registry_get_lifecycle(
            &registry, ref, &info), CMETA_PLUGIN_OK);
        check_equal(info.active_leases, (size_t)0u);
    }

    it("releases candidate leases when provider admission fails") {
        const salts_service_host_generation_storage storage = {
            deployments, 1u,
            instances, 1u,
            dependencies, 1u,
            activation_order, 1u,
            modules, 1u
        };
        const salts_service_host_source source = {
            ref,
            "missing-provider-export",
            NULL,
            NULL
        };
        cmeta_plugin_lifecycle_info info;

        check_equal(salts_service_host_generation_build(
            &generation,
            UINT64_C(1),
            &registry,
            &storage,
            NULL, 0u,
            &source, 1u,
            NULL, 0u),
            SALTS_SERVICE_HOST_PLUGIN_ERROR);
        check_equal(generation.state,
                    SALTS_SERVICE_HOST_GENERATION_FAILED);

        check_equal(cmeta_plugin_registry_get_lifecycle(
            &registry, ref, &info), CMETA_PLUGIN_OK);
        check_equal(info.active_leases, (size_t)0u);
        check_equal(
            salts_service_host_generation_failure(&generation)
                ->source_index,
            (size_t)0u);
        check_equal(
            salts_service_host_generation_failure(&generation)
                ->plugin_status,
            CMETA_PLUGIN_UNKNOWN_EXPORT);

        {
            const salts_service_host_source retry = {
                ref,
                COMPONENT_PROVIDER_EXPORT_ID,
                NULL,
                NULL
            };
            check_equal(salts_service_host_generation_build(
                &generation,
                UINT64_C(2),
                &registry,
                &storage,
                NULL, 0u,
                &retry, 1u,
                NULL, 0u),
                SALTS_SERVICE_HOST_OK);
            check_equal(salts_service_host_generation_discard(
                &generation), SALTS_SERVICE_HOST_OK);
        }

        check_equal(cmeta_plugin_registry_get_lifecycle(
            &registry, ref, &info), CMETA_PLUGIN_OK);
        check_equal(info.active_leases, (size_t)0u);
    }

    it("discharges explicit ObjectRef cleanup before releasing its Plugin lease") {
        const salts_service_host_generation_storage storage = {
            deployments, 1u,
            instances, 1u,
            dependencies, 1u,
            activation_order, 1u,
            modules, 1u
        };
        const salts_service_host_source source = {
            ref, COMPONENT_PROVIDER_EXPORT_ID, NULL, NULL
        };
        test_lease_observer observer = {0};
        cmeta_object_lifecycle lifecycle;
        cmeta_object_ref temporary = CMETA_OBJECT_REF_INIT;
        cmeta_cleanup cleanup = CMETA_CLEANUP_INIT;
        cmeta_plugin_lifecycle_info info;
        int value = 41;

        observer.registry = &registry;
        observer.ref = ref;
        lifecycle = (cmeta_object_lifecycle){
            sizeof(cmeta_object_lifecycle),
            &observer, NULL, NULL, test_observed_object_destroy
        };

        check_equal(salts_service_host_generation_build(
            &generation, UINT64_C(4), &registry, &storage,
            NULL, 0u, &source, 1u, NULL, 0u),
            SALTS_SERVICE_HOST_OK);
        check_equal(cmeta_plugin_registry_get_lifecycle(
            &registry, ref, &info), CMETA_PLUGIN_OK);
        check_equal(info.active_leases, (size_t)1u);

        check_equal(cmeta_object_borrow(
            &temporary, &value, &cmeta_data_int, NULL), CMETA_OK);
        check_equal(cmeta_object_take(&temporary, &lifecycle), CMETA_OK);
        check_equal(cmeta_cleanup_object(&cleanup, &temporary), CMETA_OK);

        /* A lexical cleanup must run while the provider code is still
         * authorized by the outer lease. It does not unload Plugin itself. */
        cmeta_cleanup_run(&cleanup);
        cmeta_cleanup_run(&cleanup);
        check_equal(observer.destroys, 1u);
        check_equal(observer.active_leases_at_destroy, (size_t)1u);
        check_false(cmeta_object_ref_valid(&temporary));

        check_equal(salts_service_host_generation_discard(
            &generation), SALTS_SERVICE_HOST_OK);
        check_equal(cmeta_plugin_registry_get_lifecycle(
            &registry, ref, &info), CMETA_PLUGIN_OK);
        check_equal(info.active_leases, (size_t)0u);
        check_equal(observer.destroys, 1u);
    }

    it("shares one Plugin lease across two provider exports") {
        salts_service_host_generation multi = {0};
        salts_configurator_deployment multi_deployments[2];
        salts_configurator_instance multi_instances[2];
        salts_component_dependency multi_dependencies[2];
        size_t multi_order[2];
        salts_service_host_module multi_modules[1];
        const salts_service_host_generation_storage storage = {
            multi_deployments, 2u,
            multi_instances, 2u,
            multi_dependencies, 2u,
            multi_order, 2u,
            multi_modules, 1u
        };
        const salts_service_host_source sources[2] = {
            { ref, COMPONENT_PROVIDER_EXPORT_ID, NULL, NULL },
            { ref, COMPONENT_PROVIDER_AUX_EXPORT_ID, NULL, NULL }
        };
        salts_configurator_service service;
        component_plugin_value value =
            component_plugin_value_bind(NULL, NULL);
        component_plugin_aux aux =
            component_plugin_aux_bind(NULL, NULL);
        cmeta_plugin_lifecycle_info info;

        check_equal(salts_service_host_generation_build(
            &multi,
            UINT64_C(3),
            &registry,
            &storage,
            NULL, 0u,
            sources, 2u,
            NULL, 0u),
            SALTS_SERVICE_HOST_OK);

        check_equal(multi.deployment_count, (size_t)2u);
        check_equal(multi.module_count, (size_t)1u);

        check_equal(cmeta_plugin_registry_get_lifecycle(
            &registry, ref, &info), CMETA_PLUGIN_OK);
        check_equal(info.active_leases, (size_t)1u);

        check_equal(salts_configurator_context_find_service(
            &multi.components,
            component_plugin_value_interface(),
            &service), SALTS_COMPONENT_OK);
        check_equal(component_plugin_value_borrow_from_object(
            service.object, service.interfaces, &value), CMETA_OK);
        check_equal(component_plugin_value_get(&value),
                    COMPONENT_PROVIDER_VALUE);

        check_equal(salts_configurator_context_find_service(
            &multi.components,
            component_plugin_aux_interface(),
            &service), SALTS_COMPONENT_OK);
        check_equal(component_plugin_aux_borrow_from_object(
            service.object, service.interfaces, &aux), CMETA_OK);
        check_equal(component_plugin_aux_get(&aux),
                    COMPONENT_PROVIDER_AUX_VALUE);

        check_equal(salts_service_host_generation_discard(
            &multi), SALTS_SERVICE_HOST_OK);
        check_equal(cmeta_plugin_registry_get_lifecycle(
            &registry, ref, &info), CMETA_PLUGIN_OK);
        check_equal(info.active_leases, (size_t)0u);
    }

    it("builds one typed Component graph under one Plugin lease") {
        const salts_service_host_generation_storage storage = {
            deployments, 1u,
            instances, 1u,
            dependencies, 1u,
            activation_order, 1u,
            modules, 1u
        };
        const salts_service_host_source source = {
            ref,
            COMPONENT_PROVIDER_EXPORT_ID,
            NULL,
            NULL
        };
        salts_configurator_service service;
        component_plugin_value value =
            component_plugin_value_bind(NULL, NULL);
        cmeta_plugin_lifecycle_info info;

        check_equal(salts_service_host_generation_build(
            &generation,
            UINT64_C(1),
            &registry,
            &storage,
            NULL, 0u,
            &source, 1u,
            NULL, 0u),
            SALTS_SERVICE_HOST_OK);

        check_equal(generation.state,
                    SALTS_SERVICE_HOST_GENERATION_BUILT);
        check_equal(generation.deployment_count, (size_t)1u);
        check_equal(generation.module_count, (size_t)1u);

        check_equal(salts_configurator_context_find_service(
            &generation.components,
            component_plugin_value_interface(),
            &service),
            SALTS_COMPONENT_OK);
        check_equal(component_plugin_value_borrow_from_object(
            service.object, service.interfaces, &value), CMETA_OK);
        check_equal(component_plugin_value_get(&value),
                    COMPONENT_PROVIDER_VALUE);

        check_equal(cmeta_plugin_registry_get_lifecycle(
            &registry, ref, &info), CMETA_PLUGIN_OK);
        check_equal(info.active_leases, (size_t)1u);

        check_equal(cmeta_plugin_registry_request_stop(
            &registry, ref), CMETA_PLUGIN_OK);
        check_equal(cmeta_plugin_registry_unload(
            &registry, ref), CMETA_PLUGIN_BUSY);

        check_equal(salts_service_host_generation_discard(
            &generation), SALTS_SERVICE_HOST_OK);
        check_equal(generation.state,
                    SALTS_SERVICE_HOST_GENERATION_DRAINED);

        check_equal(cmeta_plugin_registry_get_lifecycle(
            &registry, ref, &info), CMETA_PLUGIN_OK);
        check_equal(info.active_leases, (size_t)0u);
    }
}
