#include <salts/plugin.h>

#include "plugin_loader_internal.h"

#include <salts/thread.h>
#if defined(SALTS_CORE_BACKED_PROFILE) && SALTS_CORE_BACKED_PROFILE
/* Unreleased 3.0 profile: strict UTF-8 is owned by installed shared Core. */
#include <salts_core/utf8.h>
#else
#include <vstr.h>
#endif

#include <limits.h>
#include <stdlib.h>

typedef struct cmeta_plugin_registry_slot {
    cmeta_plugin_library library;
    const cmeta_plugin_manifest *manifest;
    uint32_t generation;
    cmeta_plugin_lifecycle_state state;
    cmeta_plugin_status failure;
    size_t callbacks_inflight;
    size_t active_leases;
    uint64_t lease_active_mask;
    uint32_t lease_generations[CMETA_PLUGIN_MAX_LEASES_PER_PLUGIN];
    bool occupied;
    bool unloading;
    bool destroy_called;
} cmeta_plugin_registry_slot;

typedef struct cmeta_plugin_registry_impl {
    cmeta_plugin_registry_slot *slots;
    size_t capacity;
    size_t count;
    size_t loads_inflight;
    cmeta_mutex_t lock;
    bool destroying;
} cmeta_plugin_registry_impl;

static bool bounded_cstr_valid(const char *value, size_t max_bytes) {
    size_t index;

    if (value == NULL || max_bytes == 0u)
        return false;

    for (index = 0u; index <= max_bytes; ++index) {
        if (value[index] == '\0')
            return index != 0u;
    }
    return false;
}

static bool bounded_utf8_path_valid(const char *path) {
    size_t length;

    if (path == NULL)
        return false;

    for (length = 0u; length <= CMETA_PLUGIN_PATH_MAX; ++length) {
        if (path[length] == '\0') {
            if (length == 0u)
                return false;
#if defined(SALTS_CORE_BACKED_PROFILE) && SALTS_CORE_BACKED_PROFILE
            return salts_core_utf8_valid(path, length) != 0;
#else
            return vstr_utf8_valid(vstr_from_buf(path, length)) != 0;
#endif
        }
    }
    return false;
}

static bool plugin_id_equal(const char *left, const char *right) {
    size_t index;

    if (!bounded_cstr_valid(left, CMETA_PLUGIN_ID_MAX) ||
        !bounded_cstr_valid(right, CMETA_PLUGIN_ID_MAX))
        return false;

    for (index = 0u; index <= CMETA_PLUGIN_ID_MAX; ++index) {
        if (left[index] != right[index])
            return false;
        if (left[index] == '\0')
            return true;
    }
    return false;
}

static uint32_t next_generation(uint32_t current) {
    uint32_t next = current + 1u;
    return next == 0u ? 1u : next;
}

static cmeta_plugin_status close_rejected_library(
    cmeta_plugin_library *library,
    cmeta_plugin_status rejection) {
    cmeta_plugin_status close_status = cmeta_plugin_platform_close(library);
    return close_status == CMETA_PLUGIN_OK ? rejection : close_status;
}

static void release_load_reservation(
    cmeta_plugin_registry_impl *impl) {
    cmeta_mutex_lock(&impl->lock);
    --impl->loads_inflight;
    cmeta_mutex_unlock(&impl->lock);
}

static cmeta_plugin_status close_rejected_load(
    cmeta_plugin_registry_impl *impl,
    cmeta_plugin_library *library,
    cmeta_plugin_status rejection) {
    cmeta_plugin_status status =
        close_rejected_library(library, rejection);
    release_load_reservation(impl);
    return status;
}

static cmeta_plugin_registry_impl *registry_impl(
    const cmeta_plugin_registry *registry) {
    return registry == NULL
        ? NULL
        : (cmeta_plugin_registry_impl *)registry->impl;
}

