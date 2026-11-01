#include <cmeta/service_host.h>
#include <cmeta/plugin.h>
#include <cmeta/service_host_abi.h>
#include <runtime_abi.h>
#include <cmeta_buffer.h>
#include <type_traits>
static_assert(std::is_standard_layout<salts_service_host_scope>::value,
              "ServiceHost scope requires a C-native record");
int main() {
  salts_service_host_runtime runtime = SALTS_SERVICE_HOST_RUNTIME_INIT;
  salts_core_runtime_abi_0_1_required();
  if (salts_core_runtime_abi_version() != SALTS_CORE_RUNTIME_ABI_VERSION ||
      salts_core_runtime_mem_slice_size() != sizeof(mem_slice_t) ||
      salts_service_host_abi_version() != SALTS_SERVICE_HOST_ABI_VERSION ||
      salts_service_host_runtime_init(&runtime) != SALTS_SERVICE_HOST_OK)
    return 1;
  return salts_service_host_runtime_destroy(&runtime) == SALTS_SERVICE_HOST_OK ? 0 : 2;
}
