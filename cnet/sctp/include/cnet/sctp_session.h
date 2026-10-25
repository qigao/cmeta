#ifndef CNET_SCTP_SESSION_H
#define CNET_SCTP_SESSION_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
#error "The experimental CNet SCTP session has a C11-only interface"
#endif

#define CNET_SCTP_SESSION_ABI_VERSION 1u
#define CNET_SCTP_SESSION_MAX_PACKET_BYTES 1280u
#define CNET_SCTP_SESSION_MAX_MESSAGE_BYTES 24576u
#define CNET_SCTP_SESSION_MAX_STREAMS 16u
#define CNET_SCTP_SESSION_MAC_BYTES 32u

typedef struct cnet_sctp_session cnet_sctp_session;

/* All operations are synchronous and must be serialized by one caller-defined
 * Owner; this API supplies no lock, worker, socket or clock. Packet and message
 * inputs are borrowed only for the call. now_ms is a nondecreasing monotonic
 * clock. CAPACITY leaves the corresponding wire/state transaction uncommitted;
 * OWNER means is_owner rejected the call; CLOCK rejects time regression.
 * This standalone ABI is experimental and is not Salts::CNet. */

typedef enum cnet_sctp_session_status {
  CNET_SCTP_SESSION_OK = 0,
  CNET_SCTP_SESSION_EMPTY = 1,
  CNET_SCTP_SESSION_INVALID = -1,
  CNET_SCTP_SESSION_OWNER = -2,
  CNET_SCTP_SESSION_CAPACITY = -3,
  CNET_SCTP_SESSION_PROTOCOL = -4,
  CNET_SCTP_SESSION_BUSY = -5,
  CNET_SCTP_SESSION_CLOSED = -6,
  CNET_SCTP_SESSION_CLOCK = -7,
  CNET_SCTP_SESSION_STALE = -8,
  CNET_SCTP_SESSION_TRANSPORT = -9
} cnet_sctp_session_status;

typedef enum cnet_sctp_session_channel_type {
  CNET_SCTP_SESSION_CHANNEL_RELIABLE_ORDERED = 0x00,
  CNET_SCTP_SESSION_CHANNEL_RETRANSMIT_ORDERED = 0x01,
  CNET_SCTP_SESSION_CHANNEL_TIMED_ORDERED = 0x02,
  CNET_SCTP_SESSION_CHANNEL_RELIABLE_UNORDERED = 0x80,
  CNET_SCTP_SESSION_CHANNEL_RETRANSMIT_UNORDERED = 0x81,
  CNET_SCTP_SESSION_CHANNEL_TIMED_UNORDERED = 0x82
} cnet_sctp_session_channel_type;

typedef enum cnet_sctp_session_event_kind {
  CNET_SCTP_SESSION_EVENT_NONE = 0,
  CNET_SCTP_SESSION_EVENT_CHANNEL_OPEN = 1,
  CNET_SCTP_SESSION_EVENT_MESSAGE = 2,
  CNET_SCTP_SESSION_EVENT_CHANNEL_CLOSED = 3,
  CNET_SCTP_SESSION_EVENT_CHANNEL_CLOSE_FAILED = 4,
  CNET_SCTP_SESSION_EVENT_MESSAGE_ABANDONED = 5
} cnet_sctp_session_event_kind;

typedef enum cnet_sctp_session_reliability_mode {
  CNET_SCTP_SESSION_RELIABLE = 0,
  CNET_SCTP_SESSION_RETRANSMITS = 1,
  CNET_SCTP_SESSION_TIMED = 2
} cnet_sctp_session_reliability_mode;

typedef struct cnet_sctp_session_reliability {
  cnet_sctp_session_reliability_mode mode;
  uint32_t value;
} cnet_sctp_session_reliability;

typedef enum cnet_sctp_session_abandon_reason {
  CNET_SCTP_SESSION_ABANDON_TIMED = 1,
  CNET_SCTP_SESSION_ABANDON_RETRANSMITS = 2
} cnet_sctp_session_abandon_reason;

typedef enum cnet_sctp_session_reset_result {
  CNET_SCTP_SESSION_RESET_NOTHING_TO_DO = 0,
  CNET_SCTP_SESSION_RESET_PERFORMED = 1,
  CNET_SCTP_SESSION_RESET_DENIED = 2,
  CNET_SCTP_SESSION_RESET_WRONG_SSN = 3,
  CNET_SCTP_SESSION_RESET_REQUEST_IN_PROGRESS = 4,
  CNET_SCTP_SESSION_RESET_BAD_SEQUENCE = 5,
  CNET_SCTP_SESSION_RESET_IN_PROGRESS = 6,
  CNET_SCTP_SESSION_RESET_TIMEOUT = 7
} cnet_sctp_session_reset_result;

