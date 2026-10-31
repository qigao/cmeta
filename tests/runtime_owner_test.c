/* Native runtime owner and Provider lease regression. */
#include <salts/service_host.h>
#include <salts/plugin.h>
#include <salts/service_host_abi.h>
#include <salts_core/runtime_abi.h>
#include <salts_core/utf8.h>
#include <cmeta_buffer.h>
#include <salts/thread.h>
#include <cmeta/data_reflect.h>
#include <stdio.h>
#include <string.h>
#ifndef SALTS_TEST_PROVIDER
#error SALTS_TEST_PROVIDER must reference native fixture DSO
#endif

#define CHECK(e) do { if (!(e)) { \
  fprintf(stderr, "Native runtime owner %d: %s\n",__LINE__,#e); \
  return __LINE__; } } while (0)

typedef struct native_aligned_value {
  _Alignas(64) int value;
} native_aligned_value;
cmeta_reflect_value(native_aligned_value, "test.native.aligned-value",
  cmeta_data_field(int, value, &cmeta_data_int, &cmeta_type_int)
);

static int aligned_temp_lifetime(void) {
  const cmeta_data_desc *desc = cmeta_reflected_data(native_aligned_value);
  cmeta_data_temp temp = {0};
  CHECK(cmeta_data_desc_valid(desc));
  CHECK(CMETA_ALIGNOF(native_aligned_value) > CMETA_ALIGNOF(cmeta_capture_storage));
  CHECK(cmeta_data_temp_open(desc, sizeof(native_aligned_value) - 1u, &temp) ==
        CMETA_CAPACITY_EXCEEDED);
  CHECK(temp.storage == NULL);
  CHECK(cmeta_data_temp_open(desc, sizeof(native_aligned_value), &temp) == CMETA_OK);
  CHECK((uintptr_t)temp.storage % CMETA_ALIGNOF(native_aligned_value) == 0u);
  CHECK(((native_aligned_value *)temp.storage)->value == 0);
  ((native_aligned_value *)temp.storage)->value = 42;
  cmeta_data_temp_close(&temp);
  CHECK(temp.storage == NULL && temp.data == NULL && temp.extent == 0u);
  cmeta_data_temp_close(&temp);
  CHECK(cmeta_data_temp_open(&cmeta_data_int, sizeof(int), &temp) == CMETA_OK);
  CHECK(*(int *)temp.storage == 0);
  cmeta_data_temp_close(&temp);
  CHECK(temp.storage == NULL);
  return 0;
}

int main(void) {
  salts_service_host_runtime runtime = SALTS_SERVICE_HOST_RUNTIME_INIT;
  cmeta_plugin_registry registry = {0};
  cmeta_plugin_registry_config cfg = {2u};
  cmeta_plugin_ref ref = {0};
  cmeta_plugin_lease lease = {0};
  const cmeta_plugin_manifest *manifest = NULL;
  bool quiet = false;
  CHECK(aligned_temp_lifetime() == 0);
  salts_core_runtime_abi_0_1_required();
  CHECK(salts_core_runtime_abi_version() == SALTS_CORE_RUNTIME_ABI_VERSION);
  CHECK(salts_core_runtime_mem_pool_size() == sizeof(mem_pool_t));
  CHECK(salts_core_runtime_mem_slice_size() == sizeof(mem_slice_t));
  CHECK(salts_core_utf8_valid("\xe6\x97\xa5",3u));
  CHECK(salts_configurator_abi_version() == SALTS_SERVICE_HOST_ABI_VERSION);
  CHECK(salts_service_host_abi_version() == SALTS_SERVICE_HOST_ABI_VERSION);
  CHECK(cmeta_plugin_registry_init(&registry,&cfg) == CMETA_PLUGIN_OK);
  CHECK(cmeta_plugin_registry_load(&registry,"\xed\xa0\x80",&ref) ==
        CMETA_PLUGIN_INVALID_ARGUMENT);
  /* Load the native Provider DSO and prove
   * that borrowed plugin code cannot unload while a live lease exists. */
  CHECK(cmeta_plugin_registry_load(&registry,
        SALTS_TEST_PROVIDER, &ref) == CMETA_PLUGIN_OK);
  CHECK(cmeta_plugin_registry_start(&registry, ref) == CMETA_PLUGIN_OK);
  CHECK(cmeta_plugin_registry_acquire(&registry, ref, &lease, &manifest) ==
        CMETA_PLUGIN_OK);
  CHECK(manifest != NULL &&
        strcmp(manifest->plugin_id, "test.component.provider") == 0);
  CHECK(cmeta_plugin_registry_request_stop(&registry, ref) == CMETA_PLUGIN_OK);
  CHECK(cmeta_plugin_registry_unload(&registry, ref) == CMETA_PLUGIN_BUSY);
  CHECK(cmeta_plugin_registry_release(&registry, &lease) == CMETA_PLUGIN_OK);
  CHECK(cmeta_plugin_registry_poll_quiescent(&registry, ref, &quiet) ==
        CMETA_PLUGIN_OK);
  CHECK(quiet);
  CHECK(cmeta_plugin_registry_unload(&registry, ref) == CMETA_PLUGIN_OK);
  CHECK(cmeta_plugin_registry_count(&registry) == 0u);
  CHECK(cmeta_plugin_registry_destroy(&registry) == CMETA_PLUGIN_OK);
  CHECK(salts_service_host_runtime_init(&runtime) == SALTS_SERVICE_HOST_OK);
  CHECK(salts_service_host_runtime_destroy(&runtime) == SALTS_SERVICE_HOST_OK);
  CHECK(mem_global() != NULL && cmeta_thread_current_token() != NULL);
  return 0;
}
