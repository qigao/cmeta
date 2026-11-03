#include "registry_lifetime_adapter.h"
#include "../src/plugin_loader_internal.h"

static void (*after_close)(void *);
static void *after_close_context;
static bool fail_close;

void cmeta_plugin_test_after_close(void (*callback)(void *), void *context) {
    after_close = callback;
    after_close_context = context;
}

void cmeta_plugin_test_fail_close_once(void) {
    fail_close = true;
}

static cmeta_plugin_status registry_test_close(cmeta_plugin_library *library) {
    cmeta_plugin_status status;
    if (fail_close) {
        fail_close = false;
        return CMETA_PLUGIN_UNLOAD_FAILED;
    }
    status = cmeta_plugin_platform_close(library);
    if (status == CMETA_PLUGIN_OK && after_close != NULL) {
        void (*callback)(void *) = after_close;
        void *context = after_close_context;
        /* Nested load rejection can close another DSO; invoke only once. */
        after_close = NULL;
        after_close_context = NULL;
        callback(context);
    }
    return status;
}

/* Exercise the original Registry TU with a deterministic interleaving after
 * actual DSO close, before its slot is retired. No second Registry algorithm
 * and no provider/native synchronization implementation are introduced. */
#define cmeta_plugin_platform_close registry_test_close
#include "../src/plugin_registry.c"
