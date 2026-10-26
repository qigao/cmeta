#ifndef CNET_SCTP_DATA_INTERNAL_H
#define CNET_SCTP_DATA_INTERNAL_H

#include <cnet/sctp_wire.h>
#include <stddef.h>
#include <stdint.h>

#include "sctp_reliability_internal.h"

/* PRIVATE C11 SCTP DATA/SACK transport core. No heap, worker, socket or lock.
 * The one CNet Owner drives send, input, take, T3 and stop; no copied pointer
 * may outlive a call. The prototype supports bounded DATA/I-DATA
 * fragmentation, RFC 9260 gap/duplicate SACKs and fixed path/stream ceilings.
 * RFC 3758/RFC 8260 timed/max-retransmit abandonment uses the negotiated
 * FORWARD-TSN variant. Shutdown and complete RFC 7 cwnd conformance remain
 * separate mandatory gates before browser compatibility.
 */
#define CNET_SCTP_DATA_MTU 1200u
/* WebRTC peers commonly deliver a 1280-byte SCTP packet after DTLS removes
 * its record framing. Keep local egress/congestion accounting at the existing
 * conservative 1200-byte MTU, but admit that bounded peer ingress size. */
#define CNET_SCTP_DATA_INGRESS_MTU 1280u
/* Local sender fragmentation stays conservative at 1024 bytes. A peer using
 * the ingress maximum can place 1252 bytes in a legacy DATA chunk
 * (1280 - common header 12 - Chunk header 4 - DATA value header 12).
 * I-DATA's larger header naturally limits its peer payload to 1248 bytes. */
#define CNET_SCTP_DATA_PAYLOAD_MAX 1024u
#define CNET_SCTP_DATA_RX_PAYLOAD_MAX 1252u
#define CNET_SCTP_DATA_MESSAGE_MAX 24576u /* 24 KiB, >16 KiB WebRTC gate */
#define CNET_SCTP_DATA_TX_SLOTS 16u
#define CNET_SCTP_DATA_BUNDLE_MAX 4u
#define CNET_SCTP_DATA_RX_SLOTS 32u
#define CNET_SCTP_DATA_STREAMS 16u
#define CNET_SCTP_DATA_RECEIVE_CEILING \
  (CNET_SCTP_DATA_RX_SLOTS * CNET_SCTP_DATA_PAYLOAD_MAX)

typedef enum cnet_sctp_data_status {
  CNET_SCTP_DATA_OK = 0,
  CNET_SCTP_DATA_EMPTY = 1,
  CNET_SCTP_DATA_INVALID = -1,
  CNET_SCTP_DATA_OWNER = -2,
  CNET_SCTP_DATA_CAPACITY = -3,
  CNET_SCTP_DATA_PROTOCOL = -4,
  CNET_SCTP_DATA_CLOCK = -5,
  CNET_SCTP_DATA_EXPIRED = -6,
  CNET_SCTP_DATA_CLOSED = -7,
  /* Call advance() before a new direct send or pending fragment. */
  CNET_SCTP_DATA_RETRANSMIT_PENDING = -8
} cnet_sctp_data_status;

typedef int (*cnet_sctp_data_owner_fn)(void *context);

typedef enum cnet_sctp_data_abandon_reason {
  CNET_SCTP_DATA_ABANDON_TIMED = 1,
  CNET_SCTP_DATA_ABANDON_RETRANSMITS = 2
} cnet_sctp_data_abandon_reason;

/* Exactly one bounded Owner-pulled record is produced for an abandoned user
 * message, even when the message owns several DATA/I-DATA TSNs. */
typedef struct cnet_sctp_data_abandon_event {
  uint16_t stream_id;
  uint32_t ppid;
  uint32_t message_identifier; /* SSN projection or negotiated MID */
  uint64_t admission_generation; /* immutable original Owner/ULP SID lifetime */
  size_t message_size;
  uint32_t first_tsn;
  uint32_t last_tsn;
  cnet_sctp_data_reliability reliability;
  cnet_sctp_data_abandon_reason reason;
  uint8_t unordered;
  uint8_t interleaved;
  uint8_t has_tsn;
} cnet_sctp_data_abandon_event;

typedef struct cnet_sctp_data_config {
  uint16_t local_port, remote_port;
  uint16_t outbound_streams, inbound_streams;
  uint32_t local_tag, peer_tag;
  uint32_t local_initial_tsn, peer_initial_tsn;
  uint32_t peer_receive_window;
  uint64_t t3_initial_ms, t3_max_ms;
  uint8_t max_retransmits;
  uint8_t forward_tsn_supported;
  uint8_t interleaving_supported;
  cnet_sctp_data_owner_fn is_owner;
  void *owner_context;
} cnet_sctp_data_config;

