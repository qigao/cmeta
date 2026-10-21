#include "sctp_assoc_internal.h"
#include "sctp_collision_internal.h"
#include "sctp_init_params_internal.h"

#include <limits.h>
#include <string.h>

#define SCTP_ASSOC_MAGIC UINT32_C(0x53314341)
enum {
  SCTP_TYPE_INIT = 1,
  SCTP_TYPE_INIT_ACK = 2,
  SCTP_TYPE_ABORT = 6,
  SCTP_TYPE_ERROR = 9,
  SCTP_TYPE_COOKIE_ECHO = 10,
  SCTP_TYPE_COOKIE_ACK = 11,
  SCTP_TYPE_SHUTDOWN = 7,
  SCTP_TYPE_SHUTDOWN_ACK = 8,
  SCTP_TYPE_SHUTDOWN_COMPLETE = 14,
  SCTP_PARAM_COOKIE = 7,
  SCTP_PARAM_UNRECOGNIZED = 8,
  SCTP_ERROR_UNRECOGNIZED = 8,
  SCTP_CLAIMS_BYTES = 56
};

static uint16_t rd16(const uint8_t *p) {
  return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}
static uint32_t rd32(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
         ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}
static uint64_t rd64(const uint8_t *p) {
  return ((uint64_t)rd32(p) << 32) | rd32(p + 4);
}
static void wr16(uint8_t *p, uint16_t n) {
  p[0] = (uint8_t)(n >> 8); p[1] = (uint8_t)n;
}
static void wr32(uint8_t *p, uint32_t n) {
  p[0] = (uint8_t)(n >> 24); p[1] = (uint8_t)(n >> 16);
  p[2] = (uint8_t)(n >> 8); p[3] = (uint8_t)n;
}
static void wr64(uint8_t *p, uint64_t n) {
  wr32(p, (uint32_t)(n >> 32)); wr32(p + 4, (uint32_t)n);
}

static cnet_sctp_assoc_status guard(cnet_sctp_assoc *s) {
  if (!s || s->magic != SCTP_ASSOC_MAGIC) return CNET_SCTP_ASSOC_INVALID;
  if (!s->config.is_owner(s->config.provider_context)) return CNET_SCTP_ASSOC_OWNER;
  if (s->in_progress) return CNET_SCTP_ASSOC_BUSY;
  return CNET_SCTP_ASSOC_OK;
}
static cnet_sctp_assoc_status encode_one(const cnet_sctp_assoc *s,
    uint32_t tag, uint8_t type, const uint8_t *value, size_t size,
    uint8_t *output, size_t capacity, size_t *written);

/* RFC-defined and registered extension types are not "unknown" merely
 * because this bounded Association profile has no handler for them. */
static int recognized_chunk_type(uint8_t type) {
  switch (type) {
    case 0u: case 1u: case 2u: case 3u: case 4u: case 5u:
    case 6u: case 7u: case 8u: case 9u: case 10u: case 11u:
    case 12u: case 13u: case 14u:
    case 15u:  /* AUTH, RFC 4895; registered but unsupported here */
    case 64u:  /* I-DATA, RFC 8260 */
    case 65u:  /* DTLS, temporary IANA registration; unsupported here */
    case 128u: /* ASCONF-ACK, RFC 5061 */
    case 130u: /* RE-CONFIG, RFC 6525 */
    case 132u: /* PAD, RFC 4820; registered but unsupported here */
    case 192u: /* FORWARD-TSN, RFC 3758 */
    case 193u: /* ASCONF, RFC 5061 */
    case 194u: /* I-FORWARD-TSN, RFC 8260 */
      return 1;
    default:
      return 0;
  }
}

/* Handle the bounded single-unknown-Chunk subset after framing, ports and
 * Verification Tag validation. All-unknown multi-Chunk packets use the
 * separate transactional handler below; mixed unknown/recognized bundles
 * remain fail-closed until the full legal-combination/transaction matrix exists. */
static cnet_sctp_assoc_status handle_unknown_single_chunk(
    cnet_sctp_assoc *s, uint64_t now, const cnet_sctp_wire_chunk *chunk,
    uint8_t *out, size_t cap, size_t *written) {
  cnet_sctp_wire_unknown_action action =
      cnet_sctp_wire_unknown_chunk_action(chunk->type);
  if (action == CNET_SCTP_WIRE_UNKNOWN_STOP_DISCARD ||
      action == CNET_SCTP_WIRE_UNKNOWN_SKIP) {
    s->last_time_ms = now;
    return CNET_SCTP_ASSOC_OK;
  }
  {
    /* Keep single-Chunk reporting within the same private 1200-byte
     * staging/egress bound as multi-Chunk reporting. Never allocate the
     * public Wire decoder's 65 KiB maximum on the Owner's stack or let a
     * large caller output buffer bypass this Association packet ceiling. */
    uint8_t cause[CNET_SCTP_ASSOC_MAX_PACKET];
    uint8_t response[CNET_SCTP_ASSOC_MAX_PACKET];
    size_t raw_size = chunk->value_size + 4u;
    size_t cause_size = raw_size + 4u;
    size_t response_size = 0u;
    cnet_sctp_assoc_status status;
    const uint8_t *raw = chunk->value - 4u;
    if (raw_size > UINT16_MAX - 4u || cause_size > sizeof(cause))
      return CNET_SCTP_ASSOC_CAPACITY;
    wr16(cause, 6u); /* Unrecognized Chunk Type */
    wr16(cause + 2u, (uint16_t)cause_size);
    memcpy(cause + 4u, raw, raw_size); /* Type, Flags, Length, Value */
    status = encode_one(s, s->peer_tag, SCTP_TYPE_ERROR, cause, cause_size,
        response, sizeof(response), &response_size);
    if (status != CNET_SCTP_ASSOC_OK) return status;
    if (response_size > cap) return CNET_SCTP_ASSOC_CAPACITY;
    memcpy(out, response, response_size);
    *written = response_size;
    s->last_time_ms = now;
    return CNET_SCTP_ASSOC_OK;
  }
}

/* Process a packet containing only genuinely unrecognized Chunks. This
 * bounded extension is intentionally separate from mixed unknown/known
 * bundles: it applies each action in wire order and accumulates REPORT causes
 * in private storage before publishing any response or advancing time. */
static cnet_sctp_assoc_status handle_unknown_multi_chunks(
    cnet_sctp_assoc *s, uint64_t now, const cnet_sctp_wire_decoder *decoded,
    size_t count, uint8_t *out, size_t cap, size_t *written) {
  uint8_t causes[CNET_SCTP_ASSOC_MAX_PACKET];
  uint8_t response[CNET_SCTP_ASSOC_MAX_PACKET];
  cnet_sctp_wire_decoder cursor;
  size_t causes_size = 0u;
  size_t response_size = 0u;
  size_t i;
  cnet_sctp_assoc_status status;
  if (!s || !decoded || count < 2u || !out || !written)
    return CNET_SCTP_ASSOC_INVALID;
  cursor = *decoded;
  for (i = 0u; i < count; ++i) {
    cnet_sctp_wire_chunk chunk;
    cnet_sctp_wire_unknown_action action;
    if (cnet_sctp_wire_next_chunk(&cursor, &chunk) != CNET_SCTP_WIRE_OK)
      return CNET_SCTP_ASSOC_PROTOCOL;
    action = cnet_sctp_wire_unknown_chunk_action(chunk.type);
    if (action == CNET_SCTP_WIRE_UNKNOWN_STOP_REPORT ||
        action == CNET_SCTP_WIRE_UNKNOWN_SKIP_REPORT) {
      size_t raw_size = chunk.value_size + 4u;
      size_t cause_size = raw_size + 4u;
      size_t padded_size = (cause_size + 3u) & ~(size_t)3u;
      if (cause_size > UINT16_MAX ||
          padded_size > sizeof(causes) - causes_size)
        return CNET_SCTP_ASSOC_CAPACITY;
      wr16(causes + causes_size, 6u); /* Unrecognized Chunk Type */
      wr16(causes + causes_size + 2u, (uint16_t)cause_size);
      memcpy(causes + causes_size + 4u, chunk.value - 4u, raw_size);
      if (padded_size > cause_size)
        memset(causes + causes_size + cause_size, 0,
               padded_size - cause_size);
      causes_size += padded_size;
    }
    if (action == CNET_SCTP_WIRE_UNKNOWN_STOP_DISCARD ||
        action == CNET_SCTP_WIRE_UNKNOWN_STOP_REPORT)
      break;
  }
  if (causes_size == 0u) {
    s->last_time_ms = now;
    return CNET_SCTP_ASSOC_OK;
  }
  status = encode_one(s, s->peer_tag, SCTP_TYPE_ERROR, causes, causes_size,
      response, sizeof(response), &response_size);
  if (status != CNET_SCTP_ASSOC_OK) return status;
  if (response_size > cap) return CNET_SCTP_ASSOC_CAPACITY;
  memcpy(out, response, response_size);
  *written = response_size;
  s->last_time_ms = now;
  return CNET_SCTP_ASSOC_OK;
}

static cnet_sctp_assoc_status check_time(cnet_sctp_assoc *s, uint64_t now) {
  return now < s->last_time_ms ? CNET_SCTP_ASSOC_CLOCK : CNET_SCTP_ASSOC_OK;
}
static cnet_sctp_assoc_status encode_one(const cnet_sctp_assoc *s,
    uint32_t tag, uint8_t type, const uint8_t *value, size_t size,
    uint8_t *output, size_t capacity, size_t *written) {
  cnet_sctp_wire_chunk chunk;
  cnet_sctp_wire_status status;
  chunk.type = type;
  chunk.flags = 0u;
  chunk.value = value;
  chunk.value_size = size;
  status = cnet_sctp_wire_encode(s->config.local_port, s->config.remote_port,
      tag, &chunk, 1u, output, capacity, written);
  if (status == CNET_SCTP_WIRE_CAPACITY) return CNET_SCTP_ASSOC_CAPACITY;
  return status == CNET_SCTP_WIRE_OK ? CNET_SCTP_ASSOC_OK : CNET_SCTP_ASSOC_PROTOCOL;
}
/* Construct reporting fields synchronously from the preflight's borrowed
 * offsets. No pointer survives this call, and each exact unknown TLV is
 * padded for its enclosing INIT ACK parameter or ERROR cause. */
static size_t append_unknown_reports(uint8_t *out, size_t cap,
    const uint8_t *original_value, size_t original_size,
    const cnet_sctp_init_param_view *parsed, int init_ack) {
  size_t pos = 0u;
  size_t i;
  for (i = 0u; i < parsed->report_count; ++i) {
    const cnet_sctp_init_report *r = &parsed->reports[i];
    size_t padded = (r->size + 3u) & ~(size_t)3u;
    size_t total = padded + (init_ack ? 4u : 0u);
    if (r->size < 4u || r->offset > original_size ||
        r->size > original_size - r->offset || total > cap - pos)
      return SIZE_MAX;
    if (init_ack) {
      wr16(out + pos, SCTP_PARAM_UNRECOGNIZED);
      wr16(out + pos + 2u, (uint16_t)(r->size + 4u));
      pos += 4u;
    }
    memcpy(out + pos, original_value + r->offset, r->size);
    if (padded > r->size) memset(out + pos + r->size, 0, padded - r->size);
    pos += padded;
  }
  return pos;
}

/* ABORTs in this small handshake profile always use an expected peer
 * Verification Tag (T-bit=0) and no optional error cause. */
