#include <salts/component_runtime_abi.h>
#include <salts/configurator.h>
#include <salts/service_host.h>
#include <cstdint>
static_assert(SALTS_COMPONENT_RUNTIME_ABI_MAJOR == 1u, "unexpected ABI epoch");
static_assert(SALTS_COMPONENT_RUNTIME_ABI_VERSION == 0x00010000u, "unexpected ABI version");
int main()
{
    salts_service_host_runtime runtime = SALTS_SERVICE_HOST_RUNTIME_INIT;
    if (salts_configurator_abi_version() != SALTS_COMPONENT_RUNTIME_ABI_VERSION ||
        salts_service_host_abi_version() != SALTS_COMPONENT_RUNTIME_ABI_VERSION)
        return 1;
    if (salts_service_host_runtime_init(&runtime) != SALTS_SERVICE_HOST_OK)
        return 2;
    if (salts_service_host_runtime_destroy(&runtime) != SALTS_SERVICE_HOST_OK)
        return 3;
    return 0;
}
