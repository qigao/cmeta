#include <salts/component_runtime_abi.h>
#include <salts/configurator.h>
uint32_t salts_configurator_abi_version(void)
{
    return SALTS_COMPONENT_RUNTIME_ABI_VERSION;
}
size_t salts_configurator_abi_context_size(void)
{
    return sizeof(salts_configurator_context);
}
