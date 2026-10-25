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
  uint8_t cumul_acked; /* validated irreversible cumulative SACK latched */
  uint8_t fully_emitted; /* all reliable message fragments admitted */
  uint8_t queued;        /* pending copied message belongs to this ticket */
  uint8_t used;
} cnet_sctp_session_reliable_track;

typedef struct cnet_sctp_session_forward_track {
  cnet_sctp_session_forward_progress result;
  uint8_t used;
  uint8_t cumul_confirmed;
} cnet_sctp_session_forward_track;

typedef struct cnet_sctp_session {
  cnet_sctp_assoc association;
  cnet_sctp_dcep dcep;
  cnet_sctp_session_reliable_track tracked[CNET_SCTP_DATA_TX_SLOTS];
  cnet_sctp_session_forward_track forward_tracks[CNET_SCTP_DATA_TX_SLOTS];
  uint64_t next_tracked_ticket;
  uint8_t terminal_retry_exhausted; /* set ONLY by genuine advance EXPIRED */
  uint8_t peer_abort_seen; /* valid after peer ABORT until explicit stop */
  uint64_t closing_generation[CNET_SCTP_DCEP_STREAMS];
  uint16_t pending_dcep_ack;
  uint32_t magic;
} cnet_sctp_session;

cnet_sctp_session_status cnet_sctp_session_init(cnet_sctp_session *session,
    const cnet_sctp_session_config *config, uint64_t initial_time_ms);
cnet_sctp_session_status cnet_sctp_session_destroy_in_place(
    cnet_sctp_session *session);

#endif
