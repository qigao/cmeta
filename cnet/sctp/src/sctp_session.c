#include "sctp_session_internal.h"

#include "sctp_webrtc_internal.h"

#include <stdlib.h>
#include <string.h>

#define SESSION_MAGIC UINT32_C(0x53554c50)

_Static_assert(CNET_SCTP_SESSION_MAX_PACKET_BYTES ==
    CNET_SCTP_DATA_INGRESS_MTU, "public packet bound drifted");
_Static_assert(CNET_SCTP_SESSION_MAX_MESSAGE_BYTES ==
    CNET_SCTP_DATA_MESSAGE_MAX, "public message bound drifted");
_Static_assert(CNET_SCTP_SESSION_MAX_STREAMS == CNET_SCTP_DCEP_STREAMS,
    "public stream bound drifted");
_Static_assert(CNET_SCTP_SESSION_MAX_TRACKED_RELIABLE_RECEIPTS ==
    CNET_SCTP_DATA_TX_SLOTS, "public reliable ACK receipt bound drifted");
_Static_assert(CNET_SCTP_SESSION_MAC_BYTES == CNET_SCTP_ASSOC_MAC_BYTES,
    "public MAC size drifted");
_Static_assert(CNET_SCTP_SESSION_MAX_PEER_ABORT_REASON_BYTES ==
    CNET_SCTP_ASSOC_MAX_ABORT_REASON, "peer ABORT reason bound drifted");

static cnet_sctp_session_status map_assoc(cnet_sctp_assoc_status status) {
  switch (status) {
    case CNET_SCTP_ASSOC_OK: return CNET_SCTP_SESSION_OK;
    case CNET_SCTP_ASSOC_EMPTY: return CNET_SCTP_SESSION_EMPTY;
    case CNET_SCTP_ASSOC_INVALID: return CNET_SCTP_SESSION_INVALID;
    case CNET_SCTP_ASSOC_OWNER: return CNET_SCTP_SESSION_OWNER;
    case CNET_SCTP_ASSOC_CAPACITY: return CNET_SCTP_SESSION_CAPACITY;
    case CNET_SCTP_ASSOC_PROTOCOL:
    case CNET_SCTP_ASSOC_AUTH: return CNET_SCTP_SESSION_PROTOCOL;
    case CNET_SCTP_ASSOC_CLOCK: return CNET_SCTP_SESSION_CLOCK;
    case CNET_SCTP_ASSOC_CLOSED_ERROR: return CNET_SCTP_SESSION_CLOSED;
    case CNET_SCTP_ASSOC_BUSY:
    case CNET_SCTP_ASSOC_RETRANSMIT_PENDING: return CNET_SCTP_SESSION_BUSY;
    case CNET_SCTP_ASSOC_EXPIRED:
    case CNET_SCTP_ASSOC_ENTROPY: return CNET_SCTP_SESSION_TRANSPORT;
    default: return CNET_SCTP_SESSION_TRANSPORT;
  }
}

static cnet_sctp_session_status map_dcep(cnet_sctp_dcep_status status) {
  switch (status) {
    case CNET_SCTP_DCEP_OK: return CNET_SCTP_SESSION_OK;
    case CNET_SCTP_DCEP_INVALID: return CNET_SCTP_SESSION_INVALID;
    case CNET_SCTP_DCEP_OWNER: return CNET_SCTP_SESSION_OWNER;
    case CNET_SCTP_DCEP_CAPACITY: return CNET_SCTP_SESSION_CAPACITY;
    case CNET_SCTP_DCEP_PROTOCOL:
    case CNET_SCTP_DCEP_UNSUPPORTED: return CNET_SCTP_SESSION_PROTOCOL;
    case CNET_SCTP_DCEP_BUSY: return CNET_SCTP_SESSION_BUSY;
    case CNET_SCTP_DCEP_CLOSED: return CNET_SCTP_SESSION_CLOSED;
    case CNET_SCTP_DCEP_STALE: return CNET_SCTP_SESSION_STALE;
    default: return CNET_SCTP_SESSION_TRANSPORT;
  }
}

static cnet_sctp_session_status guard(cnet_sctp_session *session) {
  if (!session || session->magic != SESSION_MAGIC ||
      !session->association.config.is_owner)
    return CNET_SCTP_SESSION_INVALID;
  if (!session->association.config.is_owner(
          session->association.config.provider_context))
    return CNET_SCTP_SESSION_OWNER;
  if (session->association.state == CNET_SCTP_ASSOC_STOPPED)
    return CNET_SCTP_SESSION_CLOSED;
  return CNET_SCTP_SESSION_OK;
}

static uint16_t first_set(uint16_t bits) {
  uint16_t sid = 0u;
  while ((bits & UINT16_C(1)) == 0u) {
    bits = (uint16_t)(bits >> 1u);
    ++sid;
  }
  return sid;
}

cnet_sctp_session_status cnet_sctp_session_init(cnet_sctp_session *session,
    const cnet_sctp_session_config *config, uint64_t initial_time_ms) {
  uint16_t streams;
  cnet_sctp_assoc_status as;
  cnet_sctp_dcep_status ds;
  if (!session || !config || config->is_dtls_client > 1u)
    return CNET_SCTP_SESSION_INVALID;
  if (session->magic == SESSION_MAGIC) return CNET_SCTP_SESSION_BUSY;
  streams = config->association.outbound_streams;
  if (streams == 0u || streams != config->association.inbound_streams ||
      streams > CNET_SCTP_DCEP_STREAMS)
    return CNET_SCTP_SESSION_INVALID;
  memset(session, 0, sizeof(*session));
  as = cnet_sctp_assoc_init(&session->association, &config->association,
      initial_time_ms);
  if (as != CNET_SCTP_ASSOC_OK) {
    memset(session, 0, sizeof(*session));
    return map_assoc(as);
  }
  ds = cnet_sctp_dcep_init(&session->dcep, streams,
      config->is_dtls_client, config->association.is_owner,
      config->association.provider_context);
  if (ds != CNET_SCTP_DCEP_OK) {
    (void)cnet_sctp_assoc_stop(&session->association);
    (void)cnet_sctp_assoc_destroy(&session->association);
    memset(session, 0, sizeof(*session));
    return map_dcep(ds);
  }
  session->magic = SESSION_MAGIC;
  return CNET_SCTP_SESSION_OK;
}

