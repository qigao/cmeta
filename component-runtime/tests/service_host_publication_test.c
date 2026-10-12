#include "service_host_publication_fixture.h"
#include "tinytest.h"
#include <salts/native_io_ace_token.h>
#include <cmeta/ace_interceptor.h>

/* The ACT has NO authority to own/release this borrowed Plugin Scope.
 * The component runtime's lease stays authoritative until observed terminal
 * settlement AND explicit application release of the stable scope. */
NATIVE_IO_ACE_TOKEN_TYPE(ace_plugin_scope_act, salts_service_host_scope);



/* POSA2 Interceptor under an actual ComponentPlugin generation Scope.
 * CMeta describes the exact FunctionAbi and borrows the native function.
 * ComponentPlugin alone admits/drains the Scope and retains the DSO lease. */
CMETA_INTERCEPTOR_TYPE(ace_plugin_interceptor, int, int);

typedef struct ace_plugin_interceptor_call {
    salts_service_host_scope *scope;
    unsigned targets;
} ace_plugin_interceptor_call;

typedef struct ace_plugin_interceptor_probe {
    unsigned events[12];
    size_t count;
    bool reject;
    bool fail_before;
    cmeta_status error;
} ace_plugin_interceptor_probe;

static cmeta_status ace_plugin_interceptor_target(
    void *user, const int *request, int *result) {
    ace_plugin_interceptor_call *call = (ace_plugin_interceptor_call *)user;
    const int current = scope_value(call->scope);
    ++call->targets;
    if (current < 0 || *request < 0) return CMETA_CALLBACK_ERROR;
    *result = current + *request;
    return CMETA_OK;
}

static cmeta_status ace_plugin_interceptor_before(
    void *user, const int *request, bool *proceed) {
    ace_plugin_interceptor_probe *probe = (ace_plugin_interceptor_probe *)user;
    (void)request;
    probe->events[probe->count++] = 1u;
    *proceed = !probe->reject;
    return probe->fail_before ? CMETA_CALLBACK_ERROR : CMETA_OK;
}

static void ace_plugin_interceptor_after(
    void *user, const int *request, const int *result) {
    ace_plugin_interceptor_probe *probe = (ace_plugin_interceptor_probe *)user;
    (void)request; (void)result;
    probe->events[probe->count++] = 3u;
}

static void ace_plugin_interceptor_error(
    void *user, const int *request, cmeta_status status) {
    ace_plugin_interceptor_probe *probe = (ace_plugin_interceptor_probe *)user;
    (void)request;
    probe->error = status;
    probe->events[probe->count++] = 4u;
}

/* Native function pointer and borrowed descriptor are independently checked.
 * This test-local metadata does not create a second dynamic invocation ABI. */
static const cmeta_type_desc ace_plugin_interceptor_status_desc = {
    "cmeta_status", sizeof(cmeta_status), CMETA_ALIGNOF(cmeta_status),
    CMETA_T_INTEGER, NULL, NULL, NULL
};
CMETA_FUNCTION_METADATA_AS_ABI_RESULT(
    ace_plugin_interceptor_contract, "component.plugin.interceptor.target",
    io, &ace_plugin_interceptor_status_desc, CMETA_ABI_ENUM, CMETA_RESULT_VALUE,
    (void *, context, CMETA_PARAM_IN | CMETA_PARAM_BORROWED,
     &cmeta_type_void_ptr, CMETA_ABI_OBJECT_POINTER),
    (const int *, request, CMETA_PARAM_IN | CMETA_PARAM_BORROWED,
     &cmeta_type_int_ptr, CMETA_ABI_OBJECT_POINTER),
    (int *, result, CMETA_PARAM_OUT | CMETA_PARAM_BORROWED,
     &cmeta_type_int_ptr, CMETA_ABI_OBJECT_POINTER));
CMETA_STATIC_ASSERT(
    CMETA_TYPE_MATCHES(&ace_plugin_interceptor_target, ace_plugin_interceptor_target_fn),
    "Interceptor provider must match its canonical native callable");

