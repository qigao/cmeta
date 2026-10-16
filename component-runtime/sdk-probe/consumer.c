#include <salts/component_runtime_abi.h>
#include <salts/configurator.h>
#include <salts/service_host.h>
#include <stdint.h>
_Static_assert(SALTS_COMPONENT_RUNTIME_ABI_MAJOR == 1u, "unexpected ABI epoch");
_Static_assert(SALTS_COMPONENT_RUNTIME_ABI_VERSION == 0x00010000u, "unexpected ABI version");
int main(void)
{
    salts_service_host_runtime runtime = SALTS_SERVICE_HOST_RUNTIME_INIT;
    salts_service_host_abi_v1_0_required();
    if (salts_configurator_abi_context_size() != sizeof(salts_configurator_context) ||
        salts_service_host_abi_scope_size() != sizeof(salts_service_host_scope))
        return 4;
    if (salts_configurator_abi_version() != SALTS_COMPONENT_RUNTIME_ABI_VERSION ||
        salts_service_host_abi_version() != SALTS_COMPONENT_RUNTIME_ABI_VERSION)
        return 1;
    if (salts_service_host_runtime_init(&runtime) != SALTS_SERVICE_HOST_OK)
        return 2;
    if (salts_service_host_runtime_destroy(&runtime) != SALTS_SERVICE_HOST_OK)
        return 3;
    return 0;
}
