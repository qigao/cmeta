/* Original Salts CMeta/Configurator plus installed SaltsCore shared owner. */
#include <cmeta/abi.h>
#include <cmeta/cmeta.h>
#include <salts/configurator.h>
#include <salts/component_runtime_abi.h>
#include <salts_core/bytes.h>
#include <salts_core/runtime_abi.h>
#include <cmeta_buffer.h>
#include <salts/thread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern mem_pool_t *salts_m6_provider_pool(void);
extern const void *salts_m6_provider_thread_token(void);
extern uint32_t salts_m6_provider_cmeta_epoch(void);
extern uint32_t salts_m6_provider_core_epoch(void);
extern int salts_m6_provider_component_admitted(void);
extern mem_slice_t salts_m6_provider_make_slice(void);
extern void salts_m6_provider_release(mem_slice_t *);

#define REQUIRE(expr) do { if (!(expr)) { \
  fprintf(stderr, "Salts M6 C11 line %d: %s\n", __LINE__, #expr); \
  return __LINE__; \
} } while (0)

int main(void) {
  salts_core_bytes_view view = {0};
  salts_configurator_context ctx = SALTS_CONFIGURATOR_CONTEXT_INIT;
  mem_buffer_t *buf = NULL;
  mem_slice_t local = {0};
  mem_slice_t remote = {0};
  /* Calls from the executable and provider DSO must resolve the same 0.1
   * link marker and concrete runtime layout (no legacy Salts shim). */
  salts_core_runtime_abi_0_1_required();
  REQUIRE(salts_core_runtime_abi_version() == SALTS_CORE_RUNTIME_ABI_VERSION);
  REQUIRE(salts_m6_provider_core_epoch() == SALTS_CORE_RUNTIME_ABI_VERSION);
  REQUIRE(salts_core_runtime_mem_pool_size() == sizeof(mem_pool_t));
  REQUIRE(salts_core_runtime_mem_buffer_size() == sizeof(mem_buffer_t));
  REQUIRE(salts_core_runtime_mem_slice_size() == sizeof(mem_slice_t));
  REQUIRE(salts_core_bytes_view_init(&view, "abc", 3u) == SALTS_CORE_OK);
  REQUIRE(view.len == 3u && memcmp(view.data, "abc", 3u) == 0);
  REQUIRE(cmeta_reflection_abi_version() == CMETA_REFLECTION_ABI_VERSION);
  REQUIRE(salts_m6_provider_cmeta_epoch() == CMETA_REFLECTION_ABI_VERSION);
  REQUIRE(salts_configurator_abi_version() == SALTS_COMPONENT_RUNTIME_ABI_VERSION);
  REQUIRE(cmeta_type_desc_valid(&cmeta_type_int32));
  REQUIRE(salts_m6_provider_component_admitted());

  REQUIRE(salts_configurator_context_init(&ctx, NULL, 0u, NULL, 0u,
                                          NULL, 0u, NULL, 0u,
                                          NULL, 0u) == SALTS_COMPONENT_OK);
  REQUIRE(ctx.state == SALTS_CONFIGURATOR_CONTEXT_READY);
  REQUIRE(salts_configurator_context_resolve(&ctx) == SALTS_COMPONENT_OK);
  REQUIRE(salts_configurator_context_start(&ctx) == SALTS_COMPONENT_OK);
  REQUIRE(salts_configurator_context_stop(&ctx) == SALTS_COMPONENT_OK);
  REQUIRE(ctx.state == SALTS_CONFIGURATOR_CONTEXT_STOPPED);

  REQUIRE(salts_m6_provider_pool() == mem_global());
  REQUIRE(salts_m6_provider_thread_token() == cmeta_thread_current_token());
  buf = mem_get_buffer(mem_global(), 1u);
  REQUIRE(buf != NULL);
  mem_buffer_data(buf)[0] = (char)0x2a;
  mem_set_used(buf, 1u);
  local = mem_slice(buf, 0u, 1u);
  REQUIRE(local.buffer && mem_buffer_pool(local.buffer) == salts_m6_provider_pool());
  mem_buffer_release(buf);
  REQUIRE((unsigned char)local.data[0] == 0x2au);
  salts_m6_provider_release(&local);
  REQUIRE(!local.buffer);

  remote = salts_m6_provider_make_slice();
  REQUIRE(remote.buffer != NULL && remote.length == 1u);
  REQUIRE(mem_buffer_pool(remote.buffer) == mem_global());
  REQUIRE((unsigned char)remote.data[0] == 0x3cu);
  mem_slice_release(&remote);
  REQUIRE(!remote.buffer);
  return 0;
}
