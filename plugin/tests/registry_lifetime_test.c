#include <cmeta/plugin.h>
#include <tinytest.h>
#include "registry_lifetime_adapter.h"

#ifndef PLUGIN_REGISTRY_LIFETIME_PATH
#error PLUGIN_REGISTRY_LIFETIME_PATH must name the real passive fixture DSO
#endif

#if !defined(PLUGIN_REGISTRY_OTHER_PATH) || !defined(PLUGIN_REGISTRY_DUPLICATE_PATH)
#error Registry lifetime tests require distinct other-ID and duplicate-ID DSOs
#endif

typedef struct registry_close_observation {
    cmeta_plugin_registry *registry;
    cmeta_plugin_ref found;
    cmeta_plugin_ref duplicate;
    cmeta_plugin_ref other;
    cmeta_plugin_status find_status;
    cmeta_plugin_status duplicate_status;
    cmeta_plugin_status other_status;
    size_t count;
    unsigned calls;
} registry_close_observation;

static void observe_closed_slot(void *context) {
    registry_close_observation *observation = context;
    ++observation->calls;
    observation->find_status = cmeta_plugin_registry_find(
        observation->registry, "test.plugin.registry.lifetime", &observation->found);
    observation->duplicate_status = cmeta_plugin_registry_load(
        observation->registry, PLUGIN_REGISTRY_DUPLICATE_PATH, &observation->duplicate);
    observation->other_status = cmeta_plugin_registry_load(
        observation->registry, PLUGIN_REGISTRY_OTHER_PATH, &observation->other);
    observation->count = cmeta_plugin_registry_count(observation->registry);
}

suite("Plugin Registry lifetime-scoped identity") {
    it("keeps identity readable and reserved after native close until slot retirement") {
        const cmeta_plugin_registry_config config = {3u};
        const cmeta_plugin_registry_config holding_config = {2u};
        cmeta_plugin_registry registry = {0};
        cmeta_plugin_registry holding_registry = {0};
        cmeta_plugin_ref original = {0};
        cmeta_plugin_ref held_other = {0};
        cmeta_plugin_ref held_duplicate = {0};
        cmeta_plugin_ref found = {0};
        cmeta_plugin_lifecycle_info info = {0};
        registry_close_observation observation = {0};

        check_equal(cmeta_plugin_registry_init(&registry, &config), CMETA_PLUGIN_OK);
        check_equal(cmeta_plugin_registry_init(&holding_registry, &holding_config), CMETA_PLUGIN_OK);
        check_equal(cmeta_plugin_registry_load(
            &registry, PLUGIN_REGISTRY_LIFETIME_PATH, &original), CMETA_PLUGIN_OK);
        /* Keep both distinct candidate binaries mapped before closing the
         * original. Native reload cannot remap either onto its expired
         * manifest address and accidentally hide a stale manifest read. */
        check_equal(cmeta_plugin_registry_load(
            &holding_registry, PLUGIN_REGISTRY_OTHER_PATH, &held_other), CMETA_PLUGIN_OK);
        check_equal(cmeta_plugin_registry_load(
            &holding_registry, PLUGIN_REGISTRY_DUPLICATE_PATH, &held_duplicate), CMETA_PLUGIN_OK);

        observation.registry = &registry;
        cmeta_plugin_test_after_close(observe_closed_slot, &observation);
        check_equal(cmeta_plugin_registry_unload(&registry, original), CMETA_PLUGIN_OK);
        check_equal(observation.calls, 1u);
        check_equal(observation.find_status, CMETA_PLUGIN_OK);
        check_equal(observation.found.slot, original.slot);
        check_equal(observation.found.generation, original.generation);
        check_equal(observation.duplicate_status, CMETA_PLUGIN_DUPLICATE_PLUGIN_ID);
        check_true(!cmeta_plugin_ref_valid(observation.duplicate));
        check_equal(observation.other_status, CMETA_PLUGIN_OK);
        check_equal(observation.count, (size_t)2u);
        check_equal(cmeta_plugin_registry_count(&registry), (size_t)1u);
        check_equal(cmeta_plugin_registry_find(
            &registry, "test.plugin.registry.lifetime", &found), CMETA_PLUGIN_UNKNOWN_PLUGIN);
        check_equal(cmeta_plugin_registry_get_lifecycle(
            &registry, original, &info), CMETA_PLUGIN_STALE);
        check_equal(cmeta_plugin_registry_unload(&registry, observation.other), CMETA_PLUGIN_OK);
        check_equal(cmeta_plugin_registry_destroy(&registry), CMETA_PLUGIN_OK);
        check_equal(cmeta_plugin_registry_destroy(&holding_registry), CMETA_PLUGIN_OK);
    }

    it("retains identity and duplicate exclusion when native close fails") {
        const cmeta_plugin_registry_config config = {2u};
        cmeta_plugin_registry registry = {0};
        cmeta_plugin_ref original = {0};
        cmeta_plugin_ref found = {0};
        cmeta_plugin_ref duplicate = {0};

        check_equal(cmeta_plugin_registry_init(&registry, &config), CMETA_PLUGIN_OK);
        check_equal(cmeta_plugin_registry_load(
            &registry, PLUGIN_REGISTRY_LIFETIME_PATH, &original), CMETA_PLUGIN_OK);
        cmeta_plugin_test_fail_close_once();
        check_equal(cmeta_plugin_registry_unload(&registry, original), CMETA_PLUGIN_UNLOAD_FAILED);
        check_equal(cmeta_plugin_registry_find(
            &registry, "test.plugin.registry.lifetime", &found), CMETA_PLUGIN_OK);
        check_equal(found.slot, original.slot);
        check_equal(found.generation, original.generation);
        check_equal(cmeta_plugin_registry_load(
            &registry, PLUGIN_REGISTRY_DUPLICATE_PATH, &duplicate), CMETA_PLUGIN_DUPLICATE_PLUGIN_ID);
        check_true(!cmeta_plugin_ref_valid(duplicate));
        check_equal(cmeta_plugin_registry_unload(&registry, original), CMETA_PLUGIN_OK);
        check_equal(cmeta_plugin_registry_find(
            &registry, "test.plugin.registry.lifetime", &found), CMETA_PLUGIN_UNKNOWN_PLUGIN);
        check_equal(cmeta_plugin_registry_destroy(&registry), CMETA_PLUGIN_OK);
    }

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