typedef struct cnet_sctp_session_event {
  cnet_sctp_session_event_kind kind;
  uint16_t stream_id;
  uint64_t generation;
  size_t message_size;
  uint32_t message_identifier;
  uint32_t ppid;
  uint32_t first_tsn;
  uint32_t last_tsn;
  cnet_sctp_session_reliability reliability;
  cnet_sctp_session_abandon_reason abandon_reason;
  uint8_t binary;
  uint8_t locally_initiated;
  uint8_t incoming_reset;
  uint8_t unordered;
  uint8_t interleaved;
  uint8_t has_tsn;
  cnet_sctp_session_reset_result reset_result;
} cnet_sctp_session_event;

/* Owner-only generation-fenced snapshot of SCTP application wire-payload
 * bytes. queued_bytes are copied but not yet emitted. retained_bytes count
 * emitted DATA tickets awaiting cumulative ACK/FORWARD progress, including
 * revocable gap_acked_bytes and abandoned_bytes as subsets, not additional
 * bytes. An emitted FORWARD-TSN transfers abandoned DATA into a separate
 * retained association-level control packet (see association_forward_pending);
 * zero retained_bytes alone is NOT proof that the forward barrier is retired.
 * DCEP PPID50 is excluded. PPID56/57 wire placeholders count as a
 * byte here despite their zero logical application length. Neither this
 * snapshot nor its changes imply JavaScript buffered_amount or an exactly-
 * once per-message completion event. No borrowed output survives the call. */
typedef struct cnet_sctp_session_tx_usage {
  uint64_t generation;
  size_t queued_bytes;
  size_t retained_bytes;
  size_t gap_acked_bytes;
  size_t abandoned_bytes;
  uint16_t retained_fragments;
  uint8_t has_pending_message;
  /* ASSOCIATION-wide, not attributable to this SID alone. After abandoned
   * DATA tickets transfer into a retained FORWARD-TSN control packet, their
   * per-SID retained_bytes can be zero while the peer has NOT acknowledged
   * the forward barrier. This conservative flag prevents false completion. */
  uint8_t association_forward_pending;
} cnet_sctp_session_tx_usage;

typedef int (*cnet_sctp_session_owner_fn)(void *context);
typedef int (*cnet_sctp_session_entropy_fn)(void *context,
    uint8_t *destination, size_t destination_size);
typedef int (*cnet_sctp_session_mac_fn)(void *context,
    const uint8_t *message, size_t message_size,
    uint8_t output[CNET_SCTP_SESSION_MAC_BYTES]);
typedef int (*cnet_sctp_session_mac_verify_fn)(void *context,
    const uint8_t *message, size_t message_size,
    const uint8_t claimed_mac[CNET_SCTP_SESSION_MAC_BYTES]);

/* The pointed-to provider context and all four callbacks are retained until
 * destroy. They must remain valid and callable only under the admitted Owner.
 * peer_binding is an opaque, non-pointer DTLS generation identity and must be
 * nonzero. Ports, stream counts, timers, receive_window (at least 1500),
 * cookie_lifetime_ms and max_retransmits must be nonzero; t1_max_ms must be at
 * least t1_ms. Streams must be equal in both directions and no greater than
 * CNET_SCTP_SESSION_MAX_STREAMS. A zero t5_shutdown_guard_ms disables T5. */
typedef struct cnet_sctp_session_create_config {
  uint32_t abi_version;
  size_t struct_size;
  uint16_t local_port;
  uint16_t remote_port;
  uint32_t receive_window;
  uint16_t outbound_streams;
  uint16_t inbound_streams;
  uint64_t t1_ms;
  uint64_t t1_max_ms;
  uint64_t t5_shutdown_guard_ms;
  uint64_t cookie_lifetime_ms;
  uint64_t peer_binding;
  uint8_t max_retransmits;
  uint8_t is_dtls_client;
  cnet_sctp_session_owner_fn is_owner;
  cnet_sctp_session_entropy_fn entropy;
  cnet_sctp_session_mac_fn mac;
  cnet_sctp_session_mac_verify_fn verify_mac;
  void *provider_context;
} cnet_sctp_session_create_config;

/* Creates one heap-allocated, fixed-capacity session. abi_version and
 * struct_size must exactly match this header. The caller owns *out and must
 * stop, drain and destroy it on the same admitted Owner. On failure *out is
 * NULL. INVALID reports config/ABI failure; CAPACITY reports allocation
 * failure; callback/provider errors are mapped to OWNER or TRANSPORT. */
cnet_sctp_session_status cnet_sctp_session_create(
    const cnet_sctp_session_create_config *config,
    uint64_t initial_time_ms, cnet_sctp_session **out);

/* start emits INIT. input consumes one complete SCTP packet and may emit one
 * reply. advance emits the next due retransmission/control packet. Output is
 * copied into caller storage and its size is returned through written. */