static cmeta_plugin_status slot_for_ref_locked(
    cmeta_plugin_registry_impl *impl,
    cmeta_plugin_ref ref,
    cmeta_plugin_registry_slot **out_slot,
    size_t *out_index) {
    size_t index;
    cmeta_plugin_registry_slot *slot;

    if (out_slot != NULL)
        *out_slot = NULL;
    if (out_index != NULL)
        *out_index = SIZE_MAX;

    if (impl == NULL || !cmeta_plugin_ref_valid(ref))
        return CMETA_PLUGIN_INVALID_ARGUMENT;

    index = (size_t)ref.slot - 1u;
    if (index >= impl->capacity)
        return CMETA_PLUGIN_STALE;

    slot = &impl->slots[index];
    if (!slot->occupied || slot->generation != ref.generation)
        return CMETA_PLUGIN_STALE;

    if (out_slot != NULL)
        *out_slot = slot;
    if (out_index != NULL)
        *out_index = index;
    return CMETA_PLUGIN_OK;
}

static void clear_slot_locked(
    cmeta_plugin_registry_impl *impl,
    cmeta_plugin_registry_slot *slot) {
    size_t index;

    slot->manifest = NULL;
    slot->occupied = false;
    slot->unloading = false;
    slot->destroy_called = false;
    slot->state = (cmeta_plugin_lifecycle_state)0;
    slot->failure = CMETA_PLUGIN_OK;
    slot->callbacks_inflight = 0u;
    slot->active_leases = 0u;
    slot->lease_active_mask = 0u;
    slot->generation = next_generation(slot->generation);
    for (index = 0u; index < CMETA_PLUGIN_MAX_LEASES_PER_PLUGIN; ++index)
        slot->lease_generations[index] =
            next_generation(slot->lease_generations[index]);
    if (impl->count != 0u)
        --impl->count;
}

cmeta_plugin_status cmeta_plugin_registry_init(
    cmeta_plugin_registry *registry,
    const cmeta_plugin_registry_config *config) {
    cmeta_plugin_registry_impl *impl;
    size_t slot_index;
    size_t lease_index;

    if (registry == NULL || config == NULL || registry->impl != NULL ||
        config->capacity == 0u)
        return CMETA_PLUGIN_INVALID_ARGUMENT;

    if (config->capacity > (size_t)UINT32_MAX ||
        config->capacity > SIZE_MAX / sizeof(cmeta_plugin_registry_slot))
        return CMETA_PLUGIN_CAPACITY_EXCEEDED;

    impl = (cmeta_plugin_registry_impl *)calloc(1u, sizeof(*impl));
    if (impl == NULL)
        return CMETA_PLUGIN_ALLOCATION_FAILED;

    impl->slots = (cmeta_plugin_registry_slot *)calloc(
        config->capacity, sizeof(*impl->slots));
    if (impl->slots == NULL) {
        free(impl);
        return CMETA_PLUGIN_ALLOCATION_FAILED;
    }

    cmeta_mutex_init(&impl->lock);
    if (impl->lock == NULL) {
        free(impl->slots);
        free(impl);
        return CMETA_PLUGIN_ALLOCATION_FAILED;
    }

    impl->capacity = config->capacity;
    for (slot_index = 0u; slot_index < impl->capacity; ++slot_index) {
        impl->slots[slot_index].generation = 1u;
        for (lease_index = 0u;
             lease_index < CMETA_PLUGIN_MAX_LEASES_PER_PLUGIN;
             ++lease_index)
            impl->slots[slot_index].lease_generations[lease_index] = 1u;
    }

    registry->impl = impl;
    return CMETA_PLUGIN_OK;
}

