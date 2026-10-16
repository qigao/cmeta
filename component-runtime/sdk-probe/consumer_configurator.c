#include <salts/component_runtime_abi.h>
#include <salts/configurator.h>
_Static_assert(SALTS_COMPONENT_RUNTIME_ABI_MAJOR == 1u, "ABI epoch mismatch");
int main(void)
{
    if (salts_configurator_abi_version() != SALTS_COMPONENT_RUNTIME_ABI_VERSION)
        return 1;
    if (salts_configurator_abi_context_size() != sizeof(salts_configurator_context))
        return 2;
    return 0;
}