static cnet_sctp_assoc_status respond_abort(cnet_sctp_assoc *s,
    uint32_t peer_initiate_tag, int abort_local,
    uint8_t *output, size_t cap, size_t *written) {
  cnet_sctp_assoc_status status;
  status = encode_one(s, peer_initiate_tag, SCTP_TYPE_ABORT,
                      NULL, 0u, output, cap, written);
  if (status == CNET_SCTP_ASSOC_OK && abort_local) {
    s->state = CNET_SCTP_ASSOC_FAILED;
    s->saved_size = 0u;
    s->deadline_ms = 0u;
    memset(s->saved_packet, 0, sizeof(s->saved_packet));
  }
  return status;
}

/* An ERROR bundled with COOKIE ECHO is an advisory unrecognized-parameter
 * cause, not an application message and not an excuse to create a second
 * delivery or lifetime path. Reject malformed or other causes. */
static int valid_cookie_echo_error(const cnet_sctp_wire_chunk *chunk) {
  const uint8_t *p;
  size_t pos = 4u;
  if (!chunk || chunk->type != SCTP_TYPE_ERROR || chunk->value_size < 8u)
    return 0;
  p = chunk->value;
  if (rd16(p) != SCTP_ERROR_UNRECOGNIZED ||
      rd16(p + 2u) != chunk->value_size) return 0;
  while (pos < chunk->value_size) {
    size_t len;
    size_t padded;
    if (chunk->value_size - pos < 4u) return 0;
    len = rd16(p + pos + 2u);
    if (len < 4u || len > chunk->value_size - pos) return 0;
    padded = (len + 3u) & ~(size_t)3u;
    if (padded > chunk->value_size - pos) return 0;
    if ((rd16(p + pos) >> 14) != 1u &&
        (rd16(p + pos) >> 14) != 3u) return 0;
    pos += padded;
  }
  return pos == chunk->value_size;
}

/* Validate SACK's internal length and ordered gap-block encoding without
 * mutating a DATA TCB. This preflight must run before the absolute T5 path:
 * malformed SACK bytes are not allowed to trigger a shutdown ABORT. */
static int valid_sack_shape(const cnet_sctp_wire_chunk *chunk) {
  uint16_t blocks, duplicates, previous_end = 0u;
  size_t i, expected;
  /* RFC 9260 3.3.4: reserved SACK flags do not invalidate a received SACK. */
  if (!chunk || chunk->type != 3u || chunk->value_size < 12u) return 0;
  blocks = rd16(chunk->value + 8u);
  duplicates = rd16(chunk->value + 10u);
  if (blocks > 16u) return 0;
  expected = 12u + 4u * ((size_t)blocks + (size_t)duplicates);
  if (chunk->value_size != expected) return 0;
  for (i = 0u; i < blocks; ++i) {
    const uint8_t *p = chunk->value + 12u + 4u * i;
    const uint16_t first = rd16(p);
    const uint16_t last = rd16(p + 2u);
    if (first == 0u || first <= previous_end || last < first) return 0;
    previous_end = last;
  }
  return 1;
}

/* The DATA engine borrows the existing handshake's single Owner and tags.
 * Nothing here opens a socket, starts a worker, or changes SCTP wire format.
 * Peer stream counts and rwnd are authenticated by INIT ACK/State Cookie. */
static cnet_sctp_assoc_status data_prepare(const cnet_sctp_assoc *s,
    cnet_sctp_data *candidate, uint64_t now, uint32_t peer_tag,
    uint32_t peer_tsn, uint32_t peer_rwnd,
    uint16_t peer_outbound, uint16_t peer_inbound) {
  cnet_sctp_data_config cfg = {0};
  uint16_t outbound, inbound;
  cnet_sctp_data_status result;
  if (!candidate || peer_tag == 0u || peer_rwnd < 1500u ||
      peer_outbound == 0u || peer_inbound == 0u)
    return CNET_SCTP_ASSOC_PROTOCOL;
  outbound = s->config.outbound_streams < peer_inbound ?
      s->config.outbound_streams : peer_inbound;
  inbound = s->config.inbound_streams < peer_outbound ?
      s->config.inbound_streams : peer_outbound;
  if (outbound > CNET_SCTP_DATA_STREAMS) outbound = CNET_SCTP_DATA_STREAMS;
  if (inbound > CNET_SCTP_DATA_STREAMS) inbound = CNET_SCTP_DATA_STREAMS;
  cfg.local_port = s->config.local_port;
  cfg.remote_port = s->config.remote_port;
  cfg.local_tag = s->local_tag;
  cfg.peer_tag = peer_tag;
  cfg.local_initial_tsn = s->initial_tsn;
  cfg.peer_initial_tsn = peer_tsn;
  cfg.outbound_streams = outbound;
  cfg.inbound_streams = inbound;
  cfg.peer_receive_window = peer_rwnd;
  cfg.t3_initial_ms = s->config.t1_ms; /* private timer baseline until T3 config */
  cfg.t3_max_ms = s->config.t1_max_ms;
  cfg.max_retransmits = s->config.max_retransmits;
  cfg.is_owner = s->config.is_owner;
  cfg.owner_context = s->config.provider_context;
  result = cnet_sctp_data_init(candidate, &cfg, now);
  return result == CNET_SCTP_DATA_OK ? CNET_SCTP_ASSOC_OK :
      result == CNET_SCTP_DATA_OWNER ? CNET_SCTP_ASSOC_OWNER :
      CNET_SCTP_ASSOC_PROTOCOL;
}
static cnet_sctp_assoc_status data_result(cnet_sctp_data_status status) {
  switch (status) {
    case CNET_SCTP_DATA_OK: return CNET_SCTP_ASSOC_OK;
    case CNET_SCTP_DATA_EMPTY: return CNET_SCTP_ASSOC_EMPTY;
    case CNET_SCTP_DATA_OWNER: return CNET_SCTP_ASSOC_OWNER;
    case CNET_SCTP_DATA_CAPACITY: return CNET_SCTP_ASSOC_CAPACITY;
    case CNET_SCTP_DATA_RETRANSMIT_PENDING:
      return CNET_SCTP_ASSOC_RETRANSMIT_PENDING;
    case CNET_SCTP_DATA_CLOCK: return CNET_SCTP_ASSOC_CLOCK;
    case CNET_SCTP_DATA_EXPIRED: return CNET_SCTP_ASSOC_EXPIRED;
    case CNET_SCTP_DATA_CLOSED: return CNET_SCTP_ASSOC_CLOSED_ERROR;
    case CNET_SCTP_DATA_INVALID: return CNET_SCTP_ASSOC_INVALID;
    default: return CNET_SCTP_ASSOC_PROTOCOL;
  }
}

/* Dispatch may provisionally set the Owner's monotonic input watermark.
 * Only a completed input (OK) or terminal expiry (EXPIRED) commits it.
 * In particular, a CAPACITY refusal from a bounded DATA/SACK handler or
 * staged handshake response published NO reply and NO TCB transaction:
 * the caller must be able to retry with a larger output at the SAME time,
 * without an otherwise invisible clock advance. */
static cnet_sctp_assoc_status dispatch_result(cnet_sctp_assoc *s,
    cnet_sctp_assoc_status status, uint64_t prior_time) {
  if (status != CNET_SCTP_ASSOC_OK &&
      status != CNET_SCTP_ASSOC_EXPIRED)
    s->last_time_ms = prior_time;
  return status;
}

static void prepare_init(const cnet_sctp_assoc *s, uint8_t value[16]) {
  wr32(value, s->local_tag);
  wr32(value + 4u, s->config.receive_window);
  wr16(value + 8u, s->config.outbound_streams);
  wr16(value + 10u, s->config.inbound_streams);
  wr32(value + 12u, s->initial_tsn);
}
static int good_init_fields(const uint8_t *v, size_t len) {
  return len >= 16u && rd32(v) != 0u && rd32(v + 4u) >= 1500u &&
         rd16(v + 8u) != 0u && rd16(v + 10u) != 0u;
}

/* Basic syntax preflight for the chunk subset this Association understands.
 * T5 is an absolute deadline, but malformed supported Chunks must not use it
 * as a side effect before their own parser would reject them. */
static int valid_error_cause_vector(const cnet_sctp_wire_chunk *chunk) {
  size_t pos = 0u;
  /* ERROR Chunk Flags are ignored on receipt (RFC 9260 3.3.10). */
  if (!chunk || chunk->type != SCTP_TYPE_ERROR ||
      chunk->value_size < 4u)
    return 0;
  while (pos < chunk->value_size) {
    size_t len, padded;
    if (chunk->value_size - pos < 4u) return 0;
    len = rd16(chunk->value + pos + 2u);
    if (len < 4u || len > chunk->value_size - pos) return 0;
    padded = (len + 3u) & ~(size_t)3u;
    if (padded > chunk->value_size - pos) return 0;
    pos += padded;
  }
  return pos == chunk->value_size;
}

static int assoc_chunk_shape_valid(const cnet_sctp_wire_chunk *chunk) {
  cnet_sctp_init_param_view params;
  cnet_sctp_init_param_status parsed;
  if (!chunk) return 0;
  switch (chunk->type) {
    case 0u: /* DATA */
      /* RFC 9260 3.3.1: the high reserved bits are ignored and I=1
       * requests immediate acknowledgement. Both are legal on receive. */
      return chunk->value_size >= 13u;
    case SCTP_TYPE_INIT:
    case SCTP_TYPE_INIT_ACK:
      /* INIT/INIT ACK flags are sent as zero and ignored on receipt. */
      if (!good_init_fields(chunk->value, chunk->value_size)) return 0;
      parsed = cnet_sctp_init_params_preflight(chunk->value,
          chunk->value_size, chunk->type == SCTP_TYPE_INIT_ACK, &params);
      return parsed == CNET_SCTP_INIT_PARAMS_OK ||
          parsed == CNET_SCTP_INIT_PARAMS_ABORT_REQUIRED;
    case 3u: /* SACK */
      return valid_sack_shape(chunk);
    case SCTP_TYPE_ABORT:
      /* Only T=0 supported; ignore the seven reserved ABORT bits. */
      return (chunk->flags & 1u) == 0u && chunk->value_size == 0u;
    case SCTP_TYPE_SHUTDOWN:
      return chunk->value_size == 4u;
    case SCTP_TYPE_SHUTDOWN_ACK:
    case SCTP_TYPE_COOKIE_ACK:
      return chunk->value_size == 0u;
    case SCTP_TYPE_SHUTDOWN_COMPLETE:
      /* T=1 remains outside the bounded profile; other bits ignored. */
      return (chunk->flags & 1u) == 0u && chunk->value_size == 0u;
    case SCTP_TYPE_ERROR:
      return valid_error_cause_vector(chunk);
    case SCTP_TYPE_COOKIE_ECHO:
      return chunk->value_size == CNET_SCTP_ASSOC_COOKIE_BYTES;
    default:
      /* A registered but unimplemented control has no private parser, so
       * its payload cannot qualify for a packet-triggered T5 transition.
       * Truly unknown Chunks still use their RFC action bits after T5. */
      return !recognized_chunk_type(chunk->type);
  }
}

static int assoc_packet_shape_preflight(
    const cnet_sctp_wire_decoder *decoded, size_t count) {
  cnet_sctp_wire_decoder cursor;
  size_t i;
  if (!decoded || count == 0u) return 0;
  cursor = *decoded;
  for (i = 0u; i < count; ++i) {
    cnet_sctp_wire_chunk chunk;
    if (cnet_sctp_wire_next_chunk(&cursor, &chunk) != CNET_SCTP_WIRE_OK ||
        !assoc_chunk_shape_valid(&chunk)) return 0;
  }
  return 1;
}
static cnet_sctp_assoc_status publish_packet(uint8_t *output, size_t cap,
    size_t *written, const uint8_t *scratch, size_t size) {
  if (size > cap) return CNET_SCTP_ASSOC_CAPACITY;
  memcpy(output, scratch, size);
  *written = size;
  return CNET_SCTP_ASSOC_OK;
}

