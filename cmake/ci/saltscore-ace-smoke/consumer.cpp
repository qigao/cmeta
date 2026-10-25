#include <cmeta/abi.h>
#include <cmeta/cmeta.h>
#include <salts/configurator.h>
#include <salts/component_runtime_abi.h>
#include <salts_core/bytes.h>
#include <salts_core/runtime_abi.h>
#include <cmeta_buffer.h>
#include <salts/thread.h>
#include <type_traits>
#include <cstdint>

extern "C" mem_pool_t *salts_m6_provider_pool(void);
extern "C" const void *salts_m6_provider_thread_token(void);
extern "C" uint32_t salts_m6_provider_cmeta_epoch(void);
extern "C" uint32_t salts_m6_provider_core_epoch(void);

static_assert(std::is_standard_layout<salts_configurator_context>::value,
              "Configurator must remain a C/C++ compatible ABI record");
static_assert(std::is_standard_layout<mem_slice_t>::value,
              "C11 retained slices must work from C++17");
int main() {
  salts_core_runtime_abi_0_1_required();
  salts_core_bytes_view view{};
  const bool ok =
      salts_core_bytes_view_init(&view, "x", 1) == SALTS_CORE_OK &&
      view.len == 1 &&
      salts_core_runtime_abi_version() == SALTS_CORE_RUNTIME_ABI_VERSION &&
      salts_m6_provider_core_epoch() == SALTS_CORE_RUNTIME_ABI_VERSION &&
      salts_core_runtime_mem_pool_size() == sizeof(mem_pool_t) &&
      salts_core_runtime_mem_buffer_size() == sizeof(mem_buffer_t) &&
      salts_core_runtime_mem_slice_size() == sizeof(mem_slice_t) &&
      cmeta_reflection_abi_version() == CMETA_REFLECTION_ABI_VERSION &&
      salts_configurator_abi_version() == SALTS_COMPONENT_RUNTIME_ABI_VERSION &&
      salts_m6_provider_cmeta_epoch() == CMETA_REFLECTION_ABI_VERSION &&
      salts_m6_provider_pool() == mem_global() &&
      salts_m6_provider_thread_token() == cmeta_thread_current_token() &&
      salts_configurator_status_string(SALTS_COMPONENT_OK) != nullptr;
  return ok ? 0 : 1;
}
