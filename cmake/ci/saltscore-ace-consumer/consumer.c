/* Independent installed C11 host: no Salts source in this build. */
#include <salts/service_host.h>
#include <salts/plugin.h>
#include <salts/component_runtime_abi.h>
#include <salts_core/runtime_abi.h>
#include <salts_core/utf8.h>
#include <cmeta_buffer.h>
#include <salts/thread.h>
#include <stdio.h>
#include <string.h>
#ifndef SALTS_M6_INSTALLED_PROVIDER
#error SALTS_M6_INSTALLED_PROVIDER must reference installed fixture DSO
#endif

#define CHECK(e) do { if (!(e)) { \
  fprintf(stderr, "M6 installed consumer %d: %s\n",__LINE__,#e); \
  return __LINE__; } } while (0)

int main(void) {
  salts_service_host_runtime runtime = SALTS_SERVICE_HOST_RUNTIME_INIT;
  cmeta_plugin_registry registry = {0};
  cmeta_plugin_registry_config cfg = {2u};
  cmeta_plugin_ref ref = {0};
  cmeta_plugin_lease lease = {0};
  const cmeta_plugin_manifest *manifest = NULL;
  bool quiet = false;
  salts_core_runtime_abi_0_1_required();
  CHECK(salts_core_runtime_abi_version() == SALTS_CORE_RUNTIME_ABI_VERSION);
  CHECK(salts_core_runtime_mem_pool_size() == sizeof(mem_pool_t));
  CHECK(salts_core_runtime_mem_slice_size() == sizeof(mem_slice_t));
  CHECK(salts_core_utf8_valid("\xe6\x97\xa5",3u));
  CHECK(salts_configurator_abi_version() == SALTS_COMPONENT_RUNTIME_ABI_VERSION);
  CHECK(salts_service_host_abi_version() == SALTS_COMPONENT_RUNTIME_ABI_VERSION);
  CHECK(cmeta_plugin_registry_init(&registry,&cfg) == CMETA_PLUGIN_OK);
  CHECK(cmeta_plugin_registry_load(&registry,"\xed\xa0\x80",&ref) ==
        CMETA_PLUGIN_INVALID_ARGUMENT);
  /* Load the installed DSO from the relocated package prefix and prove
   * that borrowed plugin code cannot unload while a live lease exists. */
  CHECK(cmeta_plugin_registry_load(&registry,
        SALTS_M6_INSTALLED_PROVIDER, &ref) == CMETA_PLUGIN_OK);
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
