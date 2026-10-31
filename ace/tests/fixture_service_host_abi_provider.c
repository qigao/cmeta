/* Exact original strict FR Provider test DSO, compiled solely against the
 * canonical native Salts ABI. Two different module identities, no loader copy. */
#if !defined(ABI_TEST_PROVIDER_INDEX)
#error ABI_TEST_PROVIDER_INDEX must be selected by the native test
#elif ABI_TEST_PROVIDER_INDEX == 1
#define ABI_TEST_SDK_PROVIDER_ID "test.installed.servicehost.n"
#elif ABI_TEST_PROVIDER_INDEX == 2
#define ABI_TEST_SDK_PROVIDER_ID "test.installed.servicehost.next"
#else
#error unsupported Provider module instance
#endif

#include "service_host_fixture.h"
#include <salts/plugin_decl.h>

cmeta_component(ServiceHostProviderFixture,
    cmeta_provides(service_host_provider_value));

cmeta_component(ServiceHostProviderAuxFixture,
    cmeta_provides(service_host_provider_aux));

cmeta_component(ServiceHostProviderLegacyFixture,
    cmeta_provides(service_host_provider_legacy));

static int fixture_value = COMPONENT_PROVIDER_VALUE;
static int fixture_aux_value = COMPONENT_PROVIDER_AUX_VALUE;

static int fixture_value_get(void *self) {
    return *(int *)self;
}

CMETA_IMPLEMENTS(
    service_host_provider_value,
    fixture_value_impl,
    0u,
    .get = fixture_value_get);

CMETA_IMPLEMENTS(
    service_host_provider_aux,
    fixture_aux_impl,
    0u,
    .get = fixture_value_get);

static cmeta_status fixture_project(
    void *context,
    const cmeta_object_ref *object,
    const cmeta_interface_desc *expected,
    cmeta_interface_projection *out) {
    (void)context;
    if (object == NULL || out == NULL)
        return CMETA_INVALID_ARGUMENT;
    out->size = sizeof(*out);
    out->self = object->object;

    if (cmeta_interface_desc_equal(
            expected, service_host_provider_value_interface())) {
        out->interface = service_host_provider_value_interface();
        out->dispatch = &fixture_value_impl_vtable;
        return CMETA_OK;
    }

    if (cmeta_interface_desc_equal(
            expected, service_host_provider_aux_interface())) {
        out->interface = service_host_provider_aux_interface();
        out->dispatch = &fixture_aux_impl_vtable;
        return CMETA_OK;
    }

    return CMETA_TRAIT_MISSING;
}

static const cmeta_object_interface_provider fixture_interfaces = {
    sizeof(cmeta_object_interface_provider),
    NULL,
    fixture_project
};

static cmeta_status SALTS_CONFIGURATOR_CALL fixture_create(
    void *provider_context,
    const cmeta_data_desc *config_data,
    const void *config_value,
    const salts_configurator_dependency *dependencies,
    size_t dependency_count,
    cmeta_object_ref *out_instance) {
    (void)config_data;
    (void)config_value;
    (void)dependencies;
    (void)dependency_count;
    if (config_data != NULL || config_value != NULL ||
        dependency_count != 0u)
        return CMETA_INVALID_ARGUMENT;
    if (provider_context == NULL)
        return CMETA_INVALID_ARGUMENT;
    return cmeta_object_borrow(
        out_instance, provider_context, &cmeta_data_int, NULL);
}

static const salts_configurator_provider_binding fixture_binding = {
    sizeof(salts_configurator_provider_binding),
    SALTS_CONFIGURATOR_PROVIDER_BINDING_ABI_VERSION,
    cmeta_component_meta(ServiceHostProviderFixture),
    &fixture_value,
    &fixture_interfaces,
    fixture_create,
    NULL,
    NULL
};

static const salts_configurator_provider_binding fixture_aux_binding = {
    sizeof(salts_configurator_provider_binding),
    SALTS_CONFIGURATOR_PROVIDER_BINDING_ABI_VERSION,
    cmeta_component_meta(ServiceHostProviderAuxFixture),
    &fixture_aux_value,
    &fixture_interfaces,
    fixture_create,
    NULL,
    NULL
};

/* ProviderBinding is structurally valid; its borrowed service method is not
 * fully reflected. Dynamic admission must reject without calling create(). */
static const salts_configurator_provider_binding fixture_legacy_binding = {
    sizeof(salts_configurator_provider_binding),
    SALTS_CONFIGURATOR_PROVIDER_BINDING_ABI_VERSION,
    cmeta_component_meta(ServiceHostProviderLegacyFixture),
    &fixture_value,
    &fixture_interfaces,
    fixture_create,
    NULL,
    NULL
};

static const salts_configurator_provider_binding *fixture_get_binding(void *self) {
    return (const salts_configurator_provider_binding *)self;
}

CMETA_IMPLEMENTS(
    salts_service_host_provider,
    fixture_component_provider_impl,
    0u,
    .get_binding = fixture_get_binding);

static salts_service_host_provider fixture_component_provider = {
    (void *)&fixture_binding,
    &fixture_component_provider_impl_vtable
};

static salts_service_host_provider fixture_aux_component_provider = {
    (void *)&fixture_aux_binding,
    &fixture_component_provider_impl_vtable
};

static salts_service_host_provider fixture_legacy_component_provider = {
    (void *)&fixture_legacy_binding,
    &fixture_component_provider_impl_vtable
};

#define COMPONENT_PROVIDER_EXPORTS(X) \
    X(interface, (salts_service_host_provider, &fixture_component_provider), \
      COMPONENT_PROVIDER_EXPORT_ID, SALTS_SERVICE_HOST_PROVIDER_CONTRACT_ID, \
      SALTS_SERVICE_HOST_PROVIDER_CONTRACT_VERSION, 0) \
    X(interface, (salts_service_host_provider, &fixture_aux_component_provider), \
      COMPONENT_PROVIDER_AUX_EXPORT_ID, SALTS_SERVICE_HOST_PROVIDER_CONTRACT_ID, \
      SALTS_SERVICE_HOST_PROVIDER_CONTRACT_VERSION, 0) \
    X(interface, (salts_service_host_provider, &fixture_legacy_component_provider), \
      COMPONENT_PROVIDER_LEGACY_EXPORT_ID, SALTS_SERVICE_HOST_PROVIDER_CONTRACT_ID, \
      SALTS_SERVICE_HOST_PROVIDER_CONTRACT_VERSION, 0)

CMETA_PLUGIN_DECLARE(
    component_provider_fixture,
    ABI_TEST_SDK_PROVIDER_ID,
    (1,0,0),
    COMPONENT_PROVIDER_EXPORTS,
    CMETA_PLUGIN_PASSIVE());