/* Preflight the entire bounded packet's Chunk ordering before an Association
 * handler can mutate state or call the DATA engine. This is the deliberately
 * supported private bundle subset, not the complete RFC 9260 matrix:
 * DATA-only, or a leading SACK followed by DATA, COOKIE-ECHO + advisory
 * ERROR, and SHUTDOWN + SACK. All other multi-control bundles are rejected. */
static int assoc_bundle_preflight(const cnet_sctp_wire_decoder *decoded,
    size_t count) {
  cnet_sctp_wire_decoder cursor;
  cnet_sctp_wire_chunk chunk;
  size_t i;
  int data_seen = 0;
  if (!decoded || count == 0u || count > CNET_SCTP_WIRE_MAX_CHUNKS)
    return 0;
  cursor = *decoded;
  if (count == 1u) return 1; /* Single chunks receive state-specific checks. */
  if (count == 2u) {
    cnet_sctp_wire_decoder pair = cursor;
    cnet_sctp_wire_chunk first, second;
    if (cnet_sctp_wire_next_chunk(&pair, &first) != CNET_SCTP_WIRE_OK ||
        cnet_sctp_wire_next_chunk(&pair, &second) != CNET_SCTP_WIRE_OK)
      return 0;
    if (first.type == SCTP_TYPE_COOKIE_ECHO)
      return second.type == SCTP_TYPE_ERROR;
    if (first.type == SCTP_TYPE_SHUTDOWN)
      return second.type == 3u;
  }
  for (i = 0u; i < count; ++i) {
    if (cnet_sctp_wire_next_chunk(&cursor, &chunk) != CNET_SCTP_WIRE_OK)
      return 0;
    if (chunk.type == 0u) {
      data_seen = 1;
    } else if (chunk.type == 3u && i == 0u) {
      /* Only a leading SACK may accompany DATA in this private profile. */
    } else {
      return 0;
    }
  }
  return data_seen;
}

static void arm_timer(cnet_sctp_assoc *s, uint64_t now) {
  s->current_rto_ms = s->config.t1_ms;
  s->deadline_ms = now + s->current_rto_ms;
  s->retransmits = 0u;
}
static cnet_sctp_assoc_status begin_packet(cnet_sctp_assoc *s, uint64_t now,
    const uint8_t *scratch, size_t size, cnet_sctp_assoc_state next,
    uint8_t *output, size_t cap, size_t *written) {
  cnet_sctp_assoc_status status = publish_packet(output, cap, written, scratch, size);
  if (status != CNET_SCTP_ASSOC_OK) return status;
  memcpy(s->saved_packet, scratch, size);
  s->saved_size = size;
  s->state = next;
  arm_timer(s, now);
  s->last_time_ms = now;
  return CNET_SCTP_ASSOC_OK;
}

/* A separate required CSPRNG fills the local verification tag and initial
 * TSN. No caller-supplied, predictable or protocol-state-derived fallback.
 * Initialization is serialized on the CNet Owner, and callback reentry is
 * rejected while the provider is executing. */
cnet_sctp_assoc_status cnet_sctp_assoc_init(cnet_sctp_assoc *s,
    const cnet_sctp_assoc_config *config, uint64_t initial_time_ms) {
  uint8_t entropy[CNET_SCTP_ASSOC_ENTROPY_BYTES] = {0};
  int entropy_result;
  uint32_t tag;
  if (!s || !config || !config->is_owner || !config->mac ||
      !config->verify_mac || !config->entropy ||
      !config->is_owner(config->provider_context) ||
      config->local_port == 0u || config->remote_port == 0u ||
      config->receive_window < 1500u ||
      config->outbound_streams == 0u || config->inbound_streams == 0u ||
      config->t1_ms == 0u || config->t1_max_ms < config->t1_ms ||
      config->t1_max_ms > UINT64_MAX - initial_time_ms ||
      config->cookie_lifetime_ms == 0u || config->peer_binding == 0u ||
      config->max_retransmits == 0u)
    return CNET_SCTP_ASSOC_INVALID;
  /* The object must be zero-initialized by the owner before first init;
   * never overwrite an admitted session or a provider-reentrant init. */
  if (s->magic == SCTP_ASSOC_MAGIC) return CNET_SCTP_ASSOC_BUSY;
  memset(s, 0, sizeof(*s));
  s->config = *config;
  s->magic = SCTP_ASSOC_MAGIC;
  s->in_progress = 1u;
  entropy_result = s->config.entropy(s->config.provider_context,
                                    entropy, sizeof(entropy));
  s->in_progress = 0u;
  tag = rd32(entropy);
  if (entropy_result != 0 || tag == 0u) {
    /* Even a broken callback's partial entropy output is never retained. */
    memset(entropy, 0, sizeof(entropy));
    memset(s, 0, sizeof(*s));
    return CNET_SCTP_ASSOC_ENTROPY;
  }
  s->local_tag = tag;
  s->initial_tsn = rd32(entropy + 4u);
  memset(entropy, 0, sizeof(entropy));
  s->last_time_ms = initial_time_ms;
  s->state = CNET_SCTP_ASSOC_CLOSED;
  return CNET_SCTP_ASSOC_OK;
}

cnet_sctp_assoc_status cnet_sctp_assoc_start(cnet_sctp_assoc *s,
    uint64_t now, uint8_t *output, size_t cap, size_t *written) {
  uint8_t value[16];
  uint8_t scratch[CNET_SCTP_ASSOC_MAX_PACKET];
  size_t size = 0u;
  cnet_sctp_assoc_status status = guard(s);
  if (status != CNET_SCTP_ASSOC_OK) return status;
  if (!output || !written) return CNET_SCTP_ASSOC_INVALID;
  *written = 0u;
  if (s->state != CNET_SCTP_ASSOC_CLOSED) return CNET_SCTP_ASSOC_CLOSED_ERROR;
  if ((status = check_time(s, now)) != CNET_SCTP_ASSOC_OK ||
      now > UINT64_MAX - s->config.t1_ms) return CNET_SCTP_ASSOC_CLOCK;
  prepare_init(s, value);
  status = encode_one(s, 0u, SCTP_TYPE_INIT, value, sizeof(value),
      scratch, sizeof(scratch), &size);
  if (status != CNET_SCTP_ASSOC_OK) return status;
  return begin_packet(s, now, scratch, size, CNET_SCTP_ASSOC_COOKIE_WAIT,
      output, cap, written);
}

/* Cookie is versioned, self-contained association attributes + provider MAC.
 * No pointer, key, heap state or raw runtime object is serialized into it.
 * Critical: MAC provider must be a real cryptographic PRF before any release. */
static cnet_sctp_assoc_status make_cookie(cnet_sctp_assoc *s,
    uint64_t now, const uint8_t *peer_init,
    uint8_t cookie[CNET_SCTP_ASSOC_COOKIE_BYTES]) {
  memset(cookie, 0, CNET_SCTP_ASSOC_COOKIE_BYTES);
  cookie[0] = 'S'; cookie[1] = 'C'; cookie[2] = '1'; cookie[3] = '1';
  wr64(cookie + 4, now);
  wr16(cookie + 12, s->config.remote_port); /* initiator port */
  wr16(cookie + 14, s->config.local_port);  /* responder port */
  wr32(cookie + 16, rd32(peer_init));
  wr32(cookie + 20, s->local_tag);
  wr32(cookie + 24, rd32(peer_init + 12));
  wr32(cookie + 28, s->initial_tsn);
  wr16(cookie + 32, rd16(peer_init + 8));
  wr16(cookie + 34, rd16(peer_init + 10));
  wr32(cookie + 36, rd32(peer_init + 4));
  wr32(cookie + 40, s->config.receive_window);
  wr16(cookie + 44, s->config.outbound_streams);
  wr16(cookie + 46, s->config.inbound_streams);
  wr64(cookie + 48, s->config.peer_binding);
  s->in_progress = 1u;
  if (s->config.mac(s->config.provider_context, cookie, SCTP_CLAIMS_BYTES,
        cookie + SCTP_CLAIMS_BYTES) != 0) {
    s->in_progress = 0u;
    return CNET_SCTP_ASSOC_AUTH;
  }
  s->in_progress = 0u;
  return CNET_SCTP_ASSOC_OK;
}
static cnet_sctp_assoc_status verify_cookie(cnet_sctp_assoc *s,
    uint64_t now, const uint8_t *cookie, size_t cookie_size) {
  uint64_t issued;
  int mac_result;
  if (cookie_size != CNET_SCTP_ASSOC_COOKIE_BYTES ||
      memcmp(cookie, "SC11", 4u) != 0) return CNET_SCTP_ASSOC_AUTH;
  if (rd16(cookie + 12u) != s->config.remote_port ||
      rd16(cookie + 14u) != s->config.local_port ||
      rd32(cookie + 20u) != s->local_tag ||
      rd32(cookie + 28u) != s->initial_tsn ||
      rd32(cookie + 16u) == 0u ||
      rd16(cookie + 32u) == 0u || rd16(cookie + 34u) == 0u ||
      rd32(cookie + 36u) < 1500u ||
      rd32(cookie + 40u) != s->config.receive_window ||
      rd16(cookie + 44u) != s->config.outbound_streams ||
      rd16(cookie + 46u) != s->config.inbound_streams ||
      rd64(cookie + 48u) != s->config.peer_binding)
    return CNET_SCTP_ASSOC_AUTH;
  s->in_progress = 1u;
  mac_result = s->config.verify_mac(s->config.provider_context, cookie,
      SCTP_CLAIMS_BYTES, cookie + SCTP_CLAIMS_BYTES);
  s->in_progress = 0u;
  if (mac_result != 0) return CNET_SCTP_ASSOC_AUTH;
  issued = rd64(cookie + 4u);
  if (issued > now) return CNET_SCTP_ASSOC_EXPIRED;
  if (now - issued > s->config.cookie_lifetime_ms) {
    /* RFC 9260 Section 5.2.4, existing-association case E: an authenticated
     * duplicate Cookie that matches BOTH current verification tags may be
     * ACKed after its nominal TTL. Never use this branch to admit a new TCB,
     * switch peer identity or revive a stopped/closing generation. The
     * additional peer TSN check is conservative for this M2 prototype. */
    if (s->state != CNET_SCTP_ASSOC_ESTABLISHED ||
        s->peer_tag == 0u || s->peer_tag != rd32(cookie + 16u) ||
        s->peer_initial_tsn != rd32(cookie + 24u))
      return CNET_SCTP_ASSOC_EXPIRED;
  }
  return CNET_SCTP_ASSOC_OK;
}

