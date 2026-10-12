#include "component_plugin_fixture.h"
#include "tinytest.h"

#include <salts/service_host.h>

/*
 * ACE N -> N+1 exclusive-resource handoff.
 *
 * This is a deterministic domain-owner fixture, NOT a second socket/Plugin
 * loader. The unique external listener/writer stays with one stable owner.
 * Build/activate borrow it; only the domain owner changes its fencing epoch
 * after publication. The caller must use the fence before invoking a service:
 * a generation scope pins callback code/storage, not exclusive I/O authority.
 */
typedef struct exclusive_domain_owner {
    bool resource_open;
    uint64_t epoch;
    unsigned bind_count;
    unsigned close_count;
    unsigned accepted_reads;
    unsigned rejected_reads;
} exclusive_domain_owner;

typedef struct exclusive_provider {
    int value;  /* exact native cmeta_data_int storage */
    exclusive_domain_owner *domain;
    unsigned creates;
    unsigned activates;
    unsigned deactivates;
} exclusive_provider;

typedef struct exclusive_generation_fixture {
    salts_service_host_generation generation;
    salts_configurator_deployment deployment;
    salts_configurator_instance instance;
    size_t activation_order;
    salts_component_provider_binding binding;
    exclusive_provider provider;
} exclusive_generation_fixture;

/* One control-plane bind per acquired scope. The typed dispatch is borrowed;
 * repeated hot calls do not resolve metadata or touch a registry. */
typedef struct exclusive_bound_service {
    component_plugin_value value;
    uint64_t generation_id;
    bool live;
} exclusive_bound_service;

cmeta_component(ExclusiveDomainWriter,
    cmeta_provides(component_plugin_value));

static int exclusive_get(void *self) {
    return *(int *)self;
}

CMETA_IMPLEMENTS(
    component_plugin_value,
    exclusive_writer_impl,
    0u,
    .get = exclusive_get);

static cmeta_status exclusive_project(
    void *context,
    const cmeta_object_ref *object,
    const cmeta_interface_desc *expected,
    cmeta_interface_projection *out) {
    (void)context;
    if (object == NULL || out == NULL)
        return CMETA_INVALID_ARGUMENT;
    if (!cmeta_interface_desc_equal(
            expected, component_plugin_value_interface()))
        return CMETA_TRAIT_MISSING;

    out->size = sizeof(*out);
    out->interface = component_plugin_value_interface();
    out->self = object->object;
    out->dispatch = &exclusive_writer_impl_vtable;
    return CMETA_OK;
}

static const cmeta_object_interface_provider exclusive_interfaces = {
    sizeof(cmeta_object_interface_provider),
    NULL,
    exclusive_project
};

static cmeta_status SALTS_COMPONENT_CALL exclusive_create(
    void *provider_context,
    const cmeta_data_desc *config_data,
    const void *config_value,
    const salts_component_dependency *dependencies,
    size_t dependency_count,
    cmeta_object_ref *out_instance) {
    exclusive_provider *provider = (exclusive_provider *)provider_context;
    (void)dependencies;
    if (provider == NULL || out_instance == NULL ||
        config_data != NULL || config_value != NULL ||
        dependency_count != 0u)
        return CMETA_INVALID_ARGUMENT;
    ++provider->creates;
    return cmeta_object_borrow(
        out_instance, &provider->value, &cmeta_data_int, NULL);
}

static cmeta_status SALTS_COMPONENT_CALL exclusive_activate(
    void *provider_context,
    const cmeta_object_ref *instance) {
    exclusive_provider *provider = (exclusive_provider *)provider_context;
    if (provider == NULL || provider->domain == NULL ||
        !provider->domain->resource_open ||
        !cmeta_object_ref_valid(instance))
        return CMETA_CALLBACK_ERROR;
    ++provider->activates;
    /* Deliberately no resource bind or implicit switch on activate(). */
    return CMETA_OK;
}

static void SALTS_COMPONENT_CALL exclusive_deactivate(
    void *provider_context,
    const cmeta_object_ref *instance) {
    exclusive_provider *provider = (exclusive_provider *)provider_context;
    (void)instance;
    ++provider->deactivates;
    /* Neither generation owns/tears down the stable external resource. */
}