cnet_sctp_session_status cnet_sctp_session_start(cnet_sctp_session *session,
    uint64_t now_ms, void *packet, size_t capacity, size_t *written);
cnet_sctp_session_status cnet_sctp_session_input(cnet_sctp_session *session,
    uint64_t now_ms, const void *packet, size_t packet_size,
    void *reply, size_t capacity, size_t *written);
cnet_sctp_session_status cnet_sctp_session_advance(cnet_sctp_session *session,
    uint64_t now_ms, void *packet, size_t capacity, size_t *written);
cnet_sctp_session_status cnet_sctp_session_next_deadline(
    cnet_sctp_session *session, uint64_t *deadline_ms, int *has_deadline);

/* open_channel emits reliably ordered DCEP OPEN. type is one of the channel
 * constants above; label/protocol are borrowed and individually bounded to
 * 128 bytes. send emits a SINGLE SCTP DATA/I-DATA packet immediately; it
 * returns CAPACITY for messages requiring fragmentation without changing
 * the stream's send state. For a message up to MAX_MESSAGE_BYTES, use the
 * queue_message/pump_message pair below, which copies the entire input into
 * one bounded, Owner-owned pending slot and emits one fragment per pump.
 * close_channel emits a bidirectional reset and returns the generation
 * that identifies the stream lifetime. */
cnet_sctp_session_status cnet_sctp_session_open_channel(
    cnet_sctp_session *session, uint64_t now_ms, uint16_t stream,
    uint8_t type, uint16_t priority, uint32_t reliability,
    const void *label, size_t label_size,
    const void *protocol, size_t protocol_size,
    void *packet, size_t capacity, size_t *written);
cnet_sctp_session_status cnet_sctp_session_send(cnet_sctp_session *session,
    uint64_t now_ms, uint16_t stream, int binary,
    const void *message, size_t message_size,
    void *packet, size_t capacity, size_t *written);
/* queue_message owns a full copied user message before any SCTP TSN is
 * assigned; the caller may free the input immediately. The pending queue is
 * bounded to ONE message per Association: a second queue returns CAPACITY
 * without consuming bytes. DCEP OPEN/ACK and reliability policy must already
 * admit the SID. No borrowed application input survives the call.
 *
 * pump_message emits AT MOST ONE DATA/I-DATA fragment. EMPTY means no queued
 * bytes remain, CAPACITY means congestion/receive-window/buffer backpressure
 * without consuming the pending fragment, and BUSY requires Owner advance()
 * to service an earlier retransmission/reset. Caller must keep pumping after
 * subsequent SACKs/Owner deadlines; there is no implicit worker or I/O.
 * For timed policies the lifetime begins at queue admission, not pump time.
 * Neither entry point changes production Salts::CNet or its backend. */
cnet_sctp_session_status cnet_sctp_session_queue_message(
    cnet_sctp_session *session, uint64_t now_ms, uint16_t stream, int binary,
    const void *message, size_t message_size);
cnet_sctp_session_status cnet_sctp_session_pump_message(
    cnet_sctp_session *session, uint64_t now_ms,
    void *packet, size_t capacity, size_t *written);

/* Only the exact active DCEP generation may read a SID's TX usage. Stale
 * generations return STALE and clear *usage; free SIDs return PROTOCOL.
 * No ticket is settled and no new timer/callback is created by this query. */
cnet_sctp_session_status cnet_sctp_session_get_tx_usage(
    cnet_sctp_session *session, uint16_t stream,
    uint64_t expected_generation, cnet_sctp_session_tx_usage *usage);

cnet_sctp_session_status cnet_sctp_session_close_channel(
    cnet_sctp_session *session, uint64_t now_ms, uint16_t stream,
    void *packet, size_t capacity, size_t *written,
    uint64_t *generation);

/* Poll copies user DATA into message and never retains that buffer. packet is
 * an independent optional wire reply (for example DCEP ACK). EMPTY means no
 * ULP event is available. CAPACITY retains a pending reply/event for retry. */
cnet_sctp_session_status cnet_sctp_session_poll(cnet_sctp_session *session,
    uint64_t now_ms, cnet_sctp_session_event *event,
    void *message, size_t message_capacity,
    void *packet, size_t packet_capacity, size_t *packet_size);

/* stop is abortive and rejects later protocol operations. drained becomes
 * nonzero only after retained ULP state is consumed. destroy frees the handle
 * only on OK; on BUSY/OWNER the handle remains owned by the caller. */
cnet_sctp_session_status cnet_sctp_session_stop(cnet_sctp_session *session);
int cnet_sctp_session_drained(const cnet_sctp_session *session);
cnet_sctp_session_status cnet_sctp_session_destroy(cnet_sctp_session *session);

#endif
