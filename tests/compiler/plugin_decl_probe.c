/* Ordinary explicit Plugin manifest ABI, not linker-section discovery. */
#include <salts/plugin_linker.h>
#include <stdio.h>
#include <string.h>

CMETA_PLUGIN_DECLARE_EMPTY(
    compiler_probe_empty,
    "salts.compiler.probe",
    (1u,0u,0u),
    CMETA_PLUGIN_PASSIVE());

#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "Plugin declaration probe failed at %d: %s\n", __LINE__, #expr); \
    return 1; \
} } while (0)

int main(void) {
    const cmeta_plugin_manifest *m = cmeta_plugin_query(CMETA_PLUGIN_ABI_VERSION);
    CHECK(m != NULL);
    CHECK(m->export_count == 0u && m->exports == NULL);
    CHECK(strcmp(m->plugin_id, "salts.compiler.probe") == 0);
    CHECK(cmeta_plugin_query(CMETA_PLUGIN_ABI_VERSION + 1u) == NULL);
    CHECK(CMETA_PLUGIN_HAS_LINKER_EXPORTS ==
          (CMETA_SECTION_BACKEND != CMETA_SECTION_NONE));
    printf("Plugin explicit manifest: PASS; linker section backend=%d (%s)\n",
           (int)CMETA_SECTION_BACKEND,
           CMETA_PLUGIN_HAS_LINKER_EXPORTS ? "not independently qualified" :
                                              "unavailable; fail closed");
    return 0;
}