cnet_sctp_session_status cnet_sctp_session_create(
    const cnet_sctp_session_create_config *config,
    uint64_t initial_time_ms, cnet_sctp_session **out) {
  cnet_sctp_session_config internal = {0};
  cnet_sctp_session *session;
  cnet_sctp_session_status status;
  if (!out) return CNET_SCTP_SESSION_INVALID;
  *out = NULL;
  if (!config || config->abi_version != CNET_SCTP_SESSION_ABI_VERSION ||
      config->struct_size != sizeof(*config))
    return CNET_SCTP_SESSION_INVALID;
  internal.association.local_port = config->local_port;
  internal.association.remote_port = config->remote_port;
  internal.association.receive_window = config->receive_window;
  internal.association.outbound_streams = config->outbound_streams;
  internal.association.inbound_streams = config->inbound_streams;
  internal.association.t1_ms = config->t1_ms;
  internal.association.t1_max_ms = config->t1_max_ms;
  internal.association.t5_shutdown_guard_ms = config->t5_shutdown_guard_ms;
  internal.association.cookie_lifetime_ms = config->cookie_lifetime_ms;
  internal.association.peer_binding = config->peer_binding;
  internal.association.max_retransmits = config->max_retransmits;
  internal.association.is_owner = config->is_owner;
  internal.association.entropy = config->entropy;
  internal.association.mac = config->mac;
  internal.association.verify_mac = config->verify_mac;
  internal.association.provider_context = config->provider_context;
  internal.is_dtls_client = config->is_dtls_client;
  session = (cnet_sctp_session *)calloc(1u, sizeof(*session));
  if (!session) return CNET_SCTP_SESSION_CAPACITY;
  status = cnet_sctp_session_init(session, &internal, initial_time_ms);
  if (status != CNET_SCTP_SESSION_OK) {
    free(session);
    return status;
  }
  *out = session;
  return CNET_SCTP_SESSION_OK;
}

cnet_sctp_session_status cnet_sctp_session_start(cnet_sctp_session *session,
    uint64_t now_ms, void *packet, size_t capacity, size_t *written) {
  cnet_sctp_session_status status = guard(session);
  if (status != CNET_SCTP_SESSION_OK) return status;
  return map_assoc(cnet_sctp_assoc_start(&session->association, now_ms,
      (uint8_t *)packet, capacity, written));
}

/* Latch cumulatively ACKed tracked reliable tickets when validated SCTP
 * input commits. Holding an ACK until application consumption must remain
 * safe across DCEP SID reset/reuse and subsequent TSN-space progression.
 * An input failure cannot consume tickets or fabricate ACK events. */
static void latch_reliable_cumulative_ack(cnet_sctp_session *session) {
  size_t i;
  const cnet_sctp_data *data;
  if (!session->association.data_ready) return;
  data = &session->association.data;
  for (i = 0u; i < CNET_SCTP_DATA_TX_SLOTS; ++i) {
    cnet_sctp_session_reliable_track *tracked = &session->tracked[i];
    if (tracked->used && tracked->fully_emitted &&
        !tracked->cumul_acked &&
        (uint32_t)(data->last_ack - tracked->ack.tsn) <
            UINT32_C(0x80000000))
      tracked->cumul_acked = 1u;
  }
  /* Latch only validated cumulative progress, never a revocable Gap SACK
   * or merely emitted FORWARD-TSN. Preserve result through TSN wrap. */
  for (i = 0u; i < CNET_SCTP_DATA_TX_SLOTS; ++i) {
    cnet_sctp_session_forward_track *track = &session->forward_tracks[i];
    if (track->used && !track->cumul_confirmed &&
        (uint32_t)(data->last_ack - track->result.last_tsn) <
            UINT32_C(0x80000000))
      track->cumul_confirmed = 1u;
  }
}
cnet_sctp_session_status cnet_sctp_session_input(cnet_sctp_session *session,
    uint64_t now_ms, const void *packet, size_t packet_size,
    void *reply, size_t capacity, size_t *written) {
  cnet_sctp_session_status status = guard(session);
  cnet_sctp_assoc_status as;
  if (status != CNET_SCTP_SESSION_OK) return status;
  as = cnet_sctp_assoc_input(&session->association, now_ms,
      (const uint8_t *)packet, packet_size, (uint8_t *)reply, capacity,
      written);
  if (as == CNET_SCTP_ASSOC_OK)
    latch_reliable_cumulative_ack(session);
  return map_assoc(as);
}

static void finish_tracked_reliable_fragments(cnet_sctp_session *session);
cnet_sctp_session_status cnet_sctp_session_advance(cnet_sctp_session *session,
    uint64_t now_ms, void *packet, size_t capacity, size_t *written) {
  cnet_sctp_session_status status = guard(session);
  cnet_sctp_assoc_status as;
  if (status != CNET_SCTP_SESSION_OK) return status;
  as = cnet_sctp_assoc_advance(&session->association, now_ms,
      (uint8_t *)packet, capacity, written);
  if (as == CNET_SCTP_ASSOC_OK && written && *written)
    finish_tracked_reliable_fragments(session);
  if (as == CNET_SCTP_ASSOC_EXPIRED &&
      session->association.state == CNET_SCTP_ASSOC_FAILED)
    session->terminal_retry_exhausted = 1u;
  return map_assoc(as);
}

cnet_sctp_session_status cnet_sctp_session_next_deadline(
    cnet_sctp_session *session, uint64_t *deadline_ms, int *has_deadline) {
  cnet_sctp_session_status status = guard(session);
  if (status != CNET_SCTP_SESSION_OK) return status;
  return map_assoc(cnet_sctp_assoc_next_deadline(&session->association,
      deadline_ms, has_deadline));
}

cnet_sctp_session_status cnet_sctp_session_open_channel(
    cnet_sctp_session *session, uint64_t now_ms, uint16_t stream,
    uint8_t type, uint16_t priority, uint32_t reliability,
    const void *label, size_t label_size,
    const void *protocol, size_t protocol_size,
    void *packet, size_t capacity, size_t *written) {
  uint8_t open[CNET_SCTP_DCEP_OPEN_MAX];
  size_t open_size = 0u;
  uint64_t generation = 0u;
  cnet_sctp_session_status status = guard(session);
  cnet_sctp_dcep_status ds;
  cnet_sctp_assoc_status as;
  if (written) *written = 0u;
  if (status != CNET_SCTP_SESSION_OK) return status;
  ds = cnet_sctp_dcep_open(&session->dcep, stream, type, priority,
      reliability, label, label_size, protocol, protocol_size,
      open, sizeof(open), &open_size);
  if (ds != CNET_SCTP_DCEP_OK) return map_dcep(ds);
  ds = cnet_sctp_dcep_generation(&session->dcep, stream, &generation);
  if (ds != CNET_SCTP_DCEP_OK) return map_dcep(ds);
  as = cnet_sctp_assoc_send_message(&session->association, now_ms, stream,
      CNET_SCTP_DCEP_PPID, 0, open, open_size, packet, capacity, written);
  if (as != CNET_SCTP_ASSOC_OK) {
    ds = cnet_sctp_dcep_open_cancel(&session->dcep, stream, generation);
    if (ds != CNET_SCTP_DCEP_OK) return map_dcep(ds);
    return map_assoc(as);
  }
  return CNET_SCTP_SESSION_OK;
}