typedef struct cnet_sctp_data_tx {
  uint8_t packet[CNET_SCTP_DATA_MTU];
  size_t packet_size;
  size_t user_size;
  uint32_t tsn;
  uint64_t first_sent_ms;
  uint8_t retransmits;
  /* Zero-window retries count toward PR-SCTP's per-message policy without
   * consuming the association's ordinary loss retransmission budget. */
  uint32_t probe_retransmits;
  /* Gap ACK is revocable: retain the owned wire packet until cumulative ACK.
   * ack_counted prevents repeated gap/revoke/re-ACK from growing cwnd. */
  uint8_t gap_acked;
  uint8_t ack_counted;
  uint8_t missing_reports;
  uint8_t fast_retransmitted;
  uint8_t flight_accounted; /* actual transmitted and not gap-ACKed */
  uint8_t pending_retransmit; /* loss intent, never flight credit */
  uint8_t probe_retransmitted; /* suppress ambiguous zero-window RTT samples */
  uint16_t stream_id, ssn;
  uint32_t mid, fsn, ppid;
  uint32_t message_first_tsn;
  uint64_t admission_generation; /* fixed on first successful admission */
  size_t message_size;
  uint8_t unordered;
  uint8_t interleaved;
  uint8_t reliability_mode;
  uint32_t reliability_value;
  uint64_t expires_at_ms;
  uint8_t abandoned;
  uint8_t used;
} cnet_sctp_data_tx;

typedef struct cnet_sctp_data_rx {
  uint8_t payload[CNET_SCTP_DATA_RX_PAYLOAD_MAX];
  size_t size;
  uint32_t tsn, ppid;
  uint16_t stream_id, ssn;
  uint32_t mid, fsn;
  uint8_t unordered;
  uint8_t interleaved;
  uint8_t flags; /* RFC 9260 DATA U/B/E for bounded reassembly */
  uint8_t used;
} cnet_sctp_data_rx;

typedef struct cnet_sctp_data_pending {
  uint8_t bytes[CNET_SCTP_DATA_MESSAGE_MAX];
  size_t size, offset;
  uint16_t stream_id, ssn;
  uint32_t ppid, mid, next_fsn;
  uint32_t first_tsn;
  cnet_sctp_data_reliability reliability;
  uint64_t expires_at_ms;
  uint64_t admission_generation; /* copied before any TSN is assigned */
  uint8_t unordered;
  uint8_t interleaved;
  uint8_t has_tsn;
  uint8_t active;
} cnet_sctp_data_pending;

typedef struct cnet_sctp_data_message {
  uint16_t stream_id, stream_sequence;
  uint32_t ppid, tsn, message_identifier;
  size_t size;
  uint8_t unordered;
  uint8_t interleaved;
} cnet_sctp_data_message;

typedef struct cnet_sctp_data {
  cnet_sctp_data_config config;
  cnet_sctp_data_tx tx[CNET_SCTP_DATA_TX_SLOTS];
  cnet_sctp_data_rx rx[CNET_SCTP_DATA_RX_SLOTS];
  cnet_sctp_data_pending pending;
  cnet_sctp_data_abandon_event abandon_events[CNET_SCTP_DATA_TX_SLOTS];
  uint8_t abandon_event_head, abandon_event_count;
  uint16_t tx_ssn[CNET_SCTP_DATA_STREAMS], rx_ssn[CNET_SCTP_DATA_STREAMS];
  uint16_t rx_forward_ssn[CNET_SCTP_DATA_STREAMS];
  uint16_t rx_forward_streams;
  uint32_t tx_mid_ordered[CNET_SCTP_DATA_STREAMS];
  uint32_t tx_mid_unordered[CNET_SCTP_DATA_STREAMS];
  uint32_t rx_mid_ordered[CNET_SCTP_DATA_STREAMS];
  uint32_t rx_mid_unordered_floor[CNET_SCTP_DATA_STREAMS];
  uint32_t rx_forward_mid_ordered[CNET_SCTP_DATA_STREAMS];
  uint32_t rx_forward_mid_unordered[CNET_SCTP_DATA_STREAMS];
  uint16_t rx_forward_mid_ordered_streams;
  uint16_t rx_forward_mid_unordered_streams;
  uint32_t next_tsn, last_ack, cumulative_rx, rx_gaps;
  uint32_t peer_rwnd, cwnd, ssthresh;
  uint32_t last_advertised_rwnd;
  uint8_t window_update_pending;
  uint32_t partial_bytes_acked; /* padded DATA bytes, not application bytes */
  size_t in_flight, buffered_rx;
  uint64_t last_time_ms, t3_deadline_ms, rto_ms;
  /* RFC 9260 zero-window probe timer; at most one retained DATA TSN. */
  uint64_t probe_deadline_ms, probe_interval_ms;
  uint32_t probe_tsn;
  uint8_t probe_active;
  uint8_t probe_unanswered; /* silence, not a peer-advertised closed window */
  uint64_t srtt_x8_ms, rttvar_x4_ms;
  uint32_t recovery_exit_tsn;
  uint32_t t3_wait_tsn;
  uint8_t t3_wait_ack; /* no additional packet until this retransmit is ACKed */
  uint8_t recovery_active;
  uint8_t rtt_valid;
  uint8_t closed;
  uint8_t forward_active;
  uint32_t forward_cumulative_tsn;
  uint64_t forward_deadline_ms;
  uint8_t forward_retransmits;
  size_t forward_packet_size;
  uint8_t forward_packet[CNET_SCTP_DATA_MTU];
  uint32_t magic;
} cnet_sctp_data;

