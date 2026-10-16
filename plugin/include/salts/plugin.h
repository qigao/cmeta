#ifndef CMETA_PLUGIN_RUNTIME_H
#define CMETA_PLUGIN_RUNTIME_H

#include <cmeta/cmeta.h>
#include <cmeta/function.h>
#include <cmeta/interface.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Salts::Plugin supports exactly one current ABI.
 *
 * The version is an exact admission epoch, not a compatibility range. Hosts do
 * not negotiate or retry older layouts. It covers this publication layout and
 * the incompatible CMeta Reflection layouts transitively exposed by exports.
 * A plugin built for another epoch must be rebuilt before descriptor pointers
 * are consumed.
 */
#define CMETA_PLUGIN_ABI_VERSION 5u
#define CMETA_PLUGIN_QUERY_SYMBOL "cmeta_plugin_query"

#define CMETA_PLUGIN_MAX_EXPORTS 256u
#define CMETA_PLUGIN_ID_MAX 255u
#define CMETA_PLUGIN_EXPORT_ID_MAX 255u
#define CMETA_PLUGIN_CONTRACT_ID_MAX 255u
#define CMETA_PLUGIN_PATH_MAX 4095u
#define CMETA_PLUGIN_MAX_LEASES_PER_PLUGIN 64u

#if defined(__cplusplus)
#  define CMETA_PLUGIN_EXTERN_C extern "C"
#else
#  define CMETA_PLUGIN_EXTERN_C
#endif

#if defined(_WIN32)
#  define CMETA_PLUGIN_ENTRY __declspec(dllexport)
#  define CMETA_PLUGIN_CALL __cdecl
#elif defined(__GNUC__) && __GNUC__ >= 4
#  define CMETA_PLUGIN_ENTRY __attribute__((visibility("default")))
#  define CMETA_PLUGIN_CALL
#else
#  define CMETA_PLUGIN_ENTRY
#  define CMETA_PLUGIN_CALL
#endif

#define CMETA_PLUGIN_QUERY_EXPORT CMETA_PLUGIN_EXTERN_C CMETA_PLUGIN_ENTRY

typedef enum cmeta_plugin_status {
    CMETA_PLUGIN_OK = 0,
    CMETA_PLUGIN_INVALID_ARGUMENT,
    CMETA_PLUGIN_INVALID_MANIFEST,
    CMETA_PLUGIN_UNSUPPORTED_ABI,
    CMETA_PLUGIN_DUPLICATE_PLUGIN_ID,
    CMETA_PLUGIN_DUPLICATE_EXPORT,
    CMETA_PLUGIN_UNKNOWN_EXPORT,
    CMETA_PLUGIN_INCOMPATIBLE_CONTRACT,
    CMETA_PLUGIN_CAPACITY_EXCEEDED,
    CMETA_PLUGIN_ALLOCATION_FAILED,
    CMETA_PLUGIN_LOAD_FAILED,
    CMETA_PLUGIN_QUERY_MISSING,
    CMETA_PLUGIN_QUERY_REJECTED,
    CMETA_PLUGIN_UNKNOWN_PLUGIN,
    CMETA_PLUGIN_STALE,
    CMETA_PLUGIN_UNLOAD_FAILED,
    CMETA_PLUGIN_ALREADY,
    CMETA_PLUGIN_BUSY,
    CMETA_PLUGIN_INVALID_STATE
} cmeta_plugin_status;

typedef enum cmeta_plugin_export_kind {
    CMETA_PLUGIN_EXPORT_INTERFACE = 1,
    CMETA_PLUGIN_EXPORT_FUNCTION = 2
} cmeta_plugin_export_kind;

typedef struct cmeta_plugin_version {
    uint32_t major;
    uint32_t minor;
    uint32_t patch;
} cmeta_plugin_version;

typedef cmeta_plugin_status (CMETA_PLUGIN_CALL *cmeta_plugin_start_fn)(void *self);
typedef cmeta_plugin_status (CMETA_PLUGIN_CALL *cmeta_plugin_request_stop_fn)(void *self);
typedef bool (CMETA_PLUGIN_CALL *cmeta_plugin_is_quiescent_fn)(const void *self);
typedef void (CMETA_PLUGIN_CALL *cmeta_plugin_destroy_fn)(void *self);

