/* POSIX loader interoperability probe. Not Salts::Plugin lease/unload logic. */
#include <salts/plugin.h>
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "DSO query failed at %d: %s\n", __LINE__, #expr); \
    return 1; \
} } while (0)

int main(int argc, char **argv) {
    CHECK(argc == 2 && argv != NULL && argv[1] != NULL);
    void *module = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (module == NULL) {
        fprintf(stderr, "dlopen: %s\n", dlerror());
        return 1;
    }
    /* POSIX supports dlsym for function symbols; memcpy avoids an ISO C
     * function-pointer/object-pointer conversion in this diagnostic client. */
    void *symbol = dlsym(module, CMETA_PLUGIN_QUERY_SYMBOL);
    cmeta_plugin_query_fn query = NULL;
    if (symbol == NULL || sizeof(symbol) != sizeof(query)) {
        fprintf(stderr, "missing/unsupported query symbol: %s\n",
                CMETA_PLUGIN_QUERY_SYMBOL);
        (void)dlclose(module);
        return 1;
    }
    memcpy(&query, &symbol, sizeof(query));
    const cmeta_plugin_manifest *m = query(CMETA_PLUGIN_ABI_VERSION);
    int accepted = m != NULL &&
                   m->struct_size == CMETA_PLUGIN_MANIFEST_SIZE &&
                   m->abi_version == CMETA_PLUGIN_ABI_VERSION &&
                   m->plugin_id != NULL &&
                   strcmp(m->plugin_id, "salts.compiler.dso") == 0 &&
                   m->version.major == 1u && m->version.minor == 2u &&
                   m->version.patch == 3u &&
                   m->export_count == 0u && m->exports == NULL &&
                   m->self == NULL && m->start == NULL &&
                   m->request_stop == NULL && m->is_quiescent == NULL &&
                   m->destroy == NULL;
    int rejected = query(CMETA_PLUGIN_ABI_VERSION + 1u) == NULL;
    /* All borrowed data is checked before unloading the provider. */
    int closed = dlclose(module) == 0;
    CHECK(accepted && rejected && closed);
    puts("Native shared Plugin query ABI: PASS (bootstrap only, no lease admission)");
    return 0;
}