/* The owner allocates and zero-initializes the object before init.
 * No implicit Owner, heap-backed queue or background retry is created. */
cnet_sctp_data_status cnet_sctp_data_init(cnet_sctp_data *d,
    const cnet_sctp_data_config *cfg, uint64_t now_ms);

/* Creates one bounded, single-fragment SCTP DATA packet and retains a copy
 * for T3 only AFTER the caller's output capacity has been admitted.
 * ordered=0 uses the U flag; text/binary PPID is caller-defined.
 */
cnet_sctp_data_status cnet_sctp_data_send(cnet_sctp_data *d, uint64_t now_ms,
    uint16_t stream_id, uint32_t ppid, int unordered,
    const void *bytes, size_t length, void *output, size_t output_capacity,
    size_t *written);
cnet_sctp_data_status cnet_sctp_data_send_pr(cnet_sctp_data *d,
    uint64_t now_ms, uint16_t stream_id, uint32_t ppid, int unordered,
    const cnet_sctp_data_reliability *reliability,
    const void *bytes, size_t length, void *output, size_t output_capacity,
    size_t *written);
/* Internal Owner-token binding. The unbound API above remains usable by
 * standalone DATA conformance tests; only the higher Session layer supplies
 * a nonzero original SID generation at successful PR admission. */
cnet_sctp_data_status cnet_sctp_data_send_pr_bound(cnet_sctp_data *d,
    uint64_t now_ms, uint16_t stream_id, uint32_t ppid, int unordered,
    const cnet_sctp_data_reliability *reliability, const void *bytes,
    size_t length, void *output, size_t output_capacity, size_t *written,
    uint64_t admission_generation);

/* Input: one or more DATA chunks, optionally preceded by one SACK, on
 * a CRC-checked packet bounded to the private 1280-byte peer-ingress limit.
 * Local egress remains bounded by CNET_SCTP_DATA_MTU. Bundles
 * commit atomically after all semantic and response-capacity checks;
 * DATA returns one final cumulative SACK. No application callback runs. */
/* One fragmented user message may be queued at a time. A copied, bounded
 * pending buffer owns all bytes. pump() emits at most one DATA/I-DATA
 * fragment per call. The FCFS sender has one active message; negotiated
 * I-DATA uses MID/FSN, and the receiver accepts fragments interleaved across
 * streams without assuming contiguous TSNs.
 * Backpressure/CWND/T3, retransmission priority and output-capacity
 * failures do not advance offset. Queue copies bytes for an explicit
 * one-byte zero-window probe from advance(); direct send stays blocked.
 * Every sender call and deadline remains driven by the original Owner.
 */
/* Optional RFC 9260 DATA bundling: up to four independent user records
 * in one bounded packet, subject to current peer rwnd and retained TX slots.
 * Each TSN retains its OWN single-DATA wire ticket
 * so Fast Retransmit and T3 remain Owner-driven, independently reusable.
 * No input pointers are retained after return. Entire batch is atomic. */
typedef struct cnet_sctp_data_bundle_item {
  uint16_t stream_id;
  uint32_t ppid;
  int unordered;
  const void *payload;
  size_t payload_size;
} cnet_sctp_data_bundle_item;
cnet_sctp_data_status cnet_sctp_data_send_bundle(cnet_sctp_data *d,
    uint64_t now_ms, const cnet_sctp_data_bundle_item *items, size_t count,
    void *output, size_t output_capacity, size_t *written);

