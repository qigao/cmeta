#ifndef SALTS_SERVICE_HOST_PROVIDER_ABI_H
#define SALTS_SERVICE_HOST_PROVIDER_ABI_H

#include <cmeta/configurator_abi.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SALTS_SERVICE_HOST_PROVIDER_CONTRACT_ID "salts.service_host.provider.v2"
#define SALTS_SERVICE_HOST_PROVIDER_CONTRACT_VERSION UINT32_C(2)

static const cmeta_type_identity salts_configurator_provider_binding_identity =
    CMETA_TYPE_ID_ATOM_INIT("salts.configurator.provider_binding.v2");
static const cmeta_type_identity salts_configurator_provider_binding_const_identity =
    CMETA_TYPE_ID_CONST_INIT(&salts_configurator_provider_binding_identity);
static const cmeta_type_identity salts_configurator_provider_binding_pointer_identity =
    CMETA_TYPE_ID_POINTER_INIT(&salts_configurator_provider_binding_const_identity);

static const cmeta_type_desc salts_configurator_provider_binding_const_type = {
    "const salts_configurator_provider_binding",
    sizeof(salts_configurator_provider_binding),
    CMETA_ALIGNOF(salts_configurator_provider_binding),
    CMETA_T_OBJECT,
    NULL,
    NULL,
    &salts_configurator_provider_binding_const_identity
};

static const cmeta_type_desc salts_configurator_provider_binding_pointer_type = {
    "const salts_configurator_provider_binding *",
    sizeof(const salts_configurator_provider_binding *),
    CMETA_ALIGNOF(const salts_configurator_provider_binding *),
    CMETA_T_POINTER,
    &salts_configurator_provider_binding_const_type,
    NULL,
    &salts_configurator_provider_binding_pointer_identity
};

#define SALTS_SERVICE_HOST_PROVIDER_METHODS(X, I) \
    X(I,FR0,const salts_configurator_provider_binding *,get_binding,value, \
      &salts_configurator_provider_binding_pointer_type, \
      CMETA_ABI_OBJECT_POINTER,CMETA_RESULT_BORROWED)

CMETA_INTERFACE(salts_service_host_provider, SALTS_SERVICE_HOST_PROVIDER_METHODS);

#ifdef __cplusplus
}
#endif

#endif /* SALTS_SERVICE_HOST_PROVIDER_ABI_H */