static cnet_sctp_assoc_status handle_init(cnet_sctp_assoc *s, uint64_t now,
    const cnet_sctp_wire_header *header, const cnet_sctp_wire_chunk *chunk,
    uint8_t *output, size_t cap, size_t *written) {
  enum { INIT_ACK_BASE = 16u + 4u + CNET_SCTP_ASSOC_COOKIE_BYTES };
  uint8_t value[INIT_ACK_BASE + CNET_SCTP_INIT_REPORT_BYTES +
                4u * CNET_SCTP_INIT_REPORT_MAX];
  uint8_t scratch[CNET_SCTP_ASSOC_MAX_PACKET];
  cnet_sctp_init_param_view params;
  cnet_sctp_init_param_status parsed;
  size_t value_size = INIT_ACK_BASE;
  size_t extra;
  size_t size = 0u;
  cnet_sctp_assoc_status status;
  /* RFC 9260 §5.2.1: simultaneous INIT may arrive while our own INIT
   * or COOKIE ECHO awaits a response. Emit a stateless INIT ACK from the
   * original parameters WITHOUT changing T1, saved packet, local Tag/TSN
   * or the current state. Established-state restart needs authenticated
   * Tie-Tag and Generation transactions and remains deliberately blocked. */
  if ((s->state != CNET_SCTP_ASSOC_CLOSED &&
       s->state != CNET_SCTP_ASSOC_COOKIE_WAIT &&
       s->state != CNET_SCTP_ASSOC_COOKIE_ECHOED) ||
      header->verification_tag != 0u ||
      !good_init_fields(chunk->value, chunk->value_size))
    return CNET_SCTP_ASSOC_PROTOCOL;
  parsed = cnet_sctp_init_params_preflight(chunk->value, chunk->value_size, 0, &params);
  if (parsed == CNET_SCTP_INIT_PARAMS_ABORT_REQUIRED)
    return respond_abort(s, rd32(chunk->value), 0, output, cap, written);
  if (parsed != CNET_SCTP_INIT_PARAMS_OK) return CNET_SCTP_ASSOC_PROTOCOL;
  extra = params.report_wire_size + 4u * params.report_count;
  if (extra > sizeof(value) - INIT_ACK_BASE ||
      INIT_ACK_BASE + extra > CNET_SCTP_ASSOC_MAX_PACKET - 16u ||
      INIT_ACK_BASE + extra > cap ||
      16u > cap - (INIT_ACK_BASE + extra))
    return CNET_SCTP_ASSOC_CAPACITY;
  memset(value, 0, sizeof(value));
  prepare_init(s, value);
  wr16(value + 16u, SCTP_PARAM_COOKIE);
  wr16(value + 18u, (uint16_t)(4u + CNET_SCTP_ASSOC_COOKIE_BYTES));
  {
    size_t reported = append_unknown_reports(value + INIT_ACK_BASE,
        sizeof(value) - INIT_ACK_BASE, chunk->value, chunk->value_size,
        &params, 1);
    if (reported != extra) return CNET_SCTP_ASSOC_PROTOCOL;
    value_size += reported;
  }
  /* If output cannot fit the full required response, do not even call
   * the signing provider; no incomplete signed INIT ACK may escape. */
  status = make_cookie(s, now, chunk->value, value + 20u);
  if (status != CNET_SCTP_ASSOC_OK) return status;
  status = encode_one(s, rd32(chunk->value), SCTP_TYPE_INIT_ACK,
      value, value_size, scratch, sizeof(scratch), &size);
  if (status != CNET_SCTP_ASSOC_OK) return status;
  status = publish_packet(output, cap, written, scratch, size);
  if (status == CNET_SCTP_ASSOC_OK) s->last_time_ms = now;
  return status;
}

static cnet_sctp_assoc_status handle_init_ack(cnet_sctp_assoc *s,
    uint64_t now, const cnet_sctp_wire_header *header,
    const cnet_sctp_wire_chunk *chunk,
    uint8_t *output, size_t cap, size_t *written) {
  cnet_sctp_init_param_view params;
  cnet_sctp_init_param_status parsed;
  uint8_t error[4u + CNET_SCTP_INIT_REPORT_BYTES];
  const uint8_t *cookie;
  size_t cookie_size;
  uint8_t scratch[CNET_SCTP_ASSOC_MAX_PACKET];
  size_t size = 0u;
  uint32_t peer_tag;
  cnet_sctp_assoc_status status;
  if (s->state != CNET_SCTP_ASSOC_COOKIE_WAIT ||
      header->verification_tag != s->local_tag ||
      !good_init_fields(chunk->value, chunk->value_size))
    return CNET_SCTP_ASSOC_PROTOCOL;
  parsed = cnet_sctp_init_params_preflight(chunk->value, chunk->value_size, 1, &params);
  peer_tag = rd32(chunk->value);
  if (parsed == CNET_SCTP_INIT_PARAMS_ABORT_REQUIRED)
    return respond_abort(s, peer_tag, 1, output, cap, written);
  if (parsed != CNET_SCTP_INIT_PARAMS_OK) return CNET_SCTP_ASSOC_PROTOCOL;
  cookie = params.cookie;
  cookie_size = params.cookie_size;
  if (cookie_size > CNET_SCTP_ASSOC_MAX_PACKET - 16u)
    return CNET_SCTP_ASSOC_CAPACITY;
  if (params.report_count == 0u) {
    status = encode_one(s, peer_tag, SCTP_TYPE_COOKIE_ECHO,
        cookie, cookie_size, scratch, sizeof(scratch), &size);
    if (status != CNET_SCTP_ASSOC_OK) return status;
  } else {
    cnet_sctp_wire_chunk chunks[2];
    size_t report_bytes;
    cnet_sctp_wire_status wire_status;
    memset(error, 0, sizeof(error));
    report_bytes = append_unknown_reports(error + 4u, sizeof(error) - 4u,
        chunk->value, chunk->value_size, &params, 0);
    if (report_bytes != params.report_wire_size || report_bytes == 0u)
      return CNET_SCTP_ASSOC_PROTOCOL;
    wr16(error, SCTP_ERROR_UNRECOGNIZED);
    wr16(error + 2u, (uint16_t)(4u + report_bytes));
    chunks[0].type = SCTP_TYPE_COOKIE_ECHO;
    chunks[0].flags = 0u;
    chunks[0].value = cookie;
    chunks[0].value_size = cookie_size;
    chunks[1].type = SCTP_TYPE_ERROR;
    chunks[1].flags = 0u;
    chunks[1].value = error;
    chunks[1].value_size = 4u + report_bytes;
    wire_status = cnet_sctp_wire_encode(s->config.local_port,
        s->config.remote_port, peer_tag, chunks, 2u,
        scratch, sizeof(scratch), &size);
    if (wire_status == CNET_SCTP_WIRE_CAPACITY)
      return CNET_SCTP_ASSOC_CAPACITY;
    if (wire_status != CNET_SCTP_WIRE_OK)
      return CNET_SCTP_ASSOC_PROTOCOL;
  }
  if (now > UINT64_MAX - s->config.t1_ms) return CNET_SCTP_ASSOC_CLOCK;
  status = begin_packet(s, now, scratch, size, CNET_SCTP_ASSOC_COOKIE_ECHOED,
      output, cap, written);
  if (status == CNET_SCTP_ASSOC_OK) {
    s->peer_tag = peer_tag;
    s->peer_initial_tsn = rd32(chunk->value + 12u);
    s->peer_receive_window = rd32(chunk->value + 4u);
    s->peer_outbound_streams = rd16(chunk->value + 8u);
    s->peer_inbound_streams = rd16(chunk->value + 10u);
  }
  return status;
}
/* Accept RFC Table 12 cases B/D only while no DATA, resource generation
 * replacement, or external callback can exist. Restart A and stale C remain
 * blocked; do NOT convert a decision-only classifier into a live restart.
 * Publishing the complete COOKIE ACK is the last fallible operation before
 * an Owner-local state/peer transaction. */
static cnet_sctp_assoc_status handle_cookie_echo(cnet_sctp_assoc *s,
    uint64_t now, const cnet_sctp_wire_header *header,
    const cnet_sctp_wire_chunk *chunk,
    uint8_t *output, size_t cap, size_t *written) {
  cnet_sctp_cookie_tags tags = {0};
  cnet_sctp_cookie_decision decision;
  uint8_t scratch[CNET_SCTP_ASSOC_MAX_PACKET];
  cnet_sctp_data candidate = {0};
  size_t size = 0u;
  uint32_t peer_tag, peer_tsn;
  int may_commit;
  cnet_sctp_assoc_status status;
  if ((s->state != CNET_SCTP_ASSOC_CLOSED &&
       s->state != CNET_SCTP_ASSOC_COOKIE_WAIT &&
       s->state != CNET_SCTP_ASSOC_COOKIE_ECHOED &&
       s->state != CNET_SCTP_ASSOC_ESTABLISHED) ||
      header->verification_tag != s->local_tag)
    return CNET_SCTP_ASSOC_PROTOCOL;
  status = verify_cookie(s, now, chunk->value, chunk->value_size);
  if (status != CNET_SCTP_ASSOC_OK) return status;
  peer_tag = rd32(chunk->value + 16u);
  peer_tsn = rd32(chunk->value + 24u);
  tags.existing_local = s->local_tag;
  tags.existing_peer = s->peer_tag;
  tags.cookie_local = rd32(chunk->value + 20u);
  tags.cookie_peer = peer_tag;
  decision = cnet_sctp_cookie_classify(&tags);
  may_commit = s->state == CNET_SCTP_ASSOC_CLOSED ||
      (s->state == CNET_SCTP_ASSOC_COOKIE_WAIT &&
       decision == CNET_SCTP_COOKIE_COLLISION_REQUIRES_COMMIT) ||
      (s->state == CNET_SCTP_ASSOC_COOKIE_ECHOED &&
       decision == CNET_SCTP_COOKIE_DUPLICATE_ACK &&
       s->peer_initial_tsn == peer_tsn);
  if (!may_commit &&
      !(s->state == CNET_SCTP_ASSOC_ESTABLISHED &&
        decision == CNET_SCTP_COOKIE_DUPLICATE_ACK &&
        s->peer_initial_tsn == peer_tsn))
    return CNET_SCTP_ASSOC_AUTH; /* no live Restart A/C, no peer replacement */
  if (may_commit) {
    status = data_prepare(s, &candidate, now, peer_tag, peer_tsn,
        rd32(chunk->value + 36u), rd16(chunk->value + 32u),
        rd16(chunk->value + 34u));
    if (status != CNET_SCTP_ASSOC_OK) return status;
  }
  status = encode_one(s, peer_tag, SCTP_TYPE_COOKIE_ACK,
      NULL, 0u, scratch, sizeof(scratch), &size);
  if (status != CNET_SCTP_ASSOC_OK) return status;
  status = publish_packet(output, cap, written, scratch, size);
  if (status != CNET_SCTP_ASSOC_OK) return status;
  if (may_commit) {
    /* Only the single Owner executes this bounded, no-fail state change.
     * Once the ACK is admitted, no old T1 packet is retained or resent. */
    s->peer_tag = peer_tag;
    s->peer_initial_tsn = peer_tsn;
    s->peer_receive_window = rd32(chunk->value + 36u);
    s->data = candidate;
    s->data_ready = 1u;
    s->peer_outbound_streams = rd16(chunk->value + 32u);
    s->peer_inbound_streams = rd16(chunk->value + 34u);
    s->state = CNET_SCTP_ASSOC_ESTABLISHED;
    s->deadline_ms = 0u;
    s->saved_size = 0u;
    s->retransmits = 0u;
    s->current_rto_ms = 0u;
    memset(s->saved_packet, 0, sizeof(s->saved_packet));
  }
  s->last_time_ms = now;
  return CNET_SCTP_ASSOC_OK;
}
static cnet_sctp_assoc_status handle_cookie_ack(cnet_sctp_assoc *s,
    uint64_t now, const cnet_sctp_wire_header *header,
    const cnet_sctp_wire_chunk *chunk) {
  if (header->verification_tag != s->local_tag ||
      chunk->value_size != 0u) return CNET_SCTP_ASSOC_PROTOCOL;
  /* RFC 9260 §5.2.5: duplicate COOKIE ACK cannot reopen or reset a live
   * association, peer tag, counters or Timer T1. It is a no-op. */
  if (s->state == CNET_SCTP_ASSOC_ESTABLISHED)
    return CNET_SCTP_ASSOC_OK;
  if (s->state != CNET_SCTP_ASSOC_COOKIE_ECHOED)
    return CNET_SCTP_ASSOC_PROTOCOL;
  /* Establish per-Association DATA capacity and TSN starting points from
   * INIT ACK before exposing ESTABLISHED. No alloc/worker/second Owner. */
  if (!s->data_ready) {
    cnet_sctp_assoc_status prepared = data_prepare(s, &s->data, now,
        s->peer_tag, s->peer_initial_tsn, s->peer_receive_window,
        s->peer_outbound_streams, s->peer_inbound_streams);
    if (prepared != CNET_SCTP_ASSOC_OK) return prepared;
    s->data_ready = 1u;
  }
  s->state = CNET_SCTP_ASSOC_ESTABLISHED;
  s->saved_size = 0u;
  memset(s->saved_packet, 0, sizeof(s->saved_packet));
  s->deadline_ms = 0u;
  s->last_time_ms = now;
  return CNET_SCTP_ASSOC_OK;
}

