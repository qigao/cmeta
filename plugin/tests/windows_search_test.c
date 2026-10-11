#include "windows_fixture.h"
#include "windows_dependency.h"
#include "tinytest.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static cmeta_plugin_registry registry;
static cmeta_plugin_ref ref;
static cmeta_plugin_lease lease;
static bool initialized;
static bool loaded;
static bool started;
static bool leased;
static DLL_DIRECTORY_COOKIE directory_cookie;

static void load_and_invoke(const char *path) {
    const cmeta_plugin_manifest *manifest = NULL;
    const cmeta_plugin_export *entry = NULL;
    cmeta_plugin_status status;
    enum { INPUT = 11 };
    int input = INPUT;
    int output = 0;
    const void *args[] = {&input};

    status = cmeta_plugin_registry_load(&registry, path, &ref);
    loaded = status == CMETA_PLUGIN_OK;
    check_equal(status, CMETA_PLUGIN_OK);
    status = cmeta_plugin_registry_start(&registry, ref);
    started = status == CMETA_PLUGIN_OK;
    check_equal(status, CMETA_PLUGIN_OK);
    status = cmeta_plugin_registry_acquire(&registry, ref, &lease, &manifest);
    leased = status == CMETA_PLUGIN_OK;
    check_equal(status, CMETA_PLUGIN_OK);
    check_equal(cmeta_plugin_manifest_find_export(
        manifest, PLUGIN_WINDOWS_EXPORT_ID, &entry), CMETA_PLUGIN_OK);
    check_equal(cmeta_plugin_export_require_function(entry,
        PLUGIN_WINDOWS_CONTRACT_ID, PLUGIN_WINDOWS_CONTRACT_VERSION, 0), CMETA_PLUGIN_OK);
    check_true(cmeta_function_abi_contract_compatible(
        FunctionAbi(plugin_windows_transform), entry->value.function.abi));
    check_true(entry->value.function.invoke(
        entry->value.function.context, &output, args, 1));
    check_equal(output, INPUT + PLUGIN_WINDOWS_BIAS);
}

suite("Windows Plugin explicit-path dependency admission") {
    before_each() {
        const cmeta_plugin_registry_config config = {1};
        cmeta_plugin_status status;
        initialized = loaded = started = leased = false;
        directory_cookie = NULL;
        status = cmeta_plugin_registry_init(&registry, &config);
        initialized = status == CMETA_PLUGIN_OK;
        check_equal(status, CMETA_PLUGIN_OK);
    }

    after_each() {
        bool quiet = false;
        if (leased)
            check_equal(cmeta_plugin_registry_release(&registry, &lease), CMETA_PLUGIN_OK);
        if (started) {
            check_equal(cmeta_plugin_registry_request_stop(&registry, ref), CMETA_PLUGIN_OK);
            check_equal(cmeta_plugin_registry_poll_quiescent(&registry, ref, &quiet), CMETA_PLUGIN_OK);
            check_true(quiet);
        }
        if (loaded)
            check_equal(cmeta_plugin_registry_unload(&registry, ref), CMETA_PLUGIN_OK);
        if (initialized)
            check_equal(cmeta_plugin_registry_destroy(&registry), CMETA_PLUGIN_OK);
        /* Keep the test-owned directory admitted until its module is unloaded. */
        if (directory_cookie)
            check_true(RemoveDllDirectory(directory_cookie));
    }

    it("loads a colocated dependency for an absolute plugin path") {
        load_and_invoke(PRIVATE_PLUGIN_PATH);
    }

    it("normalizes a relative plugin path before dependency discovery") {
        load_and_invoke(RELATIVE_PLUGIN_PATH);
    }

    it("honors an explicitly registered dependency directory") {
        directory_cookie = AddDllDirectory(REGISTERED_DEPENDENCY_DIR);
        check_not_null(directory_cookie);
        load_and_invoke(REGISTERED_PLUGIN_PATH);
    }
}
