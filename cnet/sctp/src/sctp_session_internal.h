#ifndef CNET_SCTP_SESSION_INTERNAL_H
#define CNET_SCTP_SESSION_INTERNAL_H

#include "sctp_assoc_internal.h"
#include "sctp_dcep_internal.h"

/* Private Owner-scoped WebRTC ULP facade. It intentionally remains outside
 * the installed SDK until the complete #1181 portability and cutover gates
 * pass. No heap, callback, socket, timer thread, or borrowed payload survives
 * a call. */
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

typedef enum cnet_sctp_session_event_kind {
  CNET_SCTP_SESSION_EVENT_NONE = 0,
  CNET_SCTP_SESSION_EVENT_CHANNEL_OPEN = 1,
  CNET_SCTP_SESSION_EVENT_MESSAGE = 2,
  CNET_SCTP_SESSION_EVENT_CHANNEL_CLOSED = 3,
  CNET_SCTP_SESSION_EVENT_CHANNEL_CLOSE_FAILED = 4
} cnet_sctp_session_event_kind;

typedef struct cnet_sctp_session_event {
  cnet_sctp_session_event_kind kind;
  uint16_t stream_id;
  uint64_t generation;
  size_t message_size;
  uint8_t binary;
  uint8_t locally_initiated;
  uint8_t incoming_reset;
  cnet_sctp_assoc_stream_reset_result reset_result;
} cnet_sctp_session_event;

typedef struct cnet_sctp_session_config {
  cnet_sctp_assoc_config association;
  uint8_t is_dtls_client;
} cnet_sctp_session_config;

typedef struct cnet_sctp_session {
  cnet_sctp_assoc association;
  cnet_sctp_dcep dcep;
  uint64_t closing_generation[CNET_SCTP_DCEP_STREAMS];
  uint16_t pending_dcep_ack;
  uint32_t magic;
} cnet_sctp_session;

cnet_sctp_session_status cnet_sctp_session_init(cnet_sctp_session *session,
    const cnet_sctp_session_config *config, uint64_t initial_time_ms);
cnet_sctp_session_status cnet_sctp_session_start(cnet_sctp_session *session,
    uint64_t now_ms, void *packet, size_t capacity, size_t *written);
cnet_sctp_session_status cnet_sctp_session_input(cnet_sctp_session *session,
    uint64_t now_ms, const void *packet, size_t packet_size,
    void *reply, size_t capacity, size_t *written);
cnet_sctp_session_status cnet_sctp_session_advance(cnet_sctp_session *session,
    uint64_t now_ms, void *packet, size_t capacity, size_t *written);
cnet_sctp_session_status cnet_sctp_session_next_deadline(
    cnet_sctp_session *session, uint64_t *deadline_ms, int *has_deadline);

/* OPEN is transactional across DCEP state and SCTP admission. If the
 * Association rejects the reliable ordered PPID-50 DATA, the exact DCEP
 * generation is rolled back and the SID remains reusable. */
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

/* Admit one bidirectional RFC 6525 reset before changing DCEP state. The
 * returned generation binds both completion directions to this SID lifetime. */
cnet_sctp_session_status cnet_sctp_session_close_channel(
    cnet_sctp_session *session, uint64_t now_ms, uint16_t stream,
    void *packet, size_t capacity, size_t *written,
    uint64_t *generation);

/* Pull one ULP event. DCEP messages are consumed internally. A peer OPEN may
 * also emit one reliable ordered ACK packet. If packet capacity is short, the
 * one-byte ACK obligation is retained and retried on the next poll. User DATA
 * is copied into message and never retained by this facade. */
cnet_sctp_session_status cnet_sctp_session_poll(cnet_sctp_session *session,
    uint64_t now_ms, cnet_sctp_session_event *event,
    void *message, size_t message_capacity,
    void *packet, size_t packet_capacity, size_t *packet_size);

cnet_sctp_session_status cnet_sctp_session_stop(cnet_sctp_session *session);
cnet_sctp_session_status cnet_sctp_session_destroy(cnet_sctp_session *session);
int cnet_sctp_session_drained(const cnet_sctp_session *session);

#endif