suite("ComponentPlugin generation publication") {

    it("borrows a real Plugin DSO through typed Interceptor success/short-circuit/error") {
        const cmeta_plugin_registry_config registry_config = {1};
        cmeta_plugin_registry registry = {0};
        cmeta_plugin_ref ref;
        cmeta_plugin_lifecycle_info info;
        publication_generation_fixture generation = {0};
        salts_service_host_runtime runtime = {0};
        salts_service_host_generation *retired = NULL;
        salts_service_host_scope scope = {0};
        ace_plugin_interceptor_call call = {&scope, 0u};
        ace_plugin_interceptor_probe probe = {0};
        const ace_plugin_interceptor_hook hook = {
            &probe, ace_plugin_interceptor_before,
            ace_plugin_interceptor_after, ace_plugin_interceptor_error
        };
        const cmeta_function_abi_desc *abi =
            &ace_plugin_interceptor_contract__function_abi_meta;
        ace_plugin_interceptor chain = {0};
        bool quiet = false;
        int request = 3;
        int response = -1;

        check_true(cmeta_function_abi_desc_valid(abi));
        check_equal(cmeta_plugin_registry_init(
            &registry, &registry_config), CMETA_PLUGIN_OK);
        check_equal(cmeta_plugin_registry_load(
            &registry, COMPONENT_PROVIDER_PLUGIN_PATH, &ref), CMETA_PLUGIN_OK);
        check_equal(cmeta_plugin_registry_start(
            &registry, ref), CMETA_PLUGIN_OK);
        check_equal(build_generation(
            &generation, UINT64_C(21), &registry, ref), SALTS_SERVICE_HOST_OK);
        check_equal(salts_service_host_runtime_init(
            &runtime), SALTS_SERVICE_HOST_OK);
        check_equal(salts_service_host_runtime_publish(
            &runtime, &generation.generation, &retired), SALTS_SERVICE_HOST_OK);
        check_null(retired);
        check_equal(salts_service_host_scope_acquire(
            &runtime, &scope), SALTS_SERVICE_HOST_OK);
        check_equal(ace_plugin_interceptor_admit(
            &chain, &call, ace_plugin_interceptor_target, &hook, 1u,
            abi, abi), CMETA_OK);

        /* The generation is no longer public, but already admitted work can
         * call the real provider through its still-live borrowing Scope. */
        check_equal(salts_service_host_runtime_close(
            &runtime, &retired), SALTS_SERVICE_HOST_OK);
        check_true(retired == &generation.generation);
        check_equal(salts_service_host_generation_drain(
            &runtime, retired), SALTS_SERVICE_HOST_BUSY);
        check_equal(cmeta_plugin_registry_get_lifecycle(
            &registry, ref, &info), CMETA_PLUGIN_OK);
        check_equal(info.active_leases, (size_t)1u);

        check_equal(ace_plugin_interceptor_invoke(
            &chain, &request, &response), CMETA_OK);
        check_equal(response, COMPONENT_PROVIDER_VALUE + request);
        check_equal(call.targets, 1u);
        check_equal(probe.count, 2u);
        check_equal(probe.events[0], 1u);
        check_equal(probe.events[1], 3u);

        /* Before rejects: no DSO invocation, no result transfer, one error. */
        probe.count = 0u;
        probe.reject = true;
        response = 97;
        check_equal(ace_plugin_interceptor_invoke(
            &chain, &request, &response), CMETA_CALLBACK_ERROR);
        check_equal(call.targets, 1u);
        check_equal(response, 97);
        check_equal(probe.count, 2u);
        check_equal(probe.events[0], 1u);
        check_equal(probe.events[1], 4u);
        check_equal(probe.error, CMETA_CALLBACK_ERROR);

        /* A target failure uses the same borrowed Scope and unwinds on_error,
         * never calling after or fabricating an owned response. */
        probe.count = 0u;
        probe.reject = false;
        request = -1;
        response = 71;
        check_equal(ace_plugin_interceptor_invoke(
            &chain, &request, &response), CMETA_CALLBACK_ERROR);
        check_equal(call.targets, 2u);
        check_equal(response, 71);
        check_equal(probe.count, 2u);
        check_equal(probe.events[0], 1u);
        check_equal(probe.events[1], 4u);

        /* Even a rejected BEFORE callback itself has exactly one error hook. */
        probe.count = 0u;
        probe.fail_before = true;
        request = 7;
        check_equal(ace_plugin_interceptor_invoke(
            &chain, &request, &response), CMETA_CALLBACK_ERROR);
        check_equal(call.targets, 2u);
        check_equal(response, 71);
        check_equal(probe.count, 2u);
        check_equal(probe.events[0], 1u);
        check_equal(probe.events[1], 4u);

        check_equal(scope_value(&scope), COMPONENT_PROVIDER_VALUE);
        check_equal(salts_service_host_generation_drain(
            &runtime, retired), SALTS_SERVICE_HOST_BUSY);
        check_equal(cmeta_plugin_registry_request_stop(
            &registry, ref), CMETA_PLUGIN_OK);
        check_equal(cmeta_plugin_registry_unload(
            &registry, ref), CMETA_PLUGIN_BUSY);
        /* Interceptor callbacks do not release a Plugin lease: the calling
         * application explicitly releases Scope before the generation drain. */
        check_equal(salts_service_host_scope_release(
            &scope), SALTS_SERVICE_HOST_OK);
        check_equal(salts_service_host_generation_drain(
            &runtime, retired), SALTS_SERVICE_HOST_OK);
        check_equal(cmeta_plugin_registry_get_lifecycle(
            &registry, ref, &info), CMETA_PLUGIN_OK);
        check_equal(info.active_leases, (size_t)0u);
        check_equal(salts_service_host_runtime_destroy(
            &runtime), SALTS_SERVICE_HOST_OK);
        check_equal(cmeta_plugin_registry_poll_quiescent(
            &registry, ref, &quiet), CMETA_PLUGIN_OK);
        check_true(quiet);
        check_equal(cmeta_plugin_registry_unload(
            &registry, ref), CMETA_PLUGIN_OK);
        check_equal(cmeta_plugin_registry_destroy(
            &registry), CMETA_PLUGIN_OK);
    }



    it("pins a real provider generation across typed ACT terminal settlement") {
        const cmeta_plugin_registry_config registry_config = {1};
        cmeta_plugin_registry registry = {0};
        cmeta_plugin_ref ref;
        cmeta_plugin_lifecycle_info info;
        publication_generation_fixture generation1 = {0};
        salts_service_host_runtime runtime = {0};
        salts_service_host_scope scope = {0};
        salts_service_host_scope *settled_scope = NULL;
        salts_service_host_generation *retired = NULL;
        ace_plugin_scope_act token = {0};
        bool quiet = false;

        /* Synthetic terminal identity for this scope test. The real NativeIO
         * read/cancel terminal behavior is separately exercised by
         * native_io_test.c; this fixture tests Plugin lease ordering only. */
        const native_io_request request = {1u, 3u};
        const native_io_endpoint endpoint = {1u, 7u};
        native_io_completion completion = {0};
        completion.request = request;
        completion.endpoint = endpoint;
        completion.kind = NATIVE_IO_COMPLETION_CANCELLED;
        completion.status = SALTS_ECANCELED;
        completion.user_data = (uintptr_t)17u;

        check_equal(cmeta_plugin_registry_init(
            &registry, &registry_config), CMETA_PLUGIN_OK);
        check_equal(cmeta_plugin_registry_load(
            &registry, COMPONENT_PROVIDER_PLUGIN_PATH, &ref), CMETA_PLUGIN_OK);
        check_equal(cmeta_plugin_registry_start(&registry, ref), CMETA_PLUGIN_OK);
        check_equal(build_generation(
            &generation1, UINT64_C(11), &registry, ref), SALTS_SERVICE_HOST_OK);
        check_equal(salts_service_host_runtime_init(
            &runtime), SALTS_SERVICE_HOST_OK);
        check_equal(salts_service_host_runtime_publish(
            &runtime, &generation1.generation, &retired), SALTS_SERVICE_HOST_OK);
        check_null(retired);

        check_equal(salts_service_host_scope_acquire(
            &runtime, &scope), SALTS_SERVICE_HOST_OK);
        check_equal(scope_value(&scope), COMPONENT_PROVIDER_VALUE);
        check_equal(ace_plugin_scope_act_bind(
            &token, request, endpoint, (uintptr_t)17u, &scope), SALTS_OK);
        check_equal(cmeta_plugin_registry_get_lifecycle(
            &registry, ref, &info), CMETA_PLUGIN_OK);
        check_equal(info.active_leases, (size_t)1u);

        /* No new generation admission; the existing work remains authorized.
         * Closing admission or asking for cancellation does NOT settle ACT. */
        check_equal(salts_service_host_runtime_close(
            &runtime, &retired), SALTS_SERVICE_HOST_OK);
        check_true(retired == &generation1.generation);
        check_equal(salts_service_host_generation_drain(
            &runtime, retired), SALTS_SERVICE_HOST_BUSY);
        native_io_completion stale = completion;
        ++stale.request.generation;
        check_equal(ace_plugin_scope_act_settle(
            &token, &stale, &settled_scope), SALTS_ENOENT);
        check_null(settled_scope);
        check_true(token.active);
        check_equal(scope_value(&scope), COMPONENT_PROVIDER_VALUE);
        check_equal(cmeta_plugin_registry_get_lifecycle(
            &registry, ref, &info), CMETA_PLUGIN_OK);
        check_equal(info.active_leases, (size_t)1u);

        check_equal(ace_plugin_scope_act_settle(
            &token, &completion, &settled_scope), SALTS_OK);
        check_true(settled_scope == &scope);
        check_equal(ace_plugin_scope_act_settle(
            &token, &completion, &settled_scope), SALTS_EALREADY);
        check_null(settled_scope);
        /* ACT settlement does not manufacture or release a Plugin lease. */
        check_equal(salts_service_host_generation_drain(
            &runtime, retired), SALTS_SERVICE_HOST_BUSY);
        check_equal(salts_service_host_scope_release(
            &scope), SALTS_SERVICE_HOST_OK);
        check_equal(salts_service_host_generation_drain(
            &runtime, retired), SALTS_SERVICE_HOST_OK);
        check_equal(cmeta_plugin_registry_get_lifecycle(
            &registry, ref, &info), CMETA_PLUGIN_OK);
        check_equal(info.active_leases, (size_t)0u);

        check_equal(salts_service_host_runtime_destroy(
            &runtime), SALTS_SERVICE_HOST_OK);
        check_equal(cmeta_plugin_registry_request_stop(
            &registry, ref), CMETA_PLUGIN_OK);
        check_equal(cmeta_plugin_registry_poll_quiescent(
            &registry, ref, &quiet), CMETA_PLUGIN_OK);
        check_true(quiet);
        check_equal(cmeta_plugin_registry_unload(
            &registry, ref), CMETA_PLUGIN_OK);
        check_equal(cmeta_plugin_registry_destroy(
            &registry), CMETA_PLUGIN_OK);
    }


    it("publishes N+1 atomically while admitted N drains") {
        const cmeta_plugin_registry_config registry_config = {1};
        cmeta_plugin_registry registry = {0};
        cmeta_plugin_ref ref;
        cmeta_plugin_lifecycle_info info;
        publication_generation_fixture g1 = {0};
        publication_generation_fixture g2 = {0};
        publication_generation_fixture g3 = {0};
        publication_generation_fixture g4 = {0};
        salts_service_host_runtime runtime = {0};
        salts_service_host_scope scope1 = {0};
        salts_service_host_scope scope2 = {0};
        salts_service_host_scope scope3 = {0};
        salts_service_host_scope copied_scope = {0};
        salts_service_host_scope reopened_scope = {0};
        salts_service_host_generation *previous = NULL;
        bool quiet = false;

        check_equal(cmeta_plugin_registry_init(
            &registry, &registry_config), CMETA_PLUGIN_OK);
        check_equal(cmeta_plugin_registry_load(
            &registry, COMPONENT_PROVIDER_PLUGIN_PATH, &ref),
            CMETA_PLUGIN_OK);
        check_equal(cmeta_plugin_registry_start(
            &registry, ref), CMETA_PLUGIN_OK);

        check_equal(build_generation(
            &g1, UINT64_C(1), &registry, ref),
            SALTS_SERVICE_HOST_OK);
        check_equal(build_generation(
            &g2, UINT64_C(2), &registry, ref),
            SALTS_SERVICE_HOST_OK);

        check_equal(cmeta_plugin_registry_get_lifecycle(
            &registry, ref, &info), CMETA_PLUGIN_OK);
        check_equal(info.active_leases, (size_t)2u);

        check_equal(salts_service_host_runtime_init(
            &runtime), SALTS_SERVICE_HOST_OK);

        check_equal(salts_service_host_runtime_publish(
            &runtime, &g1.generation, &previous),
            SALTS_SERVICE_HOST_OK);
        check_null(previous);

        {
            salts_service_host_generation stale =
                SALTS_SERVICE_HOST_GENERATION_INIT;
            const salts_service_host_generation_storage empty_storage = {0};

            check_equal(salts_service_host_generation_build(
                &stale,
                UINT64_C(1),
                NULL,
                &empty_storage,
                NULL, 0u,
                NULL, 0u,
                NULL, 0u),
                SALTS_SERVICE_HOST_OK);
            check_equal(salts_service_host_runtime_publish(
                &runtime, &stale, &previous),
                SALTS_SERVICE_HOST_INVALID_STATE);
            check_equal(salts_service_host_generation_discard(
                &stale), SALTS_SERVICE_HOST_OK);
        }

        check_equal(salts_service_host_scope_acquire(
            &runtime, &scope1), SALTS_SERVICE_HOST_OK);
        check_equal(salts_service_host_scope_generation_id(&scope1),
                    UINT64_C(1));
        check_equal(scope_value(&scope1), COMPONENT_PROVIDER_VALUE);

        /* A copied scope is only a stale byte-value, not a second owner. */
        copied_scope = scope1;
        check_equal(salts_service_host_scope_generation_id(&copied_scope),
                    UINT64_C(0));
        {
            salts_configurator_service copied_service;
            check_equal(salts_service_host_scope_find_service(
                &copied_scope, component_plugin_value_interface(), &copied_service),
                SALTS_SERVICE_HOST_INVALID_STATE);
            check_equal(salts_service_host_scope_find_service_from(
                &copied_scope, "ComponentPluginFixture",
                component_plugin_value_interface(), &copied_service),
                SALTS_SERVICE_HOST_INVALID_STATE);
        }
        check_equal(salts_service_host_scope_release(
            &copied_scope), SALTS_SERVICE_HOST_INVALID_STATE);
        check_equal(g1.generation.active_scopes, (size_t)1u);
        check_equal(runtime.active_scopes, (size_t)1u);
        check_equal(scope_value(&scope1), COMPONENT_PROVIDER_VALUE);

        check_equal(salts_service_host_runtime_publish(
            &runtime, &g2.generation, &previous),
            SALTS_SERVICE_HOST_OK);
        check_true(previous == &g1.generation);
        check_equal(g1.generation.state,
                    SALTS_SERVICE_HOST_GENERATION_DRAINING);

        check_equal(salts_service_host_scope_acquire(
            &runtime, &scope2), SALTS_SERVICE_HOST_OK);
        check_equal(salts_service_host_scope_generation_id(&scope2),
                    UINT64_C(2));
        check_equal(scope_value(&scope2), COMPONENT_PROVIDER_VALUE);

        check_equal(build_generation(
            &g3, UINT64_C(3), &registry, ref),
            SALTS_SERVICE_HOST_OK);
        check_equal(salts_service_host_runtime_publish(
            &runtime, &g3.generation, &previous),
            SALTS_SERVICE_HOST_BUSY);
        check_true(previous == &g1.generation);
        check_equal(g2.generation.state,
                    SALTS_SERVICE_HOST_GENERATION_PUBLISHED);
        check_equal(g3.generation.state,
                    SALTS_SERVICE_HOST_GENERATION_BUILT);

        /* Existing admitted work remains on generation 1. */
        check_equal(salts_service_host_scope_generation_id(&scope1),
                    UINT64_C(1));
        check_equal(scope_value(&scope1), COMPONENT_PROVIDER_VALUE);

        check_equal(salts_service_host_generation_drain(
            &runtime, &g1.generation), SALTS_SERVICE_HOST_BUSY);
        check_equal(salts_service_host_scope_release(
            &scope1), SALTS_SERVICE_HOST_OK);
        check_equal(salts_service_host_scope_release(
            &copied_scope), SALTS_SERVICE_HOST_INVALID_STATE);
        check_equal(salts_service_host_scope_release(
            &scope1), SALTS_SERVICE_HOST_INVALID_ARGUMENT);
        check_equal(salts_service_host_generation_drain(
            &runtime, &g1.generation), SALTS_SERVICE_HOST_OK);

        check_equal(cmeta_plugin_registry_get_lifecycle(
            &registry, ref, &info), CMETA_PLUGIN_OK);
        check_equal(info.active_leases, (size_t)2u);

        check_equal(salts_service_host_runtime_publish(
            &runtime, &g3.generation, &previous),
            SALTS_SERVICE_HOST_OK);
        check_true(previous == &g2.generation);
        check_equal(g2.generation.state,
                    SALTS_SERVICE_HOST_GENERATION_DRAINING);

        check_equal(salts_service_host_scope_acquire(
            &runtime, &scope3), SALTS_SERVICE_HOST_OK);
        check_equal(salts_service_host_scope_generation_id(&scope3),
                    UINT64_C(3));
        check_equal(scope_value(&scope3), COMPONENT_PROVIDER_VALUE);

        check_equal(salts_service_host_scope_generation_id(&scope2),
                    UINT64_C(2));
        check_equal(scope_value(&scope2), COMPONENT_PROVIDER_VALUE);

        check_equal(salts_service_host_generation_drain(
            &runtime, &g2.generation), SALTS_SERVICE_HOST_BUSY);
        check_equal(salts_service_host_scope_release(
            &scope2), SALTS_SERVICE_HOST_OK);
        check_equal(salts_service_host_generation_drain(
            &runtime, &g2.generation), SALTS_SERVICE_HOST_OK);

        check_equal(cmeta_plugin_registry_get_lifecycle(
            &registry, ref, &info), CMETA_PLUGIN_OK);
        check_equal(info.active_leases, (size_t)1u);

        check_equal(salts_service_host_runtime_close(
            &runtime, &previous), SALTS_SERVICE_HOST_OK);
        check_true(previous == &g3.generation);
        check_equal(salts_service_host_generation_drain(
            &runtime, &g3.generation), SALTS_SERVICE_HOST_BUSY);

        check_equal(salts_service_host_scope_release(
            &scope3), SALTS_SERVICE_HOST_OK);
        check_equal(salts_service_host_generation_drain(
            &runtime, &g3.generation), SALTS_SERVICE_HOST_OK);

        check_equal(cmeta_plugin_registry_get_lifecycle(
            &registry, ref, &info), CMETA_PLUGIN_OK);
        check_equal(info.active_leases, (size_t)0u);

        /* close() unpublishes admission; it is not runtime_destroy(). An
         * explicit higher generation may be published after a full drain. */
        check_equal(salts_service_host_scope_acquire(
            &runtime, &reopened_scope), SALTS_SERVICE_HOST_INVALID_STATE);
        check_equal(build_generation(
            &g4, UINT64_C(4), &registry, ref), SALTS_SERVICE_HOST_OK);
        check_equal(salts_service_host_runtime_publish(
            &runtime, &g4.generation, &previous), SALTS_SERVICE_HOST_OK);
        check_null(previous);
        check_equal(salts_service_host_scope_acquire(
            &runtime, &reopened_scope), SALTS_SERVICE_HOST_OK);
        check_equal(salts_service_host_scope_generation_id(
            &reopened_scope), UINT64_C(4));
        check_equal(scope_value(&reopened_scope), COMPONENT_PROVIDER_VALUE);
        check_equal(salts_service_host_runtime_close(
            &runtime, &previous), SALTS_SERVICE_HOST_OK);
        check_true(previous == &g4.generation);
        check_equal(salts_service_host_generation_drain(
            &runtime, &g4.generation), SALTS_SERVICE_HOST_BUSY);
        check_equal(salts_service_host_scope_release(
            &reopened_scope), SALTS_SERVICE_HOST_OK);
        check_equal(salts_service_host_generation_drain(
            &runtime, &g4.generation), SALTS_SERVICE_HOST_OK);

        check_equal(salts_service_host_runtime_destroy(
            &runtime), SALTS_SERVICE_HOST_OK);

        check_equal(cmeta_plugin_registry_request_stop(
            &registry, ref), CMETA_PLUGIN_OK);
        check_equal(cmeta_plugin_registry_poll_quiescent(
            &registry, ref, &quiet), CMETA_PLUGIN_OK);
        check_true(quiet);
        check_equal(cmeta_plugin_registry_unload(
            &registry, ref), CMETA_PLUGIN_OK);
        check_equal(cmeta_plugin_registry_destroy(
            &registry), CMETA_PLUGIN_OK);
    }
}
