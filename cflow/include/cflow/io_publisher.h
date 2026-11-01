#ifndef CFLOW_IO_PUBLISHER_H
#define CFLOW_IO_PUBLISHER_H

#include <cflow/io_actor.h>
#include <cflow/publishers.h>

#include <error_codes.h>

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** External owner for a Publisher backed by one bounded IO Actor. */
typedef struct cflow_io_publisher_owner {
    void *impl;
} cflow_io_publisher_owner;

enum {
    /** Binds one resume's preparation work to the Subscription pump quantum. */
    CFLOW_IO_PUBLISHER_MAX_WINDOW = 64u
};

typedef enum cflow_io_publisher_prepare_status {
    CFLOW_IO_PUBLISHER_PREPARE_OPERATION = 0,
    CFLOW_IO_PUBLISHER_PREPARE_DONE,
    CFLOW_IO_PUBLISHER_PREPARE_ERROR
} cflow_io_publisher_prepare_status;

/**
 * Prepares one move-only operation on the Subscription pump thread. Returning
 * OPERATION transfers ownership to the adapter; DONE ends the Publisher without
 * submitting an Actor request; ERROR publishes the borrowed, owner-lifetime
 * error string through error.
 */
typedef cflow_io_publisher_prepare_status (*cflow_io_publisher_prepare_fn)(
    void *user,
    cflow_io_operation *operation,
    const char **error);

/**
 * Encodes one authoritative completion into the adapter's typed output slot.
 * request_id, lease_id, operation_user, and completion are borrowed for this
 * callback. error, when used for CFLOW_READ_ERROR, must remain valid until
 * owner close. CFLOW_READ_WOULD_BLOCK is a protocol error for this callback.
 */
typedef cflow_read_status (*cflow_io_publisher_encode_fn)(
    void *user,
    cflow_io_request_id request_id,
    cflow_io_lease_id lease_id,
    void *operation_user,
    const cflow_io_completion *completion,
    void *out_value,
    const char **error);

/**
 * Borrowed configuration for cflow_publisher_from_io_actor(). name, type,
 * backend, backend_user, prepare, encode, user, and any non-NULL drive and
 * drive_user remain valid until cflow_io_publisher_owner_close() succeeds. type must have
 * TRIVIAL_COPY and TRIVIAL_DESTROY traits. drive is an advisory coalescing
 * edge notification. A serialized backend without an external batch boundary
 * may call owner_run_ready() directly; otherwise drive schedules the owner
 * thread. A blocking owner integration must post its bounded backend wake
 * before signaling the owner runnable condition, so a late control edge cannot
 * overtake the next submitted batch. Edges raised by an active owner are
 * consumed by that owner without a reentrant callback. NULL drive selects explicit caller-driven progress; callers must
 * run the owner after demand, completion, and cancellation edges. The callback
 * must not synchronously close or destroy the owner.
 */
typedef struct cflow_io_publisher_config {
    const char *name;
    const cmeta_type_desc *type;
    cflow_io_backend_ops backend;
    void *backend_user;
    cflow_io_publisher_prepare_fn prepare;
    cflow_io_publisher_encode_fn encode;
    void *user;
    cflow_io_wake_fn drive;
    void *drive_user;
} cflow_io_publisher_config;

/** Diagnostic adapter snapshot; Actor counters reflect the configured capacity. */
typedef struct cflow_io_publisher_stats {
    cflow_io_actor_stats actor;
    bool publisher_live;
    bool request_active;
    bool result_ready;
    bool close_requested;
} cflow_io_publisher_stats;

/**
 * Diagnostic snapshot of the Adapter-owned bounded window. occupied includes
 * entries waiting for result emission or Actor acknowledge. demand_reserved
 * counts accepted operations whose values have not reached the Subscriber.
 */
typedef struct cflow_io_publisher_window_stats {
    size_t capacity;
    size_t occupied;
    size_t demand_reserved;
    size_t results_ready;
    size_t peak_occupied;
} cflow_io_publisher_window_stats;

