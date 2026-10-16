#ifndef SALTS_COMPONENT_PROVIDER_ABI_H
#define SALTS_COMPONENT_PROVIDER_ABI_H

#include <salts/component_abi.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SALTS_COMPONENT_PROVIDER_CONTRACT_ID "salts.component.provider.v1"
#define SALTS_COMPONENT_PROVIDER_CONTRACT_VERSION UINT32_C(1)

static const cmeta_type_identity salts_component_provider_binding_identity =
    CMETA_TYPE_ID_ATOM_INIT("salts.component.provider_binding.v1");
static const cmeta_type_identity salts_component_provider_binding_const_identity =
    CMETA_TYPE_ID_CONST_INIT(&salts_component_provider_binding_identity);
static const cmeta_type_identity salts_component_provider_binding_pointer_identity =
    CMETA_TYPE_ID_POINTER_INIT(&salts_component_provider_binding_const_identity);

static const cmeta_type_desc salts_component_provider_binding_const_type = {
    "const salts_component_provider_binding",
    sizeof(salts_component_provider_binding),
    CMETA_ALIGNOF(salts_component_provider_binding),
    CMETA_T_OBJECT,
    NULL,
    NULL,
    &salts_component_provider_binding_const_identity
};

static const cmeta_type_desc salts_component_provider_binding_pointer_type = {
    "const salts_component_provider_binding *",
    sizeof(const salts_component_provider_binding *),
    CMETA_ALIGNOF(const salts_component_provider_binding *),
    CMETA_T_POINTER,
    &salts_component_provider_binding_const_type,
    NULL,
    &salts_component_provider_binding_pointer_identity
};

#define SALTS_COMPONENT_PROVIDER_METHODS(X, I) \
    X(I,FR0,const salts_component_provider_binding *,get_binding,value, \
      &salts_component_provider_binding_pointer_type, \
      CMETA_ABI_OBJECT_POINTER,CMETA_RESULT_BORROWED)

CMETA_INTERFACE(salts_component_provider, SALTS_COMPONENT_PROVIDER_METHODS);

#ifdef __cplusplus
}
#endif

#endif /* SALTS_COMPONENT_PROVIDER_ABI_H */