cmeta_plugin_status cmeta_plugin_registry_load(
    cmeta_plugin_registry *registry,
    const char *path,
    cmeta_plugin_ref *out_ref) {
    cmeta_plugin_registry_impl *impl;
    cmeta_plugin_registry_slot *slot = NULL;
    cmeta_plugin_library library = {0};
    cmeta_plugin_query_fn query = NULL;
    const cmeta_plugin_manifest *manifest;
    cmeta_plugin_status status;
    size_t slot_index = 0u;
    size_t index;

    if (out_ref == NULL)
        return CMETA_PLUGIN_INVALID_ARGUMENT;
    *out_ref = (cmeta_plugin_ref){0};

    impl = registry_impl(registry);
    if (impl == NULL || !bounded_utf8_path_valid(path))
        return CMETA_PLUGIN_INVALID_ARGUMENT;

    /*
     * Reserve bounded admission capacity under the registry lock, then execute
     * platform loader and plugin-owned query code without that lock held.
     * destroy() observes loads_inflight and cannot free impl while the
     * reservation exists.
     */
    cmeta_mutex_lock(&impl->lock);
    if (impl->destroying) {
        cmeta_mutex_unlock(&impl->lock);
        return CMETA_PLUGIN_BUSY;
    }
    if (impl->count >= impl->capacity ||
        impl->loads_inflight >= impl->capacity - impl->count) {
        cmeta_mutex_unlock(&impl->lock);
        return CMETA_PLUGIN_CAPACITY_EXCEEDED;
    }
    ++impl->loads_inflight;
    cmeta_mutex_unlock(&impl->lock);

    status = cmeta_plugin_platform_open(path, &library, &query);
    if (status != CMETA_PLUGIN_OK) {
        cmeta_mutex_lock(&impl->lock);
        --impl->loads_inflight;
        cmeta_mutex_unlock(&impl->lock);
        return status;
    }

    manifest = query(CMETA_PLUGIN_ABI_VERSION);
    if (manifest == NULL) {
        return close_rejected_load(
            impl, &library, CMETA_PLUGIN_QUERY_REJECTED);
    }

    status = cmeta_plugin_manifest_validate(manifest);
    if (status != CMETA_PLUGIN_OK)
        return close_rejected_load(impl, &library, status);

    /*
     * Publication is the only second locked phase. Re-check registry state and
     * duplicate identity because another concurrent load may have published
     * while this candidate was open/querying.
     */
    cmeta_mutex_lock(&impl->lock);

    if (impl->destroying) {
        status = CMETA_PLUGIN_BUSY;
        goto reject_locked;
    }

    for (index = 0u; index < impl->capacity; ++index) {
        const cmeta_plugin_registry_slot *existing = &impl->slots[index];
        if (existing->occupied &&
            plugin_id_equal(existing->manifest->plugin_id,
                            manifest->plugin_id)) {
            status = CMETA_PLUGIN_DUPLICATE_PLUGIN_ID;
            goto reject_locked;
        }
    }

    for (slot_index = 0u; slot_index < impl->capacity; ++slot_index) {
        if (!impl->slots[slot_index].occupied) {
            slot = &impl->slots[slot_index];
            break;
        }
    }
    if (slot == NULL) {
        status = CMETA_PLUGIN_CAPACITY_EXCEEDED;
        goto reject_locked;
    }

    slot->library = library;
    slot->manifest = manifest;
    slot->state = CMETA_PLUGIN_LIFECYCLE_LOADED;
    slot->failure = CMETA_PLUGIN_OK;
    slot->occupied = true;
    slot->unloading = false;
    slot->destroy_called = false;
    slot->callbacks_inflight = 0u;
    slot->active_leases = 0u;
    slot->lease_active_mask = 0u;
    ++impl->count;
    --impl->loads_inflight;

    out_ref->slot = (uint32_t)(slot_index + 1u);
    out_ref->generation = slot->generation;
    cmeta_mutex_unlock(&impl->lock);
    return CMETA_PLUGIN_OK;

reject_locked:
    cmeta_mutex_unlock(&impl->lock);
    return close_rejected_load(impl, &library, status);
}

