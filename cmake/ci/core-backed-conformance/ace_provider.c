/* Real Salts Component ABI provider DSO on the installed Core owner. */
#include <salts/component_abi.h>
#include <cmeta/abi.h>
#include <cmeta/cmeta.h>
#include <cmeta_buffer.h>
#include <salts_core/runtime_abi.h>
#include <salts/thread.h>
#include <stdint.h>

#if defined(_WIN32)
# define M6_EXPORT __declspec(dllexport)
#elif defined(__GNUC__) || defined(__clang__)
# define M6_EXPORT __attribute__((visibility("default")))
#else
# define M6_EXPORT
#endif

M6_EXPORT mem_pool_t *salts_m6_provider_pool(void) { return mem_global(); }
M6_EXPORT const void *salts_m6_provider_thread_token(void) {
  return cmeta_thread_current_token();
}
/* Exact link-marker admission from a separate provider DSO, not an alias. */
M6_EXPORT uint32_t salts_m6_provider_core_epoch(void) {
  salts_core_runtime_abi_0_1_required();
  return salts_core_runtime_abi_version();
}
M6_EXPORT uint32_t salts_m6_provider_cmeta_epoch(void) {
  return cmeta_reflection_abi_version();
}
M6_EXPORT int salts_m6_provider_component_admitted(void) {
  return !salts_component_provider_binding_valid(NULL) &&
         cmeta_type_desc_valid(&cmeta_type_int32);
}
M6_EXPORT mem_slice_t salts_m6_provider_make_slice(void) {
  mem_slice_t slice = {0};
  mem_buffer_t *buffer = mem_get_buffer(mem_global(), 1u);
  if (!buffer) return slice;
  mem_buffer_data(buffer)[0] = (char)0x3c;
  mem_set_used(buffer, 1u);
  slice = mem_slice(buffer, 0u, 1u);
  mem_buffer_release(buffer);
  return slice;
}
M6_EXPORT void salts_m6_provider_release(mem_slice_t *slice) {
  mem_slice_release(slice);
}