cnet_sctp_session_status cnet_sctp_session_send(cnet_sctp_session *session,
    uint64_t now_ms, uint16_t stream, int binary,
    const void *message, size_t message_size,
    void *packet, size_t capacity, size_t *written) {
  cnet_sctp_data_reliability reliability;
  cnet_sctp_webrtc_payload payload;
  int unordered = 0;
  cnet_sctp_session_status status = guard(session);
  cnet_sctp_dcep_status ds;
  if (written) *written = 0u;
  if (status != CNET_SCTP_SESSION_OK) return status;
  ds = cnet_sctp_dcep_send_policy(&session->dcep, stream, &unordered,
      &reliability);
  if (ds != CNET_SCTP_DCEP_OK) return map_dcep(ds);
  if (cnet_sctp_webrtc_prepare(binary, message, message_size, &payload) !=
      CNET_SCTP_WEBRTC_OK)
    return CNET_SCTP_SESSION_INVALID;
  return map_assoc(cnet_sctp_assoc_send_message_pr(&session->association,
      now_ms, stream, payload.ppid, unordered, &reliability,
      payload.wire_bytes, payload.wire_size, packet, capacity, written));
}

/* The DATA sub-engine copies the entire bounded application message to a
 * single Owner-owned slot. No fragment/TSN can leave until pump_message()
 * is explicitly admitted. Reuse the exact DCEP policy projection used by
 * direct send; no reliable fallback on a partial-reliable channel. */
/* Only reliably sent, single-fragment application DATA participates here.
 * Reserve receipt space before committing an SCTP TSN; callbacks and borrowed
 * input do not survive admission. Original DCEP generation is immutable. */
cnet_sctp_session_status cnet_sctp_session_send_reliable_tracked(
    cnet_sctp_session *session, uint64_t now_ms,
    uint16_t stream, uint64_t expected_generation, int binary,
    const void *message, size_t message_size,
    void *packet, size_t capacity, size_t *written, uint64_t *ticket) {
  cnet_sctp_session_status status;
  cnet_sctp_dcep_status ds;
  cnet_sctp_assoc_status as;
  cnet_sctp_data_reliability reliability;
  cnet_sctp_webrtc_payload payload;
  cnet_sctp_session_reliable_track *slot = NULL;
  uint64_t generation = 0u;
  int unordered = 0;
  size_t index;
  if (written) *written = 0u;
  if (ticket) *ticket = 0u;
  status = guard(session);
  if (status != CNET_SCTP_SESSION_OK) return status;
  if (!ticket || !packet || !written || !expected_generation ||
      stream >= session->dcep.streams)
    return CNET_SCTP_SESSION_INVALID;
  ds = cnet_sctp_dcep_generation(&session->dcep, stream, &generation);
  if (ds != CNET_SCTP_DCEP_OK) return map_dcep(ds);
  if (generation != expected_generation) return CNET_SCTP_SESSION_STALE;
  ds = cnet_sctp_dcep_send_policy(&session->dcep, stream, &unordered,
      &reliability);
  if (ds != CNET_SCTP_DCEP_OK) return map_dcep(ds);
  if (reliability.mode != CNET_SCTP_DATA_RELIABLE)
    return CNET_SCTP_SESSION_PROTOCOL;
  if (cnet_sctp_webrtc_prepare(binary, message, message_size, &payload) !=
      CNET_SCTP_WEBRTC_OK) return CNET_SCTP_SESSION_INVALID;
  if (session->next_tracked_ticket == UINT64_MAX)
    return CNET_SCTP_SESSION_CAPACITY;
  for (index = 0u; index < CNET_SCTP_DATA_TX_SLOTS; ++index)
    if (!session->tracked[index].used) {
      slot = &session->tracked[index];
      break;
    }
  if (!slot) return CNET_SCTP_SESSION_CAPACITY;
  as = cnet_sctp_assoc_send_message_pr(&session->association, now_ms,
      stream, payload.ppid, unordered, &reliability,
      payload.wire_bytes, payload.wire_size, packet, capacity, written);
  if (as != CNET_SCTP_ASSOC_OK) return map_assoc(as);
  slot->ack.generation = generation;
  slot->ack.ticket = ++session->next_tracked_ticket;
  slot->ack.stream_id = stream;
  slot->ack.tsn = session->association.data.next_tsn - 1u;
  slot->ack.ppid = payload.ppid;
  slot->ack.message_size = payload.logical_size;
  slot->ack.binary = payload.binary;
  slot->fully_emitted = 1u;
  slot->queued = 0u;
  slot->used = 1u;
  *ticket = slot->ack.ticket;
  return CNET_SCTP_SESSION_OK;
}

/* Only the peer's validated cumulative SCTP acknowledgement can make a
 * tracked RELIABLE packet eligible. Latch it at input commit, not at read
 * time, so later TSN wrap/reset cannot erase an earlier completion.
 * A revocable Gap ACK never marks an event. */
/* A local abandoned result is separate from actual peer progress, which
 * the input Owner latches only after a validated cumulative SACK. */
/* Consume only original-generation, non-ACKed, explicitly tracked messages
 * after real retry-budget exhaustion. Success receipts are never failed;
 * queued/partially emitted sends have no valid final DATA TSN. */
/* A peer ABORT is its own one-shot association event, NEVER a local
 * retransmission-budget failure. Preserve the core Owner-only, capacity-
 * atomic reason-copy behavior without leaking private Association structs. */
cnet_sctp_session_status cnet_sctp_session_take_peer_abort(
    cnet_sctp_session *session, cnet_sctp_session_peer_abort *info,
    void *reason, size_t capacity, size_t *written) {
  cnet_sctp_session_status status;
  cnet_sctp_assoc_status as;
  cnet_sctp_assoc_peer_abort_info detail = {0};
  if (written) *written = 0u;
  if (info) memset(info, 0, sizeof(*info));
  status = guard(session);
  if (status != CNET_SCTP_SESSION_OK) return status;
  if (!info || !written) return CNET_SCTP_SESSION_INVALID;
  as = cnet_sctp_assoc_take_peer_abort(&session->association,
      &detail, reason, capacity, written);
  if (as != CNET_SCTP_ASSOC_OK) return map_assoc(as);
  info->last_acked_tsn = detail.last_acked_tsn;
  info->last_sent_tsn = detail.last_sent_tsn;
  info->has_tsn = detail.has_tsn;
  info->has_user_reason = detail.has_user_reason;
  return CNET_SCTP_SESSION_OK;
}