/* T2 uses the existing bounded Owner clock/RTO envelope.  This is
 * a private one-path shutdown subset, not a second scheduler/timer thread. */
static cnet_sctp_assoc_status begin_shutdown_control(cnet_sctp_assoc *s,
    uint64_t now, int acknowledgement, uint8_t *out, size_t cap,
    size_t *written) {
  uint8_t scratch[CNET_SCTP_ASSOC_MAX_PACKET];
  uint8_t value[4] = {0};
  size_t n = 0u;
  cnet_sctp_assoc_status status;
  if (now > UINT64_MAX - s->config.t1_ms ||
      (!acknowledgement && s->config.t5_shutdown_guard_ms != 0u &&
       now > UINT64_MAX - s->config.t5_shutdown_guard_ms))
    return CNET_SCTP_ASSOC_CLOCK;
  if (!acknowledgement) wr32(value, s->data.cumulative_rx);
  status = encode_one(s, s->peer_tag,
      acknowledgement ? SCTP_TYPE_SHUTDOWN_ACK : SCTP_TYPE_SHUTDOWN,
      acknowledgement ? NULL : value, acknowledgement ? 0u : sizeof(value),
      scratch, sizeof(scratch), &n);
  if (status != CNET_SCTP_ASSOC_OK) return status;
  status = begin_packet(s, now, scratch, n,
      acknowledgement ? CNET_SCTP_ASSOC_SHUTDOWN_ACK_SENT :
          CNET_SCTP_ASSOC_SHUTDOWN_SENT, out, cap, written);
  if (status == CNET_SCTP_ASSOC_OK && !acknowledgement &&
      s->config.t5_shutdown_guard_ms != 0u)
    s->t5_shutdown_deadline_ms = now + s->config.t5_shutdown_guard_ms;
  return status;
}
static void finish_shutdown(cnet_sctp_assoc *s) {
  if (s->data_ready && s->data.buffered_rx == 0u) {
    (void)cnet_sctp_data_stop(&s->data);
    memset(&s->data, 0, sizeof(s->data));
    s->data_ready = 0u;
  }
  s->state = CNET_SCTP_ASSOC_STOPPED;
  s->deadline_ms = 0u;
  s->t5_shutdown_deadline_ms = 0u;
  s->saved_size = 0u;
  s->current_rto_ms = 0u;
  s->retransmits = 0u;
  memset(s->saved_packet, 0, sizeof(s->saved_packet));
}
/* T5 is absolute: both timer advancement and a late inbound packet
 * must observe it before accepting a SHUTDOWN ACK. Output admission is
 * transactional so the Owner can retry ABORT publication with a larger buffer. */
static cnet_sctp_assoc_status shutdown_t5_abort(cnet_sctp_assoc *s,
    uint64_t now, uint8_t *output, size_t cap, size_t *written) {
  uint8_t packet[CNET_SCTP_ASSOC_MAX_PACKET];
  size_t packet_size = 0u;
  cnet_sctp_assoc_status status;
  if (s->state != CNET_SCTP_ASSOC_SHUTDOWN_SENT ||
      s->t5_shutdown_deadline_ms == 0u ||
      now < s->t5_shutdown_deadline_ms)
    return CNET_SCTP_ASSOC_EMPTY;
  status = encode_one(s, s->peer_tag, SCTP_TYPE_ABORT, NULL, 0u,
      packet, sizeof(packet), &packet_size);
  if (status != CNET_SCTP_ASSOC_OK) return status;
  if (packet_size > cap) return CNET_SCTP_ASSOC_CAPACITY;
  memcpy(output, packet, packet_size);
  *written = packet_size;
  s->state = CNET_SCTP_ASSOC_FAILED;
  s->deadline_ms = 0u;
  s->t5_shutdown_deadline_ms = 0u;
  s->saved_size = 0u;
  memset(s->saved_packet, 0, sizeof(s->saved_packet));
  s->last_time_ms = now;
  return CNET_SCTP_ASSOC_OK;
}
/* SHUTDOWN cumulatively acknowledges peer DATA.  Preview all mutation on
 * a bounded stack-owned candidate before emitting SHUTDOWN ACK, so an
 * undersized output buffer cannot half-retire a sender's TX tickets. */
static cnet_sctp_assoc_status handle_shutdown(cnet_sctp_assoc *s,
    uint64_t now, const cnet_sctp_wire_header *header,
    const cnet_sctp_wire_chunk *chunk, uint8_t *out, size_t cap,
    size_t *written) {
  cnet_sctp_data candidate;
  cnet_sctp_assoc_status status;
  if (header->verification_tag != s->local_tag ||
      chunk->value_size != 4u || !s->data_ready)
    return CNET_SCTP_ASSOC_PROTOCOL;
  if (s->state == CNET_SCTP_ASSOC_SHUTDOWN_ACK_SENT) {
    /* RFC 9260 §9.2: a duplicate SHUTDOWN requires retransmitting the
     * saved SHUTDOWN ACK and restarting T2. Publish first so a short output
     * buffer cannot move the timer or consume retry budget. */
    if (now > UINT64_MAX - s->current_rto_ms)
      return CNET_SCTP_ASSOC_CLOCK;
    status = publish_packet(out, cap, written,
        s->saved_packet, s->saved_size);
    if (status != CNET_SCTP_ASSOC_OK) return status;
    s->deadline_ms = now + s->current_rto_ms;
    /* Restart T2 for the repeated SHUTDOWN, but do not reset the ACK
     * retransmission budget: this control duplicate proves no data progress. */
    return CNET_SCTP_ASSOC_OK;
  }
  if (s->state != CNET_SCTP_ASSOC_ESTABLISHED &&
      s->state != CNET_SCTP_ASSOC_SHUTDOWN_PENDING &&
      s->state != CNET_SCTP_ASSOC_SHUTDOWN_RECEIVED &&
      s->state != CNET_SCTP_ASSOC_SHUTDOWN_SENT)
    return CNET_SCTP_ASSOC_PROTOCOL;
  memcpy(&candidate, &s->data, sizeof(candidate));
  status = data_result(cnet_sctp_data_shutdown_cumack(&candidate,
      now, rd32(chunk->value)));
  if (status != CNET_SCTP_ASSOC_OK) return status;
  if (!cnet_sctp_data_outbound_drained(&candidate)) {
    if (s->state == CNET_SCTP_ASSOC_SHUTDOWN_SENT)
      return CNET_SCTP_ASSOC_PROTOCOL;
    memcpy(&s->data, &candidate, sizeof(candidate));
    s->state = CNET_SCTP_ASSOC_SHUTDOWN_RECEIVED;
    s->last_time_ms = now;
    return CNET_SCTP_ASSOC_OK;
  }
  status = begin_shutdown_control(s, now, 1, out, cap, written);
  if (status != CNET_SCTP_ASSOC_OK) return status;
  memcpy(&s->data, &candidate, sizeof(candidate));
  return CNET_SCTP_ASSOC_OK;
}
static cnet_sctp_assoc_status handle_shutdown_ack(cnet_sctp_assoc *s,
    uint64_t now, const cnet_sctp_wire_header *header,
    const cnet_sctp_wire_chunk *chunk, uint8_t *out, size_t cap,
    size_t *written) {
  cnet_sctp_assoc_status status;
  if (header->verification_tag != s->local_tag ||
      chunk->value_size != 0u ||
      (s->state != CNET_SCTP_ASSOC_SHUTDOWN_SENT &&
       s->state != CNET_SCTP_ASSOC_SHUTDOWN_ACK_SENT))
    return CNET_SCTP_ASSOC_PROTOCOL;
  status = encode_one(s, s->peer_tag, SCTP_TYPE_SHUTDOWN_COMPLETE,
      NULL, 0u, out, cap, written);
  if (status != CNET_SCTP_ASSOC_OK) return status;
  finish_shutdown(s);
  s->last_time_ms = now;
  return CNET_SCTP_ASSOC_OK;
}

/* RFC 9260 §9.2: DATA received while SHUTDOWN-SENT requires an
 * immediate SHUTDOWN response and a restarted T2. Stage receive processing,
 * SACK generation, and both encodings before committing any association
 * state so output backpressure cannot consume DATA or alter the retry timer. */
static cnet_sctp_assoc_status handle_shutdown_sent_data(
    cnet_sctp_assoc *s, uint64_t now, const uint8_t *packet,
    size_t packet_size, uint8_t *out, size_t cap, size_t *written) {
  cnet_sctp_data candidate;
  cnet_sctp_wire_header sack_header = {0};
  cnet_sctp_wire_decoder sack_decoder = {0};
  cnet_sctp_wire_chunk sack_chunk = {0}, response_chunks[2];
  uint8_t sack_packet[CNET_SCTP_DATA_MTU];
  uint8_t shutdown_value[4], saved_shutdown[CNET_SCTP_ASSOC_MAX_PACKET];
  uint8_t response[CNET_SCTP_ASSOC_MAX_PACKET];
  size_t sack_size = 0u, saved_size = 0u, response_size = 0u;
  size_t response_count = 1u;
  cnet_sctp_assoc_status status;
  cnet_sctp_wire_status wire_status;
  int peer_data_progress;
  if (!s->data_ready || s->current_rto_ms == 0u ||
      now > UINT64_MAX - s->current_rto_ms)
    return CNET_SCTP_ASSOC_CLOCK;
  memcpy(&candidate, &s->data, sizeof(candidate));
  status = data_result(cnet_sctp_data_input(&candidate, now, packet,
      packet_size, sack_packet, sizeof(sack_packet), &sack_size));
  if (status != CNET_SCTP_ASSOC_OK) return status;
  if (sack_size == 0u ||
      cnet_sctp_wire_decode(sack_packet, sack_size, &sack_header,
          &sack_decoder) != CNET_SCTP_WIRE_OK ||
      sack_header.chunk_count != 1u ||
      cnet_sctp_wire_next_chunk(&sack_decoder, &sack_chunk) !=
          CNET_SCTP_WIRE_OK || sack_chunk.type != 3u ||
      sack_chunk.value_size < 12u)
    return CNET_SCTP_ASSOC_PROTOCOL;
  /* SHUTDOWN cumulatively acknowledges all contiguous DATA. Bundle a SACK
   * only when it carries gap/duplicate evidence that SHUTDOWN cannot convey. */
  if (sack_chunk.value_size > 12u) response_count = 2u;
  wr32(shutdown_value, candidate.cumulative_rx);
  response_chunks[0].type = SCTP_TYPE_SHUTDOWN;
  response_chunks[0].flags = 0u;
  response_chunks[0].value = shutdown_value;
  response_chunks[0].value_size = sizeof(shutdown_value);
  response_chunks[1] = sack_chunk;
  wire_status = cnet_sctp_wire_encode(s->config.local_port,
      s->config.remote_port, s->peer_tag, response_chunks, response_count,
      response, sizeof(response), &response_size);
  if (wire_status != CNET_SCTP_WIRE_OK)
    return wire_status == CNET_SCTP_WIRE_CAPACITY ?
        CNET_SCTP_ASSOC_CAPACITY : CNET_SCTP_ASSOC_PROTOCOL;
  status = encode_one(s, s->peer_tag, SCTP_TYPE_SHUTDOWN,
      shutdown_value, sizeof(shutdown_value), saved_shutdown,
      sizeof(saved_shutdown), &saved_size);
  if (status != CNET_SCTP_ASSOC_OK) return status;
  if (response_size > cap) return CNET_SCTP_ASSOC_CAPACITY;
  /* A syntactically valid duplicate DATA packet restarts T2 and is ACKed,
   * but it is not peer progress and must not replenish the bounded retry
   * budget. New RX bytes, cumulative TSN movement or a changed gap map are
   * evidence that previously unseen DATA was admitted. */
  peer_data_progress = candidate.buffered_rx != s->data.buffered_rx ||
      candidate.cumulative_rx != s->data.cumulative_rx ||
      candidate.rx_gaps != s->data.rx_gaps;
  memcpy(out, response, response_size);
  *written = response_size;
  memcpy(&s->data, &candidate, sizeof(candidate));
  memcpy(s->saved_packet, saved_shutdown, saved_size);
  s->saved_size = saved_size;
  s->deadline_ms = now + s->current_rto_ms;
  if (peer_data_progress) s->retransmits = 0u;
  s->last_time_ms = now;
  return CNET_SCTP_ASSOC_OK;
}

