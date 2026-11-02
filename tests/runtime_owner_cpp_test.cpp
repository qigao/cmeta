#include <cmeta/service_host.h>
#include <cmeta/plugin.h>
#include <cmeta/service_host_abi.h>
#include <runtime_abi.h>
#include <cmeta_buffer.h>
#include <type_traits>
#include <cmeta/cnet_policy.h>
static_assert(std::is_standard_layout<salts_service_host_scope>::value,
              "ServiceHost scope requires a C-native record");
int main() {
  mem_buffer_t *backing = mem_get_buffer(mem_global(), 8u);
  if (backing == nullptr) return 3;
  mem_set_used(backing, 8u);
  mem_slice_t slice = mem_slice(backing, 0u, 8u);
  cmeta_cleanup obligation = CMETA_CLEANUP_INIT;
  if (cmeta_cnet_slice_cleanup(&obligation, &slice) != CMETA_OK) {
    mem_slice_release(&slice); mem_buffer_release(backing); return 4;
  }
  try {
    cmeta::cleanup_scope guard(obligation);
    throw 1;
  } catch (int) {}
  bool released = slice.buffer == nullptr && mem_buffer_ref_count(backing) == 1u;
  mem_buffer_release(backing);
  if (!released) return 5;
  if (!cmeta_data_desc_valid(cmeta_reflected_data(cnet_send_usage)) ||
      cmeta_data_struct_constructible(cmeta_reflected_data(cnet_send_usage))) return 6;
  salts_service_host_runtime runtime = SALTS_SERVICE_HOST_RUNTIME_INIT;
  salts_core_runtime_abi_0_1_required();
  if (salts_core_runtime_abi_version() != SALTS_CORE_RUNTIME_ABI_VERSION ||
      salts_core_runtime_mem_slice_size() != sizeof(mem_slice_t) ||
      salts_service_host_abi_version() != SALTS_SERVICE_HOST_ABI_VERSION ||
      salts_service_host_runtime_init(&runtime) != SALTS_SERVICE_HOST_OK)
    return 1;
  return salts_service_host_runtime_destroy(&runtime) == SALTS_SERVICE_HOST_OK ? 0 : 2;
}
