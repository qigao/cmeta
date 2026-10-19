#include "service_host_fixture.h"
#include "tinytest.h"

#include <salts/service_host.h>
#include <cmeta/object_scope.h>

#ifndef COMPONENT_PROVIDER_SECONDARY_PLUGIN_PATH
#error COMPONENT_PROVIDER_SECONDARY_PLUGIN_PATH must name the secondary Provider DSO
#endif

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
static cmeta_plugin_ref secondary_ref;
static bool secondary_loaded;
static salts_service_host_generation generation;
static salts_configurator_deployment deployments[1];
static salts_configurator_instance instances[1];
static salts_component_dependency dependencies[1];
static size_t activation_order[1];
static salts_service_host_module modules[1];

suite("ServiceHost generation") {
    before_each() {
        const cmeta_plugin_registry_config config = {2};

        generation = (salts_service_host_generation){0};
        secondary_ref = (cmeta_plugin_ref){0u, 0u};
        secondary_loaded = false;

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

        /* Settle the generation's leases before trying to stop/unload the
         * secondary fixture, even if an earlier assertion aborts the case. */
        if (secondary_loaded) {
            check_equal(cmeta_plugin_registry_get_lifecycle(
                &registry, secondary_ref, &info), CMETA_PLUGIN_OK);
            if (info.state == CMETA_PLUGIN_LIFECYCLE_STARTED)
                check_equal(cmeta_plugin_registry_request_stop(
                    &registry, secondary_ref), CMETA_PLUGIN_OK);
            check_equal(cmeta_plugin_registry_poll_quiescent(
                &registry, secondary_ref, &quiet), CMETA_PLUGIN_OK);
            check_true(quiet);
            check_equal(cmeta_plugin_registry_unload(
                &registry, secondary_ref), CMETA_PLUGIN_OK);
            secondary_loaded = false;
        }

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

    it("preserves partial lease settlement and recovers after repairing the failed token") {
        salts_configurator_deployment two_deployments[2];
        salts_configurator_instance two_instances[2];
        salts_component_dependency two_dependencies[2];
        size_t two_activation_order[2];
        salts_service_host_module two_modules[2];
        const salts_service_host_generation_storage storage = {
            two_deployments, 2u,
            two_instances, 2u,
            two_dependencies, 2u,
            two_activation_order, 2u,
            two_modules, 2u
        };
        const salts_service_host_source sources[2] = {
            { ref, COMPONENT_PROVIDER_EXPORT_ID, NULL, NULL },
            { {0u, 0u}, COMPONENT_PROVIDER_EXPORT_ID, NULL, NULL }
        };
        cmeta_plugin_lease valid_lease;
        cmeta_plugin_lease failed_lease;
        cmeta_plugin_lifecycle_info primary_info;
        cmeta_plugin_lifecycle_info secondary_info;
        salts_service_host_runtime attached_runtime =
            SALTS_SERVICE_HOST_RUNTIME_INIT;
        size_t valid_module_capacity;
        uint32_t corrupted_generation;

        check_equal(cmeta_plugin_registry_load(
            &registry, COMPONENT_PROVIDER_SECONDARY_PLUGIN_PATH, &secondary_ref),
            CMETA_PLUGIN_OK);
        secondary_loaded = true;
        check_equal(cmeta_plugin_registry_start(&registry, secondary_ref),
                    CMETA_PLUGIN_OK);

        {
            salts_service_host_source admitted_sources[2] = {
                sources[0],
                { secondary_ref, COMPONENT_PROVIDER_EXPORT_ID, NULL, NULL }
            };
            check_equal(salts_service_host_generation_build(
                &generation, UINT64_C(1), &registry, &storage,
                NULL, 0u, admitted_sources, 2u, NULL, 0u),
                SALTS_SERVICE_HOST_OK);
        }
        check_equal(generation.state, SALTS_SERVICE_HOST_GENERATION_BUILT);
        check_equal(generation.module_count, (size_t)2u);
        valid_lease = two_modules[0].lease;
        check_true(cmeta_plugin_lease_valid(valid_lease));
        check_true(cmeta_plugin_lease_valid(two_modules[1].lease));

        /* Test-only owner-state fault on the first module. release_modules()
         * drains the second module in reverse order before encountering it. */
        corrupted_generation = two_modules[0].lease.generation + 1u;
        if (corrupted_generation == 0u)
            corrupted_generation = 1u;
        two_modules[0].lease.generation = corrupted_generation;
        failed_lease = two_modules[0].lease;

        check_equal(salts_service_host_generation_discard(&generation),
                    SALTS_SERVICE_HOST_PLUGIN_ERROR);
        check_equal(generation.state, SALTS_SERVICE_HOST_GENERATION_FAILED);
        check_equal(generation.module_count, (size_t)2u);
        check_equal(two_modules[0].lease.plugin.slot, failed_lease.plugin.slot);
        check_equal(two_modules[0].lease.plugin.generation, failed_lease.plugin.generation);
        check_equal(two_modules[0].lease.slot, failed_lease.slot);
        check_equal(two_modules[0].lease.generation, failed_lease.generation);
        check_equal(generation.failure.plugin_status, CMETA_PLUGIN_STALE);

        /* The valid second lease was consumed exactly once; the first
         * failed token remains unchanged and its DSO remains pinned. */
        check_false(cmeta_plugin_lease_valid(two_modules[1].lease));
        check_equal(two_modules[1].manifest, NULL);
        check_equal(cmeta_plugin_registry_get_lifecycle(
            &registry, ref, &primary_info), CMETA_PLUGIN_OK);
        check_equal(primary_info.active_leases, (size_t)1u);
        check_equal(cmeta_plugin_registry_get_lifecycle(
            &registry, secondary_ref, &secondary_info), CMETA_PLUGIN_OK);
        check_equal(secondary_info.active_leases, (size_t)0u);
        check_equal(cmeta_plugin_registry_request_stop(&registry, ref),
                    CMETA_PLUGIN_OK);
        check_equal(cmeta_plugin_registry_unload(&registry, ref),
                    CMETA_PLUGIN_BUSY);

        /* Caller-visible storage metadata cannot authorize an
         * out-of-bounds cleanup walk. The guard leaves every token intact. */
        valid_module_capacity = generation.storage.module_capacity;
        generation.storage.module_capacity = 0u;
        check_equal(salts_service_host_generation_recover(&generation),
                    SALTS_SERVICE_HOST_PLUGIN_ERROR);
        check_equal(generation.failure.plugin_status,
                    CMETA_PLUGIN_INVALID_ARGUMENT);
        check_equal(generation.module_count, (size_t)2u);
        check_equal(two_modules[0].lease.generation, corrupted_generation);
        generation.storage.module_capacity = valid_module_capacity;

        /* A malformed non-zero token must not be mistaken for an already
         * consumed lease, and the module ownership record must be preserved. */
        two_modules[0].lease.slot = 0u;
        check_equal(salts_service_host_generation_recover(&generation),
                    SALTS_SERVICE_HOST_PLUGIN_ERROR);
        check_equal(generation.failure.plugin_status,
                    CMETA_PLUGIN_INVALID_ARGUMENT);
        check_equal(generation.module_count, (size_t)2u);
        check_equal(two_modules[0].lease.plugin.slot, failed_lease.plugin.slot);
        check_equal(two_modules[0].lease.plugin.generation, failed_lease.plugin.generation);
        check_equal(two_modules[0].lease.slot, (uint32_t)0u);
        check_equal(two_modules[0].lease.generation, failed_lease.generation);
        check_true(two_modules[0].manifest != NULL);

        /* Canonical-zero token bytes are also insufficient evidence that the
         * owner released a lease if the Plugin/module record is still present. */
        two_modules[0].lease = (cmeta_plugin_lease){0};
        check_equal(salts_service_host_generation_recover(&generation),
                    SALTS_SERVICE_HOST_PLUGIN_ERROR);
        check_equal(generation.failure.plugin_status,
                    CMETA_PLUGIN_INVALID_ARGUMENT);
        check_equal(generation.module_count, (size_t)2u);
        check_equal(two_modules[0].plugin.slot, failed_lease.plugin.slot);
        check_equal(two_modules[0].plugin.generation, failed_lease.plugin.generation);
        check_true(two_modules[0].manifest != NULL);
        two_modules[0].lease = failed_lease;

        /* No build/reuse while any recorded lease is unsettled. */
        check_equal(salts_service_host_generation_build(
            &generation, UINT64_C(2), &registry, &storage,
            NULL, 0u, NULL, 0u, NULL, 0u),
            SALTS_SERVICE_HOST_INVALID_STATE);

        /* A failed recovery preserves the unsettled token and module table. */
        generation.runtime_owner = &attached_runtime;
        check_equal(salts_service_host_generation_recover(&generation),
                    SALTS_SERVICE_HOST_INVALID_STATE);
        generation.runtime_owner = NULL;
        generation.active_scopes = 1u;
        check_equal(salts_service_host_generation_recover(&generation),
                    SALTS_SERVICE_HOST_INVALID_STATE);
        generation.active_scopes = 0u;
        generation.components.activation_count = 1u;
        check_equal(salts_service_host_generation_recover(&generation),
                    SALTS_SERVICE_HOST_INVALID_STATE);
        generation.components.activation_count = 0u;
        check_equal(salts_service_host_generation_recover(&generation),
                    SALTS_SERVICE_HOST_PLUGIN_ERROR);
        check_equal(generation.state, SALTS_SERVICE_HOST_GENERATION_FAILED);
        check_equal(generation.module_count, (size_t)2u);
        check_equal(two_modules[0].lease.plugin.slot, failed_lease.plugin.slot);
        check_equal(two_modules[0].lease.plugin.generation, failed_lease.plugin.generation);
        check_equal(two_modules[0].lease.slot, failed_lease.slot);
        check_equal(two_modules[0].lease.generation, failed_lease.generation);
        check_equal(cmeta_plugin_registry_get_lifecycle(
            &registry, ref, &primary_info), CMETA_PLUGIN_OK);
        check_equal(primary_info.active_leases, (size_t)1u);

        /* Once the owner token is repaired, recovery settles the remaining
         * lease and makes the still-unpublished bundle reusable. */
        two_modules[0].lease = valid_lease;
        check_equal(salts_service_host_generation_recover(&generation),
                    SALTS_SERVICE_HOST_OK);
        check_equal(generation.state, SALTS_SERVICE_HOST_GENERATION_DRAINED);
        check_equal(generation.module_count, (size_t)0u);
        check_equal(generation.registry, NULL);
        check_equal(two_modules[0].manifest, NULL);
        check_false(cmeta_plugin_lease_valid(two_modules[0].lease));
        check_equal(cmeta_plugin_registry_get_lifecycle(
            &registry, ref, &primary_info), CMETA_PLUGIN_OK);
        check_equal(primary_info.active_leases, (size_t)0u);
        check_equal(cmeta_plugin_registry_get_lifecycle(
            &registry, secondary_ref, &secondary_info), CMETA_PLUGIN_OK);
        check_equal(secondary_info.active_leases, (size_t)0u);
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
        service_host_provider_value value =
            service_host_provider_value_bind(NULL, NULL);
        service_host_provider_aux aux =
            service_host_provider_aux_bind(NULL, NULL);
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
            service_host_provider_value_interface(),
            &service), SALTS_COMPONENT_OK);
        check_equal(service_host_provider_value_borrow_from_object(
            service.object, service.interfaces, &value), CMETA_OK);
        check_equal(service_host_provider_value_get(&value),
                    COMPONENT_PROVIDER_VALUE);

        check_equal(salts_configurator_context_find_service(
            &multi.components,
            service_host_provider_aux_interface(),
            &service), SALTS_COMPONENT_OK);
        check_equal(service_host_provider_aux_borrow_from_object(
            service.object, service.interfaces, &aux), CMETA_OK);
        check_equal(service_host_provider_aux_get(&aux),
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
        service_host_provider_value value =
            service_host_provider_value_bind(NULL, NULL);
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
            service_host_provider_value_interface(),
            &service),
            SALTS_COMPONENT_OK);
        check_equal(service_host_provider_value_borrow_from_object(
            service.object, service.interfaces, &value), CMETA_OK);
        check_equal(service_host_provider_value_get(&value),
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
