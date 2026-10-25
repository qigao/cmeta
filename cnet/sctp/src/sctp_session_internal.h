#ifndef CNET_SCTP_SESSION_INTERNAL_H
#define CNET_SCTP_SESSION_INTERNAL_H

#include <cnet/sctp_session.h>

#include "sctp_assoc_internal.h"
#include "sctp_dcep_internal.h"

/* Private Owner-scoped WebRTC ULP facade. It intentionally remains outside
 * the installed SDK until the complete #1181 portability and cutover gates
 * pass. No heap, callback, socket, timer thread, or borrowed payload survives
 * a call. */
typedef struct cnet_sctp_session_config {
  cnet_sctp_assoc_config association;
  uint8_t is_dtls_client;
} cnet_sctp_session_config;

typedef struct cnet_sctp_session_reliable_track {
  cnet_sctp_session_reliable_ack ack;
  uint8_t used;
} cnet_sctp_session_reliable_track;

typedef struct cnet_sctp_session {
  cnet_sctp_assoc association;
  cnet_sctp_dcep dcep;
  cnet_sctp_session_reliable_track tracked[CNET_SCTP_DATA_TX_SLOTS];
  uint64_t next_tracked_ticket;
  uint64_t closing_generation[CNET_SCTP_DCEP_STREAMS];
  uint16_t pending_dcep_ack;
  uint32_t magic;
} cnet_sctp_session;

cnet_sctp_session_status cnet_sctp_session_init(cnet_sctp_session *session,
    const cnet_sctp_session_config *config, uint64_t initial_time_ms);
cnet_sctp_session_status cnet_sctp_session_destroy_in_place(
    cnet_sctp_session *session);

#endif