cmeta_plugin_status cmeta_plugin_registry_find(
    const cmeta_plugin_registry *registry,
    const char *plugin_id,
    cmeta_plugin_ref *out_ref) {
    cmeta_plugin_registry_impl *impl = registry_impl(registry);
    size_t index;

    if (out_ref == NULL)
        return CMETA_PLUGIN_INVALID_ARGUMENT;
    *out_ref = (cmeta_plugin_ref){0};

    if (impl == NULL ||
        !bounded_cstr_valid(plugin_id, CMETA_PLUGIN_ID_MAX))
        return CMETA_PLUGIN_INVALID_ARGUMENT;

    cmeta_mutex_lock(&impl->lock);
    for (index = 0u; index < impl->capacity; ++index) {
        const cmeta_plugin_registry_slot *slot = &impl->slots[index];
        if (slot->occupied &&
            plugin_id_equal(slot->manifest->plugin_id, plugin_id)) {
            out_ref->slot = (uint32_t)(index + 1u);
            out_ref->generation = slot->generation;
            cmeta_mutex_unlock(&impl->lock);
            return CMETA_PLUGIN_OK;
        }
    }
    cmeta_mutex_unlock(&impl->lock);
    return CMETA_PLUGIN_UNKNOWN_PLUGIN;
}

cmeta_plugin_status cmeta_plugin_registry_start(
    cmeta_plugin_registry *registry,
    cmeta_plugin_ref ref) {
    cmeta_plugin_registry_impl *impl = registry_impl(registry);
    cmeta_plugin_registry_slot *slot;
    cmeta_plugin_start_fn callback;
    void *self;
    cmeta_plugin_status status;

    if (impl == NULL)
        return CMETA_PLUGIN_INVALID_ARGUMENT;

    cmeta_mutex_lock(&impl->lock);
    status = slot_for_ref_locked(impl, ref, &slot, NULL);
    if (status != CMETA_PLUGIN_OK) {
        cmeta_mutex_unlock(&impl->lock);
        return status;
    }
    if (impl->destroying || slot->unloading) {
        cmeta_mutex_unlock(&impl->lock);
        return CMETA_PLUGIN_BUSY;
    }
    if (slot->state == CMETA_PLUGIN_LIFECYCLE_STARTED) {
        cmeta_mutex_unlock(&impl->lock);
        return CMETA_PLUGIN_ALREADY;
    }
    if (slot->state != CMETA_PLUGIN_LIFECYCLE_LOADED) {
        cmeta_mutex_unlock(&impl->lock);
        return CMETA_PLUGIN_INVALID_STATE;
    }
    if (slot->active_leases != 0u || slot->callbacks_inflight != 0u) {
        cmeta_mutex_unlock(&impl->lock);
        return CMETA_PLUGIN_BUSY;
    }

    callback = slot->manifest->start;
    self = slot->manifest->self;
    if (callback == NULL) {
        slot->state = CMETA_PLUGIN_LIFECYCLE_STARTED;
        cmeta_mutex_unlock(&impl->lock);
        return CMETA_PLUGIN_OK;
    }

    slot->state = CMETA_PLUGIN_LIFECYCLE_STARTING;
    ++slot->callbacks_inflight;
    cmeta_mutex_unlock(&impl->lock);

    status = callback(self);

    cmeta_mutex_lock(&impl->lock);
    --slot->callbacks_inflight;
    if (status == CMETA_PLUGIN_OK) {
        slot->state = CMETA_PLUGIN_LIFECYCLE_STARTED;
    } else {
        /*
         * start() is failure-atomic by contract: a failed start has not
         * published service work and must remain destroyable/unloadable.
         */
        slot->state = CMETA_PLUGIN_LIFECYCLE_QUIESCENT;
        if (slot->failure == CMETA_PLUGIN_OK)
            slot->failure = status;
    }
    cmeta_mutex_unlock(&impl->lock);
    return status;
}

