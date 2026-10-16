#ifndef SALTS_SERVICE_HOST_FIXTURE_H
#define SALTS_SERVICE_HOST_FIXTURE_H

#include <salts/component_provider_abi.h>

#define COMPONENT_PROVIDER_EXPORT_ID "component-provider"
#define COMPONENT_PROVIDER_AUX_EXPORT_ID "component-provider-aux"
#define COMPONENT_PROVIDER_LEGACY_EXPORT_ID "component-provider-legacy"
#define COMPONENT_PROVIDER_VALUE 37
#define COMPONENT_PROVIDER_AUX_VALUE 73

#define COMPONENT_PLUGIN_VALUE_METHODS(X, I) \
    X(I, FR0, int, get, value, &cmeta_type_int, CMETA_ABI_SCALAR, CMETA_RESULT_VALUE)

CMETA_INTERFACE(component_plugin_value, COMPONENT_PLUGIN_VALUE_METHODS);
CMETA_OBJECT_INTERFACE_ADAPTER(component_plugin_value);

#define COMPONENT_PLUGIN_AUX_METHODS(X, I) \
    X(I, FR0, int, get, value, &cmeta_type_int, CMETA_ABI_SCALAR, CMETA_RESULT_VALUE)

CMETA_INTERFACE(component_plugin_aux, COMPONENT_PLUGIN_AUX_METHODS);
CMETA_OBJECT_INTERFACE_ADAPTER(component_plugin_aux);


/* Intentionally valid legacy metadata but incomplete for cross-DSO ABI. */
#define COMPONENT_PLUGIN_LEGACY_METHODS(X, I) \
    X(I, R0, int, get, _)
CMETA_INTERFACE(component_plugin_legacy, COMPONENT_PLUGIN_LEGACY_METHODS);

#endif