cnet_sctp_session_status cnet_sctp_session_take_terminal_failure(
    cnet_sctp_session *session,
    cnet_sctp_session_terminal_failure *failure) {
  cnet_sctp_session_status status;
  cnet_sctp_session_reliable_track *oldest = NULL;
  size_t i;
  if (!failure) return CNET_SCTP_SESSION_INVALID;
  memset(failure, 0, sizeof(*failure));
  status = guard(session);
  if (status != CNET_SCTP_SESSION_OK) return status;
  if (!session->terminal_retry_exhausted) return CNET_SCTP_SESSION_EMPTY;
  /* The DATA engine still owns unread original-generation local PR
   * abandonment records after local transport failure. Those MUST be
   * surfaced by poll() and reserved in the forward ledger BEFORE an
   * apparently empty terminal-receipt result can be reported. */
  if (session->association.data_ready &&
      session->association.data.abandon_event_count != 0u)
    return CNET_SCTP_SESSION_BUSY;
  for (i = 0u; i < CNET_SCTP_DATA_TX_SLOTS; ++i) {
    cnet_sctp_session_reliable_track *item = &session->tracked[i];
    if (!item->used || item->cumul_acked) continue;
    if (!oldest || item->ack.ticket < oldest->ack.ticket) oldest = item;
  }
  if (oldest) {
    failure->kind = CNET_SCTP_SESSION_TERMINAL_RELIABLE_UNACKED;
    failure->reason = CNET_SCTP_SESSION_TERMINAL_LOCAL_RETRY_EXHAUSTED;
    failure->generation = oldest->ack.generation;
    failure->ticket = oldest->ack.ticket;
    failure->stream_id = oldest->ack.stream_id;
    failure->has_final_tsn = oldest->fully_emitted;
    failure->last_tsn = oldest->fully_emitted ? oldest->ack.tsn : 0u;
    failure->ppid = oldest->ack.ppid;
    failure->message_size = oldest->ack.message_size;
    failure->binary = oldest->ack.binary;
    memset(oldest, 0, sizeof(*oldest));
    return CNET_SCTP_SESSION_OK;
  }
  for (i = 0u; i < CNET_SCTP_DATA_TX_SLOTS; ++i) {
    cnet_sctp_session_forward_track *item = &session->forward_tracks[i];
    if (!item->used || item->cumul_confirmed) continue;
    failure->kind = CNET_SCTP_SESSION_TERMINAL_FORWARD_UNCONFIRMED;
    failure->reason = CNET_SCTP_SESSION_TERMINAL_LOCAL_RETRY_EXHAUSTED;
    failure->generation = item->result.generation;
    failure->stream_id = item->result.stream_id;
    failure->last_tsn = item->result.last_tsn;
    failure->has_final_tsn = 1u;
    failure->ppid = item->result.ppid;
    failure->message_size = item->result.message_size;
    failure->binary = item->result.binary;
    memset(item, 0, sizeof(*item));
    return CNET_SCTP_SESSION_OK;
  }
  return CNET_SCTP_SESSION_EMPTY;
}

cnet_sctp_session_status cnet_sctp_session_take_forward_progress(
    cnet_sctp_session *session,
    cnet_sctp_session_forward_progress *progress) {
  cnet_sctp_session_status status;
  size_t index;
  if (!progress) return CNET_SCTP_SESSION_INVALID;
  memset(progress, 0, sizeof(*progress));
  status = guard(session);
  if (status != CNET_SCTP_SESSION_OK) return status;
  if (!session->association.data_ready) return CNET_SCTP_SESSION_CLOSED;
  for (index = 0u; index < CNET_SCTP_DATA_TX_SLOTS; ++index) {
    cnet_sctp_session_forward_track *track =
        &session->forward_tracks[index];
    if (!track->used || !track->cumul_confirmed) continue;
    *progress = track->result;
    memset(track, 0, sizeof(*track));
    return CNET_SCTP_SESSION_OK;
  }
  return CNET_SCTP_SESSION_EMPTY;
}

cnet_sctp_session_status cnet_sctp_session_take_reliable_ack(
    cnet_sctp_session *session, cnet_sctp_session_reliable_ack *ack) {
  cnet_sctp_session_status status;
  cnet_sctp_session_reliable_track *found = NULL;
  size_t index;
  if (!ack) return CNET_SCTP_SESSION_INVALID;
  memset(ack, 0, sizeof(*ack));
  status = guard(session);
  if (status != CNET_SCTP_SESSION_OK) return status;
  if (!session->association.data_ready) return CNET_SCTP_SESSION_CLOSED;
  for (index = 0u; index < CNET_SCTP_DATA_TX_SLOTS; ++index) {
    cnet_sctp_session_reliable_track *candidate = &session->tracked[index];
    if (!candidate->used || !candidate->cumul_acked) continue;
    if (!found || candidate->ack.ticket < found->ack.ticket)
      found = candidate;
  }
  if (!found) return CNET_SCTP_SESSION_EMPTY;
  *ack = found->ack;
  memset(found, 0, sizeof(*found));
  return CNET_SCTP_SESSION_OK;
}

/* Exactly one copied message may be pending per Association. Once its final
 * fragment is assigned a TSN (including a one-byte zero-window probe from
 * advance), the immutable receipt waits for cumulative ACK of that LAST TSN.
 * Failed admissions, retries or DCEP control packets never finish it. */
static void finish_tracked_reliable_fragments(cnet_sctp_session *session) {
  cnet_sctp_data *data;
  size_t i;
  if (!session->association.data_ready) return;
  data = &session->association.data;
  if (data->pending.active) return;
  for (i = 0u; i < CNET_SCTP_DATA_TX_SLOTS; ++i) {
    cnet_sctp_session_reliable_track *t = &session->tracked[i];
    if (!t->used || !t->queued || t->fully_emitted) continue;
    t->ack.tsn = data->next_tsn - 1u;
    t->fully_emitted = 1u;
    t->queued = 0u;
    break;
  }
}