cmeta_plugin_status cmeta_plugin_registry_acquire(
    cmeta_plugin_registry *registry,
    cmeta_plugin_ref ref,
    cmeta_plugin_lease *out_lease,
    const cmeta_plugin_manifest **out_manifest) {
    cmeta_plugin_registry_impl *impl = registry_impl(registry);
    cmeta_plugin_registry_slot *slot;
    cmeta_plugin_status status;
    size_t index;

    if (out_lease == NULL || out_manifest == NULL)
        return CMETA_PLUGIN_INVALID_ARGUMENT;
    *out_lease = (cmeta_plugin_lease){0};
    *out_manifest = NULL;

    if (impl == NULL)
        return CMETA_PLUGIN_INVALID_ARGUMENT;

    cmeta_mutex_lock(&impl->lock);
    status = slot_for_ref_locked(impl, ref, &slot, NULL);
    if (status != CMETA_PLUGIN_OK) {
        cmeta_mutex_unlock(&impl->lock);
        return status;
    }
    if (impl->destroying || slot->unloading) {
        cmeta_mutex_unlock(&impl->lock);
        return CMETA_PLUGIN_BUSY;
    }
    if (slot->state != CMETA_PLUGIN_LIFECYCLE_STARTED) {
        cmeta_mutex_unlock(&impl->lock);
        return CMETA_PLUGIN_INVALID_STATE;
    }

    for (index = 0u; index < CMETA_PLUGIN_MAX_LEASES_PER_PLUGIN; ++index) {
        const uint64_t bit = UINT64_C(1) << index;
        if ((slot->lease_active_mask & bit) == 0u) {
            slot->lease_active_mask |= bit;
            ++slot->active_leases;
            out_lease->plugin = ref;
            out_lease->slot = (uint32_t)(index + 1u);
            out_lease->generation = slot->lease_generations[index];
            *out_manifest = slot->manifest;
            cmeta_mutex_unlock(&impl->lock);
            return CMETA_PLUGIN_OK;
        }
    }

    cmeta_mutex_unlock(&impl->lock);
    return CMETA_PLUGIN_CAPACITY_EXCEEDED;
}

cmeta_plugin_status cmeta_plugin_registry_release(
    cmeta_plugin_registry *registry,
    cmeta_plugin_lease *lease) {
    cmeta_plugin_registry_impl *impl = registry_impl(registry);
    cmeta_plugin_registry_slot *slot;
    cmeta_plugin_status status;
    size_t index;
    uint64_t bit;

    if (impl == NULL || lease == NULL || !cmeta_plugin_lease_valid(*lease))
        return CMETA_PLUGIN_INVALID_ARGUMENT;

    cmeta_mutex_lock(&impl->lock);
    status = slot_for_ref_locked(impl, lease->plugin, &slot, NULL);
    if (status != CMETA_PLUGIN_OK) {
        cmeta_mutex_unlock(&impl->lock);
        return status;
    }

    index = (size_t)lease->slot - 1u;
    if (index >= CMETA_PLUGIN_MAX_LEASES_PER_PLUGIN) {
        cmeta_mutex_unlock(&impl->lock);
        return CMETA_PLUGIN_STALE;
    }

    bit = UINT64_C(1) << index;
    if ((slot->lease_active_mask & bit) == 0u ||
        slot->lease_generations[index] != lease->generation) {
        cmeta_mutex_unlock(&impl->lock);
        return CMETA_PLUGIN_STALE;
    }

    slot->lease_active_mask &= ~bit;
    slot->lease_generations[index] =
        next_generation(slot->lease_generations[index]);
    if (slot->active_leases != 0u)
        --slot->active_leases;
    *lease = (cmeta_plugin_lease){0};
    cmeta_mutex_unlock(&impl->lock);
    return CMETA_PLUGIN_OK;
}

