#include <cmeta/service_host_abi.h>
#include <cmeta/configurator.h>
uint32_t salts_configurator_abi_version(void)
{
    return SALTS_SERVICE_HOST_ABI_VERSION;
}
size_t salts_configurator_abi_context_size(void)
{
    return sizeof(salts_configurator_context);
}
