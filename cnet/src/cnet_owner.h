#ifndef CNET_OWNER_H
#define CNET_OWNER_H

#include <cnet/cnet.h>

#include "cnet_command.h"
#include "cnet_event.h"
#include "cnet_resolver.h"
#include "cnet_tls.h"
#include "cnet_transport.h"
#include "cnet_uri.h"
#include "cnet_write_queue.h"

enum { CNET_OWNER_ADDRESS_CAPACITY = 128 };

typedef struct cnet_owner {
  void *impl;
} cnet_owner;

typedef struct cnet_owner_connect_payload {
  cnet_uri_scheme scheme;
  uintptr_t adopted_socket;
  size_t address_length;
  unsigned char address[CNET_OWNER_ADDRESS_CAPACITY];
  size_t local_address_length;
  unsigned char local_address[CNET_OWNER_ADDRESS_CAPACITY];
  char host[CNET_RESOLVER_HOST_CAPACITY];
  uint16_t port;
  char pipe_name[CNET_URI_PATH_CAPACITY];
  cnet_tls_context *tls_context;
  char tls_server_name[CNET_TLS_SERVER_NAME_CAPACITY];
  /** Zero disables the deadline. Connect covers resolution plus transport admission. */
  uint32_t connect_timeout_ms;
  /** Zero disables the per-accepted-read deadline. */
  uint32_t read_timeout_ms;
  /** Zero disables the per-accepted-write deadline. */
  uint32_t write_timeout_ms;
  uint32_t tls_handshake_timeout_ms;
  size_t tls_io_buffer_bytes;
  cnet_stream_socket_options socket_options;
  bool adopted;
  bool adopted_connect;
  bool tls_server;
} cnet_owner_connect_payload;

/** Fixed command payload whose retained context ownership transfers to the owner. */
typedef struct cnet_owner_start_tls_payload {
  cnet_tls_context *tls_context;
  char tls_server_name[CNET_TLS_SERVER_NAME_CAPACITY];
  uint32_t tls_handshake_timeout_ms;
  size_t tls_io_buffer_bytes;
  bool tls_server;
} cnet_owner_start_tls_payload;

typedef uint64_t (*cnet_owner_now_ms_fn)(void *context);
typedef int (*cnet_owner_event_publish_fn)(void *context, const cnet_event *event);

typedef struct cnet_owner_config {
  native_io_backend_kind backend_kind;
  /**
   * Optional caller-owned NativeIO backend. When non-NULL, the owner borrows
   * this backend and never closes/destroys it. Kind and capacities must cover
   * the CNet owner contract below.
   */
  native_io_backend *borrowed_backend;
  size_t connection_capacity;
  size_t request_capacity;
  size_t completion_batch_capacity;
  size_t receive_buffer_bytes;
  size_t receive_buffer_count;
  /** Mandatory bounded logical-write ownership capacity. */
  size_t write_capacity;
  size_t max_write_bytes;
  cnet_session_table *sessions;
  cnet_command_queue *commands;
  cnet_event_queue *events;
  /** Optional poll-owner fast path; NULL publishes to events. */
  cnet_owner_event_publish_fn publish_event;
  void *event_context;
  /** Optional single-owner monotonic clock seam; NULL uses cmeta_monotonic_ms(). */
  cnet_owner_now_ms_fn now_ms;
  void *clock_context;
} cnet_owner_config;

#if defined(CNET_INTERNAL_PROFILING)
typedef enum cnet_owner_trace_kind {
  CNET_OWNER_TRACE_TLS_WRITE_SUBMIT = 1,
  CNET_OWNER_TRACE_TLS_WRITE_COMPLETION,
  CNET_OWNER_TRACE_TLS_READ_ARM,
  CNET_OWNER_TRACE_TLS_READ_COMPLETION,
  CNET_OWNER_TRACE_TLS_DECRYPT,
  CNET_OWNER_TRACE_PLAINTEXT_PUBLISH
} cnet_owner_trace_kind;