/* Reserve a reliable-only confirmation slot prior to the real copied
 * queue. No borrowed bytes, worker or implicit transport send. */
cnet_sctp_session_status cnet_sctp_session_queue_reliable_tracked(
    cnet_sctp_session *session, uint64_t now_ms,
    uint16_t stream, uint64_t expected_generation, int binary,
    const void *message, size_t message_size, uint64_t *ticket) {
  cnet_sctp_session_status status = guard(session);
  cnet_sctp_dcep_status ds;
  cnet_sctp_assoc_status as;
  cnet_sctp_data_reliability policy;
  cnet_sctp_webrtc_payload payload;
  cnet_sctp_session_reliable_track *slot = NULL;
  uint64_t generation = 0u;
  size_t i;
  int unordered = 0;
  if (ticket) *ticket = 0u;
  if (status != CNET_SCTP_SESSION_OK) return status;
  if (!ticket || !expected_generation || stream >= session->dcep.streams)
    return CNET_SCTP_SESSION_INVALID;
  ds = cnet_sctp_dcep_generation(&session->dcep, stream, &generation);
  if (ds != CNET_SCTP_DCEP_OK) return map_dcep(ds);
  if (generation != expected_generation) return CNET_SCTP_SESSION_STALE;
  ds = cnet_sctp_dcep_send_policy(&session->dcep, stream, &unordered, &policy);
  if (ds != CNET_SCTP_DCEP_OK) return map_dcep(ds);
  if (policy.mode != CNET_SCTP_DATA_RELIABLE)
    return CNET_SCTP_SESSION_PROTOCOL;
  if (cnet_sctp_webrtc_prepare(binary, message, message_size, &payload) !=
      CNET_SCTP_WEBRTC_OK) return CNET_SCTP_SESSION_INVALID;
  if (session->next_tracked_ticket == UINT64_MAX)
    return CNET_SCTP_SESSION_CAPACITY;
  for (i = 0u; i < CNET_SCTP_DATA_TX_SLOTS; ++i) {
    if (!session->tracked[i].used) {
      slot = &session->tracked[i];
      break;
    }
  }
  if (!slot) return CNET_SCTP_SESSION_CAPACITY;
  as = cnet_sctp_assoc_queue_message_pr(&session->association, now_ms,
      stream, payload.ppid, unordered, &policy,
      payload.wire_bytes, payload.wire_size);
  if (as != CNET_SCTP_ASSOC_OK) return map_assoc(as);
  slot->ack.generation = generation;
  slot->ack.ticket = ++session->next_tracked_ticket;
  slot->ack.stream_id = stream;
  slot->ack.tsn = 0u;
  slot->ack.ppid = payload.ppid;
  slot->ack.message_size = payload.logical_size;
  slot->ack.binary = payload.binary;
  slot->cumul_acked = 0u;
  slot->fully_emitted = 0u;
  slot->queued = 1u;
  slot->used = 1u;
  *ticket = slot->ack.ticket;
  return CNET_SCTP_SESSION_OK;
}

/* An all-local, pre-TSN terminal cancellation can return its exact result
 * immediately and at most once, with the original generation and ticket.
 * Copy metadata before the private DATA engine clears the 24 KiB message. */
cnet_sctp_session_status cnet_sctp_session_cancel_unsent_message(
    cnet_sctp_session *session, uint64_t now_ms,
    uint16_t stream, uint64_t expected_generation,
    cnet_sctp_session_unsent_cancelled *cancelled) {
  cnet_sctp_session_status status;
  cnet_sctp_dcep_status ds;
  cnet_sctp_assoc_status as;
  const cnet_sctp_data_pending *pending;
  cnet_sctp_session_reliable_track *tracked = NULL;
  cnet_sctp_data_reliability policy;
  uint64_t generation = 0u, old_ticket = 0u;
  uint32_t ppid;
  size_t wire_size, i;
  uint8_t unordered;
  if (!cancelled) return CNET_SCTP_SESSION_INVALID;
  memset(cancelled, 0, sizeof(*cancelled));
  status = guard(session);
  if (status != CNET_SCTP_SESSION_OK) return status;
  if (!expected_generation || stream >= session->dcep.streams)
    return CNET_SCTP_SESSION_INVALID;
  ds = cnet_sctp_dcep_generation(&session->dcep, stream, &generation);
  if (ds != CNET_SCTP_DCEP_OK) return map_dcep(ds);
  if (generation != expected_generation) return CNET_SCTP_SESSION_STALE;
  if (!session->association.data_ready) return CNET_SCTP_SESSION_CLOSED;
  pending = &session->association.data.pending;
  if (!pending->active || pending->stream_id != stream) {
    const cnet_sctp_data *data = &session->association.data;
    /* Reuse the enclosing Owner-local index. MSVC /W4 /WX must reject
     * accidental shadowing in the standalone installed C11 SDK build. */
    /* A direct-send message may have no pending copy yet still own real
     * unretired DATA TSNs. Refuse to misreport such a request as an empty,
     * safely cancellable queue. Gap-ACKed tickets remain retained. */
    for (i = 0u; i < CNET_SCTP_DATA_TX_SLOTS; ++i) {
      const cnet_sctp_data_tx *tx = &data->tx[i];
      if (tx->used && tx->stream_id == stream &&
          tx->ppid != CNET_SCTP_DCEP_PPID)
        return CNET_SCTP_SESSION_BUSY;
    }
    /* A FORWARD-TSN is association-owned, with no honest per-SID receipt.
     * Conservatively block all SIDs while that control is in flight rather
     * than call local abandonment a remote-confirmed cancellation. */
    return data->forward_active ? CNET_SCTP_SESSION_BUSY :
        CNET_SCTP_SESSION_EMPTY;
  }
  if (pending->ppid == CNET_SCTP_DCEP_PPID)
    return CNET_SCTP_SESSION_PROTOCOL;
  if (pending->has_tsn || pending->offset)
    return CNET_SCTP_SESSION_BUSY;
  for (i = 0u; i < CNET_SCTP_DATA_TX_SLOTS; ++i) {
    cnet_sctp_session_reliable_track *slot = &session->tracked[i];
    if (!slot->used || !slot->queued || slot->fully_emitted ||
        slot->ack.stream_id != stream || slot->ack.generation != generation)
      continue;
    if (tracked) return CNET_SCTP_SESSION_PROTOCOL;
    tracked = slot;
  }
  /* A local cancellation must never claim a peer-visible DATA TSN. */
  policy = pending->reliability;
  ppid = pending->ppid;
  wire_size = pending->size;
  unordered = pending->unordered;
  old_ticket = tracked ? tracked->ack.ticket : 0u;
  as = cnet_sctp_assoc_cancel_unsent_message(&session->association,
      now_ms, stream);
  if (as != CNET_SCTP_ASSOC_OK) return map_assoc(as);
  cancelled->generation = generation;
  cancelled->ticket = old_ticket;
  cancelled->stream_id = stream;
  cancelled->ppid = ppid;
  cancelled->message_size =
      (ppid == CNET_SCTP_WEBRTC_EMPTY_TEXT_PPID ||
       ppid == CNET_SCTP_WEBRTC_EMPTY_BINARY_PPID) ? 0u : wire_size;
  cancelled->reliability.mode =
      (cnet_sctp_session_reliability_mode)policy.mode;
  cancelled->reliability.value = policy.value;
  cancelled->binary = ppid == CNET_SCTP_WEBRTC_BINARY_PPID ||
      ppid == CNET_SCTP_WEBRTC_EMPTY_BINARY_PPID;
  cancelled->unordered = unordered;
  if (tracked) memset(tracked, 0, sizeof(*tracked));
  return CNET_SCTP_SESSION_OK;
}