/*
 * Exact generated execution bridge for a reflected native function.
 *
 * params[i] points at the exact C parameter object expected by the generated
 * adapter. return_storage points at an exact return object and may be NULL for
 * void returns. The adapter is generated from the same declaration/codegen
 * source as FunctionMeta/FunctionAbi; it must not reconstruct arbitrary C ABI
 * calls from runtime metadata.
 */
typedef bool (CMETA_PLUGIN_CALL *cmeta_plugin_function_invoke_fn)(
    void *context,
    void *return_storage,
    void *const *params,
    size_t param_count);

typedef struct cmeta_plugin_interface_export {
    const cmeta_interface_desc *desc;
    void *value;
} cmeta_plugin_interface_export;

typedef struct cmeta_plugin_function_export {
    const cmeta_function_desc *desc;
    const cmeta_function_abi_desc *abi;
    void *context;
    cmeta_plugin_function_invoke_fn invoke;
} cmeta_plugin_function_export;

typedef union cmeta_plugin_export_value {
    cmeta_plugin_interface_export interface;
    cmeta_plugin_function_export function;
} cmeta_plugin_export_value;

/*
 * One immutable semantic export row.
 *
 * export_id is unique within one manifest. contract_id + contract_version are
 * authoritative domain contract identity. The row is one of exactly two
 * current representations:
 *
 * INTERFACE:
 *   value.interface = { cmeta_interface_desc, mutable {self,vtable} handle }.
 *
 * FUNCTION:
 *   value.function = { FunctionMeta, FunctionAbi, context, exact invoke }.
 *
 * The tagged union makes the two representations mutually exclusive by
 * construction. All pointed-to objects/code are borrowed from the loaded
 * plugin DSO and may only be used while the host holds a live plugin lease.
 */
typedef struct cmeta_plugin_export {
    uint32_t struct_size;
    cmeta_plugin_export_kind kind;
    uint32_t contract_version;
    uint64_t capabilities;
    const char *export_id;
    const char *contract_id;
    cmeta_plugin_export_value value;
} cmeta_plugin_export;

/*
 * Immutable plugin manifest returned by CMETA_PLUGIN_QUERY_SYMBOL.
 *
 * Salts::Plugin accepts only the exact current layout and ABI epoch. There is
 * no readable-prefix compatibility, tail compatibility, ABI negotiation or
 * fallback.
 */
typedef struct cmeta_plugin_manifest {
    uint32_t struct_size;
    uint32_t abi_version;
    const char *plugin_id;
    cmeta_plugin_version version;
    const cmeta_plugin_export *exports;
    size_t export_count;

    /* Passive plugins set self and all callbacks to NULL. Managed plugins
     * provide self plus all four callbacks. */
    void *self;
    cmeta_plugin_start_fn start;
    cmeta_plugin_request_stop_fn request_stop;
    cmeta_plugin_is_quiescent_fn is_quiescent;
    cmeta_plugin_destroy_fn destroy;
} cmeta_plugin_manifest;

#define CMETA_PLUGIN_EXPORT_SIZE ((uint32_t)sizeof(cmeta_plugin_export))
#define CMETA_PLUGIN_MANIFEST_SIZE ((uint32_t)sizeof(cmeta_plugin_manifest))

typedef const cmeta_plugin_manifest *(CMETA_PLUGIN_CALL *cmeta_plugin_query_fn)(
    uint32_t host_abi);

/*
 * Registry-incarnation-scoped reference. slot is 1-based; zero fields are
 * structurally invalid. The Ref fields identify a Slot only within one
 * successfully initialized Registry's lifetime (init through final successful
 * destroy). They do NOT carry a Registry owner/epoch/incarnation identity.
 *
 * Within that live Registry, a cleared/reused Slot changes generation and a
 * historical Ref is rejected as STALE. After destroy -> init, new Slot and
 * Lease generations may start at the same numeric values as old credentials.
 * The Host MUST NOT present old Refs to a newly initialized or unrelated
 * Registry: the current ABI cannot guarantee cross-incarnation STALE fencing.
 * Changing that guarantee requires a separately versioned ABI decision.
 */
