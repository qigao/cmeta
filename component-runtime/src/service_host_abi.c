#include <salts/component_runtime_abi.h>
#include <salts/service_host.h>
uint32_t salts_service_host_abi_version(void)
{
    return SALTS_COMPONENT_RUNTIME_ABI_VERSION;
}
void salts_service_host_abi_v1_0_required(void)
{
}
size_t salts_service_host_abi_scope_size(void)
{
    return sizeof(salts_service_host_scope);
}