cnet_sctp_session_status cnet_sctp_session_queue_message(
    cnet_sctp_session *session, uint64_t now_ms, uint16_t stream, int binary,
    const void *message, size_t message_size) {
  cnet_sctp_data_reliability reliability;
  cnet_sctp_webrtc_payload payload;
  int unordered = 0;
  cnet_sctp_session_status status = guard(session);
  cnet_sctp_dcep_status ds;
  if (status != CNET_SCTP_SESSION_OK) return status;
  ds = cnet_sctp_dcep_send_policy(&session->dcep, stream, &unordered,
      &reliability);
  if (ds != CNET_SCTP_DCEP_OK) return map_dcep(ds);
  if (cnet_sctp_webrtc_prepare(binary, message, message_size, &payload) !=
      CNET_SCTP_WEBRTC_OK)
    return CNET_SCTP_SESSION_INVALID;
  return map_assoc(cnet_sctp_assoc_queue_message_pr(&session->association,
      now_ms, stream, payload.ppid, unordered, &reliability,
      payload.wire_bytes, payload.wire_size));
}

cnet_sctp_session_status cnet_sctp_session_pump_message(
    cnet_sctp_session *session, uint64_t now_ms,
    void *packet, size_t capacity, size_t *written) {
  cnet_sctp_session_status status = guard(session);
  cnet_sctp_assoc_status as;
  if (written) *written = 0u;
  if (status != CNET_SCTP_SESSION_OK) return status;
  as = cnet_sctp_assoc_pump_message(&session->association,
      now_ms, packet, capacity, written);
  if (as == CNET_SCTP_ASSOC_OK && written && *written)
    finish_tracked_reliable_fragments(session);
  return map_assoc(as);
}

/* Read-only, Owner-admitted, DCEP generation-fenced DATA ticket snapshot.
 * Gap ACKs must stay retained here until real cumulative settlement. */
cnet_sctp_session_status cnet_sctp_session_get_tx_usage(
    cnet_sctp_session *session, uint16_t stream,
    uint64_t expected_generation, cnet_sctp_session_tx_usage *usage) {
  cnet_sctp_session_status status;
  cnet_sctp_dcep_status ds;
  const cnet_sctp_data *data;
  uint64_t generation = 0u;
  size_t i;
  if (!usage) return CNET_SCTP_SESSION_INVALID;
  memset(usage, 0, sizeof(*usage));
  status = guard(session);
  if (status != CNET_SCTP_SESSION_OK) return status;
  if (!expected_generation || stream >= session->dcep.streams)
    return CNET_SCTP_SESSION_INVALID;
  ds = cnet_sctp_dcep_generation(&session->dcep, stream, &generation);
  if (ds != CNET_SCTP_DCEP_OK) return map_dcep(ds);
  if (generation != expected_generation) return CNET_SCTP_SESSION_STALE;
  if (!session->association.data_ready) return CNET_SCTP_SESSION_CLOSED;
  data = &session->association.data;
  usage->generation = generation;
  usage->association_forward_pending = data->forward_active ? 1u : 0u;
  if (data->pending.active && data->pending.stream_id == stream &&
      data->pending.ppid != CNET_SCTP_DCEP_PPID) {
    usage->has_pending_message = 1u;
    usage->queued_bytes = data->pending.size - data->pending.offset;
  }
  for (i = 0u; i < CNET_SCTP_DATA_TX_SLOTS; ++i) {
    const cnet_sctp_data_tx *tx = &data->tx[i];
    if (!tx->used || tx->stream_id != stream ||
        tx->ppid == CNET_SCTP_DCEP_PPID) continue;
    usage->retained_bytes += tx->user_size;
    ++usage->retained_fragments;
    if (tx->gap_acked) usage->gap_acked_bytes += tx->user_size;
    if (tx->abandoned) usage->abandoned_bytes += tx->user_size;
  }
  return CNET_SCTP_SESSION_OK;
}

cnet_sctp_session_status cnet_sctp_session_close_channel(
    cnet_sctp_session *session, uint64_t now_ms, uint16_t stream,
    void *packet, size_t capacity, size_t *written,
    uint64_t *generation) {
  uint64_t current = 0u;
  cnet_sctp_session_status status = guard(session);
  cnet_sctp_dcep_status ds;
  cnet_sctp_assoc_status as;
  if (written) *written = 0u;
  if (status != CNET_SCTP_SESSION_OK) return status;
  if (!generation || stream >= session->dcep.streams)
    return CNET_SCTP_SESSION_INVALID;
  ds = cnet_sctp_dcep_generation(&session->dcep, stream, &current);
  if (ds != CNET_SCTP_DCEP_OK) return map_dcep(ds);
  if (session->dcep.channels[stream].state == 3u ||
      session->dcep.channels[stream].state == 4u)
    return CNET_SCTP_SESSION_BUSY;
  as = cnet_sctp_assoc_reset_streams_bidirectional(&session->association,
      now_ms, &stream, 1u, packet, capacity, written);
  if (as != CNET_SCTP_ASSOC_OK) return map_assoc(as);
  ds = cnet_sctp_dcep_close_begin(&session->dcep, stream, generation);
  if (ds != CNET_SCTP_DCEP_OK) return map_dcep(ds);
  if (*generation != current) return CNET_SCTP_SESSION_STALE;
  session->closing_generation[stream] = current;
  return CNET_SCTP_SESSION_OK;
}

