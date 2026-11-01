#ifndef CMETA_PLUGIN_SCOPE_H
#define CMETA_PLUGIN_SCOPE_H

#include <cmeta/plugin.h>
#include <cmeta/cleanup.h>
#include <stdlib.h>

/* Caller-owned adapter borrows the registry and its one authoritative lease.
 * Keep both addresses stable through discharge; dependent obligations follow
 * this obligation in a lexical array so reverse cleanup releases them first. */
typedef struct cmeta_plugin_cleanup_lease {
    cmeta_plugin_registry *registry;
    cmeta_plugin_lease *lease;
} cmeta_plugin_cleanup_lease;
static inline void cmeta_plugin_cleanup_release_(void *authority, void *resource) {
    cmeta_plugin_cleanup_lease *owner = (cmeta_plugin_cleanup_lease *)resource;
    (void)authority;
    if (cmeta_plugin_registry_release(owner->registry, owner->lease) != CMETA_PLUGIN_OK)
        abort();
}
static inline cmeta_status cmeta_plugin_cleanup_arm(
    cmeta_cleanup *obligation, cmeta_plugin_cleanup_lease *owner) {
    if (owner == NULL || owner->registry == NULL || owner->lease == NULL)
        return CMETA_INVALID_ARGUMENT;
    return cmeta_cleanup_arm(obligation, cmeta_plugin_cleanup_release_, NULL, owner);
}

#ifdef __cplusplus
#include <exception>
#include <utility>

namespace salts {
/* Owns one lease, never the registry. Declare dependent borrowed values after
 * this object so their cleanup runs before its destructor. Single-owner access;
 * moving transfers the lease without changing the registry's active count. */
class plugin_lease_scope {
    cmeta_plugin_registry *registry_ = nullptr;
    cmeta_plugin_lease lease_ = {};
    const cmeta_plugin_manifest *manifest_ = nullptr;
public:
    plugin_lease_scope() noexcept = default;
    plugin_lease_scope(const plugin_lease_scope &) = delete;
    plugin_lease_scope &operator=(const plugin_lease_scope &) = delete;
    plugin_lease_scope &operator=(plugin_lease_scope &&) = delete;
    plugin_lease_scope(plugin_lease_scope &&other) noexcept
        : registry_(std::exchange(other.registry_, nullptr)),
          lease_(std::exchange(other.lease_, cmeta_plugin_lease{})),
          manifest_(std::exchange(other.manifest_, nullptr)) {}

    ~plugin_lease_scope() noexcept {
        if (close() != CMETA_PLUGIN_OK) std::terminate();
    }

    [[nodiscard]] cmeta_plugin_status acquire(cmeta_plugin_registry &registry,
                                               cmeta_plugin_ref ref) noexcept {
        if (registry_ != nullptr) return CMETA_PLUGIN_INVALID_STATE;
        const cmeta_plugin_status status =
            cmeta_plugin_registry_acquire(&registry, ref, &lease_, &manifest_);
        if (status == CMETA_PLUGIN_OK) registry_ = &registry;
        return status;
    }

    /* Failure preserves ownership for an explicit retry. Automatic cleanup
     * cannot report an error; an invalid registry/lease at destruction is fatal. */
    [[nodiscard]] cmeta_plugin_status close() noexcept {
        if (registry_ == nullptr) return CMETA_PLUGIN_OK;
        const cmeta_plugin_status status = cmeta_plugin_registry_release(registry_, &lease_);
        if (status == CMETA_PLUGIN_OK) {
            registry_ = nullptr;
            manifest_ = nullptr;
        }
        return status;
    }

    const cmeta_plugin_manifest *manifest() const noexcept { return manifest_; }
    explicit operator bool() const noexcept { return registry_ != nullptr; }
};
}
#else
#include <stdlib.h>

typedef cmeta_plugin_status (*cmeta_plugin_lease_body_fn)(
    const cmeta_plugin_manifest *manifest, void *context);

/* The body borrows the manifest until it returns, including early returns.
 * It must clean up dependent views before returning and must not longjmp out.
 * Keep the registry handle alive at a stable address; do not copy or reset it.
 * Failed admission never calls the body. A body error still releases the lease. */
static inline cmeta_plugin_status cmeta_plugin_with_lease(
    cmeta_plugin_registry *registry, cmeta_plugin_ref ref,
    cmeta_plugin_lease_body_fn body, void *context) {
    cmeta_plugin_lease lease = {0};
    const cmeta_plugin_manifest *manifest = NULL;
    cmeta_plugin_status status;
    if (body == NULL) return CMETA_PLUGIN_INVALID_ARGUMENT;
    status = cmeta_plugin_registry_acquire(registry, ref, &lease, &manifest);
    if (status != CMETA_PLUGIN_OK) return status;
    status = body(manifest, context);
    /* A private authoritative lease cannot fail release under this contract.
     * Do not silently strand it when the caller corrupts the registry. */
    if (cmeta_plugin_registry_release(registry, &lease) != CMETA_PLUGIN_OK) abort();
    return status;
}
#endif

#endif