static bool domain_bind(exclusive_domain_owner *domain) {
    if (domain->resource_open)
        return false;
    domain->resource_open = true;
    ++domain->bind_count;
    return true;
}

static bool domain_fence(exclusive_domain_owner *domain, uint64_t epoch) {
    if (!domain->resource_open || epoch == 0u || epoch <= domain->epoch)
        return false;
    domain->epoch = epoch;
    return true;
}

static bool domain_close(exclusive_domain_owner *domain) {
    if (!domain->resource_open)
        return false;
    domain->resource_open = false;
    ++domain->close_count;
    return true;
}

/* Bind once while the original scope holds the provider generation.
 * This does not retain the object, vtable, module or exclusive resource. */
static salts_service_host_status domain_bind_service(
    const salts_service_host_scope *scope,
    exclusive_bound_service *bound) {
    salts_configurator_service service;
    component_plugin_value value = component_plugin_value_bind(NULL, NULL);
    if (bound == NULL || bound->live ||
        salts_service_host_scope_generation_id(scope) == UINT64_C(0))
        return SALTS_SERVICE_HOST_INVALID_STATE;
    if (salts_service_host_scope_find_service_from(
            scope, "ExclusiveDomainWriter",
            component_plugin_value_interface(),
            &service) != SALTS_SERVICE_HOST_OK)
        return SALTS_SERVICE_HOST_COMPONENT_ERROR;
    if (component_plugin_value_borrow_from_object(
            service.object, service.interfaces, &value) != CMETA_OK)
        return SALTS_SERVICE_HOST_COMPONENT_ERROR;
    bound->value = value;
    bound->generation_id = salts_service_host_scope_generation_id(scope);
    bound->live = true;
    return SALTS_SERVICE_HOST_OK;
}

/* Domain-owned fast path: verify the live enclosing scope and epoch BEFORE
 * calling the once-bound typed Interface. No per-call metadata lookup. */
static int domain_read(
    exclusive_domain_owner *domain,
    const salts_service_host_scope *scope,
    exclusive_bound_service *bound) {
    if (!domain->resource_open || !bound->live ||
        domain->epoch != bound->generation_id ||
        salts_service_host_scope_generation_id(scope) !=
            bound->generation_id) {
        ++domain->rejected_reads;
        return -1;
    }
    ++domain->accepted_reads;
    return component_plugin_value_get(&bound->value);
}

static salts_service_host_status build_exclusive_generation(
    exclusive_generation_fixture *fixture,
    exclusive_domain_owner *domain,
    uint64_t epoch,
    int value) {
    const salts_service_host_generation_storage storage = {
        &fixture->deployment, 1u,
        &fixture->instance, 1u,
        NULL, 0u,
        &fixture->activation_order, 1u,
        NULL, 0u
    };
    const salts_configurator_deployment deployment = {
        &fixture->binding, NULL, NULL
    };

    fixture->provider.value = value;
    fixture->provider.domain = domain;
    fixture->binding = (salts_component_provider_binding){
        sizeof(salts_component_provider_binding),
        SALTS_COMPONENT_PROVIDER_BINDING_ABI_VERSION,
        cmeta_component_meta(ExclusiveDomainWriter),
        &fixture->provider,
        &exclusive_interfaces,
        exclusive_create, exclusive_activate, exclusive_deactivate
    };
    return salts_service_host_generation_build(
        &fixture->generation, epoch, NULL, &storage,
        &deployment, 1u, NULL, 0u, NULL, 0u);
}

