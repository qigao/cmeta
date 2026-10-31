#include <salts/service_host_abi.h>
#include <salts/configurator.h>
_Static_assert(SALTS_SERVICE_HOST_ABI_MAJOR == 2u, "ABI epoch mismatch");
int main(void)
{
    if (salts_configurator_abi_version() != SALTS_SERVICE_HOST_ABI_VERSION)
        return 1;
    if (salts_configurator_abi_context_size() != sizeof(salts_configurator_context))
        return 2;
    return 0;
}