/** One bounded, owner-local timestamp used only by private diagnostic builds. */
typedef struct cnet_owner_trace_event {
  uint64_t timestamp_ns;
  cnet_session_handle session;
  native_io_request request;
  native_io_endpoint endpoint;
  uint64_t bytes;
  cnet_owner_trace_kind kind;
} cnet_owner_trace_event;

/** Diagnostic timings collected only during an explicit quiescent sample. */
typedef struct cnet_owner_profile {
  uint64_t owner_drive_ns;
  uint64_t receive_rearm_stage_ns;
  uint64_t receive_rearm_request_lifecycle_ns;
  uint64_t command_stage_ns;
  uint64_t command_request_lifecycle_ns;
  uint64_t request_lifecycle_ns;
  uint64_t request_start_ns;
  uint64_t request_resubmit_ns;
  uint64_t observe_ns;
  uint64_t request_completion_ns;
  uint64_t tls_pump_ns;
  uint64_t event_publish_ns;
  uint64_t owner_drive_calls;
  uint64_t receive_rearm_stage_calls;
  uint64_t receive_rearm_request_lifecycle_calls;
  uint64_t command_stage_calls;
  uint64_t command_request_lifecycle_calls;
  uint64_t request_lifecycle_calls;
  uint64_t request_start_calls;
  uint64_t request_resubmit_calls;
  /** Vector-submit evidence for retained SG window qualification. */
  uint64_t vector_submit_calls;
  uint64_t vector_submit_spans;
  uint64_t vector_submit_bytes;
  uint64_t observe_calls;
  uint64_t request_completion_calls;
  uint64_t tls_pump_calls;
  uint64_t tls_ciphertext_bytes;
  uint64_t tls_write_submit_calls;
  uint64_t tls_write_submit_bytes;
  uint64_t tls_write_completion_calls;
  uint64_t tls_write_completion_bytes;
  uint64_t tls_read_completion_calls;
  uint64_t tls_read_completion_bytes;
  uint64_t tls_plaintext_receive_calls;
  uint64_t tls_plaintext_receive_bytes;
  uint64_t event_publish_calls;
  /** Producer-side command queue timing; payload publish includes payload copy. */
  uint64_t command_queue_publish_ns;
  uint64_t command_queue_payload_publish_ns;
  uint64_t command_queue_payload_copy_ns;
  uint64_t command_queue_publish_calls;
  uint64_t command_queue_payload_publish_calls;
  uint64_t command_queue_payload_copy_calls;
  uint64_t trace_event_count;
  uint64_t trace_dropped;
} cnet_owner_profile;
#endif

int cnet_owner_init(cnet_owner *owner, const cnet_owner_config *config);

/** Processes bounded commands and directly settles one NativeIO completion batch. */
int cnet_owner_drive(cnet_owner *owner, uint32_t timeout_ms);

/**
 * Owner-thread post-callback flush. Consumes already-published deferred
 * commands and immediately runs any session work they schedule, but never
 * observes NativeIO or routes another completion batch.
 */
int cnet_owner_flush_deferred(cnet_owner *owner);

/**
 * External-progress mode: advances CNet-owned commands, session work,
 * resolver state and deadlines without observing NativeIO. Valid only when
 * initialized with borrowed_backend.
 */
int cnet_owner_advance_external(cnet_owner *owner);

/**
 * Routes one completion already observed from the borrowed NativeIO backend.
 * out_consumed is false for a completion not owned by this CNet owner; such a
 * completion is untouched and may be offered to another backend consumer.
 */
int cnet_owner_route_external_completion(cnet_owner *owner,
                                         const native_io_completion *completion,
                                         bool *out_consumed);

/** Returns the next CNet-owned timer deadline, capped by max_wait_ms. */
int cnet_owner_external_timeout(cnet_owner *owner, uint32_t max_wait_ms,
                                uint32_t *out_timeout_ms);