static cnet_sctp_session_status send_pending_ack(cnet_sctp_session *session,
    uint64_t now_ms, cnet_sctp_session_event *event,
    void *packet, size_t capacity, size_t *written) {
  static const uint8_t ack = 2u;
  uint16_t sid = first_set(session->pending_dcep_ack);
  uint64_t generation = 0u;
  cnet_sctp_assoc_status as = cnet_sctp_assoc_send_message(
      &session->association, now_ms, sid, CNET_SCTP_DCEP_PPID, 0,
      &ack, 1u, packet, capacity, written);
  cnet_sctp_dcep_status ds;
  if (as != CNET_SCTP_ASSOC_OK) return map_assoc(as);
  ds = cnet_sctp_dcep_ack_admitted(&session->dcep, sid);
  if (ds != CNET_SCTP_DCEP_OK) return map_dcep(ds);
  session->pending_dcep_ack &= (uint16_t)~(UINT16_C(1) << sid);
  ds = cnet_sctp_dcep_generation(&session->dcep, sid, &generation);
  if (ds != CNET_SCTP_DCEP_OK) return map_dcep(ds);
  event->kind = CNET_SCTP_SESSION_EVENT_CHANNEL_OPEN;
  event->stream_id = sid;
  event->generation = generation;
  event->locally_initiated = 0u;
  return CNET_SCTP_SESSION_OK;
}

cnet_sctp_session_status cnet_sctp_session_poll(cnet_sctp_session *session,
    uint64_t now_ms, cnet_sctp_session_event *event,
    void *message, size_t message_capacity,
    void *packet, size_t packet_capacity, size_t *packet_size) {
  cnet_sctp_assoc_stream_reset_event reset;
  cnet_sctp_data_abandon_event abandoned;
  cnet_sctp_data_message metadata;
  cnet_sctp_webrtc_payload payload;
  uint8_t ack[1];
  size_t message_size = 0u, ack_size = 0u;
  cnet_sctp_session_forward_track *reserved_forward = NULL;
  cnet_sctp_session_status status = guard(session);
  cnet_sctp_assoc_status as;
  cnet_sctp_dcep_status ds;
  if (packet_size) *packet_size = 0u;
  if (!event || !packet_size) return CNET_SCTP_SESSION_INVALID;
  memset(event, 0, sizeof(*event));
  if (status != CNET_SCTP_SESSION_OK) return status;
  /* Once FAILED, the retained DCEP ACK cannot be emitted. Retain its
   * obligation until explicit stop, but never let this unpublishable control
   * starve the Owner's original-generation DATA abandonment event. Neither
   * an ACK packet nor a CHANNEL_OPEN result is fabricated on this path. */
  if (session->pending_dcep_ack != 0u &&
      session->association.state != CNET_SCTP_ASSOC_FAILED)
    return send_pending_ack(session, now_ms, event, packet,
        packet_capacity, packet_size);

  /* Deliver abandonment before reset completion can retire the DCEP record
   * that owns its generation. The DATA layer reserves this fixed event slot
   * at admission, so no allocation or callback is needed here. */
  /* Reserve an additional progress slot BEFORE consuming a DATA abandon
   * event, otherwise result saturation could silently lose the original
   * generation. Pre-TSN expiry has no FORWARD and needs no slot. */
  if (session->association.data_ready) {
    const cnet_sctp_data *data = &session->association.data;
    if (data->abandon_event_count) {
      const cnet_sctp_data_abandon_event *next =
          &data->abandon_events[data->abandon_event_head];
      if (next->has_tsn) {
        size_t index;
        for (index = 0u; index < CNET_SCTP_DATA_TX_SLOTS; ++index) {
          cnet_sctp_session_forward_track *slot =
              &session->forward_tracks[index];
          if (!slot->used) {
            reserved_forward = slot;
            break;
          }
        }
        if (!reserved_forward) return CNET_SCTP_SESSION_CAPACITY;
      }
    }
  }
  as = cnet_sctp_assoc_take_abandoned(&session->association, &abandoned);
  if (as == CNET_SCTP_ASSOC_OK) {
    ds = cnet_sctp_dcep_generation(&session->dcep, abandoned.stream_id,
        &event->generation);
    if (ds != CNET_SCTP_DCEP_OK) return map_dcep(ds);
    event->kind = CNET_SCTP_SESSION_EVENT_MESSAGE_ABANDONED;
    event->stream_id = abandoned.stream_id;
    event->message_size = abandoned.message_size;
    event->message_identifier = abandoned.message_identifier;
    event->ppid = abandoned.ppid;
    event->first_tsn = abandoned.first_tsn;
    event->last_tsn = abandoned.last_tsn;
    event->reliability.mode =
        (cnet_sctp_session_reliability_mode)abandoned.reliability.mode;
    event->reliability.value = abandoned.reliability.value;
    event->abandon_reason =
        (cnet_sctp_session_abandon_reason)abandoned.reason;
    event->unordered = abandoned.unordered;
    event->interleaved = abandoned.interleaved;
    event->has_tsn = abandoned.has_tsn;
    event->binary = abandoned.ppid == 53u || abandoned.ppid == 57u;
    event->locally_initiated = 1u;
    if (abandoned.has_tsn) {
      if (!reserved_forward) return CNET_SCTP_SESSION_PROTOCOL;
      reserved_forward->result.generation = event->generation;
      reserved_forward->result.stream_id = event->stream_id;
      reserved_forward->result.first_tsn = event->first_tsn;
      reserved_forward->result.last_tsn = event->last_tsn;
      reserved_forward->result.ppid = event->ppid;
      reserved_forward->result.message_identifier =
          event->message_identifier;
      reserved_forward->result.message_size = event->message_size;
      reserved_forward->result.reliability = event->reliability;
      reserved_forward->result.abandon_reason = event->abandon_reason;
      reserved_forward->result.binary = event->binary;
      reserved_forward->result.unordered = event->unordered;
      reserved_forward->result.interleaved = event->interleaved;
      reserved_forward->used = 1u;
      if (session->association.data_ready &&
          (uint32_t)(session->association.data.last_ack -
              event->last_tsn) < UINT32_C(0x80000000))
        reserved_forward->cumul_confirmed = 1u;
    }
    return CNET_SCTP_SESSION_OK;
  }
  if (as != CNET_SCTP_ASSOC_EMPTY) return map_assoc(as);

  while ((as = cnet_sctp_assoc_take_stream_reset(&session->association,
              &reset)) == CNET_SCTP_ASSOC_OK) {
    uint16_t sid = reset.stream_id;
    uint64_t generation;
    int succeeded = reset.result == CNET_SCTP_STREAM_RESET_PERFORMED;
    if (sid >= session->dcep.streams) continue;
    generation = session->closing_generation[sid];
    if (generation == 0u && reset.incoming && succeeded) {
      ds = cnet_sctp_dcep_close_begin(&session->dcep, sid, &generation);
      if (ds != CNET_SCTP_DCEP_OK) return map_dcep(ds);
      session->closing_generation[sid] = generation;
    }
    if (generation == 0u) continue;
    ds = cnet_sctp_dcep_close_reset_result(&session->dcep, sid, generation,
        reset.incoming, succeeded);
    if (!succeeded) {
      event->kind = CNET_SCTP_SESSION_EVENT_CHANNEL_CLOSE_FAILED;
      event->stream_id = sid;
      event->generation = generation;
      event->incoming_reset = reset.incoming;
      event->reset_result = (cnet_sctp_session_reset_result)reset.result;
      return CNET_SCTP_SESSION_OK;
    }
    if (ds != CNET_SCTP_DCEP_OK) return map_dcep(ds);
    if (session->dcep.channels[sid].state == 0u) {
      session->closing_generation[sid] = 0u;
      event->kind = CNET_SCTP_SESSION_EVENT_CHANNEL_CLOSED;
      event->stream_id = sid;
      event->generation = generation;
      event->reset_result = (cnet_sctp_session_reset_result)reset.result;
      return CNET_SCTP_SESSION_OK;
    }
  }
  if (as != CNET_SCTP_ASSOC_EMPTY) return map_assoc(as);

  as = cnet_sctp_assoc_take_message(&session->association, &metadata,
      message, message_capacity, &message_size);
  if (as != CNET_SCTP_ASSOC_OK) return map_assoc(as);
  if (metadata.ppid == CNET_SCTP_DCEP_PPID) {
    uint64_t generation = 0u;
    uint8_t before = metadata.stream_id < session->dcep.streams ?
        session->dcep.channels[metadata.stream_id].state : 0u;
    ds = cnet_sctp_dcep_input(&session->dcep, metadata.stream_id,
        message, message_size, ack, sizeof(ack), &ack_size);
    if (ds != CNET_SCTP_DCEP_OK) return map_dcep(ds);
    if (ack_size != 0u) {
      session->pending_dcep_ack |=
          (uint16_t)(UINT16_C(1) << metadata.stream_id);
      return send_pending_ack(session, now_ms, event, packet,
          packet_capacity, packet_size);
    }
    if (before == 1u &&
        session->dcep.channels[metadata.stream_id].state == 2u) {
      ds = cnet_sctp_dcep_generation(&session->dcep, metadata.stream_id,
          &generation);
      if (ds != CNET_SCTP_DCEP_OK) return map_dcep(ds);
      event->kind = CNET_SCTP_SESSION_EVENT_CHANNEL_OPEN;
      event->stream_id = metadata.stream_id;
      event->generation = generation;
      event->locally_initiated = 1u;
      return CNET_SCTP_SESSION_OK;
    }
    return CNET_SCTP_SESSION_EMPTY;
  }
  ds = cnet_sctp_dcep_user_data_received(&session->dcep,
      metadata.stream_id);
  if (ds != CNET_SCTP_DCEP_OK) return map_dcep(ds);
  if (cnet_sctp_webrtc_decode(metadata.ppid, message, message_size,
      &payload) != CNET_SCTP_WEBRTC_OK)
    return CNET_SCTP_SESSION_PROTOCOL;
  ds = cnet_sctp_dcep_generation(&session->dcep, metadata.stream_id,
      &event->generation);
  if (ds != CNET_SCTP_DCEP_OK) return map_dcep(ds);
  event->kind = CNET_SCTP_SESSION_EVENT_MESSAGE;
  event->stream_id = metadata.stream_id;
  event->message_size = payload.logical_size;
  event->message_identifier = metadata.message_identifier;
  event->ppid = metadata.ppid;
  event->binary = payload.binary;
  event->unordered = metadata.unordered;
  event->interleaved = metadata.interleaved;
  event->locally_initiated =
      session->dcep.channels[metadata.stream_id].locally_initiated;
  return CNET_SCTP_SESSION_OK;
}

