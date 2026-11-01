#include <cmeta/service_host_abi.h>
#include <cmeta/configurator.h>
#include <cmeta/service_host.h>
#include <cstdint>
static_assert(SALTS_SERVICE_HOST_ABI_MAJOR == 2u, "unexpected ABI epoch");
static_assert(SALTS_SERVICE_HOST_ABI_VERSION == 0x00020000u, "unexpected ABI version");
int main()
{
    if (salts_service_host_generation_recover(nullptr) !=
        SALTS_SERVICE_HOST_INVALID_ARGUMENT)
        return 5;
    salts_service_host_runtime runtime = SALTS_SERVICE_HOST_RUNTIME_INIT;
    salts_service_host_abi_v2_0_required();
    if (salts_configurator_abi_context_size() != sizeof(salts_configurator_context) ||
        salts_service_host_abi_scope_size() != sizeof(salts_service_host_scope))
        return 4;
    if (salts_configurator_abi_version() != SALTS_SERVICE_HOST_ABI_VERSION ||
        salts_service_host_abi_version() != SALTS_SERVICE_HOST_ABI_VERSION)
        return 1;
    if (salts_service_host_runtime_init(&runtime) != SALTS_SERVICE_HOST_OK)
        return 2;
    if (salts_service_host_runtime_destroy(&runtime) != SALTS_SERVICE_HOST_OK)
        return 3;
    return 0;
}