/**
 * Copies active NativeIO request identities for one live session. The snapshot
 * contains no endpoint/native handle and is invalidated by any subsequent
 * admission, advance, completion route or cancellation.
 *
 * If capacity is too small, returns SALTS_ENOBUFS and reports the required
 * count without writing a partial snapshot.
 */
int cnet_owner_external_requests(cnet_owner *owner,
                                 cnet_session_handle session,
                                 native_io_request *out_requests,
                                 size_t capacity,
                                 size_t *out_count);

typedef cnet_external_request_snapshot
    cnet_owner_external_request_snapshot;

int cnet_owner_external_request_snapshots(
    cnet_owner *owner,
    cnet_session_handle session,
    cnet_owner_external_request_snapshot *out_requests,
    size_t capacity,
    size_t *out_count);

/**
 * Owner-thread direct receive admission.
 *
 * Adds bounded receive demand and schedules one owner-local rearm for the next
 * drive when no read is active. It never publishes a callback synchronously.
 * Callback reentrancy while a completion batch is being routed must stay on
 * the deferred command path.
 */
int cnet_owner_receive_direct(cnet_owner *owner, cnet_session_handle session, size_t demand);

/**
 * Owner-thread retained send ownership admission. These retain into the bounded
 * write queue and schedule owner-local work; they never publish a callback
 * synchronously. TLS uses the same retained write ownership path.
 */
int cnet_owner_send_buffer_direct(cnet_owner *owner, cnet_session_handle session,
                                  mem_buffer_t *buffer);
int cnet_owner_send_slice_direct(cnet_owner *owner, cnet_session_handle session,
                                 const mem_slice_t *slice);
int cnet_owner_send_slicev_direct(cnet_owner *owner, cnet_session_handle session,
                                  const mem_slice_t *segments, size_t segment_count);
/** Admits one retained final vector through the same close-after-send write-slot semantics. */
int cnet_owner_send_slicev_close_direct(cnet_owner *owner, cnet_session_handle session,
                                        const mem_slice_t *segments, size_t segment_count);
/** Admits one retained final buffer using the same close-after-send write-slot semantics. */
int cnet_owner_send_buffer_close_direct(cnet_owner *owner, cnet_session_handle session,
                                        mem_buffer_t *buffer);

/**
 * Owner-thread quiescent close admission.
 *
 * Commits the canonical session to DRAINING and schedules owner-local close
 * progress for the next drive. It publishes no callback synchronously.
 * Non-quiescent sessions return SALTS_EBUSY so the caller can preserve FIFO
 * ordering through the deferred command path.
 */
int cnet_owner_close_direct(cnet_owner *owner, cnet_session_handle session);

/** Thread-safe advisory wake for an owner blocked in drive. */
int cnet_owner_wake(cnet_owner *owner);

#if defined(CNET_INTERNAL_TESTING)
typedef struct cnet_owner_test_request_snapshot {
  uintptr_t token;
  native_io_request native_request;
  native_io_endpoint endpoint;
  bool active;
  /** Private diagnostic only: distinguishes an in-flight TLS ciphertext write. */
  bool tls_write;
} cnet_owner_test_request_snapshot;

/** Test-only view of the NativeIO request and coroutine ownership beneath this owner. */
bool cnet_owner_test_backend_stats(const cnet_owner *owner,
                                   native_io_backend_stats *out_native,
                                   native_io_coroutine_stats *out_coroutine);
/** Captures the CNet routing token plus authoritative NativeIO identity for one request record. */
bool cnet_owner_test_get_request_snapshot(const cnet_owner *owner, size_t request_index,
                                          cnet_owner_test_request_snapshot *out_snapshot);
/** Observes NativeIO without routing the returned completion through the owner. */
int cnet_owner_test_observe_raw(cnet_owner *owner, native_io_completion *events,
                                size_t event_capacity, uint32_t timeout_ms,
                                size_t *out_count);