/**
 * Creates a typed Publisher and its external owner. out and owner must both be
 * zero state; on every failure valid zero-state destinations remain zero.
 * Occupied destinations are rejected without mutation. The adapter owns a
 * request-capacity-one Actor, a capacity-one manual Executor, and one typed
 * completion slot. It borrows every config field and callback context until a
 * successful owner close. prepare is called only for positive downstream
 * demand. After an operation is accepted, no next operation is prepared until
 * that operation's completion delivery has completed and its Actor request has
 * been acknowledged. Returns SALTS_OK, SALTS_EINVAL, or SALTS_ENOMEM.
 *
 * Thread safety: construction requires exclusive access. The Subscription
 * Scheduler is the only Publisher resume consumer after success.
 */
int cflow_publisher_from_io_actor(
    cflow_publisher *out,
    cflow_io_publisher_owner *owner,
    const cflow_io_publisher_config *config);

/**
 * Creates an opt-in bounded multi-request Publisher. window_capacity must be in
 * [1, CFLOW_IO_PUBLISHER_MAX_WINDOW]; one preserves the exact sequential contract
 * of cflow_publisher_from_io_actor(). Positive Subscription demand reserves at most
 * min(demand, window_capacity) independent operations. Results are emitted in
 * authoritative completion-delivery order, not preparation order.
 *
 * Actor request state remains authoritative. The Adapter allocates matching
 * fixed request, command, manual-Executor and typed-result capacity during
 * construction and never grows it while active. Full capacity applies
 * backpressure by stopping preparation; it never retries, drops, overwrites or
 * allocates a fallback queue. Ownership, callback threads, errors and shutdown
 * otherwise follow cflow_publisher_from_io_actor(). Returns SALTS_OK,
 * SALTS_EINVAL, or SALTS_ENOMEM.
 */
int cflow_publisher_from_io_actor_windowed(
    cflow_publisher *out,
    cflow_io_publisher_owner *owner,
    const cflow_io_publisher_config *config,
    size_t window_capacity);

/**
 * Drives at most max_steps owner transitions and writes the completed count to
 * progressed. Exactly one driver may run; concurrent or reentrant calls return
 * SALTS_EBUSY. Backend completions may arrive on any thread, while completion
 * encoding runs on this owner's manual Executor. Returns SALTS_OK,
 * SALTS_EINVAL, or SALTS_EBUSY.
 */
int cflow_io_publisher_owner_run_ready(
    cflow_io_publisher_owner *owner,
    size_t max_steps,
    size_t *progressed);

/**
 * Reports whether close has drained every Actor command, completion, delivery,
 * and acknowledge. The owner remains borrowed and must not be destroyed while
 * this query or any other owner operation is active.
 */
bool cflow_io_publisher_owner_is_quiescent(
    const cflow_io_publisher_owner *owner);

/**
 * Samples adapter state and Actor counters separately. request_active is a
 * conservative diagnostic: it is true during preparation, admission handoff
 * and rejection cleanup, while the adapter tracks an accepted request, or when
 * this returned Actor sample has active_requests. Concurrent transitions may
 * make either sample stale, but a returned snapshot never reports
 * request_active false alongside a nonzero actor.active_requests. Returns
 * false for invalid arguments; the returned state does not authorize
 * concurrent close/destroy.
 */
bool cflow_io_publisher_owner_get_stats(
    const cflow_io_publisher_owner *owner,
    cflow_io_publisher_stats *out);

/**
 * Copies a bounded-window snapshot. It is valid for both constructors; the
 * sequential constructor reports capacity one. A concurrent snapshot may be
 * stale immediately after return and does not authorize close.
 */
bool cflow_io_publisher_owner_get_window_stats(
    const cflow_io_publisher_owner *owner,
    cflow_io_publisher_window_stats *out);

/**
 * Closes the external owner after Publisher cancellation/destruction and
 * complete driver drain. An early close, a live Publisher, or non-quiescent
 * Actor returns
 * SALTS_EBUSY without releasing state. Success destroys the Actor, manual
 * Executor, and typed slot, clears owner, and ends all borrowed config
 * lifetimes. Returns SALTS_OK, SALTS_EINVAL, or SALTS_EBUSY.
 *
 * Thread safety: close requires exclusive owner access and never blocks for a
 * native completion; callers must continue driving after cancellation until
 * this function succeeds.
 */
int cflow_io_publisher_owner_close(cflow_io_publisher_owner *owner);

#ifdef __cplusplus
}
#endif

#endif /* CFLOW_IO_PUBLISHER_H */