typedef struct cmeta_plugin_ref {
    uint32_t slot;
    uint32_t generation;
} cmeta_plugin_ref;

typedef enum cmeta_plugin_lifecycle_state {
    CMETA_PLUGIN_LIFECYCLE_LOADED = 1,
    CMETA_PLUGIN_LIFECYCLE_STARTING,
    CMETA_PLUGIN_LIFECYCLE_STARTED,
    CMETA_PLUGIN_LIFECYCLE_STOPPING,
    CMETA_PLUGIN_LIFECYCLE_QUIESCENT
} cmeta_plugin_lifecycle_state;

/*
 * An acquired Lease is an owner-scoped, live Registry/DSO borrow, not a
 * persistable identity or a second owner when its bytes are copied. The Host
 * must release the authoritative Lease before final Registry destroy and must
 * never carry a Lease token across destroy -> init (even with the same Registry
 * object address). Numeric equality across incarnations grants NO authority.
 */
typedef struct cmeta_plugin_lease {
    cmeta_plugin_ref plugin;
    uint32_t slot;
    uint32_t generation;
} cmeta_plugin_lease;

typedef struct cmeta_plugin_lifecycle_info {
    cmeta_plugin_lifecycle_state state;
    size_t active_leases;
    size_t callbacks_inflight;
    cmeta_plugin_status failure;
} cmeta_plugin_lifecycle_info;

typedef struct cmeta_plugin_registry_config {
    size_t capacity;
} cmeta_plugin_registry_config;

typedef struct cmeta_plugin_registry {
    void *impl;
} cmeta_plugin_registry;

/* A shape check only: nonzero fields do not prove live Registry authority. */
static inline bool cmeta_plugin_ref_valid(cmeta_plugin_ref ref) {
    return ref.slot != 0u && ref.generation != 0u;
}

/* A shape check only: this does not prove the Lease is still acquired. */
static inline bool cmeta_plugin_lease_valid(cmeta_plugin_lease lease) {
    return cmeta_plugin_ref_valid(lease.plugin) &&
           lease.slot != 0u && lease.generation != 0u;
}

const char *cmeta_plugin_status_string(cmeta_plugin_status status);

bool cmeta_plugin_export_has_capabilities(const cmeta_plugin_export *entry,
                                          uint64_t required);

cmeta_plugin_status cmeta_plugin_export_require_interface(
    const cmeta_plugin_export *entry,
    const char *contract_id,
    uint32_t contract_version,
    uint64_t required_capabilities,
    const cmeta_interface_desc *expected_interface);

cmeta_plugin_status cmeta_plugin_export_require_function(
    const cmeta_plugin_export *entry,
    const char *contract_id,
    uint32_t contract_version,
    uint64_t required_capabilities);

cmeta_plugin_status cmeta_plugin_manifest_validate(
    const cmeta_plugin_manifest *manifest);

cmeta_plugin_status cmeta_plugin_manifest_find_export(
    const cmeta_plugin_manifest *manifest,
    const char *export_id,
    const cmeta_plugin_export **out_export);

