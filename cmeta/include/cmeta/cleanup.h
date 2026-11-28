#ifndef CMETA_CLEANUP_H
#define CMETA_CLEANUP_H

#include <cmeta/lifecycle.h>
#include <cmeta/function.h>

/** One lexical obligation, not a Reflection trait or resource owner. The
 * resource adapter supplies its existing nofail release authority. Records
 * and resources stay at stable addresses, under a single-threaded owner.
 * Do not copy an armed record. No allocation, registration, or hidden retain.
 *
 * Automatic discharge is a no-fail semantic contract in both C and C++: the
 * callback must complete local ownership release without a recoverable error
 * or exception. A void signature alone does not prove that contract. Adapters
 * must use an authority that promises no-fail discharge, keep its prerequisites
 * alive, and specify fail-fast termination for detected invariant violations.
 * Fallible flush/commit/close belongs in an explicit status-returning operation
 * before discharge; never cast it to this callback or discard its error. No
 * retry or external settlement is implied by lexical cleanup. */
typedef void (*cmeta_cleanup_fn)(void *authority, void *resource);
typedef struct cmeta_cleanup {
    cmeta_cleanup_fn release;
    void *authority;
    void *resource;
} cmeta_cleanup;
#define CMETA_CLEANUP_INIT { NULL, NULL, NULL }

CMETA_INLINE cmeta_status cmeta_cleanup_arm(cmeta_cleanup *obligation,
    cmeta_cleanup_fn release, void *authority, void *resource) {
    if (obligation == NULL || release == NULL || resource == NULL)
        return CMETA_INVALID_ARGUMENT;
    if (obligation->release != NULL) return CMETA_BUSY;
    obligation->authority = authority;
    obligation->resource = resource;
    obligation->release = release;
    return CMETA_OK;
}
CMETA_INLINE void cmeta_cleanup_disarm(cmeta_cleanup *obligation) {
    if (obligation == NULL) return;
    obligation->release = NULL;
    obligation->authority = NULL;
    obligation->resource = NULL;
}
CMETA_INLINE void cmeta_cleanup_run(cmeta_cleanup *obligation) {
    cmeta_cleanup saved;
    if (obligation == NULL) return;
    saved = *obligation;
    /* A reentrant release observes an already discharged obligation. */
    cmeta_cleanup_disarm(obligation);
    if (saved.release != NULL) saved.release(saved.authority, saved.resource);
}
CMETA_INLINE cmeta_status cmeta_cleanup_transfer(
    cmeta_cleanup *destination, cmeta_cleanup *source) {
    if (destination == NULL || source == NULL || destination == source)
        return CMETA_INVALID_ARGUMENT;
    if (destination->release != NULL) return CMETA_BUSY;
    *destination = *source;
    cmeta_cleanup_disarm(source);
    return CMETA_OK;
}
/** Caller-owned finite lexical array; O(count) time, O(1) auxiliary space.
 * Slots may be disarmed after a successful consume/move. Never longjmp out of
 * an owning scope; no runtime CFG/borrow tracking is implied. */
CMETA_INLINE void cmeta_cleanup_reverse(cmeta_cleanup *obligations, size_t count) {
    while (count != 0u) cmeta_cleanup_run(&obligations[--count]);
}
typedef cmeta_status (*cmeta_cleanup_body_fn)(void *context);
/** Body may arm the finite caller-owned records; its status survives reverse
 * discharge. Early native returns stay inside body; longjmp is forbidden. */
CMETA_INLINE cmeta_status cmeta_with_cleanups(cmeta_cleanup *obligations,
    size_t count, cmeta_cleanup_body_fn body, void *context) {
    cmeta_status status;
    if (body == NULL || (count != 0u && obligations == NULL)) return CMETA_INVALID_ARGUMENT;
    status = body(context);
    cmeta_cleanup_reverse(obligations,count);
    return status;
}
CMETA_INLINE void cmeta_cleanup_data_release_(void *authority, void *resource) {
#ifdef __cplusplus
    const cmeta_lifecycle_binding *binding =
        static_cast<const cmeta_lifecycle_binding *>(authority);
#else
    const cmeta_lifecycle_binding *binding = (const cmeta_lifecycle_binding *)authority;
#endif
    binding->ops->restore_zero(resource);
}
/** The binding and successfully initialized storage must outlive obligation. */
CMETA_INLINE cmeta_status cmeta_cleanup_data(cmeta_cleanup *obligation,
    cmeta_lifecycle_binding *binding, void *storage) {
    if (binding == NULL || binding->ops == NULL) return CMETA_INVALID_ARGUMENT;
    return cmeta_cleanup_arm(obligation, cmeta_cleanup_data_release_, binding, storage);
}

typedef enum cmeta_result_cleanup {
    CMETA_RESULT_CLEANUP_VALUE,
    CMETA_RESULT_CLEANUP_BORROW,
    CMETA_RESULT_CLEANUP_RELEASE,
    CMETA_RESULT_CLEANUP_DESTROY
} cmeta_result_cleanup;
/** Stable ownership facts -> generator obligation class. VALUE still uses its
 * DataDesc storage lifecycle. BORROW requires an external authoritative owner
 * for escaping use. UNKNOWN cannot authorize automatic ownership lowering.
 * This is a pure decision; no cleanup callback is guessed from a C type. */
#ifdef __cplusplus
#define CMETA_RESULT_FLAGS_CAST_(value) static_cast<cmeta_result_flags>(value)
#else
#define CMETA_RESULT_FLAGS_CAST_(value) ((cmeta_result_flags)(value))
#endif

CMETA_INLINE cmeta_status cmeta_result_cleanup_classify(
    cmeta_result_flags flags, cmeta_result_cleanup *out) {
    if (out == NULL ||
        (flags & ~CMETA_RESULT_FLAGS_CAST_(CMETA_RESULT_FLAG_MASK)) != 0u)
        return CMETA_INVALID_ARGUMENT;
    switch (flags & CMETA_RESULT_FLAGS_CAST_(CMETA_RESULT_CLASS_MASK)) {
    case CMETA_RESULT_VALUE: *out = CMETA_RESULT_CLEANUP_VALUE; return CMETA_OK;
    case CMETA_RESULT_BORROWED: *out = CMETA_RESULT_CLEANUP_BORROW; return CMETA_OK;
    case CMETA_RESULT_SHARED: *out = CMETA_RESULT_CLEANUP_RELEASE; return CMETA_OK;
    case CMETA_RESULT_OWNED: *out = CMETA_RESULT_CLEANUP_DESTROY; return CMETA_OK;
    case CMETA_RESULT_UNKNOWN: return CMETA_TRAIT_MISSING;
    default: return CMETA_INVALID_ARGUMENT;
    }
}

#ifdef __cplusplus
namespace cmeta {
/** The guard borrows a stable obligation. Declare resource then obligation then
 * guard, inside any provider/module lease. Destruction discharges once. */
class cleanup_scope {
    cmeta_cleanup *obligation_;
public:
    explicit cleanup_scope(cmeta_cleanup &obligation) noexcept : obligation_(&obligation) {}
    cleanup_scope(const cleanup_scope &) = delete;
    cleanup_scope &operator=(const cleanup_scope &) = delete;
    ~cleanup_scope() noexcept { cmeta_cleanup_run(obligation_); }
};
}
#endif
#endif