cmeta_plugin_status cmeta_plugin_registry_request_stop(
    cmeta_plugin_registry *registry,
    cmeta_plugin_ref ref) {
    cmeta_plugin_registry_impl *impl = registry_impl(registry);
    cmeta_plugin_registry_slot *slot;
    cmeta_plugin_request_stop_fn callback;
    void *self;
    cmeta_plugin_status status;

    if (impl == NULL)
        return CMETA_PLUGIN_INVALID_ARGUMENT;

    cmeta_mutex_lock(&impl->lock);
    status = slot_for_ref_locked(impl, ref, &slot, NULL);
    if (status != CMETA_PLUGIN_OK) {
        cmeta_mutex_unlock(&impl->lock);
        return status;
    }
    if (impl->destroying || slot->unloading) {
        cmeta_mutex_unlock(&impl->lock);
        return CMETA_PLUGIN_BUSY;
    }
    if (slot->state == CMETA_PLUGIN_LIFECYCLE_STOPPING ||
        slot->state == CMETA_PLUGIN_LIFECYCLE_QUIESCENT) {
        cmeta_mutex_unlock(&impl->lock);
        return CMETA_PLUGIN_ALREADY;
    }
    if (slot->state != CMETA_PLUGIN_LIFECYCLE_STARTED) {
        cmeta_mutex_unlock(&impl->lock);
        return CMETA_PLUGIN_INVALID_STATE;
    }

    slot->state = CMETA_PLUGIN_LIFECYCLE_STOPPING;
    callback = slot->manifest->request_stop;
    self = slot->manifest->self;
    if (callback == NULL) {
        cmeta_mutex_unlock(&impl->lock);
        return CMETA_PLUGIN_OK;
    }

    ++slot->callbacks_inflight;
    cmeta_mutex_unlock(&impl->lock);

    status = callback(self);

    cmeta_mutex_lock(&impl->lock);
    --slot->callbacks_inflight;
    if (status != CMETA_PLUGIN_OK && slot->failure == CMETA_PLUGIN_OK)
        slot->failure = status;
    cmeta_mutex_unlock(&impl->lock);
    return status;
}

cmeta_plugin_status cmeta_plugin_registry_poll_quiescent(
    cmeta_plugin_registry *registry,
    cmeta_plugin_ref ref,
    bool *out_quiescent) {
    cmeta_plugin_registry_impl *impl = registry_impl(registry);
    cmeta_plugin_registry_slot *slot;
    cmeta_plugin_is_quiescent_fn callback;
    const void *self;
    cmeta_plugin_status status;
    bool quiescent;

    if (out_quiescent == NULL)
        return CMETA_PLUGIN_INVALID_ARGUMENT;
    *out_quiescent = false;
    if (impl == NULL)
        return CMETA_PLUGIN_INVALID_ARGUMENT;

    cmeta_mutex_lock(&impl->lock);
    status = slot_for_ref_locked(impl, ref, &slot, NULL);
    if (status != CMETA_PLUGIN_OK) {
        cmeta_mutex_unlock(&impl->lock);
        return status;
    }
    if (impl->destroying || slot->unloading) {
        cmeta_mutex_unlock(&impl->lock);
        return CMETA_PLUGIN_BUSY;
    }
    if (slot->state == CMETA_PLUGIN_LIFECYCLE_QUIESCENT) {
        *out_quiescent = true;
        cmeta_mutex_unlock(&impl->lock);
        return CMETA_PLUGIN_OK;
    }
    if (slot->state != CMETA_PLUGIN_LIFECYCLE_STOPPING) {
        cmeta_mutex_unlock(&impl->lock);
        return CMETA_PLUGIN_INVALID_STATE;
    }
    if (slot->active_leases != 0u || slot->callbacks_inflight != 0u) {
        cmeta_mutex_unlock(&impl->lock);
        return CMETA_PLUGIN_OK;
    }

    callback = slot->manifest->is_quiescent;
    self = slot->manifest->self;
    if (callback == NULL) {
        slot->state = CMETA_PLUGIN_LIFECYCLE_QUIESCENT;
        *out_quiescent = true;
        cmeta_mutex_unlock(&impl->lock);
        return CMETA_PLUGIN_OK;
    }

    ++slot->callbacks_inflight;
    cmeta_mutex_unlock(&impl->lock);

    quiescent = callback(self);

    cmeta_mutex_lock(&impl->lock);
    --slot->callbacks_inflight;
    if (quiescent)
        slot->state = CMETA_PLUGIN_LIFECYCLE_QUIESCENT;
    *out_quiescent = quiescent;
    cmeta_mutex_unlock(&impl->lock);
    return CMETA_PLUGIN_OK;
}