cnet_sctp_session_status cnet_sctp_session_stop(cnet_sctp_session *session) {
  cnet_sctp_session_status status = guard(session);
  cnet_sctp_dcep_status ds;
  cnet_sctp_assoc_status as;
  if (status != CNET_SCTP_SESSION_OK) return status;
  ds = cnet_sctp_dcep_stop(&session->dcep);
  if (ds != CNET_SCTP_DCEP_OK) return map_dcep(ds);
  as = cnet_sctp_assoc_stop(&session->association);
  if (as != CNET_SCTP_ASSOC_OK) return map_assoc(as);
  session->pending_dcep_ack = 0u;
  session->terminal_retry_exhausted = 0u;
  memset(session->tracked, 0, sizeof(session->tracked));
  memset(session->forward_tracks, 0, sizeof(session->forward_tracks));
  memset(session->closing_generation, 0, sizeof(session->closing_generation));
  return CNET_SCTP_SESSION_OK;
}

cnet_sctp_session_status cnet_sctp_session_destroy_in_place(
    cnet_sctp_session *session) {
  cnet_sctp_assoc_status as;
  if (!session || session->magic != SESSION_MAGIC)
    return CNET_SCTP_SESSION_INVALID;
  as = cnet_sctp_assoc_destroy(&session->association);
  if (as != CNET_SCTP_ASSOC_OK) return map_assoc(as);
  memset(session, 0, sizeof(*session));
  return CNET_SCTP_SESSION_OK;
}

cnet_sctp_session_status cnet_sctp_session_destroy(cnet_sctp_session *session) {
  cnet_sctp_session_status status = cnet_sctp_session_destroy_in_place(session);
  if (status != CNET_SCTP_SESSION_OK) return status;
  free(session);
  return CNET_SCTP_SESSION_OK;
}

int cnet_sctp_session_drained(const cnet_sctp_session *session) {
  size_t i;
  if (!session || session->magic != SESSION_MAGIC ||
      session->pending_dcep_ack != 0u)
    return 0;
  for (i = 0u; i < CNET_SCTP_DCEP_STREAMS; ++i)
    if (session->closing_generation[i] != 0u) return 0;
  for (i = 0u; i < CNET_SCTP_DATA_TX_SLOTS; ++i)
    if (session->forward_tracks[i].used) return 0;
  return cnet_sctp_assoc_drained(&session->association);
}