/*
 * Bounded dynamic-plugin registry and lifecycle.
 *
 * load() admits only CMETA_PLUGIN_ABI_VERSION with exact current manifest/export
 * layouts. It never retries an older ABI.
 *
 * Plugin-owned manifest/export/FunctionMeta/FunctionAbi/type-trait/adapter/
 * interface pointers may be used only while holding a live lease acquired from
 * a STARTED plugin. request_stop() closes new lease admission before invoking
 * the plugin stop callback. unload() requires no active leases or lifecycle
 * callbacks and a LOADED-never-started or QUIESCENT state.
 *
 * A lease is a DSO/resource borrow, not necessarily one lease per function
 * call. When a reflected function uses plugin-owned native traits/descriptors,
 * the borrow must cover binding, invocation, output publication and cleanup.
 *
 * Host ownership boundary: init succeeds only for an uninitialized Registry.
 * Final successful destroy ends that incarnation; all previously issued Refs,
 * Leases and borrowed DSO/manifest/export pointers lose authority. If destroy
 * returns a failure or BUSY, the same Registry implementation may remain live;
 * it must not be silently re-initialized or replaced. Recovery is an explicit
 * Host operation after the documented quiescence/ownership requirements are
 * met, not an automatic retry.
 *
 * Do not race final destroy or subsequent re-init with Registry callers or
 * retained callbacks/borrows. The public Ref/Lease ABI has NO incarnation
 * field: syntactic validity and coincident numeric equality after re-init do
 * not establish ownership of the new incarnation. Do not persist raw runtime
 * tokens/pointers or assume cross-incarnation CMETA_PLUGIN_STALE rejection.
 */
cmeta_plugin_status cmeta_plugin_registry_init(
    cmeta_plugin_registry *registry,
    const cmeta_plugin_registry_config *config);

/*
 * path is borrowed, NUL-terminated UTF-8. On Windows, paths containing a
 * directory separator are resolved to an absolute path before loading.
 * Their dependency search uses DLL_LOAD_DIR | DEFAULT_DIRS: the plugin
 * directory, application directory, System32 and explicitly registered user
 * directories. Ordinary cwd/PATH entries are not added to that search.
 * Bare names retain the platform's existing name-search policy. Loading
 * failure returns CMETA_PLUGIN_LOAD_FAILED without retrying a broader policy.
 */
cmeta_plugin_status cmeta_plugin_registry_load(
    cmeta_plugin_registry *registry,
    const char *path,
    cmeta_plugin_ref *out_ref);

cmeta_plugin_status cmeta_plugin_registry_find(
    const cmeta_plugin_registry *registry,
    const char *plugin_id,
    cmeta_plugin_ref *out_ref);

cmeta_plugin_status cmeta_plugin_registry_start(
    cmeta_plugin_registry *registry,
    cmeta_plugin_ref ref);

cmeta_plugin_status cmeta_plugin_registry_acquire(
    cmeta_plugin_registry *registry,
    cmeta_plugin_ref ref,
    cmeta_plugin_lease *out_lease,
    const cmeta_plugin_manifest **out_manifest);

/*
 * Releases one live lease. On CMETA_PLUGIN_OK the lease is consumed and reset
 * to canonical zero. On any non-OK result the caller-supplied lease value is
 * left unchanged; callers may correct the failure cause and retry when the
 * lease is otherwise still authoritative.
 */
cmeta_plugin_status cmeta_plugin_registry_release(
    cmeta_plugin_registry *registry,
    cmeta_plugin_lease *lease);

cmeta_plugin_status cmeta_plugin_registry_request_stop(
    cmeta_plugin_registry *registry,
    cmeta_plugin_ref ref);

cmeta_plugin_status cmeta_plugin_registry_poll_quiescent(
    cmeta_plugin_registry *registry,
    cmeta_plugin_ref ref,
    bool *out_quiescent);

cmeta_plugin_status cmeta_plugin_registry_get_lifecycle(
    const cmeta_plugin_registry *registry,
    cmeta_plugin_ref ref,
    cmeta_plugin_lifecycle_info *out_info);

cmeta_plugin_status cmeta_plugin_registry_unload(
    cmeta_plugin_registry *registry,
    cmeta_plugin_ref ref);

size_t cmeta_plugin_registry_count(const cmeta_plugin_registry *registry);

/*
 * Exclusive Host teardown. A successful final destroy releases Registry
 * storage and ends this incarnation; a later init starts a NEW authority
 * domain even if the same cmeta_plugin_registry object is reused.
 * Failed destroy can retain ownership for an explicit Host recovery attempt.
 * In particular, never reuse old Ref/Lease token bytes in the new Registry.
 */
cmeta_plugin_status cmeta_plugin_registry_destroy(
    cmeta_plugin_registry *registry);

#ifdef __cplusplus
}
#endif

#endif /* CMETA_PLUGIN_RUNTIME_H */