/** Routes every supplied completion and returns the first routing error after the whole batch. */
int cnet_owner_test_process_completion_batch(cnet_owner *owner,
                                             const native_io_completion *events,
                                             size_t count);
/** Expires deadlines independently of routing a previously observed native completion. */
int cnet_owner_test_process_deadlines(cnet_owner *owner);
/** Starts currently queued owner-local session work without observing NativeIO completions. */
int cnet_owner_test_process_session_work(cnet_owner *owner);
/** Queues one advisory owner-work hint for an existing session. */
int cnet_owner_test_queue_session_work(cnet_owner *owner, cnet_session_handle session);
/** Makes the next successful/already-pending native cancellation report SALTS_EALREADY. */
int cnet_owner_test_force_cancel_ealready_once(cnet_owner *owner);
/** Caps each test-build stream-send submission without changing logical send ownership. */
int cnet_owner_test_set_send_chunk_bytes(cnet_owner *owner, size_t bytes);
#endif

#if defined(CNET_INTERNAL_PROFILING)
/** Begins/takes a quiescent, single-owner diagnostic sample. */
int cnet_owner_profile_begin(cnet_owner *owner);
int cnet_owner_profile_trace_bind(cnet_owner *owner,
                                  cnet_owner_trace_event *events,
                                  size_t capacity);
int cnet_owner_profile_take(cnet_owner *owner, cnet_owner_profile *out_profile);
#endif
int cnet_owner_tcp_local_peer(cnet_owner *owner, cnet_session_handle session,
                              cnet_stream_peer *out_peer);
int cnet_owner_tcp_remote_peer(cnet_owner *owner, cnet_session_handle session,
                               cnet_stream_peer *out_peer);

int cnet_owner_tcp_local_endpoint(
    cnet_owner *owner, cnet_session_handle session,
    cnet_stream_endpoint *out_endpoint);
int cnet_owner_tcp_remote_endpoint(
    cnet_owner *owner, cnet_session_handle session,
    cnet_stream_endpoint *out_endpoint);

int cnet_owner_tcp_option_get(cnet_owner *owner, cnet_session_handle session,
                              cnet_tcp_socket_option option, uint64_t *out_value);
int cnet_owner_tcp_option_set(cnet_owner *owner, cnet_session_handle session,
                              cnet_tcp_socket_option option, uint64_t value);

int cnet_owner_tcp_shutdown(cnet_owner *owner, cnet_session_handle session,
                            cnet_tcp_shutdown how);
int cnet_owner_preserve_send_on_eof(cnet_owner *owner, cnet_session_handle session);

int cnet_owner_tls_negotiated_version(cnet_owner *owner, cnet_session_handle session, char *buffer,
                                      size_t capacity, size_t *out_size);
int cnet_owner_tls_negotiated_cipher(cnet_owner *owner, cnet_session_handle session, char *buffer,
                                     size_t capacity, size_t *out_size);
int cnet_owner_tls_peer_certificate_sha256(
    cnet_owner *owner, cnet_session_handle session,
    char buffer[CNET_TLS_PEER_CERTIFICATE_SHA256_CAPACITY]);
int cnet_owner_tls_server_end_point_binding(
    cnet_owner *owner, cnet_session_handle session,
    uint8_t *output, size_t capacity, size_t *out_size);
int cnet_owner_tls_export_channel_binding(
    cnet_owner *owner, cnet_session_handle session,
    uint8_t output[CNET_TLS_CHANNEL_BINDING_BYTES]);

/** Clears owner metadata after the terminal notification recycled the handle. */
int cnet_owner_release_session(cnet_owner *owner, cnet_session_handle session);

/** Closes NativeIO admission after every session and request has settled. */
int cnet_owner_close(cnet_owner *owner);
int cnet_owner_destroy(cnet_owner *owner);

#endif /* CNET_OWNER_H */
