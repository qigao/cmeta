#include "sctp_session_internal.h"

#include "sctp_webrtc_internal.h"

#include <string.h>

#define SESSION_MAGIC UINT32_C(0x53554c50)

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

cnet_sctp_session_status cnet_sctp_session_start(cnet_sctp_session *session,
    uint64_t now_ms, void *packet, size_t capacity, size_t *written) {
  cnet_sctp_session_status status = guard(session);
  if (status != CNET_SCTP_SESSION_OK) return status;
  return map_assoc(cnet_sctp_assoc_start(&session->association, now_ms,
      (uint8_t *)packet, capacity, written));
}

cnet_sctp_session_status cnet_sctp_session_input(cnet_sctp_session *session,
    uint64_t now_ms, const void *packet, size_t packet_size,
    void *reply, size_t capacity, size_t *written) {
  cnet_sctp_session_status status = guard(session);
  if (status != CNET_SCTP_SESSION_OK) return status;
  return map_assoc(cnet_sctp_assoc_input(&session->association, now_ms,
      (const uint8_t *)packet, packet_size, (uint8_t *)reply, capacity,
      written));
}

cnet_sctp_session_status cnet_sctp_session_advance(cnet_sctp_session *session,
    uint64_t now_ms, void *packet, size_t capacity, size_t *written) {
  cnet_sctp_session_status status = guard(session);
  if (status != CNET_SCTP_SESSION_OK) return status;
  return map_assoc(cnet_sctp_assoc_advance(&session->association, now_ms,
      (uint8_t *)packet, capacity, written));
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
  cnet_sctp_data_message metadata;
  cnet_sctp_webrtc_payload payload;
  uint8_t ack[1];
  size_t message_size = 0u, ack_size = 0u;
  cnet_sctp_session_status status = guard(session);
  cnet_sctp_assoc_status as;
  cnet_sctp_dcep_status ds;
  if (packet_size) *packet_size = 0u;
  if (!event || !packet_size) return CNET_SCTP_SESSION_INVALID;
  memset(event, 0, sizeof(*event));
  if (status != CNET_SCTP_SESSION_OK) return status;
  if (session->pending_dcep_ack != 0u)
    return send_pending_ack(session, now_ms, event, packet,
        packet_capacity, packet_size);

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
      event->reset_result = reset.result;
      return CNET_SCTP_SESSION_OK;
    }
    if (ds != CNET_SCTP_DCEP_OK) return map_dcep(ds);
    if (session->dcep.channels[sid].state == 0u) {
      session->closing_generation[sid] = 0u;
      event->kind = CNET_SCTP_SESSION_EVENT_CHANNEL_CLOSED;
      event->stream_id = sid;
      event->generation = generation;
      event->reset_result = reset.result;
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
  event->binary = payload.binary;
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
  memset(session->closing_generation, 0, sizeof(session->closing_generation));
  return CNET_SCTP_SESSION_OK;
}

cnet_sctp_session_status cnet_sctp_session_destroy(cnet_sctp_session *session) {
  cnet_sctp_assoc_status as;
  if (!session || session->magic != SESSION_MAGIC)
    return CNET_SCTP_SESSION_INVALID;
  as = cnet_sctp_assoc_destroy(&session->association);
  if (as != CNET_SCTP_ASSOC_OK) return map_assoc(as);
  memset(session, 0, sizeof(*session));
  return CNET_SCTP_SESSION_OK;
}

int cnet_sctp_session_drained(const cnet_sctp_session *session) {
  size_t i;
  if (!session || session->magic != SESSION_MAGIC ||
      session->pending_dcep_ack != 0u)
    return 0;
  for (i = 0u; i < CNET_SCTP_DCEP_STREAMS; ++i)
    if (session->closing_generation[i] != 0u) return 0;
  return cnet_sctp_assoc_drained(&session->association);
}
