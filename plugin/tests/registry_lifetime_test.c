#include <cmeta/plugin.h>
#include <tinytest.h>

#ifndef PLUGIN_REGISTRY_LIFETIME_PATH
#error PLUGIN_REGISTRY_LIFETIME_PATH must name the real passive fixture DSO
#endif

suite("Plugin Registry lifetime-scoped identity") {
    it("retains live leases across a refused destroy and rejects stale Slots within one Registry") {
        const cmeta_plugin_registry_config config = {1u};
        cmeta_plugin_registry registry = {0};
        cmeta_plugin_ref original = {0};
        cmeta_plugin_ref recycled = {0};
        cmeta_plugin_ref next_incarnation = {0};
        cmeta_plugin_lease lease = {0};
        const cmeta_plugin_manifest *manifest = NULL;
        cmeta_plugin_lifecycle_info info = {0};
        bool quiet = false;

        check_equal(cmeta_plugin_registry_init(&registry, &config),
                    CMETA_PLUGIN_OK);
        check_equal(cmeta_plugin_registry_load(
                        &registry, PLUGIN_REGISTRY_LIFETIME_PATH, &original),
                    CMETA_PLUGIN_OK);
        check_equal(cmeta_plugin_registry_start(&registry, original),
                    CMETA_PLUGIN_OK);
        check_equal(cmeta_plugin_registry_acquire(
                        &registry, original, &lease, &manifest),
                    CMETA_PLUGIN_OK);
        check_not_null(manifest);
        check_true(cmeta_plugin_lease_valid(lease));

        /* Stop closes admission, but neither it nor a failed destroy may
         * drop the authoritative DSO borrow or reset Registry storage. */
        check_equal(cmeta_plugin_registry_request_stop(&registry, original),
                    CMETA_PLUGIN_OK);
        check_equal(cmeta_plugin_registry_poll_quiescent(
                        &registry, original, &quiet),
                    CMETA_PLUGIN_OK);
        check_true(!quiet);
        check_equal(cmeta_plugin_registry_destroy(&registry),
                    CMETA_PLUGIN_BUSY);
        check_not_null(registry.impl);
        check_equal(cmeta_plugin_registry_init(&registry, &config),
                    CMETA_PLUGIN_INVALID_ARGUMENT);
        check_equal(cmeta_plugin_registry_unload(&registry, original),
                    CMETA_PLUGIN_BUSY);
        check_equal(cmeta_plugin_registry_get_lifecycle(
                        &registry, original, &info),
                    CMETA_PLUGIN_OK);
        check_equal(info.active_leases, (size_t)1u);

        check_equal(cmeta_plugin_registry_release(&registry, &lease),
                    CMETA_PLUGIN_OK);
        manifest = NULL; /* All DSO-owned borrowed pointers expire here. */
        check_true(!cmeta_plugin_lease_valid(lease));
        check_equal(cmeta_plugin_registry_poll_quiescent(
                        &registry, original, &quiet),
                    CMETA_PLUGIN_OK);
        check_true(quiet);
        check_equal(cmeta_plugin_registry_unload(&registry, original),
                    CMETA_PLUGIN_OK);

        /* Within this one Registry, reuse of the same Slot must fence its
         * previous Ref with a different generation. */
        check_equal(cmeta_plugin_registry_load(
                        &registry, PLUGIN_REGISTRY_LIFETIME_PATH, &recycled),
                    CMETA_PLUGIN_OK);
        check_equal(recycled.slot, original.slot);
        check_true(recycled.generation != original.generation);
        check_equal(cmeta_plugin_registry_get_lifecycle(
                        &registry, original, &info),
                    CMETA_PLUGIN_STALE);
        check_equal(cmeta_plugin_registry_unload(&registry, recycled),
                    CMETA_PLUGIN_OK);
        check_equal(cmeta_plugin_registry_destroy(&registry),
                    CMETA_PLUGIN_OK);
        check_true(registry.impl == NULL);

        /* A new init is a NEW owner domain. The old Ref is not presented
         * to it; equal numeric Slot generations would not grant authority. */
        check_equal(cmeta_plugin_registry_init(&registry, &config),
                    CMETA_PLUGIN_OK);
        check_equal(cmeta_plugin_registry_load(
                        &registry, PLUGIN_REGISTRY_LIFETIME_PATH,
                        &next_incarnation),
                    CMETA_PLUGIN_OK);
        check_true(cmeta_plugin_ref_valid(next_incarnation));
        check_equal(cmeta_plugin_registry_start(&registry, next_incarnation),
                    CMETA_PLUGIN_OK);
        check_equal(cmeta_plugin_registry_acquire(
                        &registry, next_incarnation, &lease, &manifest),
                    CMETA_PLUGIN_OK);
        check_not_null(manifest);
        check_equal(cmeta_plugin_registry_request_stop(
                        &registry, next_incarnation),
                    CMETA_PLUGIN_OK);
        check_equal(cmeta_plugin_registry_release(&registry, &lease),
                    CMETA_PLUGIN_OK);
        manifest = NULL;
        quiet = false;
        check_equal(cmeta_plugin_registry_poll_quiescent(
                        &registry, next_incarnation, &quiet),
                    CMETA_PLUGIN_OK);
        check_true(quiet);
        check_equal(cmeta_plugin_registry_unload(
                        &registry, next_incarnation),
                    CMETA_PLUGIN_OK);
        check_equal(cmeta_plugin_registry_destroy(&registry),
                    CMETA_PLUGIN_OK);
    }
}