/* A SHUTDOWN response may carry a SACK when gaps/duplicates prevent
 * its cumulative TSN from describing all received DATA (RFC 9260 §9.2).
 * Stage both chunks as one transaction: apply SHUTDOWN first, then admit the
 * trailing SACK only if that transition leaves the association in a state
 * where SACK is legal. */
static cnet_sctp_assoc_status handle_shutdown_sack_bundle(
    cnet_sctp_assoc *s, uint64_t now, const cnet_sctp_wire_header *header,
    const cnet_sctp_wire_chunk *shutdown_chunk,
    const cnet_sctp_wire_chunk *sack_chunk, uint8_t *out, size_t cap,
    size_t *written) {
  cnet_sctp_assoc staged;
  cnet_sctp_wire_chunk sack_only;
  uint8_t sack_packet[CNET_SCTP_DATA_MTU], data_reply[CNET_SCTP_DATA_MTU];
  size_t sack_size = 0u, reply_size = 0u;
  cnet_sctp_wire_status ws;
  cnet_sctp_assoc_status status;
  if (!s->data_ready || sack_chunk->type != 3u ||
      (s->state != CNET_SCTP_ASSOC_ESTABLISHED &&
       s->state != CNET_SCTP_ASSOC_SHUTDOWN_PENDING &&
       s->state != CNET_SCTP_ASSOC_SHUTDOWN_SENT &&
       s->state != CNET_SCTP_ASSOC_SHUTDOWN_RECEIVED))
    return CNET_SCTP_ASSOC_PROTOCOL;
  memcpy(&staged, s, sizeof(staged));
  status = handle_shutdown(&staged, now, header, shutdown_chunk,
      data_reply, sizeof(data_reply), &reply_size);
  if (status != CNET_SCTP_ASSOC_OK) return status;
  /* If SHUTDOWN already drained the association, the trailing SACK is no
   * longer actionable in ACK-SENT. Otherwise the transition must have put
   * us in SHUTDOWN-RECEIVED before SACK processing. */
  if (staged.state == CNET_SCTP_ASSOC_SHUTDOWN_RECEIVED) {
    sack_only = *sack_chunk;
    ws = cnet_sctp_wire_encode(staged.config.remote_port,
        staged.config.local_port, staged.local_tag, &sack_only, 1u,
        sack_packet, sizeof(sack_packet), &sack_size);
    if (ws != CNET_SCTP_WIRE_OK)
      return ws == CNET_SCTP_WIRE_CAPACITY ?
          CNET_SCTP_ASSOC_CAPACITY : CNET_SCTP_ASSOC_PROTOCOL;
    status = data_result(cnet_sctp_data_input(&staged.data, now,
        sack_packet, sack_size, data_reply, sizeof(data_reply), &reply_size));
    if (status != CNET_SCTP_ASSOC_OK) return status;
    if (cnet_sctp_data_outbound_drained(&staged.data)) {
      status = begin_shutdown_control(&staged, now, 1,
          data_reply, sizeof(data_reply), &reply_size);
      if (status != CNET_SCTP_ASSOC_OK) return status;
    }
  }
  if (reply_size > cap) return CNET_SCTP_ASSOC_CAPACITY;
  if (reply_size != 0u) memcpy(out, data_reply, reply_size);
  *written = reply_size;
  memcpy(s, &staged, sizeof(staged));
  return CNET_SCTP_ASSOC_OK;
}

