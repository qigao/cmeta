#ifndef CNET_SCTP_DCEP_INTERNAL_H
#define CNET_SCTP_DCEP_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

/* RFC 8832 DCEP control plane. Private C11, no heap or external timer.
 * All DCEP packets MUST travel as reliable ordered SCTP DATA PPID=50 on
 * their selected SID. The Association has a private bounded reciprocal
 * RFC 6525 reset slice, but complete channel close still requires ULP state
 * coordination, not merely deleting a local channel record.
 */
#define CNET_SCTP_DCEP_PPID 50u
#define CNET_SCTP_DCEP_STREAMS 16u
#define CNET_SCTP_DCEP_TEXT_MAX 128u
#define CNET_SCTP_DCEP_OPEN_MAX (12u + 2u * CNET_SCTP_DCEP_TEXT_MAX)

typedef enum cnet_sctp_dcep_status {
  CNET_SCTP_DCEP_OK = 0,
  CNET_SCTP_DCEP_INVALID = -1,
  CNET_SCTP_DCEP_OWNER = -2,
  CNET_SCTP_DCEP_CAPACITY = -3,
  CNET_SCTP_DCEP_PROTOCOL = -4,
  CNET_SCTP_DCEP_UNSUPPORTED = -5,
  CNET_SCTP_DCEP_BUSY = -6,
  CNET_SCTP_DCEP_CLOSED = -7
} cnet_sctp_dcep_status;

typedef int (*cnet_sctp_dcep_owner_fn)(void *ctx);
typedef struct cnet_sctp_dcep_channel {
  uint16_t sid, priority;
  uint32_t reliability;
  uint8_t channel_type; /* reliable, max-retransmit, or timed; ordered/U bit */
  uint8_t state; /* 0 free, 1 local WAIT_ACK, 2 open, 3 peer ACK_PENDING */
  uint8_t locally_initiated; /* never accept ACK for a peer-opened stream */
  uint8_t label[CNET_SCTP_DCEP_TEXT_MAX];
  uint8_t protocol[CNET_SCTP_DCEP_TEXT_MAX];
  size_t label_size, protocol_size;
} cnet_sctp_dcep_channel;
typedef struct cnet_sctp_dcep {
  cnet_sctp_dcep_channel channels[CNET_SCTP_DCEP_STREAMS];
  uint16_t streams;
  uint8_t is_dtls_client, stopped;
  uint32_t magic;
  cnet_sctp_dcep_owner_fn is_owner;
  void *context;
} cnet_sctp_dcep;

/* Caller supplies zero-initialized storage. Reinitializing an active
 * instance returns BUSY without modifying it; after stop, storage must be
 * explicitly zeroed by the Owner before reuse. */
cnet_sctp_dcep_status cnet_sctp_dcep_init(cnet_sctp_dcep *d,
    uint16_t streams, int is_dtls_client,
    cnet_sctp_dcep_owner_fn is_owner, void *context);

/* Output exactly the RFC 8832 OPEN payload for SCTP PPID=50, ordered and
 * reliable even if the channel eventually uses unordered application DATA.
 * State changes only after sufficient caller output capacity is admitted.
 */
cnet_sctp_dcep_status cnet_sctp_dcep_open(cnet_sctp_dcep *d,
    uint16_t stream, uint8_t type, uint16_t priority, uint32_t reliability,
    const void *label, size_t label_length,
    const void *protocol, size_t protocol_length,
    void *output, size_t capacity, size_t *written);

/* Complete wire OPEN/ACK only; OPEN on an occupied or wrong-parity SID
 * rejects with no ACK and requires a reply buffer. A successful OPEN returns
 * the one-byte ACK payload. A successful ACK returns zero bytes and does not
 * require a reply buffer; caller sends output using SCTP DATA PPID=50 reliable
 * ordered on the same SID. */
cnet_sctp_dcep_status cnet_sctp_dcep_input(cnet_sctp_dcep *d,
    uint16_t stream, const void *wire, size_t n,
    void *reply, size_t capacity, size_t *written);

/* Commit a peer-opened channel only after its DCEP ACK has been admitted to
 * the underlying SCTP send queue. This is not proof of network delivery. */
cnet_sctp_dcep_status cnet_sctp_dcep_ack_admitted(cnet_sctp_dcep *d,
    uint16_t stream);
int cnet_sctp_dcep_ready(const cnet_sctp_dcep *d, uint16_t stream);
cnet_sctp_dcep_status cnet_sctp_dcep_stop(cnet_sctp_dcep *d);

#endif
