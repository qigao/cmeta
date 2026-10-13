#ifndef CNET_SCTP_DATA_INTERNAL_H
#define CNET_SCTP_DATA_INTERNAL_H

#include <cnet/sctp_wire.h>
#include <stddef.h>
#include <stdint.h>

/* PRIVATE C11 SCTP DATA/SACK transport core. No heap, worker, socket or lock.
 * The one CNet Owner drives send, input, take, T3 and stop; no copied pointer
 * may outlive a call. The prototype only accepts complete unfragmented DATA
 * chunks (B|E), RFC 9260 gap/duplicate SACKs and fixed path/stream ceilings.
 * PR-SCTP, fragmentation/interleaving, shutdown and complete RFC 7 cwnd
 * conformance remain separate mandatory gates before browser compatibility.
 */
#define CNET_SCTP_DATA_MTU 1200u
#define CNET_SCTP_DATA_PAYLOAD_MAX 1024u
#define CNET_SCTP_DATA_TX_SLOTS 16u
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
  CNET_SCTP_DATA_CLOSED = -7
} cnet_sctp_data_status;

typedef int (*cnet_sctp_data_owner_fn)(void *context);

typedef struct cnet_sctp_data_config {
  uint16_t local_port, remote_port;
  uint16_t outbound_streams, inbound_streams;
  uint32_t local_tag, peer_tag;
  uint32_t local_initial_tsn, peer_initial_tsn;
  uint32_t peer_receive_window;
  uint64_t t3_initial_ms, t3_max_ms;
  uint8_t max_retransmits;
  cnet_sctp_data_owner_fn is_owner;
  void *owner_context;
} cnet_sctp_data_config;

typedef struct cnet_sctp_data_tx {
  uint8_t packet[CNET_SCTP_DATA_MTU];
  size_t packet_size;
  size_t user_size;
  uint32_t tsn;
  uint8_t retransmits;
  uint8_t used;
} cnet_sctp_data_tx;

typedef struct cnet_sctp_data_rx {
  uint8_t payload[CNET_SCTP_DATA_PAYLOAD_MAX];
  size_t size;
  uint32_t tsn, ppid;
  uint16_t stream_id, ssn;
  uint8_t unordered;
  uint8_t used;
} cnet_sctp_data_rx;

typedef struct cnet_sctp_data_message {
  uint16_t stream_id, stream_sequence;
  uint32_t ppid, tsn;
  size_t size;
  uint8_t unordered;
} cnet_sctp_data_message;

typedef struct cnet_sctp_data {
  cnet_sctp_data_config config;
  cnet_sctp_data_tx tx[CNET_SCTP_DATA_TX_SLOTS];
  cnet_sctp_data_rx rx[CNET_SCTP_DATA_RX_SLOTS];
  uint16_t tx_ssn[CNET_SCTP_DATA_STREAMS], rx_ssn[CNET_SCTP_DATA_STREAMS];
  uint32_t next_tsn, last_ack, cumulative_rx, rx_gaps;
  uint32_t peer_rwnd, cwnd, ssthresh;
  size_t in_flight, buffered_rx;
  uint64_t last_time_ms, t3_deadline_ms, rto_ms;
  uint8_t closed;
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

/* Input: exactly one DATA or SACK chunk on a CRC-checked wire packet.
 * DATA returns a complete SACK packet via output, SACK returns 0 bytes.
 * No application callback runs; take() copies ready records explicitly.
 */
cnet_sctp_data_status cnet_sctp_data_input(cnet_sctp_data *d, uint64_t now_ms,
    const void *packet, size_t packet_size,
    void *output, size_t output_capacity, size_t *written);

cnet_sctp_data_status cnet_sctp_data_take(cnet_sctp_data *d,
    cnet_sctp_data_message *metadata, void *destination,
    size_t capacity, size_t *written);

cnet_sctp_data_status cnet_sctp_data_advance(cnet_sctp_data *d,
    uint64_t now_ms, void *output, size_t output_capacity, size_t *written);

cnet_sctp_data_status cnet_sctp_data_deadline(cnet_sctp_data *d,
    uint64_t *deadline_ms, int *has_deadline);

cnet_sctp_data_status cnet_sctp_data_stop(cnet_sctp_data *d);

#endif