cmeta_plugin_status cmeta_plugin_registry_get_lifecycle(
    const cmeta_plugin_registry *registry,
    cmeta_plugin_ref ref,
    cmeta_plugin_lifecycle_info *out_info) {
    cmeta_plugin_registry_impl *impl = registry_impl(registry);
    cmeta_plugin_registry_slot *slot;
    cmeta_plugin_status status;

    if (out_info == NULL)
        return CMETA_PLUGIN_INVALID_ARGUMENT;
    *out_info = (cmeta_plugin_lifecycle_info){0};
    if (impl == NULL)
        return CMETA_PLUGIN_INVALID_ARGUMENT;

    cmeta_mutex_lock(&impl->lock);
    status = slot_for_ref_locked(impl, ref, &slot, NULL);
    if (status == CMETA_PLUGIN_OK) {
        out_info->state = slot->state;
        out_info->active_leases = slot->active_leases;
        out_info->callbacks_inflight = slot->callbacks_inflight;
        out_info->failure = slot->failure;
    }
    cmeta_mutex_unlock(&impl->lock);
    return status;
}

cmeta_plugin_status cmeta_plugin_registry_unload(
    cmeta_plugin_registry *registry,
    cmeta_plugin_ref ref) {
    cmeta_plugin_registry_impl *impl = registry_impl(registry);
    cmeta_plugin_registry_slot *slot;
    cmeta_plugin_destroy_fn destroy_callback = NULL;
    void *self = NULL;
    cmeta_plugin_status status;

    if (impl == NULL)
        return CMETA_PLUGIN_INVALID_ARGUMENT;

    cmeta_mutex_lock(&impl->lock);
    status = slot_for_ref_locked(impl, ref, &slot, NULL);
    if (status != CMETA_PLUGIN_OK) {
        cmeta_mutex_unlock(&impl->lock);
        return status;
    }
    if (impl->destroying || slot->unloading) {
        cmeta_mutex_unlock(&impl->lock);
        return CMETA_PLUGIN_BUSY;
    }
    if (slot->state != CMETA_PLUGIN_LIFECYCLE_LOADED &&
        slot->state != CMETA_PLUGIN_LIFECYCLE_QUIESCENT) {
        cmeta_mutex_unlock(&impl->lock);
        return CMETA_PLUGIN_BUSY;
    }
    if (slot->active_leases != 0u || slot->callbacks_inflight != 0u) {
        cmeta_mutex_unlock(&impl->lock);
        return CMETA_PLUGIN_BUSY;
    }

    slot->unloading = true;
    if (!slot->destroy_called && slot->manifest->destroy != NULL) {
        destroy_callback = slot->manifest->destroy;
        self = slot->manifest->self;
        ++slot->callbacks_inflight;
    }
    cmeta_mutex_unlock(&impl->lock);

    if (destroy_callback != NULL)
        destroy_callback(self);

    cmeta_mutex_lock(&impl->lock);
    if (destroy_callback != NULL) {
        --slot->callbacks_inflight;
        slot->destroy_called = true;
        /*
         * destroy() is irreversible even if the subsequent native close fails.
         * Keep the slot quiescent so retry may only close the DSO; never permit
         * start/acquire against already-destroyed plugin state.
         */
        slot->state = CMETA_PLUGIN_LIFECYCLE_QUIESCENT;
    }
    cmeta_mutex_unlock(&impl->lock);

    status = cmeta_plugin_platform_close(&slot->library);

    cmeta_mutex_lock(&impl->lock);
    if (status == CMETA_PLUGIN_OK) {
        clear_slot_locked(impl, slot);
    } else {
        slot->unloading = false;
    }
    cmeta_mutex_unlock(&impl->lock);
    return status;
}