cnet_sctp_assoc_status cnet_sctp_assoc_input(cnet_sctp_assoc *s,
    uint64_t now, const uint8_t *packet, size_t packet_size,
    uint8_t *output, size_t cap, size_t *written) {
  cnet_sctp_wire_header header;
  cnet_sctp_wire_decoder decoder;
  cnet_sctp_wire_decoder preflight_decoder;
  cnet_sctp_wire_decoder classify_decoder;
  cnet_sctp_wire_chunk chunk;
  cnet_sctp_wire_chunk second;
  cnet_sctp_assoc_status status;
  uint64_t prior_time;
  size_t i;
  int all_unknown;
  if (written) *written = 0u;
  status = guard(s);
  if (status != CNET_SCTP_ASSOC_OK) return status;
  if (!packet || !output || !written) return CNET_SCTP_ASSOC_INVALID;
  if (s->state == CNET_SCTP_ASSOC_STOPPED || s->state == CNET_SCTP_ASSOC_FAILED)
    return CNET_SCTP_ASSOC_CLOSED_ERROR;
  if ((status = check_time(s, now)) != CNET_SCTP_ASSOC_OK)
    return status;
  if (cnet_sctp_wire_decode(packet, packet_size, &header, &decoder) !=
      CNET_SCTP_WIRE_OK) return CNET_SCTP_ASSOC_PROTOCOL;
  if (header.source_port != s->config.remote_port ||
      header.destination_port != s->config.local_port)
    return CNET_SCTP_ASSOC_PROTOCOL;
  preflight_decoder = decoder;
  classify_decoder = decoder;
  if (cnet_sctp_wire_next_chunk(&decoder, &chunk) != CNET_SCTP_WIRE_OK)
    return CNET_SCTP_ASSOC_PROTOCOL;
  /* RFC 9260 §8.5 (Verified Erratum 7852): validate the packet Verification
   * Tag before changing association state or processing any chunk. INIT is
   * the sole supported exception and must carry the zero tag. The wire
   * decoder already rejects bundling INIT. For all other chunks this bounded
   * profile accepts only the local tag; unsupported T-bit ABORT variants
   * remain rejected rather than opening a second path. */
  if ((chunk.type == SCTP_TYPE_INIT && header.verification_tag != 0u) ||
      (chunk.type != SCTP_TYPE_INIT &&
       header.verification_tag != s->local_tag))
    return CNET_SCTP_ASSOC_PROTOCOL;
  /* Classify the complete packet before dispatch. Unknown-only bundles
   * have a narrow transactional action handler; packets mixing unknown and
   * recognized types remain fail-closed until the full RFC transaction matrix
   * is implemented. */
  all_unknown = header.chunk_count == 1u && !recognized_chunk_type(chunk.type);
  if (header.chunk_count > 1u) {
    all_unknown = 1;
    for (i = 0u; i < header.chunk_count; ++i) {
      cnet_sctp_wire_chunk candidate;
      if (cnet_sctp_wire_next_chunk(&classify_decoder, &candidate) !=
          CNET_SCTP_WIRE_OK)
        return CNET_SCTP_ASSOC_PROTOCOL;
      if (recognized_chunk_type(candidate.type)) {
        all_unknown = 0;
        break;
      }
    }
  }
  /* The regular bundle preflight accepts single Chunks and the narrow known
   * bundle subset. Unknown-only multi-Chunk packets use their own complete
   * framing/type preflight above. */
  if (header.chunk_count == 1u || !all_unknown) {
    if (!assoc_bundle_preflight(&preflight_decoder, header.chunk_count))
      return CNET_SCTP_ASSOC_PROTOCOL;
  }
  if (!all_unknown) {
    if (header.chunk_count == 2u && chunk.type == SCTP_TYPE_COOKIE_ECHO) {
      /* Validate the entire advisory ERROR before advancing the clock watermark
       * or allowing COOKIE ECHO processing to begin. */
      if (cnet_sctp_wire_next_chunk(&decoder, &second) != CNET_SCTP_WIRE_OK ||
          !valid_cookie_echo_error(&second))
        return CNET_SCTP_ASSOC_PROTOCOL;
    } else if (header.chunk_count == 2u && chunk.type == SCTP_TYPE_SHUTDOWN) {
      if (cnet_sctp_wire_next_chunk(&decoder, &second) != CNET_SCTP_WIRE_OK ||
          !valid_sack_shape(&second))
        return CNET_SCTP_ASSOC_PROTOCOL;
    } else if (chunk.type == 3u && !valid_sack_shape(&chunk)) {
      return CNET_SCTP_ASSOC_PROTOCOL;
    } else if (header.chunk_count > 1u &&
               chunk.type != 0u && chunk.type != 3u) {
      /* No arbitrary control chunks can be bundled with INIT, Cookie,
       * SHUTDOWN or ABORT in the private Association profile. */
      return CNET_SCTP_ASSOC_PROTOCOL;
    }
  }
  /* Unknown Chunk action bits are meaningful only for unrecognized types.
   * Known-but-unimplemented controls (e.g. HEARTBEAT) still take the
   * unsupported-control rejection path and must never be misreported. */
  if (!assoc_packet_shape_preflight(&preflight_decoder,
      header.chunk_count))
    return CNET_SCTP_ASSOC_PROTOCOL;
  /* The limited unknown-only ERROR/SKIP path belongs to a live,
   * post-COOKIE association. During CLOSED/COOKIE-WAIT/COOKIE-ECHOED, an
   * unrecognized packet is not authenticated association traffic and may
   * not emit an ERROR with an unset peer tag or advance the Owner clock.
   * Full RFC 9260 8.4 OOTB/T-bit ABORT handling is a separate open gate;
   * fail closed here rather than fabricating a reflected-tag response. */
  if (all_unknown &&
      (s->peer_tag == 0u ||
       (s->state != CNET_SCTP_ASSOC_ESTABLISHED &&
        s->state != CNET_SCTP_ASSOC_SHUTDOWN_PENDING &&
        s->state != CNET_SCTP_ASSOC_SHUTDOWN_SENT &&
        s->state != CNET_SCTP_ASSOC_SHUTDOWN_RECEIVED &&
        s->state != CNET_SCTP_ASSOC_SHUTDOWN_ACK_SENT)))
    return CNET_SCTP_ASSOC_PROTOCOL;

  /* A packet-triggered T5 action is also a state transition. Run it only
   * after every Chunk in the bounded packet has passed structural preflight;
   * wrong-tag or malformed bundled packets cannot trigger an ABORT. The
   * Owner's advance() path remains the independent deadline service. */
  if (s->state == CNET_SCTP_ASSOC_SHUTDOWN_SENT &&
      s->t5_shutdown_deadline_ms != 0u &&
      now >= s->t5_shutdown_deadline_ms) {
    status = shutdown_t5_abort(s, now, output, cap, written);
    return status == CNET_SCTP_ASSOC_EMPTY ? CNET_SCTP_ASSOC_OK : status;
  }
  /* Validated unknown-only packets are handled after the absolute T5 guard:
   * an unknown Chunk may not postpone a due shutdown ABORT. */
  if (all_unknown && header.chunk_count == 1u)
    return handle_unknown_single_chunk(s, now, &chunk, output, cap, written);
  if (all_unknown && header.chunk_count > 1u)
    return handle_unknown_multi_chunks(s, now, &preflight_decoder,
        header.chunk_count, output, cap, written);
  if (header.chunk_count == 2u && chunk.type == SCTP_TYPE_SHUTDOWN) {
    /* The helper stages the full SHUTDOWN+SACK transaction. Do not commit
     * the packet time before it succeeds: rejected state/data gates must be
     * observationally inert just like the staged TCB and output buffer. */
    return handle_shutdown_sack_bundle(s, now, &header, &chunk, &second,
        output, cap, written);
  }
  /* Framing, ports, tag and supported bundle shape are now preflighted.
   * Authentication and state-specific validation may still reject the packet,
   * but malformed bundle members above must not advance this watermark. */
  prior_time = s->last_time_ms;
  s->last_time_ms = now;
  switch (chunk.type) {
    case 0u: /* DATA */
    case 3u: /* SACK */
      if (!s->data_ready ||
          (header.chunk_count != 1u &&
           s->state != CNET_SCTP_ASSOC_ESTABLISHED &&
           s->state != CNET_SCTP_ASSOC_SHUTDOWN_PENDING &&
           s->state != CNET_SCTP_ASSOC_SHUTDOWN_SENT &&
           s->state != CNET_SCTP_ASSOC_SHUTDOWN_RECEIVED)) {
        s->last_time_ms = prior_time;
        return CNET_SCTP_ASSOC_PROTOCOL;
      }
      if (chunk.type == 0u) {
        if (s->state == CNET_SCTP_ASSOC_SHUTDOWN_SENT) {
          if (header.verification_tag != s->local_tag) {
            s->last_time_ms = prior_time;
            return CNET_SCTP_ASSOC_PROTOCOL;
          }
          return dispatch_result(s, handle_shutdown_sent_data(s, now,
              packet, packet_size, output, cap, written), prior_time);
        }
        if (s->state != CNET_SCTP_ASSOC_ESTABLISHED &&
            s->state != CNET_SCTP_ASSOC_SHUTDOWN_PENDING) {
          s->last_time_ms = prior_time;
          return CNET_SCTP_ASSOC_PROTOCOL;
        }
      } else if (s->state != CNET_SCTP_ASSOC_ESTABLISHED &&
                 s->state != CNET_SCTP_ASSOC_SHUTDOWN_PENDING &&
                 s->state != CNET_SCTP_ASSOC_SHUTDOWN_RECEIVED) {
        s->last_time_ms = prior_time;
        return CNET_SCTP_ASSOC_PROTOCOL;
      }
      return dispatch_result(s, data_result(cnet_sctp_data_input(
          &s->data, now, packet, packet_size, output, cap, written)),
          prior_time);
    case SCTP_TYPE_INIT:
      return dispatch_result(s, handle_init(s, now, &header, &chunk,
          output, cap, written), prior_time);
    case SCTP_TYPE_INIT_ACK:
      return dispatch_result(s, handle_init_ack(s, now, &header, &chunk,
          output, cap, written), prior_time);
    case SCTP_TYPE_COOKIE_ECHO:
      return dispatch_result(s, handle_cookie_echo(s, now, &header, &chunk,
          output, cap, written), prior_time);
    case SCTP_TYPE_ABORT:
      /* This private M2 path handles only a plain ABORT using the
       * expected tag. It never responds to ABORT with another ABORT.
       * Rejected ABORTs must not commit the provisional time watermark. */
      if (header.verification_tag != s->local_tag ||
          (chunk.flags & 1u) != 0u || chunk.value_size != 0u ||
          s->state == CNET_SCTP_ASSOC_CLOSED) {
        s->last_time_ms = prior_time;
        return CNET_SCTP_ASSOC_PROTOCOL;
      }
      if (s->data_ready) {
        (void)cnet_sctp_data_stop(&s->data);
        memset(&s->data, 0, sizeof(s->data));
        s->data_ready = 0u;
      }
      s->state = CNET_SCTP_ASSOC_FAILED;
      s->deadline_ms = 0u;
      s->t5_shutdown_deadline_ms = 0u;
      s->saved_size = 0u;
      memset(s->saved_packet, 0, sizeof(s->saved_packet));
      return CNET_SCTP_ASSOC_OK;
    case SCTP_TYPE_COOKIE_ACK:
      return dispatch_result(s, handle_cookie_ack(s, now, &header, &chunk),
          prior_time);
    case SCTP_TYPE_SHUTDOWN:
      if (header.chunk_count != 1u) {
        s->last_time_ms = prior_time;
        return CNET_SCTP_ASSOC_PROTOCOL;
      }
      return dispatch_result(s, handle_shutdown(s, now, &header, &chunk,
          output, cap, written), prior_time);
    case SCTP_TYPE_SHUTDOWN_ACK:
      if (header.chunk_count != 1u) {
        s->last_time_ms = prior_time;
        return CNET_SCTP_ASSOC_PROTOCOL;
      }
      return dispatch_result(s, handle_shutdown_ack(s, now, &header, &chunk,
          output, cap, written), prior_time);
    case SCTP_TYPE_SHUTDOWN_COMPLETE:
      if (header.chunk_count != 1u ||
          header.verification_tag != s->local_tag ||
          (chunk.flags & 1u) != 0u || chunk.value_size != 0u ||
          s->state != CNET_SCTP_ASSOC_SHUTDOWN_ACK_SENT) {
        s->last_time_ms = prior_time;
        return CNET_SCTP_ASSOC_PROTOCOL;
      }
      finish_shutdown(s);
      s->last_time_ms = now;
      return CNET_SCTP_ASSOC_OK;
    default:
      /* Only recognized-but-unimplemented controls reach this switch.
       * Bounded unknown-only packets are handled before dispatch; mixed
       * unknown/recognized bundles remain fail-closed in packet preflight.
       * Roll back the provisional watermark for this unsupported-control
       * rejection so it remains observationally inert. */
      s->last_time_ms = prior_time;
      return CNET_SCTP_ASSOC_PROTOCOL;
  }
}

cnet_sctp_assoc_status cnet_sctp_assoc_advance(cnet_sctp_assoc *s,
    uint64_t now, uint8_t *output, size_t cap, size_t *written) {
  cnet_sctp_assoc_status status;
  uint64_t next_rto;
  if (written) *written = 0u;
  status = guard(s);
  if (status != CNET_SCTP_ASSOC_OK) return status;
  if (!output || !written) return CNET_SCTP_ASSOC_INVALID;
  if ((status = check_time(s, now)) != CNET_SCTP_ASSOC_OK) return status;
  if (s->state == CNET_SCTP_ASSOC_STOPPED || s->state == CNET_SCTP_ASSOC_FAILED)
    return CNET_SCTP_ASSOC_CLOSED_ERROR;
  if ((s->state == CNET_SCTP_ASSOC_SHUTDOWN_PENDING ||
       s->state == CNET_SCTP_ASSOC_SHUTDOWN_RECEIVED) &&
      s->data_ready && cnet_sctp_data_outbound_drained(&s->data)) {
    return begin_shutdown_control(s, now,
        s->state == CNET_SCTP_ASSOC_SHUTDOWN_RECEIVED,
        output, cap, written);
  }
  if ((s->state == CNET_SCTP_ASSOC_ESTABLISHED ||
       s->state == CNET_SCTP_ASSOC_SHUTDOWN_PENDING ||
       s->state == CNET_SCTP_ASSOC_SHUTDOWN_RECEIVED) && s->data_ready) {
    cnet_sctp_assoc_status mapped = data_result(
        cnet_sctp_data_advance(&s->data, now, output, cap, written));
    if (mapped == CNET_SCTP_ASSOC_EXPIRED) s->state = CNET_SCTP_ASSOC_FAILED;
    if (mapped == CNET_SCTP_ASSOC_OK ||
        mapped == CNET_SCTP_ASSOC_EXPIRED) s->last_time_ms = now;
    return mapped;
  }
  if (s->state != CNET_SCTP_ASSOC_COOKIE_WAIT &&
      s->state != CNET_SCTP_ASSOC_COOKIE_ECHOED &&
      s->state != CNET_SCTP_ASSOC_SHUTDOWN_SENT &&
      s->state != CNET_SCTP_ASSOC_SHUTDOWN_ACK_SENT) {
    s->last_time_ms = now;
    return CNET_SCTP_ASSOC_OK;
  }
  /* T5 must preempt T2 when the absolute shutdown budget expires. */
  if (s->state == CNET_SCTP_ASSOC_SHUTDOWN_SENT &&
      s->t5_shutdown_deadline_ms != 0u &&
      now >= s->t5_shutdown_deadline_ms) {
    status = shutdown_t5_abort(s, now, output, cap, written);
    return status == CNET_SCTP_ASSOC_EMPTY ? CNET_SCTP_ASSOC_OK : status;
  }
  if (now < s->deadline_ms) {
    s->last_time_ms = now;
    return CNET_SCTP_ASSOC_OK;
  }
  if (s->retransmits >= s->config.max_retransmits) {
    s->state = CNET_SCTP_ASSOC_FAILED;
    s->deadline_ms = 0u;
    s->t5_shutdown_deadline_ms = 0u;
    s->saved_size = 0u;
    memset(s->saved_packet, 0, sizeof(s->saved_packet));
    s->last_time_ms = now;
    return CNET_SCTP_ASSOC_EXPIRED;
  }
  next_rto = s->current_rto_ms;
  if (next_rto > s->config.t1_max_ms - next_rto)
    next_rto = s->config.t1_max_ms;
  else
    next_rto *= 2u;
  if (next_rto > s->config.t1_max_ms) next_rto = s->config.t1_max_ms;
  if (now > UINT64_MAX - next_rto) return CNET_SCTP_ASSOC_CLOCK;
  if (s->state == CNET_SCTP_ASSOC_SHUTDOWN_SENT) {
    /* RFC 9260 9.2: a repeated SHUTDOWN carries the latest cumulatively
     * received peer TSN, not the stale value saved when T2 was started.
     * Encode into local scratch before output-capacity admission. */
    uint8_t updated[CNET_SCTP_ASSOC_MAX_PACKET];
    uint8_t value[4];
    size_t packet_size = 0u;
    cnet_sctp_assoc_status result;
    wr32(value, s->data.cumulative_rx);
    result = encode_one(s, s->peer_tag, SCTP_TYPE_SHUTDOWN,
        value, sizeof(value), updated, sizeof(updated), &packet_size);
    if (result != CNET_SCTP_ASSOC_OK) return result;
    if (packet_size > cap) return CNET_SCTP_ASSOC_CAPACITY;
    memcpy(output, updated, packet_size);
    *written = packet_size;
    memcpy(s->saved_packet, updated, packet_size);
    s->saved_size = packet_size;
  } else {
    if (s->saved_size > cap) return CNET_SCTP_ASSOC_CAPACITY;
    memcpy(output, s->saved_packet, s->saved_size);
    *written = s->saved_size;
  }
  s->current_rto_ms = next_rto;
  s->deadline_ms = now + next_rto;
  ++s->retransmits;
  s->last_time_ms = now;
  return CNET_SCTP_ASSOC_OK;
}