suite("ACE exclusive domain resource across Component generations") {
    it("retains one listener and fences borrowed N/N+1 operations") {
        exclusive_domain_owner domain = {0};
        exclusive_generation_fixture n = {0};
        exclusive_generation_fixture next = {0};
        salts_service_host_runtime runtime =
            SALTS_SERVICE_HOST_RUNTIME_INIT;
        salts_service_host_scope old_scope =
            SALTS_SERVICE_HOST_SCOPE_INIT;
        salts_service_host_scope new_scope =
            SALTS_SERVICE_HOST_SCOPE_INIT;
        exclusive_bound_service old_view = {0};
        exclusive_bound_service new_view = {0};
        salts_service_host_generation *previous = NULL;

        check_true(domain_bind(&domain));
        check_false(domain_bind(&domain));
        check_equal(domain.bind_count, 1u);

        check_equal(build_exclusive_generation(
            &n, &domain, UINT64_C(7), 17),
            SALTS_SERVICE_HOST_OK);
        check_equal(build_exclusive_generation(
            &next, &domain, UINT64_C(8), 18),
            SALTS_SERVICE_HOST_OK);
        /* Macro-generated canonical Strategy must use the same native
         * method signature; it does not create resource/module ownership. */
        {
            component_plugin_value strategy =
                exclusive_writer_impl_as_component_plugin_value(&n.provider.value);
            check_true(component_plugin_value_valid(&strategy));
            check_equal(component_plugin_value_get(&strategy), 17);
        }
        check_equal(n.provider.activates, 1u);
        check_equal(next.provider.activates, 1u);
        check_equal(domain.bind_count, 1u);

        check_equal(salts_service_host_runtime_init(&runtime),
                    SALTS_SERVICE_HOST_OK);
        check_equal(salts_service_host_runtime_publish(
            &runtime, &n.generation, &previous),
            SALTS_SERVICE_HOST_OK);
        check_null(previous);
        check_true(domain_fence(&domain, UINT64_C(7)));

        check_equal(salts_service_host_scope_acquire(
            &runtime, &old_scope), SALTS_SERVICE_HOST_OK);
        check_equal(domain_bind_service(&old_scope, &old_view),
                    SALTS_SERVICE_HOST_OK);
        check_equal(domain_read(&domain, &old_scope, &old_view), 17);

        check_equal(salts_service_host_runtime_publish(
            &runtime, &next.generation, &previous),
            SALTS_SERVICE_HOST_OK);
        check_true(previous == &n.generation);
        check_equal(salts_service_host_scope_acquire(
            &runtime, &new_scope), SALTS_SERVICE_HOST_OK);

        /* The new generation is published but not yet domain-admitted.
         * Old scope remains usable until the explicit epoch handoff. */
        check_equal(domain_bind_service(&new_scope, &new_view),
                    SALTS_SERVICE_HOST_OK);
        check_equal(domain_read(&domain, &new_scope, &new_view), -1);
        check_equal(domain_read(&domain, &old_scope, &old_view), 17);

        check_true(domain_fence(&domain, UINT64_C(8)));
        check_false(domain_fence(&domain, UINT64_C(7)));
        check_equal(domain_read(&domain, &old_scope, &old_view), -1);
        check_equal(domain_read(&domain, &new_scope, &new_view), 18);
        check_equal(salts_service_host_generation_drain(
            &runtime, &n.generation), SALTS_SERVICE_HOST_BUSY);

        check_equal(salts_service_host_scope_release(
            &old_scope), SALTS_SERVICE_HOST_OK);
        check_equal(domain_read(&domain, &old_scope, &old_view), -1);
        old_view.live = false;
        check_equal(salts_service_host_generation_drain(
            &runtime, &n.generation), SALTS_SERVICE_HOST_OK);
        check_equal(n.provider.deactivates, 1u);
        check_equal(next.provider.deactivates, 0u);
        check_true(domain.resource_open);
        check_equal(domain.close_count, 0u);

        check_equal(salts_service_host_runtime_close(
            &runtime, &previous), SALTS_SERVICE_HOST_OK);
        check_true(previous == &next.generation);
        check_equal(salts_service_host_generation_drain(
            &runtime, &next.generation), SALTS_SERVICE_HOST_BUSY);
        check_equal(salts_service_host_scope_release(
            &new_scope), SALTS_SERVICE_HOST_OK);
        new_view.live = false;
        check_equal(salts_service_host_generation_drain(
            &runtime, &next.generation), SALTS_SERVICE_HOST_OK);
        check_equal(next.provider.deactivates, 1u);
        check_true(domain.resource_open);
        check_equal(domain.bind_count, 1u);

        check_equal(salts_service_host_runtime_destroy(&runtime),
                    SALTS_SERVICE_HOST_OK);
        check_true(domain_close(&domain));
        check_false(domain_close(&domain));
        check_equal(domain.close_count, 1u);
        check_equal(domain.accepted_reads, 3u);
        check_equal(domain.rejected_reads, 3u);
    }
}