size_t cmeta_plugin_registry_count(const cmeta_plugin_registry *registry) {
    cmeta_plugin_registry_impl *impl = registry_impl(registry);
    size_t count;

    if (impl == NULL)
        return 0u;

    cmeta_mutex_lock(&impl->lock);
    count = impl->count;
    cmeta_mutex_unlock(&impl->lock);
    return count;
}

cmeta_plugin_status cmeta_plugin_registry_destroy(
    cmeta_plugin_registry *registry) {
    cmeta_plugin_registry_impl *impl;
    size_t index;

    if (registry == NULL)
        return CMETA_PLUGIN_INVALID_ARGUMENT;
    if (registry->impl == NULL)
        return CMETA_PLUGIN_OK;

    impl = (cmeta_plugin_registry_impl *)registry->impl;
    cmeta_mutex_lock(&impl->lock);
    if (impl->destroying || impl->loads_inflight != 0u) {
        cmeta_mutex_unlock(&impl->lock);
        return CMETA_PLUGIN_BUSY;
    }

    for (index = 0u; index < impl->capacity; ++index) {
        const cmeta_plugin_registry_slot *slot = &impl->slots[index];
        if (!slot->occupied)
            continue;
        if (slot->unloading ||
            slot->active_leases != 0u ||
            slot->callbacks_inflight != 0u ||
            (slot->state != CMETA_PLUGIN_LIFECYCLE_LOADED &&
             slot->state != CMETA_PLUGIN_LIFECYCLE_QUIESCENT)) {
            cmeta_mutex_unlock(&impl->lock);
            return CMETA_PLUGIN_BUSY;
        }
    }

    impl->destroying = true;
    cmeta_mutex_unlock(&impl->lock);

    for (index = 0u; index < impl->capacity; ++index) {
        cmeta_plugin_registry_slot *slot = &impl->slots[index];
        cmeta_plugin_destroy_fn destroy_callback = NULL;
        void *self = NULL;
        cmeta_plugin_status status;

        cmeta_mutex_lock(&impl->lock);
        if (!slot->occupied) {
            cmeta_mutex_unlock(&impl->lock);
            continue;
        }
        slot->unloading = true;
        if (!slot->destroy_called && slot->manifest->destroy != NULL) {
            destroy_callback = slot->manifest->destroy;
            self = slot->manifest->self;
            ++slot->callbacks_inflight;
        }
        cmeta_mutex_unlock(&impl->lock);

        if (destroy_callback != NULL)
            destroy_callback(self);

        cmeta_mutex_lock(&impl->lock);
        if (destroy_callback != NULL) {
            --slot->callbacks_inflight;
            slot->destroy_called = true;
            slot->state = CMETA_PLUGIN_LIFECYCLE_QUIESCENT;
        }
        cmeta_mutex_unlock(&impl->lock);

        status = cmeta_plugin_platform_close(&slot->library);

        cmeta_mutex_lock(&impl->lock);
        if (status != CMETA_PLUGIN_OK) {
            slot->unloading = false;
            impl->destroying = false;
            cmeta_mutex_unlock(&impl->lock);
            return status;
        }
        clear_slot_locked(impl, slot);
        cmeta_mutex_unlock(&impl->lock);
    }

    cmeta_mutex_lock(&impl->lock);
    registry->impl = NULL;
    cmeta_mutex_unlock(&impl->lock);

    cmeta_mutex_destroy(&impl->lock);
    free(impl->slots);
    free(impl);
    return CMETA_PLUGIN_OK;
}