cnet_sctp_data_status cnet_sctp_data_queue_message(cnet_sctp_data *d,
    uint64_t now_ms, uint16_t stream_id, uint32_t ppid, int unordered,
    const void *bytes, size_t length);
cnet_sctp_data_status cnet_sctp_data_queue_message_pr(cnet_sctp_data *d,
    uint64_t now_ms, uint16_t stream_id, uint32_t ppid, int unordered,
    const cnet_sctp_data_reliability *reliability,
    const void *bytes, size_t length);
cnet_sctp_data_status cnet_sctp_data_queue_message_pr_bound(
    cnet_sctp_data *d, uint64_t now_ms, uint16_t stream_id, uint32_t ppid,
    int unordered, const cnet_sctp_data_reliability *reliability,
    const void *bytes, size_t length, uint64_t admission_generation);
cnet_sctp_data_status cnet_sctp_data_pump(cnet_sctp_data *d, uint64_t now_ms,
    void *packet, size_t packet_capacity, size_t *packet_bytes);

/* Owner-only synchronous pre-TSN cancellation. No fragment/probe may have
 * left; an already expired TIMED record must be processed by advance().
 * No new DATA, FORWARD-TSN or callback is created. */
cnet_sctp_data_status cnet_sctp_data_cancel_unsent(
    cnet_sctp_data *d, uint64_t now_ms, uint16_t stream_id);

/* Takes one copied message-abandonment record. Insufficient event capacity
 * is prevented at PR-message admission; explicit stop discards unread events. */
cnet_sctp_data_status cnet_sctp_data_take_abandoned(cnet_sctp_data *d,
    cnet_sctp_data_abandon_event *event);

/* SHUTDOWN's cumulative ACK uses the same guarded SACK accounting.
 * Polling drain must see retained revocable Gap ACK tickets. */
cnet_sctp_data_status cnet_sctp_data_shutdown_cumack(cnet_sctp_data *d,
    uint64_t now_ms, uint32_t cumulative);
int cnet_sctp_data_outbound_drained(const cnet_sctp_data *d);
/* RFC 6525 association reset is admitted only when no locally retained DATA,
 * pending copied message, or unread/gapped peer DATA can be discarded by the
 * TSN jump. Congestion/RTO estimates and negotiated stream counts survive. */
int cnet_sctp_data_association_reset_ready(const cnet_sctp_data *d);
cnet_sctp_data_status cnet_sctp_data_reset_association(cnet_sctp_data *d,
    uint64_t now_ms, uint32_t local_next_tsn, uint32_t peer_next_tsn);
int cnet_sctp_data_streams_through_tsn_drained(const cnet_sctp_data *d,
    uint16_t stream_mask, uint32_t last_tsn);
void cnet_sctp_data_reset_outbound_ssn(cnet_sctp_data *d, uint16_t stream_id);
void cnet_sctp_data_reset_inbound_ssn(cnet_sctp_data *d, uint16_t stream_id);

cnet_sctp_data_status cnet_sctp_data_input(cnet_sctp_data *d, uint64_t now_ms,
    const void *packet, size_t packet_size,
    void *output, size_t output_capacity, size_t *written);

cnet_sctp_data_status cnet_sctp_data_take(cnet_sctp_data *d,
    cnet_sctp_data_message *metadata, void *destination,
    size_t capacity, size_t *written);

/* While an RFC 6525 outgoing reset is deferred, affected records after the
 * sender's last assigned TSN remain owned by the fixed RX slots but are not
 * released to the ULP. No view or additional queue is created. */
cnet_sctp_data_status cnet_sctp_data_take_before_reset(cnet_sctp_data *d,
    uint16_t stream_mask, uint32_t last_tsn,
    cnet_sctp_data_message *metadata, void *destination,
    size_t capacity, size_t *written);

/* Owner-pulled SACK for a reopened receive window after take(). */
cnet_sctp_data_status cnet_sctp_data_window_update(cnet_sctp_data *d,
    uint64_t now_ms, void *output, size_t capacity, size_t *written);

cnet_sctp_data_status cnet_sctp_data_advance(cnet_sctp_data *d,
    uint64_t now_ms, void *output, size_t output_capacity, size_t *written);

cnet_sctp_data_status cnet_sctp_data_deadline(cnet_sctp_data *d,
    uint64_t *deadline_ms, int *has_deadline);

cnet_sctp_data_status cnet_sctp_data_stop(cnet_sctp_data *d);

#endif
