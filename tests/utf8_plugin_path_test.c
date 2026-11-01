/* Original PluginRegistry path validator against standalone Core. */
#include <cmeta/plugin.h>
#include <utf8.h>
#include <stdio.h>
#include <string.h>

#define CHECK(e) do { if (!(e)) { \
  fprintf(stderr, "M6 Plugin UTF-8 admission %d: %s\n", __LINE__, #e); \
  return __LINE__; \
} } while (0)

int main(void) {
  cmeta_plugin_registry registry = {0};
  cmeta_plugin_registry_config config = {2u};
  cmeta_plugin_ref ref = {0};
  char too_long[CMETA_PLUGIN_PATH_MAX + 2u];
  const char *const invalid[] = {
    "\xc0\xaf", "\xed\xa0\x80", "\xf4\x90\x80\x80",
    "\xe0\x80\xaf", "\xf0\x8f\xbf\xbf", "\xe6\x97", "\x80"
  };
  size_t i;
  CHECK(salts_core_utf8_valid("\xe6\x97\xa5",3u));
  CHECK(!salts_core_utf8_valid("\xed\xa0\x80",3u));
  CHECK(cmeta_plugin_registry_init(&registry, &config) == CMETA_PLUGIN_OK);
  CHECK(cmeta_plugin_registry_load(&registry, "", &ref) ==
        CMETA_PLUGIN_INVALID_ARGUMENT);
  CHECK(cmeta_plugin_registry_load(&registry, NULL, &ref) ==
        CMETA_PLUGIN_INVALID_ARGUMENT);
  for (i = 0u; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
    ref = (cmeta_plugin_ref){42u, 42u};
    CHECK(cmeta_plugin_registry_load(&registry, invalid[i], &ref) ==
          CMETA_PLUGIN_INVALID_ARGUMENT);
    CHECK(!cmeta_plugin_ref_valid(ref));
  }
  CHECK(cmeta_plugin_registry_load(&registry,
        "m6-nonexistent-plugin-xyz.so", &ref) == CMETA_PLUGIN_LOAD_FAILED);
  CHECK(cmeta_plugin_registry_load(&registry,
        "m6-\xe6\x97\xa5-\xf0\x9f\x8e\x89.so", &ref) == CMETA_PLUGIN_LOAD_FAILED);
  memset(too_long, 'a', sizeof(too_long));
  too_long[CMETA_PLUGIN_PATH_MAX + 1u] = '\0';
  CHECK(cmeta_plugin_registry_load(&registry, too_long, &ref) ==
        CMETA_PLUGIN_INVALID_ARGUMENT);
  CHECK(cmeta_plugin_registry_count(&registry) == 0u);
  CHECK(cmeta_plugin_registry_destroy(&registry) == CMETA_PLUGIN_OK);
  return 0;
}