cnet_sctp_assoc_status cnet_sctp_assoc_next_deadline(cnet_sctp_assoc *s,
    uint64_t *deadline_ms, int *has_deadline) {
  cnet_sctp_assoc_status status = guard(s);
  if (status != CNET_SCTP_ASSOC_OK) return status;
  if (!deadline_ms || !has_deadline) return CNET_SCTP_ASSOC_INVALID;
  if ((s->state == CNET_SCTP_ASSOC_SHUTDOWN_PENDING ||
       s->state == CNET_SCTP_ASSOC_SHUTDOWN_RECEIVED) &&
      s->data_ready && cnet_sctp_data_outbound_drained(&s->data)) {
    *has_deadline = 1;
    *deadline_ms = s->last_time_ms;
    return CNET_SCTP_ASSOC_OK;
  }
  if ((s->state == CNET_SCTP_ASSOC_ESTABLISHED ||
       s->state == CNET_SCTP_ASSOC_SHUTDOWN_PENDING ||
       s->state == CNET_SCTP_ASSOC_SHUTDOWN_RECEIVED) && s->data_ready)
    return data_result(cnet_sctp_data_deadline(&s->data,
                                              deadline_ms, has_deadline));
  *has_deadline = s->state == CNET_SCTP_ASSOC_COOKIE_WAIT ||
      s->state == CNET_SCTP_ASSOC_COOKIE_ECHOED ||
      s->state == CNET_SCTP_ASSOC_SHUTDOWN_SENT ||
      s->state == CNET_SCTP_ASSOC_SHUTDOWN_ACK_SENT;
  *deadline_ms = *has_deadline ? s->deadline_ms : 0u;
  if (s->state == CNET_SCTP_ASSOC_SHUTDOWN_SENT &&
      s->t5_shutdown_deadline_ms != 0u &&
      (!*has_deadline || s->t5_shutdown_deadline_ms < *deadline_ms)) {
    *has_deadline = 1;
    *deadline_ms = s->t5_shutdown_deadline_ms;
  }
  return CNET_SCTP_ASSOC_OK;
}

cnet_sctp_assoc_status cnet_sctp_assoc_send_message(cnet_sctp_assoc *s,
    uint64_t now, uint16_t stream_id, uint32_t ppid, int unordered,
    const void *payload, size_t n, void *packet, size_t cap,
    size_t *written) {
  cnet_sctp_assoc_status status = guard(s);
  if (status != CNET_SCTP_ASSOC_OK) return status;
  if (!payload || !packet || !written) return CNET_SCTP_ASSOC_INVALID;
  *written = 0u;
  if (s->state != CNET_SCTP_ASSOC_ESTABLISHED || !s->data_ready)
    return CNET_SCTP_ASSOC_CLOSED_ERROR;
  if ((status = check_time(s, now)) != CNET_SCTP_ASSOC_OK) return status;
  status = data_result(cnet_sctp_data_send(&s->data, now, stream_id,
      ppid, unordered, payload, n, packet, cap, written));
  if (status == CNET_SCTP_ASSOC_OK) s->last_time_ms = now;
  return status;
}

cnet_sctp_assoc_status cnet_sctp_assoc_take_message(cnet_sctp_assoc *s,
    cnet_sctp_data_message *message, void *payload,
    size_t cap, size_t *written) {
  cnet_sctp_assoc_status status = guard(s);
  if (status != CNET_SCTP_ASSOC_OK) return status;
  if (!message || !payload || !written) return CNET_SCTP_ASSOC_INVALID;
  *written = 0u;
  if ((s->state != CNET_SCTP_ASSOC_ESTABLISHED &&
       s->state != CNET_SCTP_ASSOC_SHUTDOWN_PENDING &&
       s->state != CNET_SCTP_ASSOC_SHUTDOWN_SENT &&
       s->state != CNET_SCTP_ASSOC_SHUTDOWN_RECEIVED &&
       s->state != CNET_SCTP_ASSOC_STOPPED) || !s->data_ready)
    return CNET_SCTP_ASSOC_CLOSED_ERROR;
  status = data_result(cnet_sctp_data_take(&s->data, message,
      payload, cap, written));
  if (status == CNET_SCTP_ASSOC_OK &&
      s->state == CNET_SCTP_ASSOC_STOPPED &&
      s->data.buffered_rx == 0u) {
    (void)cnet_sctp_data_stop(&s->data);
    memset(&s->data, 0, sizeof(s->data));
    s->data_ready = 0u;
  }
  return status;
}

cnet_sctp_assoc_status cnet_sctp_assoc_send_bundle(cnet_sctp_assoc *s,
    uint64_t now, const cnet_sctp_data_bundle_item *items, size_t count,
    void *out, size_t cap, size_t *written) {
  cnet_sctp_assoc_status status = guard(s);
  if (status != CNET_SCTP_ASSOC_OK) return status;
  if (!items || !out || !written) return CNET_SCTP_ASSOC_INVALID;
  *written = 0u;
  if (s->state != CNET_SCTP_ASSOC_ESTABLISHED || !s->data_ready)
    return CNET_SCTP_ASSOC_CLOSED_ERROR;
  if ((status = check_time(s, now)) != CNET_SCTP_ASSOC_OK) return status;
  status = data_result(cnet_sctp_data_send_bundle(
      &s->data, now, items, count, out, cap, written));
  if (status == CNET_SCTP_ASSOC_OK) s->last_time_ms = now;
  return status;
}

cnet_sctp_assoc_status cnet_sctp_assoc_queue_message(cnet_sctp_assoc *s,
    uint64_t now, uint16_t stream, uint32_t ppid, int unordered,
    const void *payload, size_t n) {
  cnet_sctp_assoc_status status = guard(s);
  if (status != CNET_SCTP_ASSOC_OK) return status;
  if (!payload) return CNET_SCTP_ASSOC_INVALID;
  if (s->state != CNET_SCTP_ASSOC_ESTABLISHED || !s->data_ready)
    return CNET_SCTP_ASSOC_CLOSED_ERROR;
  if ((status = check_time(s, now)) != CNET_SCTP_ASSOC_OK) return status;
  status = data_result(cnet_sctp_data_queue_message(&s->data, now,
      stream, ppid, unordered, payload, n));
  if (status == CNET_SCTP_ASSOC_OK) s->last_time_ms = now;
  return status;
}

cnet_sctp_assoc_status cnet_sctp_assoc_pump_message(cnet_sctp_assoc *s,
    uint64_t now, void *out, size_t cap, size_t *written) {
  cnet_sctp_assoc_status status = guard(s);
  if (status != CNET_SCTP_ASSOC_OK) return status;
  if (!out || !written) return CNET_SCTP_ASSOC_INVALID;
  *written = 0u;
  if ((s->state != CNET_SCTP_ASSOC_ESTABLISHED &&
       s->state != CNET_SCTP_ASSOC_SHUTDOWN_PENDING &&
       s->state != CNET_SCTP_ASSOC_SHUTDOWN_RECEIVED) || !s->data_ready)
    return CNET_SCTP_ASSOC_CLOSED_ERROR;
  if ((status = check_time(s, now)) != CNET_SCTP_ASSOC_OK) return status;
  status = data_result(cnet_sctp_data_pump(&s->data, now, out, cap, written));
  if (status == CNET_SCTP_ASSOC_OK) s->last_time_ms = now;
  return status;
}

/* Initiate a graceful close only after the old TX tickets and pending
 * copied message drain. No new application data is admitted afterward. */
cnet_sctp_assoc_status cnet_sctp_assoc_shutdown(cnet_sctp_assoc *s,
    uint64_t now, void *out, size_t cap, size_t *written) {
  cnet_sctp_assoc_status status = guard(s);
  if (status != CNET_SCTP_ASSOC_OK) return status;
  if (!out || !written) return CNET_SCTP_ASSOC_INVALID;
  *written = 0u;
  if (s->state != CNET_SCTP_ASSOC_ESTABLISHED || !s->data_ready)
    return CNET_SCTP_ASSOC_CLOSED_ERROR;
  if ((status = check_time(s, now)) != CNET_SCTP_ASSOC_OK) return status;
  if (!cnet_sctp_data_outbound_drained(&s->data)) {
    s->state = CNET_SCTP_ASSOC_SHUTDOWN_PENDING;
    s->last_time_ms = now;
    return CNET_SCTP_ASSOC_OK;
  }
  return begin_shutdown_control(s, now, 0, out, cap, written);
}

cnet_sctp_assoc_status cnet_sctp_assoc_window_update(cnet_sctp_assoc *s,
    uint64_t now, void *out, size_t cap, size_t *written) {
  cnet_sctp_assoc_status status = guard(s);
  if (status != CNET_SCTP_ASSOC_OK) return status;
  if (!out || !written) return CNET_SCTP_ASSOC_INVALID;
  *written = 0u;
  if ((s->state != CNET_SCTP_ASSOC_ESTABLISHED &&
       s->state != CNET_SCTP_ASSOC_SHUTDOWN_PENDING &&
       s->state != CNET_SCTP_ASSOC_SHUTDOWN_SENT &&
       s->state != CNET_SCTP_ASSOC_SHUTDOWN_RECEIVED) || !s->data_ready)
    return CNET_SCTP_ASSOC_CLOSED_ERROR;
  if ((status = check_time(s, now)) != CNET_SCTP_ASSOC_OK) return status;
  status = data_result(cnet_sctp_data_window_update(
      &s->data, now, out, cap, written));
  if (status == CNET_SCTP_ASSOC_OK) s->last_time_ms = now;
  return status;
}

cnet_sctp_assoc_status cnet_sctp_assoc_stop(cnet_sctp_assoc *s) {
  cnet_sctp_assoc_status status = guard(s);
  if (status != CNET_SCTP_ASSOC_OK) return status;
  if (s->data_ready) {
    (void)cnet_sctp_data_stop(&s->data);
    memset(&s->data, 0, sizeof(s->data));
    s->data_ready = 0u;
  }
  s->state = CNET_SCTP_ASSOC_STOPPED;
  s->deadline_ms = 0u;
  s->t5_shutdown_deadline_ms = 0u;
  s->saved_size = 0u;
  memset(s->saved_packet, 0, sizeof(s->saved_packet));
  return CNET_SCTP_ASSOC_OK;
}

cnet_sctp_assoc_status cnet_sctp_assoc_destroy(cnet_sctp_assoc *s) {
  cnet_sctp_assoc_status status = guard(s);
  if (status != CNET_SCTP_ASSOC_OK) return status;
  if (s->state != CNET_SCTP_ASSOC_STOPPED || s->data_ready)
    return CNET_SCTP_ASSOC_BUSY;
  memset(s, 0, sizeof(*s));
  return CNET_SCTP_ASSOC_OK;
}

cnet_sctp_assoc_state cnet_sctp_assoc_state_get(const cnet_sctp_assoc *s) {
  return s && s->magic == SCTP_ASSOC_MAGIC ? s->state : CNET_SCTP_ASSOC_STOPPED;
}

int cnet_sctp_assoc_drained(const cnet_sctp_assoc *s) {
  return s && s->magic == SCTP_ASSOC_MAGIC &&
      s->state == CNET_SCTP_ASSOC_STOPPED &&
      !s->data_ready && !s->in_progress;
}
