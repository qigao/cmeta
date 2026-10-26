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
  SCTP_TYPE_RECONFIG = 130,
  SCTP_TYPE_FORWARD_TSN = 192,
  SCTP_TYPE_I_DATA = 64,
  SCTP_TYPE_I_FORWARD_TSN = 194,
  SCTP_PARAM_COOKIE = 7,
  SCTP_PARAM_UNRECOGNIZED = 8,
  SCTP_PARAM_SUPPORTED_EXTENSIONS = 0x8008,
  SCTP_PARAM_FORWARD_TSN_SUPPORTED = 0xC000,
  SCTP_ERROR_UNRECOGNIZED = 8,
  SCTP_PARAM_OUTGOING_RESET = 13,
  SCTP_PARAM_INCOMING_RESET = 14,
  SCTP_PARAM_SSN_TSN_RESET = 15,
  SCTP_PARAM_RECONFIG_RESPONSE = 16,
  SCTP_PARAM_ADD_OUTGOING = 17,
  SCTP_PARAM_ADD_INCOMING = 18,
  SCTP_INIT_VALUE_BYTES = 28
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
    uint16_t peer_outbound, uint16_t peer_inbound,
    int peer_forward_tsn_supported, int peer_interleaving_supported,
    int peer_i_forward_tsn_supported) {
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
  cfg.interleaving_supported = peer_interleaving_supported ? 1u : 0u;
  cfg.forward_tsn_supported = peer_forward_tsn_supported &&
      (!peer_interleaving_supported || peer_i_forward_tsn_supported) ? 1u : 0u;
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

static void prepare_init(const cnet_sctp_assoc *s,
    uint8_t value[SCTP_INIT_VALUE_BYTES]) {
  memset(value, 0, SCTP_INIT_VALUE_BYTES);
  wr32(value, s->local_tag);
  wr32(value + 4u, s->config.receive_window);
  wr16(value + 8u, s->config.outbound_streams);
  wr16(value + 10u, s->config.inbound_streams);
  wr32(value + 12u, s->initial_tsn);
  /* RFC 6525 5.1.1 / RFC 5061 4.2.7. Length excludes the three bytes
   * of zero padding needed to align the next INIT parameter. */
  wr16(value + 16u, SCTP_PARAM_SUPPORTED_EXTENSIONS);
  wr16(value + 18u, 7u);
  value[20u] = SCTP_TYPE_RECONFIG;
  value[21u] = SCTP_TYPE_I_DATA;
  value[22u] = SCTP_TYPE_I_FORWARD_TSN;
  /* RFC 3758 3.1 negotiates FORWARD-TSN with a dedicated empty parameter,
   * not through the RFC 5061 Supported Extensions chunk-type list. */
  wr16(value + 24u, SCTP_PARAM_FORWARD_TSN_SUPPORTED);
  wr16(value + 26u, 4u);
}
static int good_init_fields(const uint8_t *v, size_t len) {
  return len >= 16u && rd32(v) != 0u && rd32(v + 4u) >= 1500u &&
         rd16(v + 8u) != 0u && rd16(v + 10u) != 0u;
}

/* Basic syntax preflight for the chunk subset this Association understands.
 * T5 is an absolute deadline, but malformed supported Chunks must not use it
 * as a side effect before their own parser would reject them. */
/* ERROR requires one or more causes; ABORT can omit them entirely.
 * Both use the RFC 9260 variable-length Cause TLV framing. A final Cause
 * need not carry its own padding inside the Chunk Length (§3.2.1/§3.2).
 * Invalid cause lengths cannot be used to trigger packet-path T5. */
static int valid_cause_vector(const cnet_sctp_wire_chunk *chunk) {
  size_t pos = 0u;
  if (!chunk ||
      (chunk->type != SCTP_TYPE_ERROR && chunk->type != SCTP_TYPE_ABORT) ||
      (chunk->type == SCTP_TYPE_ERROR && chunk->value_size < 4u))
    return 0;
  while (pos < chunk->value_size) {
    size_t len, padded, remaining = chunk->value_size - pos;
    if (remaining < 4u) return 0;
    len = rd16(chunk->value + pos + 2u);
    if (len < 4u || len > remaining) return 0;
    padded = (len + 3u) & ~(size_t)3u;
    if (padded > remaining) {
      if (len != remaining) return 0;
      padded = len; /* final Cause's padding omitted from Chunk Length */
    }
    pos += padded;
  }
  return pos == chunk->value_size;
}

typedef struct sctp_reconfig_parameter {
  uint16_t type;
  const uint8_t *bytes; /* complete borrowed TLV, including 4-byte header */
  size_t size;          /* unpadded TLV length */
} sctp_reconfig_parameter;

typedef struct sctp_reconfig_view {
  sctp_reconfig_parameter parameters[2];
  size_t count;
} sctp_reconfig_view;

static int reconfig_parameter_shape(uint16_t type, size_t length) {
  switch (type) {
    case SCTP_PARAM_OUTGOING_RESET:
      return length >= 16u && ((length - 16u) & 1u) == 0u;
    case SCTP_PARAM_INCOMING_RESET:
      return length >= 8u && ((length - 8u) & 1u) == 0u;
    case SCTP_PARAM_SSN_TSN_RESET:
      return length == 8u;
    case SCTP_PARAM_RECONFIG_RESPONSE:
      return length == 12u || length == 20u;
    case SCTP_PARAM_ADD_OUTGOING:
    case SCTP_PARAM_ADD_INCOMING:
      return length == 12u;
    default:
      return 0;
  }
}

/* RFC 6525 3.1: exactly one or two well-framed parameters, with only the
 * ten combinations listed by the RFC. This is syntax preflight only; later
 * dispatch applies the bounded subset's state and collision rules. */
static int reconfig_preflight(const cnet_sctp_wire_chunk *chunk,
    sctp_reconfig_view *view) {
  sctp_reconfig_view parsed = {0};
  size_t position = 0u;
  if (!chunk || !view || chunk->type != SCTP_TYPE_RECONFIG)
    return 0;
  while (position < chunk->value_size) {
    const size_t remaining = chunk->value_size - position;
    uint16_t type;
    size_t length, padded;
    if (parsed.count == 2u || remaining < 4u) return 0;
    type = rd16(chunk->value + position);
    length = rd16(chunk->value + position + 2u);
    if (length < 4u || length > remaining ||
        !reconfig_parameter_shape(type, length)) return 0;
    padded = (length + 3u) & ~(size_t)3u;
    if (padded > remaining) {
      if (length != remaining) return 0;
      padded = length;
    }
    parsed.parameters[parsed.count].type = type;
    parsed.parameters[parsed.count].bytes = chunk->value + position;
    parsed.parameters[parsed.count].size = length;
    ++parsed.count;
    position += padded;
  }
  if (parsed.count == 0u) return 0;
  if (parsed.count == 2u) {
    const uint16_t first = parsed.parameters[0].type;
    const uint16_t second = parsed.parameters[1].type;
    if (!((first == SCTP_PARAM_OUTGOING_RESET &&
           second == SCTP_PARAM_INCOMING_RESET) ||
          (first == SCTP_PARAM_ADD_OUTGOING &&
           second == SCTP_PARAM_ADD_INCOMING) ||
          (first == SCTP_PARAM_RECONFIG_RESPONSE &&
           second == SCTP_PARAM_OUTGOING_RESET) ||
          (first == SCTP_PARAM_RECONFIG_RESPONSE &&
           second == SCTP_PARAM_RECONFIG_RESPONSE)))
      return 0;
  }
  *view = parsed;
  return 1;
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
    case SCTP_TYPE_I_DATA:
      /* RFC 8260 I-DATA has a 16-byte value header and non-empty user data. */
      return chunk->value_size >= 17u;
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
    case SCTP_TYPE_FORWARD_TSN:
      return chunk->value_size >= 4u &&
          ((chunk->value_size - 4u) & 3u) == 0u;
    case SCTP_TYPE_I_FORWARD_TSN:
      return chunk->value_size >= 4u &&
          (chunk->value_size - 4u) % 8u == 0u;
    case SCTP_TYPE_ABORT:
      /* The T bit is verified with the packet Verification Tag. An ABORT
       * may contain any well-framed number of Error Cause TLVs. */
      return valid_cause_vector(chunk);
    case SCTP_TYPE_SHUTDOWN:
      return chunk->value_size == 4u;
    case SCTP_TYPE_SHUTDOWN_ACK:
    case SCTP_TYPE_COOKIE_ACK:
      return chunk->value_size == 0u;
    case SCTP_TYPE_SHUTDOWN_COMPLETE:
      /* T=1 remains outside the bounded profile; other bits ignored. */
      return (chunk->flags & 1u) == 0u && chunk->value_size == 0u;
    case SCTP_TYPE_ERROR:
      return valid_cause_vector(chunk);
    case SCTP_TYPE_RECONFIG: {
      sctp_reconfig_view reconfig;
      return reconfig_preflight(chunk, &reconfig);
    }
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

static int packet_violates_interleaving_mode(
    const cnet_sctp_wire_decoder *decoded, size_t count,
    int interleaving) {
  cnet_sctp_wire_decoder cursor;
  size_t i;
  if (!decoded) return 0;
  cursor = *decoded;
  for (i = 0u; i < count; ++i) {
    cnet_sctp_wire_chunk current;
    if (cnet_sctp_wire_next_chunk(&cursor, &current) != CNET_SCTP_WIRE_OK)
      return 0; /* framing is rejected by the ordinary preflight */
    if ((interleaving &&
         (current.type == SCTP_TYPE_FORWARD_TSN || current.type == 0u)) ||
        (!interleaving &&
         (current.type == SCTP_TYPE_I_FORWARD_TSN ||
          current.type == SCTP_TYPE_I_DATA)))
      return 1;
  }
  return 0;
}
/* HEARTBEAT and HEARTBEAT ACK carry exactly one Heartbeat Info TLV.
 * The final TLV may omit its own padding from Chunk Length. This validator is
 * used only before a terminal ABORT; it does not enable ordinary HB dispatch. */
static int valid_heartbeat_abort_prefix(const cnet_sctp_wire_chunk *chunk) {
  size_t length, padded;
  if (!chunk || (chunk->type != 4u && chunk->type != 5u) ||
      chunk->value_size < 4u || rd16(chunk->value) != 1u)
    return 0;
  length = rd16(chunk->value + 2u);
  if (length < 4u || length > chunk->value_size) return 0;
  padded = (length + 3u) & ~(size_t)3u;
  if (padded > chunk->value_size) {
    if (length != chunk->value_size) return 0;
    padded = length;
  }
  return padded == chunk->value_size;
}

/* RFC 9260 §§3.3.10.12/9.1/11.2.5: project one owned terminal event.
 * The private 1200-byte packet ceiling bounds the entire Cause 12 reason
 * (including a zero-length one). The preflight has checked all TLV lengths. */
static void record_peer_abort(cnet_sctp_assoc *s,
    const cnet_sctp_wire_chunk *abort_chunk) {
  cnet_sctp_assoc_peer_abort_info info = {0};
  size_t pos = 0u;
  memset(s->peer_abort_reason, 0, sizeof(s->peer_abort_reason));
  s->peer_abort_reason_size = 0u;
  if (s->data_ready) {
    info.has_tsn = 1u;
    info.last_acked_tsn = s->data.last_ack;
    info.last_sent_tsn = s->data.next_tsn - 1u;
  }
  while (pos < abort_chunk->value_size) {
    const uint8_t *cause = abort_chunk->value + pos;
    const size_t remaining = abort_chunk->value_size - pos;
    const size_t length = rd16(cause + 2u);
    const size_t padded = (length + 3u) & ~(size_t)3u;
    if (rd16(cause) == 12u && !info.has_user_reason) {
      const size_t reason_size = length - 4u;
      /* Impossible for a fully admitted <=1200-byte wire packet to exceed
       * this bound. Never truncate an authenticated ULP reason. */
      if (reason_size <= sizeof(s->peer_abort_reason)) {
        if (reason_size != 0u)
          memcpy(s->peer_abort_reason, cause + 4u, reason_size);
        s->peer_abort_reason_size = reason_size;
        info.has_user_reason = 1u;
      }
    }
    pos += padded > remaining ? remaining : padded;
  }
  s->peer_abort_info = info;
  s->peer_abort_pending = 1u;
}

/* Control chunks preceding ABORT are ordered before that terminal
 * transition (RFC 9260 §3.3.7). Preview valid pre-ABORT SACKs using the
 * existing DATA engine on an owned candidate: only the cumulative-ACK TSN
 * is projected into the ULP event. The live DATA TCB will be destroyed by
 * ABORT, so ACK settlement, congestion/timer bookkeeping and any outgoing
 * packet MUST NOT be committed or emitted by this preview. Semantically
 * inadmissible SACKs are ignored here, not allowed to block a valid ABORT. */
static uint32_t preview_abort_prefix_last_ack(const cnet_sctp_assoc *s,
    uint64_t now, const cnet_sctp_wire_decoder *decoded, size_t abort_index) {
  cnet_sctp_data candidate;
  cnet_sctp_wire_decoder cursor;
  uint8_t sack_packet[CNET_SCTP_ASSOC_MAX_PACKET];
  uint8_t ignored_response[1] = {0xCCu};
  size_t i;
  if (!s->data_ready || !decoded || abort_index == 0u)
    return s->data.last_ack;
  memcpy(&candidate, &s->data, sizeof(candidate));
  cursor = *decoded;
  for (i = 0u; i < abort_index; ++i) {
    cnet_sctp_wire_chunk prefix;
    size_t packet_size = 0u, response_size = 0u;
    if (cnet_sctp_wire_next_chunk(&cursor, &prefix) != CNET_SCTP_WIRE_OK)
      break; /* Entire packet framing was validated by Wire already. */
    if (prefix.type != 3u) continue;
    if (cnet_sctp_wire_encode(s->config.remote_port, s->config.local_port,
        s->local_tag, &prefix, 1u, sack_packet, sizeof(sack_packet),
        &packet_size) != CNET_SCTP_WIRE_OK)
      continue;
    (void)cnet_sctp_data_input(&candidate, now, sack_packet, packet_size,
        ignored_response, sizeof(ignored_response), &response_size);
    /* input_sack is atomic on non-OK; no real TCB is touched. */
  }
  return candidate.last_ack;
}

/* ABORT is terminal. Validate every control before the first ABORT, then
 * ignore later Chunk semantics as required by RFC 9260 Section 3.3.7.
 * Wire decoding has already checked framing and the DATA+ABORT prohibition. */
static int assoc_abort_bundle_preflight(
    const cnet_sctp_wire_decoder *decoded, size_t count, size_t abort_index) {
  cnet_sctp_wire_decoder cursor;
  size_t i;
  if (!decoded || count == 0u || abort_index >= count) return 0;
  cursor = *decoded;
  for (i = 0u; i <= abort_index; ++i) {
    cnet_sctp_wire_chunk chunk;
    if (cnet_sctp_wire_next_chunk(&cursor, &chunk) != CNET_SCTP_WIRE_OK)
      return 0;
    if (i == abort_index)
      return chunk.type == SCTP_TYPE_ABORT && assoc_chunk_shape_valid(&chunk);
    if (chunk.type == 0u || chunk.type == SCTP_TYPE_I_DATA ||
        chunk.type == SCTP_TYPE_INIT ||
        chunk.type == SCTP_TYPE_INIT_ACK ||
        chunk.type == SCTP_TYPE_SHUTDOWN_COMPLETE)
      return 0;
    if (!recognized_chunk_type(chunk.type)) {
      cnet_sctp_wire_unknown_action action =
          cnet_sctp_wire_unknown_chunk_action(chunk.type);
      if (action == CNET_SCTP_WIRE_UNKNOWN_STOP_DISCARD ||
          action == CNET_SCTP_WIRE_UNKNOWN_STOP_REPORT)
        return 0;
      continue;
    }
    if ((chunk.type == 4u || chunk.type == 5u) ?
        !valid_heartbeat_abort_prefix(&chunk) :
        !assoc_chunk_shape_valid(&chunk)) return 0;
  }
  return 0;
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
 * ERROR, SHUTDOWN + SACK, and SACK/ERROR followed by ABORT. Other
 * multi-control ABORT bundles remain fail-closed pending RFC qualification. */
static int assoc_bundle_preflight(const cnet_sctp_wire_decoder *decoded,
    size_t count) {
  cnet_sctp_wire_decoder cursor;
  cnet_sctp_wire_chunk chunk;
  size_t i;
  int data_seen = 0;
  uint8_t data_type = 0u;
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
    /* Two bounded control-before-ABORT combinations. The leading control
     * is validated but does not commit state when ABORT terminates the TCB. */
    if ((first.type == 3u || first.type == SCTP_TYPE_ERROR) &&
        second.type == SCTP_TYPE_ABORT)
      return 1;
    if (first.type == SCTP_TYPE_COOKIE_ECHO)
      return second.type == SCTP_TYPE_ERROR;
    if (first.type == SCTP_TYPE_SHUTDOWN)
      return second.type == 3u;
  }
  for (i = 0u; i < count; ++i) {
    if (cnet_sctp_wire_next_chunk(&cursor, &chunk) != CNET_SCTP_WIRE_OK)
      return 0;
    if (chunk.type == 0u || chunk.type == SCTP_TYPE_I_DATA) {
      if (data_seen && chunk.type != data_type) return 0;
      data_seen = 1;
      data_type = chunk.type;
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
  s->next_reset_request = s->initial_tsn;
  memset(entropy, 0, sizeof(entropy));
  s->last_time_ms = initial_time_ms;
  s->state = CNET_SCTP_ASSOC_CLOSED;
  return CNET_SCTP_ASSOC_OK;
}

cnet_sctp_assoc_status cnet_sctp_assoc_start(cnet_sctp_assoc *s,
    uint64_t now, uint8_t *output, size_t cap, size_t *written) {
  if (written) *written = 0u;
  uint8_t value[SCTP_INIT_VALUE_BYTES];
  uint8_t scratch[CNET_SCTP_ASSOC_MAX_PACKET];
  size_t size = 0u;
  cnet_sctp_assoc_status status = guard(s);
  if (status != CNET_SCTP_ASSOC_OK) return status;
  if (!output || !written) return CNET_SCTP_ASSOC_INVALID;
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
    uint64_t now, const uint8_t *peer_init, int peer_reconfig_supported,
    int peer_forward_tsn_supported, int peer_interleaving_supported,
    int peer_i_forward_tsn_supported,
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
  cookie[56] = peer_reconfig_supported ? 1u : 0u;
  cookie[57] = peer_forward_tsn_supported ? 1u : 0u;
  cookie[58] = peer_interleaving_supported ? 1u : 0u;
  cookie[59] = peer_interleaving_supported &&
      peer_i_forward_tsn_supported ? 1u : 0u;
  s->in_progress = 1u;
  if (s->config.mac(s->config.provider_context, cookie,
        CNET_SCTP_ASSOC_COOKIE_CLAIMS_BYTES,
        cookie + CNET_SCTP_ASSOC_COOKIE_CLAIMS_BYTES) != 0) {
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
      rd64(cookie + 48u) != s->config.peer_binding ||
      cookie[56u] > 1u || cookie[57u] > 1u ||
      cookie[58u] > 1u || cookie[59u] > 1u ||
      (cookie[59u] && !cookie[58u]))
    return CNET_SCTP_ASSOC_AUTH;
  s->in_progress = 1u;
  mac_result = s->config.verify_mac(s->config.provider_context, cookie,
      CNET_SCTP_ASSOC_COOKIE_CLAIMS_BYTES,
      cookie + CNET_SCTP_ASSOC_COOKIE_CLAIMS_BYTES);
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
  enum {
    INIT_ACK_COOKIE_END = 16u + 4u + CNET_SCTP_ASSOC_COOKIE_BYTES,
    INIT_ACK_BASE = INIT_ACK_COOKIE_END + 12u
  };
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
  /* prepare_init() placed the two extension parameters at offset 16. INIT
   * ACK requires the State Cookie as well; keep Cookie first and move the
   * negotiation parameters after it for a canonical, stable packet. */
  memmove(value + INIT_ACK_COOKIE_END, value + 16u, 12u);
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
  status = make_cookie(s, now, chunk->value, params.supports_reconfig,
      params.supports_forward_tsn, params.supports_interleaving,
      params.supports_i_forward_tsn, value + 20u);
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
    s->peer_reconfig_supported = params.supports_reconfig ? 1u : 0u;
    s->peer_forward_tsn_supported =
        params.supports_forward_tsn ? 1u : 0u;
    s->peer_interleaving_supported =
        params.supports_interleaving ? 1u : 0u;
    s->peer_i_forward_tsn_supported =
        params.supports_i_forward_tsn ? 1u : 0u;
    s->expected_peer_reset_request = s->peer_initial_tsn;
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
        rd16(chunk->value + 34u), chunk->value[57u],
        chunk->value[58u], chunk->value[59u]);
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
    s->peer_reconfig_supported = chunk->value[56u];
    s->peer_forward_tsn_supported = chunk->value[57u];
    s->peer_interleaving_supported = chunk->value[58u];
    s->peer_i_forward_tsn_supported = chunk->value[59u];
    s->expected_peer_reset_request = peer_tsn;
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
        s->peer_outbound_streams, s->peer_inbound_streams,
        s->peer_forward_tsn_supported, s->peer_interleaving_supported,
        s->peer_i_forward_tsn_supported);
    if (prepared != CNET_SCTP_ASSOC_OK) return prepared;
    s->data_ready = 1u;
    s->expected_peer_reset_request = s->peer_initial_tsn;
  }
  s->state = CNET_SCTP_ASSOC_ESTABLISHED;
  s->saved_size = 0u;
  memset(s->saved_packet, 0, sizeof(s->saved_packet));
  s->deadline_ms = 0u;
  s->last_time_ms = now;
  return CNET_SCTP_ASSOC_OK;
}

static uint16_t reset_stream_bit(uint16_t stream) {
  return stream < CNET_SCTP_DATA_STREAMS ?
      (uint16_t)(UINT16_C(1) << stream) : 0u;
}

static uint16_t reset_stream_mask(uint16_t stream_count) {
  return stream_count >= CNET_SCTP_DATA_STREAMS ? UINT16_MAX :
      (uint16_t)((UINT16_C(1) << stream_count) - 1u);
}

static uint16_t reset_blocked_streams(const cnet_sctp_assoc *s) {
  uint16_t blocked = (uint16_t)(s->reciprocal_reset_pending |
      s->awaiting_peer_reciprocal | s->reciprocal_reset_failed |
      s->deferred_peer_reset_streams);
  if (s->reset_active && !s->reset_requests_incoming)
    blocked |= s->reset_streams;
  return blocked;
}

static int association_reset_blocks_data(const cnet_sctp_assoc *s) {
  return s->reset_active &&
      s->reconfig_kind == CNET_SCTP_RECONFIG_SSN_TSN_RESET;
}

static int next_reciprocal_streams(const cnet_sctp_assoc *s,
    uint16_t *streams) {
  const uint16_t ready = (uint16_t)(s->reciprocal_reset_pending &
      (uint16_t)~s->outgoing_reset_events);
  uint16_t candidate;
  if (!streams) return 0;
  /* A timed-out reciprocal request has an uncertain peer outcome and keeps
   * the single retained request slot until the Owner retries it verbatim. */
  if (s->reciprocal_reset_failed != 0u) return 0;
  if (s->peer_incoming_reset_pending) {
    if ((ready & s->peer_incoming_reset_streams) !=
        s->peer_incoming_reset_streams)
      return 0;
    *streams = s->peer_incoming_reset_streams;
    return 1;
  }
  for (candidate = 0u; candidate < CNET_SCTP_DATA_STREAMS; ++candidate) {
    if ((ready & reset_stream_bit(candidate)) != 0u) {
      *streams = reset_stream_bit(candidate);
      return 1;
    }
  }
  return 0;
}

static int reset_result_success(cnet_sctp_assoc_stream_reset_result result) {
  return result == CNET_SCTP_STREAM_RESET_NOTHING_TO_DO ||
      result == CNET_SCTP_STREAM_RESET_PERFORMED;
}

static void record_incoming_reset_result(cnet_sctp_assoc *s,
    uint16_t streams, cnet_sctp_assoc_stream_reset_result result) {
  uint16_t stream;
  for (stream = 0u; stream < CNET_SCTP_DATA_STREAMS; ++stream) {
    const uint16_t bit = reset_stream_bit(stream);
    if ((streams & bit) != 0u)
      s->incoming_reset_results[stream] = (uint8_t)result;
  }
  s->incoming_reset_events |= streams;
}

static void clear_active_reset(cnet_sctp_assoc *s, int preserve_packet) {
  s->reset_active = 0u;
  s->reconfig_kind = CNET_SCTP_RECONFIG_NONE;
  s->reset_is_reciprocal = 0u;
  s->reset_requests_incoming = 0u;
  s->reset_has_paired_incoming = 0u;
  s->reset_outgoing_complete = 0u;
  s->reset_paired_incoming_complete = 0u;
  s->reset_paired_incoming_streams = 0u;
  s->reset_paired_incoming_request = 0u;
  s->reset_retransmits = 0u;
  s->reset_needs_initial_publish = 0u;
  s->add_outgoing_is_reciprocal = 0u;
  s->add_outgoing_is_paired = 0u;
  s->add_paired_result = 0u;
  s->add_outgoing_streams = 0u;
  s->add_incoming_streams = 0u;
  s->reset_deadline_ms = 0u;
  s->reset_rto_ms = 0u;
  if (!preserve_packet) {
    s->reset_streams = 0u;
    s->reset_packet_size = 0u;
    memset(s->reset_packet, 0, sizeof(s->reset_packet));
  }
}

static void record_stream_change(cnet_sctp_assoc *s, int incoming,
    cnet_sctp_assoc_stream_reset_result result) {
  cnet_sctp_assoc_stream_change_event *event = incoming ?
      &s->incoming_stream_change : &s->outgoing_stream_change;
  event->inbound_streams = s->data.config.inbound_streams;
  event->outbound_streams = s->data.config.outbound_streams;
  event->result = result;
  if (incoming)
    s->incoming_stream_change_pending = 1u;
  else
    s->outgoing_stream_change_pending = 1u;
}

static void record_association_reset(cnet_sctp_assoc *s, int local,
    cnet_sctp_assoc_stream_reset_result result,
    uint32_t local_next_tsn, uint32_t peer_next_tsn) {
  s->association_reset.local_next_tsn = local_next_tsn;
  s->association_reset.peer_next_tsn = peer_next_tsn;
  s->association_reset.initiated_locally = local ? 1u : 0u;
  s->association_reset.result = result;
  s->association_reset_pending = 1u;
}

static cnet_sctp_assoc_status finish_local_association_reset(
    cnet_sctp_assoc *s, uint64_t now,
    cnet_sctp_assoc_stream_reset_result result,
    uint32_t local_next_tsn, uint32_t peer_next_tsn) {
  if (reset_result_success(result)) {
    if (!cnet_sctp_data_association_reset_ready(&s->data))
      return CNET_SCTP_ASSOC_BUSY;
    if (cnet_sctp_data_reset_association(&s->data, now,
            local_next_tsn, peer_next_tsn) != CNET_SCTP_DATA_OK)
      return CNET_SCTP_ASSOC_PROTOCOL;
  } else {
    local_next_tsn = 0u;
    peer_next_tsn = 0u;
  }
  record_association_reset(s, 1, result, local_next_tsn, peer_next_tsn);
  clear_active_reset(s, 0);
  return CNET_SCTP_ASSOC_OK;
}

static cnet_sctp_assoc_status finish_local_add_outgoing(cnet_sctp_assoc *s,
    cnet_sctp_assoc_stream_reset_result result) {
  const int incoming = s->add_outgoing_is_reciprocal;
  cnet_sctp_assoc_stream_reset_result event_result = result;
  if (reset_result_success(result)) {
    if (s->add_outgoing_streams >
        CNET_SCTP_DATA_STREAMS - s->data.config.outbound_streams)
      return CNET_SCTP_ASSOC_PROTOCOL;
    s->data.config.outbound_streams = (uint16_t)(
        s->data.config.outbound_streams + s->add_outgoing_streams);
  }
  if (s->add_outgoing_is_paired &&
      result != CNET_SCTP_STREAM_RESET_TIMEOUT &&
      !reset_result_success((cnet_sctp_assoc_stream_reset_result)
          s->add_paired_result))
    event_result = (cnet_sctp_assoc_stream_reset_result)s->add_paired_result;
  record_stream_change(s, incoming, event_result);
  clear_active_reset(s, 0);
  return CNET_SCTP_ASSOC_OK;
}

static void finish_local_add_incoming(cnet_sctp_assoc *s,
    cnet_sctp_assoc_stream_reset_result result) {
  record_stream_change(s, 0, result);
  clear_active_reset(s, 0);
}

static void note_add_pair_result(cnet_sctp_assoc *s,
    cnet_sctp_assoc_stream_reset_result result) {
  if (!reset_result_success(result) &&
      reset_result_success((cnet_sctp_assoc_stream_reset_result)
          s->add_paired_result))
    s->add_paired_result = (uint8_t)result;
}

static void finish_local_add_pair_if_complete(cnet_sctp_assoc *s) {
  if (!s->reset_outgoing_complete ||
      !s->reset_paired_incoming_complete)
    return;
  record_stream_change(s, 0,
      (cnet_sctp_assoc_stream_reset_result)s->add_paired_result);
  clear_active_reset(s, 0);
}

static cnet_sctp_assoc_status finish_local_add_pair_outgoing(
    cnet_sctp_assoc *s, cnet_sctp_assoc_stream_reset_result result) {
  if (reset_result_success(result)) {
    if (s->add_outgoing_streams >
        CNET_SCTP_DATA_STREAMS - s->data.config.outbound_streams)
      return CNET_SCTP_ASSOC_PROTOCOL;
    s->data.config.outbound_streams = (uint16_t)(
        s->data.config.outbound_streams + s->add_outgoing_streams);
  }
  note_add_pair_result(s, result);
  s->reset_outgoing_complete = 1u;
  finish_local_add_pair_if_complete(s);
  return CNET_SCTP_ASSOC_OK;
}

static void finish_local_add_pair_incoming(cnet_sctp_assoc *s,
    cnet_sctp_assoc_stream_reset_result result) {
  note_add_pair_result(s, result);
  s->reset_paired_incoming_complete = 1u;
  finish_local_add_pair_if_complete(s);
}

static void finish_local_reset(cnet_sctp_assoc *s,
    cnet_sctp_assoc_stream_reset_result result);

static cnet_sctp_assoc_status finish_active_reconfig(cnet_sctp_assoc *s,
    cnet_sctp_assoc_stream_reset_result result) {
  if (s->reconfig_kind == CNET_SCTP_RECONFIG_SSN_TSN_RESET)
    return finish_local_association_reset(s, s->last_time_ms, result, 0u, 0u);
  if (s->reconfig_kind == CNET_SCTP_RECONFIG_ADD_OUTGOING)
    return finish_local_add_outgoing(s, result);
  if (s->reconfig_kind == CNET_SCTP_RECONFIG_ADD_INCOMING) {
    finish_local_add_incoming(s, result);
    return CNET_SCTP_ASSOC_OK;
  }
  if (s->reconfig_kind == CNET_SCTP_RECONFIG_ADD_STREAMS) {
    note_add_pair_result(s, result);
    record_stream_change(s, 0, result);
    clear_active_reset(s, 0);
    return CNET_SCTP_ASSOC_OK;
  }
  finish_local_reset(s, result);
  return CNET_SCTP_ASSOC_OK;
}

static void finish_local_reset(cnet_sctp_assoc *s,
    cnet_sctp_assoc_stream_reset_result result) {
  const uint16_t streams = s->reset_streams;
  const int success = reset_result_success(result);
  const int reciprocal = s->reset_is_reciprocal;
  const int incoming = s->reset_requests_incoming;
  const int paired = s->reset_has_paired_incoming;
  uint16_t stream;
  if (incoming) {
    record_incoming_reset_result(s, streams, result);
    clear_active_reset(s, 0);
    return;
  }
  if (!paired || !s->reset_outgoing_complete) {
    for (stream = 0u; stream < CNET_SCTP_DATA_STREAMS; ++stream) {
      const uint16_t bit = reset_stream_bit(stream);
      if ((streams & bit) == 0u) continue;
      if (success) cnet_sctp_data_reset_outbound_ssn(&s->data, stream);
      s->outgoing_reset_results[stream] = (uint8_t)result;
    }
    s->outgoing_reset_events |= streams;
  }
  if (reciprocal) {
    if (success)
      s->reciprocal_reset_failed &= (uint16_t)~streams;
    else
      s->reciprocal_reset_failed |= streams;
  } else if (!success) {
    /* A locally initiated close that never reset the outgoing SSN restores
     * the old channel state. The Owner observes the failure event and may
     * retry without leaving the SID permanently blocked. */
    s->awaiting_peer_reciprocal &= (uint16_t)~streams;
  }
  if (paired) {
    s->reset_outgoing_complete = 1u;
    if (s->reset_paired_incoming_complete)
      clear_active_reset(s, 0);
    return;
  }
  /* Timeout leaves the peer outcome unknown. Preserve the exact reciprocal
   * request so an explicit retry uses the same Request Sequence Number and
   * is valid whether the peer missed it or only its response was lost. */
  clear_active_reset(s,
      reciprocal && result == CNET_SCTP_STREAM_RESET_TIMEOUT);
}

static void finish_paired_incoming_reset(cnet_sctp_assoc *s,
    uint16_t completed_streams,
    cnet_sctp_assoc_stream_reset_result missing_result) {
  const uint16_t missing = (uint16_t)(
      s->reset_paired_incoming_streams & (uint16_t)~completed_streams);
  if (!s->reset_has_paired_incoming ||
      s->reset_paired_incoming_complete)
    return;
  if (missing != 0u)
    record_incoming_reset_result(s, missing, missing_result);
  s->reset_paired_incoming_complete = 1u;
  if (s->reset_outgoing_complete)
    clear_active_reset(s, 0);
}

static cnet_sctp_assoc_status apply_reconfig_response(cnet_sctp_assoc *s,
    uint64_t now, const sctp_reconfig_parameter *parameter) {
  uint32_t sequence, result;
  if (!s || !parameter || parameter->type != SCTP_PARAM_RECONFIG_RESPONSE)
    return CNET_SCTP_ASSOC_INVALID;
  sequence = rd32(parameter->bytes + 4u);
  result = rd32(parameter->bytes + 8u);
  if (result > CNET_SCTP_STREAM_RESET_IN_PROGRESS)
    return CNET_SCTP_ASSOC_PROTOCOL;
  if (s->reset_active &&
      s->reconfig_kind == CNET_SCTP_RECONFIG_SSN_TSN_RESET &&
      sequence == s->reset_request) {
    uint32_t sender_next_tsn, receiver_next_tsn;
    if (parameter->size != 20u) return CNET_SCTP_ASSOC_PROTOCOL;
    sender_next_tsn = rd32(parameter->bytes + 12u);
    receiver_next_tsn = rd32(parameter->bytes + 16u);
    if (result == CNET_SCTP_STREAM_RESET_IN_PROGRESS) {
      if (now > UINT64_MAX - s->reset_rto_ms)
        return CNET_SCTP_ASSOC_CLOCK;
      s->reset_retransmits = 0u;
      s->reset_deadline_ms = now + s->reset_rto_ms;
      return CNET_SCTP_ASSOC_OK;
    }
    return finish_local_association_reset(s, now,
        (cnet_sctp_assoc_stream_reset_result)result,
        receiver_next_tsn, sender_next_tsn);
  }
  if (s->reset_active &&
      s->reconfig_kind == CNET_SCTP_RECONFIG_ADD_OUTGOING &&
      sequence == s->reset_request) {
    if (parameter->size != 12u) return CNET_SCTP_ASSOC_PROTOCOL;
    if (result == CNET_SCTP_STREAM_RESET_IN_PROGRESS) {
      if (now > UINT64_MAX - s->reset_rto_ms)
        return CNET_SCTP_ASSOC_CLOCK;
      s->reset_retransmits = 0u;
      s->reset_deadline_ms = now + s->reset_rto_ms;
      return CNET_SCTP_ASSOC_OK;
    }
    return finish_local_add_outgoing(s,
        (cnet_sctp_assoc_stream_reset_result)result);
  }
  if (s->reset_active &&
      s->reconfig_kind == CNET_SCTP_RECONFIG_ADD_INCOMING &&
      sequence == s->reset_request) {
    if (parameter->size != 12u) return CNET_SCTP_ASSOC_PROTOCOL;
    if (result == CNET_SCTP_STREAM_RESET_IN_PROGRESS) {
      if (now > UINT64_MAX - s->reset_rto_ms)
        return CNET_SCTP_ASSOC_CLOCK;
      s->reset_retransmits = 0u;
      s->reset_deadline_ms = now + s->reset_rto_ms;
      return CNET_SCTP_ASSOC_OK;
    }
    /* RFC 6525 5.2.6 completes this request through a corresponding Add
     * Outgoing request. A direct success cannot commit receive streams. */
    if (reset_result_success(
            (cnet_sctp_assoc_stream_reset_result)result))
      return CNET_SCTP_ASSOC_PROTOCOL;
    finish_local_add_incoming(s,
        (cnet_sctp_assoc_stream_reset_result)result);
    return CNET_SCTP_ASSOC_OK;
  }
  if (s->reset_active &&
      s->reconfig_kind == CNET_SCTP_RECONFIG_ADD_STREAMS) {
    if (parameter->size != 12u) return CNET_SCTP_ASSOC_PROTOCOL;
    if (sequence != s->reset_request &&
        sequence != s->reset_paired_incoming_request)
      return CNET_SCTP_ASSOC_OK;
    if ((sequence == s->reset_request && s->reset_outgoing_complete) ||
        (sequence == s->reset_paired_incoming_request &&
         s->reset_paired_incoming_complete))
      return CNET_SCTP_ASSOC_OK;
    if (result == CNET_SCTP_STREAM_RESET_IN_PROGRESS) {
      if (now > UINT64_MAX - s->reset_rto_ms)
        return CNET_SCTP_ASSOC_CLOCK;
      s->reset_retransmits = 0u;
      s->reset_deadline_ms = now + s->reset_rto_ms;
      return CNET_SCTP_ASSOC_OK;
    }
    if (sequence == s->reset_request)
      return finish_local_add_pair_outgoing(s,
          (cnet_sctp_assoc_stream_reset_result)result);
    if (reset_result_success(
            (cnet_sctp_assoc_stream_reset_result)result))
      return CNET_SCTP_ASSOC_PROTOCOL;
    finish_local_add_pair_incoming(s,
        (cnet_sctp_assoc_stream_reset_result)result);
    return CNET_SCTP_ASSOC_OK;
  }
  if (s->reset_active && s->reset_has_paired_incoming &&
      !s->reset_paired_incoming_complete &&
      sequence == s->reset_paired_incoming_request) {
    if (parameter->size != 12u) return CNET_SCTP_ASSOC_PROTOCOL;
    if (result == CNET_SCTP_STREAM_RESET_IN_PROGRESS) {
      if (now > UINT64_MAX - s->reset_rto_ms)
        return CNET_SCTP_ASSOC_CLOCK;
      s->reset_retransmits = 0u;
      s->reset_deadline_ms = now + s->reset_rto_ms;
      return CNET_SCTP_ASSOC_OK;
    }
    if (result == CNET_SCTP_STREAM_RESET_PERFORMED)
      return CNET_SCTP_ASSOC_PROTOCOL;
    finish_paired_incoming_reset(s, 0u,
        (cnet_sctp_assoc_stream_reset_result)result);
    return CNET_SCTP_ASSOC_OK;
  }
  if (!s->reset_active || sequence != s->reset_request)
    return CNET_SCTP_ASSOC_OK; /* stale/duplicate response */
  /* Optional TSNs are legal only for an SSN/TSN Reset response, which this
   * bounded sender never emits. Accepting them here would bind the wrong
   * response shape to our outstanding Outgoing SSN Reset. */
  if (parameter->size != 12u) return CNET_SCTP_ASSOC_PROTOCOL;
  if (result == CNET_SCTP_STREAM_RESET_IN_PROGRESS) {
    if (now > UINT64_MAX - s->reset_rto_ms)
      return CNET_SCTP_ASSOC_CLOCK;
    s->reset_retransmits = 0u;
    s->reset_deadline_ms = now + s->reset_rto_ms;
    return CNET_SCTP_ASSOC_OK;
  }
  if (s->reset_has_paired_incoming && s->reset_outgoing_complete)
    return CNET_SCTP_ASSOC_OK;
  /* A willing peer answers an Incoming Reset with a corresponding Outgoing
   * Reset. A direct Performed response has no TSN barrier and cannot safely
   * mutate receive sequencing state. */
  if (s->reset_requests_incoming &&
      result == CNET_SCTP_STREAM_RESET_PERFORMED)
    return CNET_SCTP_ASSOC_PROTOCOL;
  finish_local_reset(s, (cnet_sctp_assoc_stream_reset_result)result);
  return CNET_SCTP_ASSOC_OK;
}

static int tsn_after(uint32_t first, uint32_t second) {
  const uint32_t distance = first - second;
  return distance != 0u && distance < UINT32_C(0x80000000);
}

static int deferred_peer_reset_ready(const cnet_sctp_assoc *s) {
  if (!s->deferred_peer_reset_active ||
      tsn_after(s->deferred_peer_reset_last_tsn, s->data.cumulative_rx))
    return 0;
  return cnet_sctp_data_streams_through_tsn_drained(
      &s->data, s->deferred_peer_reset_streams,
      s->deferred_peer_reset_last_tsn);
}

static void clear_deferred_peer_reset(cnet_sctp_assoc *s) {
  s->deferred_peer_reset_active = 0u;
  s->deferred_peer_reset_streams = 0u;
  s->deferred_peer_reset_suppress_reciprocal = 0u;
  s->deferred_peer_reset_answers_incoming = 0u;
  s->deferred_peer_reset_answers_pair = 0u;
  s->deferred_peer_reset_request = 0u;
  s->deferred_peer_reset_last_tsn = 0u;
}

static void clear_peer_incoming_reset(cnet_sctp_assoc *s) {
  s->peer_incoming_reset_pending = 0u;
  s->peer_incoming_reset_streams = 0u;
  s->peer_incoming_reset_request = 0u;
}

static void apply_peer_reset(cnet_sctp_assoc *s, uint16_t mask,
    uint16_t suppress_reciprocal) {
  uint16_t stream;
  for (stream = 0u; stream < s->data.config.inbound_streams; ++stream) {
    const uint16_t bit = (uint16_t)(UINT16_C(1) << stream);
    if ((mask & bit) == 0u) continue;
    cnet_sctp_data_reset_inbound_ssn(&s->data, stream);
    /* RFC 8831 6.7: receiving the peer's outgoing reset closes our
     * incoming direction and requires resetting the corresponding outgoing
     * stream. A simultaneous local close already satisfies that obligation. */
    if ((s->awaiting_peer_reciprocal & bit) != 0u) {
      s->awaiting_peer_reciprocal &= (uint16_t)~bit;
    } else if ((s->reciprocal_reset_failed & bit) != 0u) {
      /* Preserve the exact timed-out request: the peer reset does not prove
       * whether that outgoing request or only its response was lost. */
    } else if ((suppress_reciprocal & bit) == 0u &&
               !(s->reset_active && !s->reset_requests_incoming &&
                 (s->reset_streams & bit) != 0u) &&
               stream < s->data.config.outbound_streams) {
      s->reciprocal_reset_pending |= bit;
    }
  }
  record_incoming_reset_result(
      s, mask, CNET_SCTP_STREAM_RESET_PERFORMED);
}

static cnet_sctp_assoc_stream_reset_result evaluate_peer_reset(
    const cnet_sctp_assoc *s, const sctp_reconfig_parameter *parameter,
    uint16_t *stream_mask) {
  uint16_t mask = 0u;
  const uint32_t last_assigned = rd32(parameter->bytes + 12u);
  size_t position;
  if (parameter->size == 16u) {
    uint16_t stream;
    for (stream = 0u; stream < s->data.config.inbound_streams; ++stream)
      mask |= (uint16_t)(UINT16_C(1) << stream);
  } else {
    for (position = 16u; position < parameter->size; position += 2u) {
      const uint16_t stream = rd16(parameter->bytes + position);
      const uint16_t bit = stream < 16u ?
          (uint16_t)(UINT16_C(1) << stream) : 0u;
      if (stream >= s->data.config.inbound_streams || (mask & bit) != 0u)
        return CNET_SCTP_STREAM_RESET_WRONG_SSN;
      mask |= bit;
    }
  }
  if (last_assigned - s->data.cumulative_rx == UINT32_C(0x80000000))
    return CNET_SCTP_STREAM_RESET_DENIED;
  for (position = 0u; position < s->data.config.inbound_streams; ++position) {
    const uint16_t bit = (uint16_t)(UINT16_C(1) << position);
    if ((mask & bit) != 0u && (s->incoming_reset_events & bit) != 0u)
      return CNET_SCTP_STREAM_RESET_DENIED;
  }
  *stream_mask = mask;
  if (tsn_after(last_assigned, s->data.cumulative_rx) ||
      !cnet_sctp_data_streams_through_tsn_drained(
          &s->data, mask, last_assigned))
    return CNET_SCTP_STREAM_RESET_IN_PROGRESS;
  return CNET_SCTP_STREAM_RESET_PERFORMED;
}

static cnet_sctp_assoc_stream_reset_result evaluate_incoming_reset(
    const cnet_sctp_assoc *s, const sctp_reconfig_parameter *parameter,
    uint16_t *requested_streams) {
  uint16_t mask = 0u;
  size_t position;
  if (parameter->size == 8u) {
    mask = reset_stream_mask(s->data.config.outbound_streams);
  } else {
    for (position = 8u; position < parameter->size; position += 2u) {
      const uint16_t candidate = rd16(parameter->bytes + position);
      const uint16_t bit = reset_stream_bit(candidate);
      if (candidate >= s->data.config.outbound_streams || bit == 0u ||
          (mask & bit) != 0u)
        return CNET_SCTP_STREAM_RESET_WRONG_SSN;
      mask |= bit;
    }
  }

  if (s->reset_active && !s->reset_requests_incoming &&
      (s->reset_streams & mask) == mask)
    return CNET_SCTP_STREAM_RESET_NOTHING_TO_DO;
  if ((s->awaiting_peer_reciprocal & mask) == mask)
    return CNET_SCTP_STREAM_RESET_NOTHING_TO_DO;
  if (s->deferred_peer_reset_active || s->peer_incoming_reset_pending ||
      s->reciprocal_reset_failed != 0u ||
      (!s->reset_active && s->reciprocal_reset_pending != 0u))
    return CNET_SCTP_STREAM_RESET_REQUEST_IN_PROGRESS;
  *requested_streams = mask;
  return CNET_SCTP_STREAM_RESET_PERFORMED; /* schedule Outgoing Reset */
}

static void write_reset_response(uint8_t value[12], uint32_t sequence,
    cnet_sctp_assoc_stream_reset_result result) {
  wr16(value, SCTP_PARAM_RECONFIG_RESPONSE);
  wr16(value + 2u, 12u);
  wr32(value + 4u, sequence);
  wr32(value + 8u, (uint32_t)result);
}

static cnet_sctp_assoc_status encode_reset_response(const cnet_sctp_assoc *s,
    uint32_t sequence, cnet_sctp_assoc_stream_reset_result result,
    uint8_t packet[CNET_SCTP_ASSOC_MAX_PACKET], size_t *packet_size) {
  uint8_t value[12];
  write_reset_response(value, sequence, result);
  return encode_one(s, s->peer_tag, SCTP_TYPE_RECONFIG,
      value, sizeof(value), packet, CNET_SCTP_ASSOC_MAX_PACKET, packet_size);
}

static cnet_sctp_assoc_status encode_association_reset_response(
    const cnet_sctp_assoc *s, uint32_t sequence,
    cnet_sctp_assoc_stream_reset_result result,
    uint32_t sender_next_tsn, uint32_t receiver_next_tsn,
    uint8_t packet[CNET_SCTP_ASSOC_MAX_PACKET], size_t *packet_size) {
  uint8_t value[20];
  write_reset_response(value, sequence, result);
  wr16(value + 2u, sizeof(value));
  wr32(value + 12u, sender_next_tsn);
  wr32(value + 16u, receiver_next_tsn);
  return encode_one(s, s->peer_tag, SCTP_TYPE_RECONFIG,
      value, sizeof(value), packet, CNET_SCTP_ASSOC_MAX_PACKET, packet_size);
}

static cnet_sctp_assoc_status encode_reset_response_pair(
    const cnet_sctp_assoc *s,
    uint32_t first_sequence, cnet_sctp_assoc_stream_reset_result first_result,
    uint32_t second_sequence,
    cnet_sctp_assoc_stream_reset_result second_result,
    uint8_t packet[CNET_SCTP_ASSOC_MAX_PACKET], size_t *packet_size) {
  uint8_t value[24];
  write_reset_response(value, first_sequence, first_result);
  write_reset_response(value + 12u, second_sequence, second_result);
  return encode_one(s, s->peer_tag, SCTP_TYPE_RECONFIG,
      value, sizeof(value), packet, CNET_SCTP_ASSOC_MAX_PACKET, packet_size);
}

static cnet_sctp_assoc_status handle_add_outgoing_request(
    cnet_sctp_assoc *s, const sctp_reconfig_parameter *parameter,
    uint8_t *output, size_t cap, size_t *written) {
  const uint32_t sequence = rd32(parameter->bytes + 4u);
  const uint16_t count = rd16(parameter->bytes + 8u);
  const int completes_local_add_incoming = s->reset_active &&
      (s->reconfig_kind == CNET_SCTP_RECONFIG_ADD_INCOMING ||
       (s->reconfig_kind == CNET_SCTP_RECONFIG_ADD_STREAMS &&
        !s->reset_paired_incoming_complete));
  const int blocks_incoming_stream_change = s->reset_active &&
      s->reconfig_kind == CNET_SCTP_RECONFIG_ADD_OUTGOING &&
      s->add_outgoing_is_reciprocal;
  cnet_sctp_assoc_stream_reset_result result;
  uint8_t response[CNET_SCTP_ASSOC_MAX_PACKET];
  size_t response_size = 0u;
  cnet_sctp_assoc_status status;

  if (sequence == s->last_peer_reset_request &&
      s->last_reset_response_size != 0u) {
    if (s->last_reset_response_size > cap)
      return CNET_SCTP_ASSOC_CAPACITY;
    memcpy(output, s->last_reset_response, s->last_reset_response_size);
    *written = s->last_reset_response_size;
    return CNET_SCTP_ASSOC_OK;
  }
  if (sequence != s->expected_peer_reset_request) {
    result = CNET_SCTP_STREAM_RESET_BAD_SEQUENCE;
  } else if (completes_local_add_incoming ?
             s->outgoing_stream_change_pending :
             (s->incoming_stream_change_pending ||
              blocks_incoming_stream_change)) {
    /* Preserve the fixed Owner-pulled event slot. The sequence is not
     * consumed, so the peer can retry after the Owner drains the event. */
    result = CNET_SCTP_STREAM_RESET_REQUEST_IN_PROGRESS;
  } else if (count == 0u) {
    result = CNET_SCTP_STREAM_RESET_NOTHING_TO_DO;
  } else if (count >
             CNET_SCTP_DATA_STREAMS - s->data.config.inbound_streams) {
    result = CNET_SCTP_STREAM_RESET_DENIED;
  } else {
    result = CNET_SCTP_STREAM_RESET_PERFORMED;
  }
  status = encode_reset_response(s, sequence, result,
      response, &response_size);
  if (status != CNET_SCTP_ASSOC_OK) return status;
  if (response_size > cap) return CNET_SCTP_ASSOC_CAPACITY;

  if (sequence == s->expected_peer_reset_request &&
      result != CNET_SCTP_STREAM_RESET_REQUEST_IN_PROGRESS) {
    s->last_peer_reset_request = sequence;
    ++s->expected_peer_reset_request;
    memcpy(s->last_reset_response, response, response_size);
    s->last_reset_response_size = response_size;
    if (reset_result_success(result)) {
      s->data.config.inbound_streams = (uint16_t)(
          s->data.config.inbound_streams + count);
      if (completes_local_add_incoming) {
        if (result == CNET_SCTP_STREAM_RESET_PERFORMED) {
          if (s->reconfig_kind == CNET_SCTP_RECONFIG_ADD_STREAMS) {
            finish_local_add_pair_incoming(s, result);
          } else {
            record_stream_change(s, 0, result);
            clear_active_reset(s, 0);
          }
        }
      } else {
        record_stream_change(s, 1, result);
      }
    }
  }
  memcpy(output, response, response_size);
  *written = response_size;
  return CNET_SCTP_ASSOC_OK;
}

static cnet_sctp_assoc_status complete_deferred_peer_reset(
    cnet_sctp_assoc *s, uint64_t now, uint8_t *output, size_t cap,
    size_t *written) {
  uint8_t response[CNET_SCTP_ASSOC_MAX_PACKET];
  size_t response_size = 0u;
  const uint16_t mask = s->deferred_peer_reset_streams;
  const uint32_t sequence = s->deferred_peer_reset_request;
  cnet_sctp_assoc_status status;
  if (!deferred_peer_reset_ready(s)) return CNET_SCTP_ASSOC_EMPTY;
  status = encode_reset_response(s, sequence,
      CNET_SCTP_STREAM_RESET_PERFORMED, response, &response_size);
  if (status != CNET_SCTP_ASSOC_OK) return status;
  if (response_size > cap) return CNET_SCTP_ASSOC_CAPACITY;

  apply_peer_reset(s, mask, s->deferred_peer_reset_suppress_reciprocal);
  if (s->deferred_peer_reset_answers_pair) {
    finish_paired_incoming_reset(s, mask,
        CNET_SCTP_STREAM_RESET_DENIED);
  } else if (s->deferred_peer_reset_answers_incoming) {
    const uint16_t missing = (uint16_t)(
        s->awaiting_local_incoming_reset & (uint16_t)~mask);
    if (missing != 0u)
      record_incoming_reset_result(
          s, missing, CNET_SCTP_STREAM_RESET_DENIED);
    s->awaiting_local_incoming_reset = 0u;
    s->awaiting_local_incoming_reset_request = 0u;
  }
  clear_deferred_peer_reset(s);
  memcpy(s->last_reset_response, response, response_size);
  s->last_reset_response_size = response_size;
  memcpy(output, response, response_size);
  *written = response_size;
  s->last_time_ms = now;
  return CNET_SCTP_ASSOC_OK;
}

static cnet_sctp_assoc_status start_local_reset_with_prefix(cnet_sctp_assoc *s,
    uint64_t now, uint16_t streams, int reciprocal,
    uint32_t response_sequence, const uint8_t *prefix, size_t prefix_size,
    void *out, size_t cap, size_t *written) {
  uint8_t value[16u + 2u * CNET_SCTP_DATA_STREAMS];
  uint8_t combined[12u + 16u + 2u * CNET_SCTP_DATA_STREAMS];
  uint8_t packet[CNET_SCTP_ASSOC_MAX_PACKET];
  const uint16_t valid = reset_stream_mask(s->data.config.outbound_streams);
  size_t packet_size = 0u, value_size = 16u;
  uint16_t stream;
  cnet_sctp_assoc_status status;
  if (streams == 0u || (streams & (uint16_t)~valid) != 0u)
    return CNET_SCTP_ASSOC_INVALID;
  if ((prefix == NULL) != (prefix_size == 0u) ||
      (prefix_size != 0u && prefix_size != 12u))
    return CNET_SCTP_ASSOC_INVALID;
  if (s->reset_active || (s->outgoing_reset_events & streams) != 0u)
    return CNET_SCTP_ASSOC_BUSY;
  /* A stronger-than-required admission rule keeps Sender's Last Assigned
   * TSN cumulatively acknowledged before the reset leaves this endpoint.
   * Existing retained DATA may drain while an automatic reciprocal waits. */
  if (!cnet_sctp_data_outbound_drained(&s->data))
    return CNET_SCTP_ASSOC_BUSY;
  if (now > UINT64_MAX - s->config.t1_ms)
    return CNET_SCTP_ASSOC_CLOCK;
  if (reciprocal && (s->reciprocal_reset_failed & streams) == streams &&
      s->reset_streams == streams && s->reset_packet_size != 0u) {
    if (s->reset_packet_size > cap) return CNET_SCTP_ASSOC_CAPACITY;
    memcpy(out, s->reset_packet, s->reset_packet_size);
    *written = s->reset_packet_size;
    s->reset_active = 1u;
    s->reconfig_kind = CNET_SCTP_RECONFIG_STREAM_RESET;
    s->reset_is_reciprocal = 1u;
    s->reset_requests_incoming = 0u;
    s->reset_retransmits = 0u;
    s->reset_rto_ms = s->config.t1_ms;
    s->reset_deadline_ms = now + s->reset_rto_ms;
    s->reciprocal_reset_failed &= (uint16_t)~streams;
    s->last_time_ms = now;
    return CNET_SCTP_ASSOC_OK;
  }
  wr16(value, SCTP_PARAM_OUTGOING_RESET);
  wr32(value + 4u, s->next_reset_request);
  wr32(value + 8u, response_sequence);
  wr32(value + 12u, s->data.next_tsn - 1u);
  if (streams != valid) {
    for (stream = 0u; stream < CNET_SCTP_DATA_STREAMS; ++stream) {
      if ((streams & reset_stream_bit(stream)) == 0u) continue;
      wr16(value + value_size, stream);
      value_size += 2u;
    }
  }
  wr16(value + 2u, (uint16_t)value_size);
  if (prefix_size != 0u) {
    memcpy(combined, prefix, prefix_size);
    memcpy(combined + prefix_size, value, value_size);
    status = encode_one(s, s->peer_tag, SCTP_TYPE_RECONFIG,
        combined, prefix_size + value_size,
        packet, sizeof(packet), &packet_size);
  } else {
    status = encode_one(s, s->peer_tag, SCTP_TYPE_RECONFIG,
        value, value_size, packet, sizeof(packet), &packet_size);
  }
  if (status != CNET_SCTP_ASSOC_OK) return status;
  if (packet_size > cap) return CNET_SCTP_ASSOC_CAPACITY;
  memcpy(out, packet, packet_size);
  *written = packet_size;
  memcpy(s->reset_packet, packet, packet_size);
  s->reset_packet_size = packet_size;
  s->reset_streams = streams;
  s->reset_request = s->next_reset_request++;
  s->reset_active = 1u;
  s->reconfig_kind = CNET_SCTP_RECONFIG_STREAM_RESET;
  s->reset_is_reciprocal = reciprocal != 0;
  s->reset_requests_incoming = 0u;
  s->reset_retransmits = 0u;
  s->reset_rto_ms = s->config.t1_ms;
  s->reset_deadline_ms = now + s->reset_rto_ms;
  if (reciprocal) {
    s->reciprocal_reset_pending &= (uint16_t)~streams;
    s->reciprocal_reset_failed &= (uint16_t)~streams;
  } else {
    s->awaiting_peer_reciprocal |= streams;
  }
  s->last_time_ms = now;
  return CNET_SCTP_ASSOC_OK;
}

static cnet_sctp_assoc_status start_local_reset(cnet_sctp_assoc *s,
    uint64_t now, uint16_t streams, int reciprocal,
    uint32_t response_sequence,
    void *out, size_t cap, size_t *written) {
  return start_local_reset_with_prefix(s, now, streams, reciprocal,
      response_sequence, NULL, 0u, out, cap, written);
}

static cnet_sctp_assoc_status start_local_incoming_reset(
    cnet_sctp_assoc *s, uint64_t now, uint16_t streams, int all_streams,
    void *out, size_t cap, size_t *written) {
  uint8_t value[8u + 2u * CNET_SCTP_DATA_STREAMS];
  uint8_t packet[CNET_SCTP_ASSOC_MAX_PACKET];
  size_t value_size = 8u, packet_size = 0u;
  uint16_t stream;
  cnet_sctp_assoc_status status;
  if (streams == 0u || s->reset_active ||
      s->awaiting_local_incoming_reset != 0u)
    return CNET_SCTP_ASSOC_BUSY;
  if ((s->incoming_reset_events & streams) != 0u ||
      (s->deferred_peer_reset_streams & streams) != 0u)
    return CNET_SCTP_ASSOC_BUSY;
  if (now > UINT64_MAX - s->config.t1_ms)
    return CNET_SCTP_ASSOC_CLOCK;

  wr16(value, SCTP_PARAM_INCOMING_RESET);
  wr32(value + 4u, s->next_reset_request);
  if (!all_streams) {
    for (stream = 0u; stream < CNET_SCTP_DATA_STREAMS; ++stream) {
      if ((streams & reset_stream_bit(stream)) == 0u) continue;
      wr16(value + value_size, stream);
      value_size += 2u;
    }
  }
  wr16(value + 2u, (uint16_t)value_size);
  status = encode_one(s, s->peer_tag, SCTP_TYPE_RECONFIG,
      value, value_size, packet, sizeof(packet), &packet_size);
  if (status != CNET_SCTP_ASSOC_OK) return status;
  if (packet_size > cap) return CNET_SCTP_ASSOC_CAPACITY;

  memcpy(out, packet, packet_size);
  *written = packet_size;
  memcpy(s->reset_packet, packet, packet_size);
  s->reset_packet_size = packet_size;
  s->reset_streams = streams;
  s->reset_request = s->next_reset_request++;
  s->reset_active = 1u;
  s->reconfig_kind = CNET_SCTP_RECONFIG_STREAM_RESET;
  s->reset_is_reciprocal = 0u;
  s->reset_requests_incoming = 1u;
  s->reset_retransmits = 0u;
  s->reset_rto_ms = s->config.t1_ms;
  s->reset_deadline_ms = now + s->reset_rto_ms;
  s->last_time_ms = now;
  return CNET_SCTP_ASSOC_OK;
}

static cnet_sctp_assoc_status start_local_paired_reset(
    cnet_sctp_assoc *s, uint64_t now, uint16_t outgoing_streams,
    uint16_t incoming_streams, int all_streams,
    void *out, size_t cap, size_t *written) {
  uint8_t value[24u + 4u * CNET_SCTP_DATA_STREAMS];
  uint8_t packet[CNET_SCTP_ASSOC_MAX_PACKET];
  const uint16_t valid_outgoing =
      reset_stream_mask(s->data.config.outbound_streams);
  const uint16_t valid_incoming =
      reset_stream_mask(s->data.config.inbound_streams);
  size_t outgoing_size = 16u, incoming_offset;
  size_t incoming_size = 8u, value_size, packet_size = 0u;
  uint16_t stream;
  cnet_sctp_assoc_status status;
  if (outgoing_streams == 0u || incoming_streams == 0u ||
      (outgoing_streams & (uint16_t)~valid_outgoing) != 0u ||
      (incoming_streams & (uint16_t)~valid_incoming) != 0u)
    return CNET_SCTP_ASSOC_INVALID;
  if (s->reset_active || s->awaiting_local_incoming_reset != 0u ||
      s->deferred_peer_reset_active || s->peer_incoming_reset_pending ||
      s->reciprocal_reset_pending != 0u ||
      s->reciprocal_reset_failed != 0u ||
      (s->awaiting_peer_reciprocal & outgoing_streams) != 0u ||
      (s->outgoing_reset_events & outgoing_streams) != 0u ||
      (s->incoming_reset_events & incoming_streams) != 0u)
    return CNET_SCTP_ASSOC_BUSY;
  if (!cnet_sctp_data_outbound_drained(&s->data))
    return CNET_SCTP_ASSOC_BUSY;
  if (now > UINT64_MAX - s->config.t1_ms)
    return CNET_SCTP_ASSOC_CLOCK;

  memset(value, 0, sizeof(value));
  wr16(value, SCTP_PARAM_OUTGOING_RESET);
  wr32(value + 4u, s->next_reset_request);
  wr32(value + 8u, s->expected_peer_reset_request - 1u);
  wr32(value + 12u, s->data.next_tsn - 1u);
  if (!all_streams) {
    for (stream = 0u; stream < CNET_SCTP_DATA_STREAMS; ++stream) {
      if ((outgoing_streams & reset_stream_bit(stream)) == 0u) continue;
      wr16(value + outgoing_size, stream);
      outgoing_size += 2u;
    }
  }
  wr16(value + 2u, (uint16_t)outgoing_size);

  incoming_offset = (outgoing_size + 3u) & ~(size_t)3u;
  wr16(value + incoming_offset, SCTP_PARAM_INCOMING_RESET);
  wr32(value + incoming_offset + 4u, s->next_reset_request + 1u);
  if (!all_streams) {
    for (stream = 0u; stream < CNET_SCTP_DATA_STREAMS; ++stream) {
      if ((incoming_streams & reset_stream_bit(stream)) == 0u) continue;
      wr16(value + incoming_offset + incoming_size, stream);
      incoming_size += 2u;
    }
  }
  wr16(value + incoming_offset + 2u, (uint16_t)incoming_size);
  value_size = incoming_offset + incoming_size;
  status = encode_one(s, s->peer_tag, SCTP_TYPE_RECONFIG,
      value, value_size, packet, sizeof(packet), &packet_size);
  if (status != CNET_SCTP_ASSOC_OK) return status;
  if (packet_size > cap) return CNET_SCTP_ASSOC_CAPACITY;

  memcpy(out, packet, packet_size);
  *written = packet_size;
  memcpy(s->reset_packet, packet, packet_size);
  s->reset_packet_size = packet_size;
  s->reset_streams = outgoing_streams;
  s->reset_paired_incoming_streams = incoming_streams;
  s->reset_request = s->next_reset_request;
  s->reset_paired_incoming_request = s->next_reset_request + 1u;
  s->next_reset_request += 2u;
  s->reset_active = 1u;
  s->reconfig_kind = CNET_SCTP_RECONFIG_STREAM_RESET;
  s->reset_is_reciprocal = 0u;
  s->reset_requests_incoming = 0u;
  s->reset_has_paired_incoming = 1u;
  s->reset_outgoing_complete = 0u;
  s->reset_paired_incoming_complete = 0u;
  s->reset_retransmits = 0u;
  s->reset_rto_ms = s->config.t1_ms;
  s->reset_deadline_ms = now + s->reset_rto_ms;
  s->last_time_ms = now;
  return CNET_SCTP_ASSOC_OK;
}

static cnet_sctp_assoc_status start_local_association_reset(
    cnet_sctp_assoc *s, uint64_t now,
    void *out, size_t cap, size_t *written) {
  uint8_t value[8] = {0};
  uint8_t packet[CNET_SCTP_ASSOC_MAX_PACKET];
  size_t packet_size = 0u;
  cnet_sctp_assoc_status status;
  if (s->reset_active || s->association_reset_pending ||
      s->deferred_peer_reset_active || s->peer_incoming_reset_pending ||
      s->awaiting_local_incoming_reset != 0u ||
      s->reciprocal_reset_pending != 0u ||
      s->awaiting_peer_reciprocal != 0u ||
      s->reciprocal_reset_failed != 0u)
    return CNET_SCTP_ASSOC_BUSY;
  if (s->association_reset_request_sent &&
      now - s->last_association_reset_request_ms <
          CNET_SCTP_ASSOC_RESET_MIN_INTERVAL_MS)
    return CNET_SCTP_ASSOC_BUSY;
  if (!cnet_sctp_data_association_reset_ready(&s->data))
    return CNET_SCTP_ASSOC_BUSY;
  if (now > UINT64_MAX - s->config.t1_ms)
    return CNET_SCTP_ASSOC_CLOCK;

  wr16(value, SCTP_PARAM_SSN_TSN_RESET);
  wr16(value + 2u, sizeof(value));
  wr32(value + 4u, s->next_reset_request);
  status = encode_one(s, s->peer_tag, SCTP_TYPE_RECONFIG,
      value, sizeof(value), packet, sizeof(packet), &packet_size);
  if (status != CNET_SCTP_ASSOC_OK) return status;
  if (packet_size > cap) return CNET_SCTP_ASSOC_CAPACITY;

  memcpy(out, packet, packet_size);
  *written = packet_size;
  memcpy(s->reset_packet, packet, packet_size);
  s->reset_packet_size = packet_size;
  s->reset_streams = 0u;
  s->reset_request = s->next_reset_request++;
  s->reset_active = 1u;
  s->reconfig_kind = CNET_SCTP_RECONFIG_SSN_TSN_RESET;
  s->reset_retransmits = 0u;
  s->reset_rto_ms = s->config.t1_ms;
  s->reset_deadline_ms = now + s->reset_rto_ms;
  s->last_association_reset_request_ms = now;
  s->association_reset_request_sent = 1u;
  s->last_time_ms = now;
  return CNET_SCTP_ASSOC_OK;
}

static cnet_sctp_assoc_status start_local_add_outgoing(
    cnet_sctp_assoc *s, uint64_t now, uint16_t stream_count, int reciprocal,
    void *out, size_t cap, size_t *written) {
  uint8_t value[12] = {0};
  uint8_t packet[CNET_SCTP_ASSOC_MAX_PACKET];
  size_t packet_size = 0u;
  cnet_sctp_assoc_status status;
  if (s->reset_active || (reciprocal ?
      s->incoming_stream_change_pending :
      s->outgoing_stream_change_pending))
    return CNET_SCTP_ASSOC_BUSY;
  if (now > UINT64_MAX - s->config.t1_ms)
    return CNET_SCTP_ASSOC_CLOCK;

  wr16(value, SCTP_PARAM_ADD_OUTGOING);
  wr16(value + 2u, sizeof(value));
  wr32(value + 4u, s->next_reset_request);
  wr16(value + 8u, stream_count);
  /* Reserved is zero from initialization, as recommended by RFC 6525. */
  status = encode_one(s, s->peer_tag, SCTP_TYPE_RECONFIG,
      value, sizeof(value), packet, sizeof(packet), &packet_size);
  if (status != CNET_SCTP_ASSOC_OK) return status;
  if (packet_size > cap) return CNET_SCTP_ASSOC_CAPACITY;

  memcpy(out, packet, packet_size);
  *written = packet_size;
  memcpy(s->reset_packet, packet, packet_size);
  s->reset_packet_size = packet_size;
  s->reset_streams = 0u;
  s->add_outgoing_is_reciprocal = reciprocal ? 1u : 0u;
  s->add_outgoing_streams = stream_count;
  s->reset_request = s->next_reset_request++;
  s->reset_active = 1u;
  s->reconfig_kind = CNET_SCTP_RECONFIG_ADD_OUTGOING;
  s->reset_retransmits = 0u;
  s->reset_rto_ms = s->config.t1_ms;
  s->reset_deadline_ms = now + s->reset_rto_ms;
  s->last_time_ms = now;
  return CNET_SCTP_ASSOC_OK;
}

static cnet_sctp_assoc_status start_local_add_incoming(
    cnet_sctp_assoc *s, uint64_t now, uint16_t stream_count,
    void *out, size_t cap, size_t *written) {
  uint8_t value[12] = {0};
  uint8_t packet[CNET_SCTP_ASSOC_MAX_PACKET];
  size_t packet_size = 0u;
  cnet_sctp_assoc_status status;
  if (s->reset_active || s->outgoing_stream_change_pending)
    return CNET_SCTP_ASSOC_BUSY;
  if (now > UINT64_MAX - s->config.t1_ms)
    return CNET_SCTP_ASSOC_CLOCK;

  wr16(value, SCTP_PARAM_ADD_INCOMING);
  wr16(value + 2u, sizeof(value));
  wr32(value + 4u, s->next_reset_request);
  wr16(value + 8u, stream_count);
  status = encode_one(s, s->peer_tag, SCTP_TYPE_RECONFIG,
      value, sizeof(value), packet, sizeof(packet), &packet_size);
  if (status != CNET_SCTP_ASSOC_OK) return status;
  if (packet_size > cap) return CNET_SCTP_ASSOC_CAPACITY;

  memcpy(out, packet, packet_size);
  *written = packet_size;
  memcpy(s->reset_packet, packet, packet_size);
  s->reset_packet_size = packet_size;
  s->reset_streams = 0u;
  s->add_incoming_streams = stream_count;
  s->reset_request = s->next_reset_request++;
  s->reset_active = 1u;
  s->reconfig_kind = CNET_SCTP_RECONFIG_ADD_INCOMING;
  s->reset_retransmits = 0u;
  s->reset_rto_ms = s->config.t1_ms;
  s->reset_deadline_ms = now + s->reset_rto_ms;
  s->last_time_ms = now;
  return CNET_SCTP_ASSOC_OK;
}

static cnet_sctp_assoc_status start_local_add_streams(
    cnet_sctp_assoc *s, uint64_t now, uint16_t incoming_stream_count,
    uint16_t outgoing_stream_count,
    void *out, size_t cap, size_t *written) {
  uint8_t value[24] = {0};
  uint8_t packet[CNET_SCTP_ASSOC_MAX_PACKET];
  size_t packet_size = 0u;
  cnet_sctp_assoc_status status;
  if (s->reset_active || s->outgoing_stream_change_pending)
    return CNET_SCTP_ASSOC_BUSY;
  if (now > UINT64_MAX - s->config.t1_ms)
    return CNET_SCTP_ASSOC_CLOCK;

  wr16(value, SCTP_PARAM_ADD_OUTGOING);
  wr16(value + 2u, 12u);
  wr32(value + 4u, s->next_reset_request);
  wr16(value + 8u, outgoing_stream_count);
  wr16(value + 12u, SCTP_PARAM_ADD_INCOMING);
  wr16(value + 14u, 12u);
  wr32(value + 16u, s->next_reset_request + 1u);
  wr16(value + 20u, incoming_stream_count);
  status = encode_one(s, s->peer_tag, SCTP_TYPE_RECONFIG,
      value, sizeof(value), packet, sizeof(packet), &packet_size);
  if (status != CNET_SCTP_ASSOC_OK) return status;
  if (packet_size > cap) return CNET_SCTP_ASSOC_CAPACITY;

  memcpy(out, packet, packet_size);
  *written = packet_size;
  memcpy(s->reset_packet, packet, packet_size);
  s->reset_packet_size = packet_size;
  s->reset_streams = 0u;
  s->add_outgoing_streams = outgoing_stream_count;
  s->add_incoming_streams = incoming_stream_count;
  s->reset_request = s->next_reset_request;
  s->reset_paired_incoming_request = s->next_reset_request + 1u;
  s->next_reset_request += 2u;
  s->reset_active = 1u;
  s->reconfig_kind = CNET_SCTP_RECONFIG_ADD_STREAMS;
  s->reset_outgoing_complete = 0u;
  s->reset_paired_incoming_complete = 0u;
  s->add_paired_result = CNET_SCTP_STREAM_RESET_PERFORMED;
  s->reset_retransmits = 0u;
  s->reset_rto_ms = s->config.t1_ms;
  s->reset_deadline_ms = now + s->reset_rto_ms;
  s->last_time_ms = now;
  return CNET_SCTP_ASSOC_OK;
}

static cnet_sctp_assoc_status handle_add_incoming_request(
    cnet_sctp_assoc *s, uint64_t now,
    const sctp_reconfig_parameter *parameter,
    uint8_t *output, size_t cap, size_t *written) {
  const uint32_t sequence = rd32(parameter->bytes + 4u);
  const uint16_t count = rd16(parameter->bytes + 8u);
  cnet_sctp_assoc_stream_reset_result result;
  uint8_t response[CNET_SCTP_ASSOC_MAX_PACKET];
  size_t response_size = 0u;
  cnet_sctp_assoc_status status;

  if (sequence == s->last_peer_reset_request &&
      s->last_reset_response_size != 0u) {
    if (s->last_reset_response_size > cap)
      return CNET_SCTP_ASSOC_CAPACITY;
    memcpy(output, s->last_reset_response, s->last_reset_response_size);
    *written = s->last_reset_response_size;
    return CNET_SCTP_ASSOC_OK;
  }
  if (sequence != s->expected_peer_reset_request) {
    result = CNET_SCTP_STREAM_RESET_BAD_SEQUENCE;
  } else if (s->reset_active || s->incoming_stream_change_pending) {
    /* A successful Add Incoming consumes the Association's one retained
     * RE-CONFIG request slot until its corresponding Add Outgoing completes. */
    result = CNET_SCTP_STREAM_RESET_REQUEST_IN_PROGRESS;
  } else if (count == 0u) {
    result = CNET_SCTP_STREAM_RESET_NOTHING_TO_DO;
  } else if (count >
             CNET_SCTP_DATA_STREAMS - s->data.config.outbound_streams) {
    result = CNET_SCTP_STREAM_RESET_DENIED;
  } else {
    status = start_local_add_outgoing(s, now, count, 1,
        output, cap, written);
    if (status != CNET_SCTP_ASSOC_OK) return status;
    s->last_peer_reset_request = sequence;
    ++s->expected_peer_reset_request;
    memcpy(s->last_reset_response, output, *written);
    s->last_reset_response_size = *written;
    return CNET_SCTP_ASSOC_OK;
  }

  status = encode_reset_response(s, sequence, result,
      response, &response_size);
  if (status != CNET_SCTP_ASSOC_OK) return status;
  if (response_size > cap) return CNET_SCTP_ASSOC_CAPACITY;
  if (sequence == s->expected_peer_reset_request &&
      result != CNET_SCTP_STREAM_RESET_REQUEST_IN_PROGRESS) {
    s->last_peer_reset_request = sequence;
    ++s->expected_peer_reset_request;
    memcpy(s->last_reset_response, response, response_size);
    s->last_reset_response_size = response_size;
    if (reset_result_success(result))
      record_stream_change(s, 1, result);
  }
  memcpy(output, response, response_size);
  *written = response_size;
  return CNET_SCTP_ASSOC_OK;
}

static cnet_sctp_assoc_status handle_add_streams_request(
    cnet_sctp_assoc *s, uint64_t now,
    const sctp_reconfig_parameter *add_outgoing,
    const sctp_reconfig_parameter *add_incoming,
    uint8_t *output, size_t cap, size_t *written) {
  const uint32_t outgoing_sequence = rd32(add_outgoing->bytes + 4u);
  const uint32_t incoming_sequence = rd32(add_incoming->bytes + 4u);
  const uint16_t inbound_count = rd16(add_outgoing->bytes + 8u);
  const uint16_t outbound_count = rd16(add_incoming->bytes + 8u);
  cnet_sctp_assoc_stream_reset_result outgoing_result;
  cnet_sctp_assoc_stream_reset_result incoming_result;
  cnet_sctp_assoc_stream_reset_result event_result;
  uint8_t corresponding[CNET_SCTP_ASSOC_MAX_PACKET];
  uint8_t response[CNET_SCTP_ASSOC_MAX_PACKET];
  size_t corresponding_size = 0u, response_size = 0u;
  cnet_sctp_assoc_status status;
  int started_corresponding = 0;

  if (incoming_sequence == s->last_peer_reset_request &&
      outgoing_sequence + 1u == incoming_sequence &&
      s->last_reset_response_size != 0u) {
    if (s->last_reset_response_size > cap)
      return CNET_SCTP_ASSOC_CAPACITY;
    memcpy(output, s->last_reset_response, s->last_reset_response_size);
    *written = s->last_reset_response_size;
    return CNET_SCTP_ASSOC_OK;
  }
  if (outgoing_sequence != s->expected_peer_reset_request ||
      outgoing_sequence + 1u != incoming_sequence) {
    status = encode_reset_response_pair(s,
        outgoing_sequence, CNET_SCTP_STREAM_RESET_BAD_SEQUENCE,
        incoming_sequence, CNET_SCTP_STREAM_RESET_BAD_SEQUENCE,
        response, &response_size);
    if (status != CNET_SCTP_ASSOC_OK) return status;
    if (response_size > cap) return CNET_SCTP_ASSOC_CAPACITY;
    memcpy(output, response, response_size);
    *written = response_size;
    return CNET_SCTP_ASSOC_OK;
  }
  if (s->reset_active || s->incoming_stream_change_pending) {
    outgoing_result = CNET_SCTP_STREAM_RESET_REQUEST_IN_PROGRESS;
    incoming_result = CNET_SCTP_STREAM_RESET_REQUEST_IN_PROGRESS;
  } else {
    outgoing_result = inbound_count == 0u ?
        CNET_SCTP_STREAM_RESET_NOTHING_TO_DO :
        (inbound_count >
             CNET_SCTP_DATA_STREAMS - s->data.config.inbound_streams ?
         CNET_SCTP_STREAM_RESET_DENIED :
         CNET_SCTP_STREAM_RESET_PERFORMED);
    incoming_result = outbound_count == 0u ?
        CNET_SCTP_STREAM_RESET_NOTHING_TO_DO :
        (outbound_count >
             CNET_SCTP_DATA_STREAMS - s->data.config.outbound_streams ?
         CNET_SCTP_STREAM_RESET_DENIED :
         CNET_SCTP_STREAM_RESET_PERFORMED);
  }
  if (outgoing_result == CNET_SCTP_STREAM_RESET_REQUEST_IN_PROGRESS) {
    status = encode_reset_response_pair(s,
        outgoing_sequence, outgoing_result,
        incoming_sequence, incoming_result,
        response, &response_size);
  } else if (incoming_result == CNET_SCTP_STREAM_RESET_PERFORMED) {
    status = start_local_add_outgoing(s, now, outbound_count, 1,
        corresponding, sizeof(corresponding), &corresponding_size);
    if (status != CNET_SCTP_ASSOC_OK) return status;
    s->add_outgoing_is_paired = 1u;
    s->add_paired_result = (uint8_t)outgoing_result;
    s->reset_needs_initial_publish = 1u;
    status = encode_reset_response(s, outgoing_sequence, outgoing_result,
        response, &response_size);
    started_corresponding = 1;
  } else {
    status = encode_reset_response_pair(s,
        outgoing_sequence, outgoing_result,
        incoming_sequence, incoming_result,
        response, &response_size);
  }
  if (status != CNET_SCTP_ASSOC_OK) return status;
  if (response_size > cap) return CNET_SCTP_ASSOC_CAPACITY;
  if (outgoing_result == CNET_SCTP_STREAM_RESET_REQUEST_IN_PROGRESS) {
    memcpy(output, response, response_size);
    *written = response_size;
    return CNET_SCTP_ASSOC_OK;
  }

  s->last_peer_reset_request = incoming_sequence;
  s->expected_peer_reset_request = incoming_sequence + 1u;
  memcpy(s->last_reset_response, response, response_size);
  s->last_reset_response_size = response_size;
  if (reset_result_success(outgoing_result)) {
    s->data.config.inbound_streams = (uint16_t)(
        s->data.config.inbound_streams + inbound_count);
  }
  if (!started_corresponding &&
      (reset_result_success(outgoing_result) ||
       reset_result_success(incoming_result))) {
    event_result = !reset_result_success(outgoing_result) ?
        outgoing_result : incoming_result;
    record_stream_change(s, 1, event_result);
  }
  memcpy(output, response, response_size);
  *written = response_size;
  return CNET_SCTP_ASSOC_OK;
}

static void acknowledge_local_incoming_reset(cnet_sctp_assoc *s) {
  s->awaiting_local_incoming_reset = s->reset_streams;
  s->awaiting_local_incoming_reset_request = s->reset_request;
  clear_active_reset(s, 0);
}

static void finish_awaiting_local_incoming_reset(cnet_sctp_assoc *s,
    uint16_t completed_streams,
    cnet_sctp_assoc_stream_reset_result missing_result) {
  const uint16_t missing = (uint16_t)(
      s->awaiting_local_incoming_reset & (uint16_t)~completed_streams);
  if (missing != 0u)
    record_incoming_reset_result(s, missing, missing_result);
  s->awaiting_local_incoming_reset = 0u;
  s->awaiting_local_incoming_reset_request = 0u;
}

static cnet_sctp_assoc_status handle_association_reset_request(
    cnet_sctp_assoc *s, uint64_t now,
    const sctp_reconfig_parameter *parameter,
    uint8_t *output, size_t cap, size_t *written) {
  const uint32_t sequence = rd32(parameter->bytes + 4u);
  const uint32_t sender_next_tsn = s->data.next_tsn;
  const uint32_t receiver_next_tsn =
      s->data.cumulative_rx + 1u + UINT32_C(0x80000000);
  cnet_sctp_assoc_stream_reset_result result;
  uint8_t response[CNET_SCTP_ASSOC_MAX_PACKET];
  size_t response_size = 0u;
  cnet_sctp_assoc_status status;

  if (sequence == s->last_peer_reset_request &&
      s->last_reset_response_size != 0u) {
    if (s->last_reset_response_size > cap)
      return CNET_SCTP_ASSOC_CAPACITY;
    memcpy(output, s->last_reset_response, s->last_reset_response_size);
    *written = s->last_reset_response_size;
    return CNET_SCTP_ASSOC_OK;
  }
  if (sequence != s->expected_peer_reset_request) {
    result = CNET_SCTP_STREAM_RESET_BAD_SEQUENCE;
  } else if (s->association_reset_pending) {
    /* Preserve the single Owner-pulled event slot and let the peer retry the
     * same sequence after it is drained. */
    result = CNET_SCTP_STREAM_RESET_IN_PROGRESS;
  } else if (s->reset_active ||
             s->deferred_peer_reset_active ||
             s->peer_incoming_reset_pending ||
             s->awaiting_local_incoming_reset != 0u ||
             s->reciprocal_reset_pending != 0u ||
             s->awaiting_peer_reciprocal != 0u ||
             s->reciprocal_reset_failed != 0u) {
    result = CNET_SCTP_STREAM_RESET_REQUEST_IN_PROGRESS;
  } else if (!cnet_sctp_data_association_reset_ready(&s->data)) {
    /* The same request can complete after retained/unread DATA drains.
     * Result 6 restarts the requester's timer without consuming sequence. */
    result = CNET_SCTP_STREAM_RESET_IN_PROGRESS;
  } else {
    result = CNET_SCTP_STREAM_RESET_PERFORMED;
  }
  status = encode_association_reset_response(s, sequence, result,
      sender_next_tsn, receiver_next_tsn, response, &response_size);
  if (status != CNET_SCTP_ASSOC_OK) return status;
  if (response_size > cap) return CNET_SCTP_ASSOC_CAPACITY;

  if (sequence == s->expected_peer_reset_request &&
      result != CNET_SCTP_STREAM_RESET_IN_PROGRESS) {
    if (result == CNET_SCTP_STREAM_RESET_PERFORMED) {
      if (cnet_sctp_data_reset_association(&s->data, now,
              sender_next_tsn, receiver_next_tsn) != CNET_SCTP_DATA_OK)
        return CNET_SCTP_ASSOC_PROTOCOL;
      record_association_reset(s, 0, result,
          sender_next_tsn, receiver_next_tsn);
    }
    s->last_peer_reset_request = sequence;
    ++s->expected_peer_reset_request;
    memcpy(s->last_reset_response, response, response_size);
    s->last_reset_response_size = response_size;
  }
  memcpy(output, response, response_size);
  *written = response_size;
  return CNET_SCTP_ASSOC_OK;
}

static cnet_sctp_assoc_status handle_reconfig(cnet_sctp_assoc *s,
    uint64_t now, const cnet_sctp_wire_header *header,
    const cnet_sctp_wire_chunk *chunk, uint8_t *output, size_t cap,
    size_t *written) {
  sctp_reconfig_view view;
  cnet_sctp_assoc staged;
  const sctp_reconfig_parameter *outgoing_request = NULL;
  const sctp_reconfig_parameter *incoming_request = NULL;
  const sctp_reconfig_parameter *add_outgoing_request = NULL;
  const sctp_reconfig_parameter *add_incoming_request = NULL;
  const sctp_reconfig_parameter *association_reset_request = NULL;
  uint8_t response[CNET_SCTP_ASSOC_MAX_PACKET];
  size_t response_size = 0u, i;
  cnet_sctp_assoc_status status;
  if (s->state != CNET_SCTP_ASSOC_ESTABLISHED || !s->data_ready ||
      header->verification_tag != s->local_tag ||
      !reconfig_preflight(chunk, &view))
    return CNET_SCTP_ASSOC_PROTOCOL;
  if (!s->peer_reconfig_supported)
    return CNET_SCTP_ASSOC_PROTOCOL;
  memcpy(&staged, s, sizeof(staged));
  /* Apply response parameters first, matching the RFC's allowed
   * Response+Outgoing-Reset ordering, but commit only with the whole packet. */
  for (i = 0u; i < view.count; ++i) {
    if (view.parameters[i].type == SCTP_PARAM_RECONFIG_RESPONSE) {
      status = apply_reconfig_response(&staged, now, &view.parameters[i]);
      if (status != CNET_SCTP_ASSOC_OK) return status;
    } else if (view.parameters[i].type == SCTP_PARAM_OUTGOING_RESET) {
      outgoing_request = &view.parameters[i];
    } else if (view.parameters[i].type == SCTP_PARAM_INCOMING_RESET) {
      incoming_request = &view.parameters[i];
    } else if (view.parameters[i].type == SCTP_PARAM_ADD_OUTGOING) {
      add_outgoing_request = &view.parameters[i];
    } else if (view.parameters[i].type == SCTP_PARAM_ADD_INCOMING) {
      add_incoming_request = &view.parameters[i];
    } else if (view.parameters[i].type == SCTP_PARAM_SSN_TSN_RESET) {
      association_reset_request = &view.parameters[i];
    } else {
      return CNET_SCTP_ASSOC_PROTOCOL;
    }
  }
  if (association_reset_request) {
    if (view.count != 1u) return CNET_SCTP_ASSOC_PROTOCOL;
    status = handle_association_reset_request(&staged, now,
        association_reset_request, output, cap, written);
    if (status != CNET_SCTP_ASSOC_OK) return status;
    staged.last_time_ms = now;
    memcpy(s, &staged, sizeof(staged));
    return CNET_SCTP_ASSOC_OK;
  }
  if (add_outgoing_request || add_incoming_request) {
    if (add_outgoing_request && add_incoming_request) {
      status = handle_add_streams_request(&staged, now,
          add_outgoing_request, add_incoming_request,
          output, cap, written);
    } else if (view.count != 1u) {
      return CNET_SCTP_ASSOC_PROTOCOL;
    } else if (add_outgoing_request) {
      status = handle_add_outgoing_request(&staged, add_outgoing_request,
          output, cap, written);
    } else {
      status = handle_add_incoming_request(&staged, now, add_incoming_request,
          output, cap, written);
    }
    if (status != CNET_SCTP_ASSOC_OK) return status;
    staged.last_time_ms = now;
    memcpy(s, &staged, sizeof(staged));
    return CNET_SCTP_ASSOC_OK;
  }
  if (outgoing_request && incoming_request) {
    const uint32_t outgoing_sequence =
        rd32(outgoing_request->bytes + 4u);
    const uint32_t incoming_sequence =
        rd32(incoming_request->bytes + 4u);
    const uint32_t last_assigned =
        rd32(outgoing_request->bytes + 12u);
    cnet_sctp_assoc_stream_reset_result outgoing_result;
    cnet_sctp_assoc_stream_reset_result incoming_result;
    uint8_t outgoing_response[12];
    uint16_t incoming_mask = 0u, outgoing_streams = 0u;
    int started_corresponding = 0;

    /* Each request parameter consumes its own monotonically increasing
     * sequence number; RFC 6525 Appendix A encodes OUT:X then IN:X+1. */
    if (incoming_sequence == staged.last_peer_reset_request &&
        outgoing_sequence + 1u == incoming_sequence &&
        staged.last_reset_response_size != 0u) {
      if (staged.last_reset_response_size > cap)
        return CNET_SCTP_ASSOC_CAPACITY;
      memcpy(output, staged.last_reset_response,
             staged.last_reset_response_size);
      *written = staged.last_reset_response_size;
      staged.last_time_ms = now;
      memcpy(s, &staged, sizeof(staged));
      return CNET_SCTP_ASSOC_OK;
    }
    if (outgoing_sequence != staged.expected_peer_reset_request ||
        outgoing_sequence + 1u != incoming_sequence) {
      status = encode_reset_response_pair(&staged,
          outgoing_sequence, CNET_SCTP_STREAM_RESET_BAD_SEQUENCE,
          incoming_sequence, CNET_SCTP_STREAM_RESET_BAD_SEQUENCE,
          response, &response_size);
      if (status != CNET_SCTP_ASSOC_OK) return status;
      if (response_size > cap) return CNET_SCTP_ASSOC_CAPACITY;
      memcpy(output, response, response_size);
      *written = response_size;
      staged.last_time_ms = now;
      memcpy(s, &staged, sizeof(staged));
      return CNET_SCTP_ASSOC_OK;
    }

    /* Keep the two-request admission atomic. A collision with any existing
     * local/deferred transaction asks the peer to retry the original chunk. */
    if (staged.reset_active ||
        staged.awaiting_local_incoming_reset != 0u ||
        staged.deferred_peer_reset_active ||
        staged.peer_incoming_reset_pending) {
      outgoing_result = CNET_SCTP_STREAM_RESET_REQUEST_IN_PROGRESS;
      incoming_result = CNET_SCTP_STREAM_RESET_REQUEST_IN_PROGRESS;
    } else {
      outgoing_result =
          evaluate_peer_reset(&staged, outgoing_request, &incoming_mask);
      incoming_result = evaluate_incoming_reset(
          &staged, incoming_request, &outgoing_streams);
      if (outgoing_result == CNET_SCTP_STREAM_RESET_REQUEST_IN_PROGRESS ||
          incoming_result == CNET_SCTP_STREAM_RESET_REQUEST_IN_PROGRESS) {
        outgoing_result = CNET_SCTP_STREAM_RESET_REQUEST_IN_PROGRESS;
        incoming_result = CNET_SCTP_STREAM_RESET_REQUEST_IN_PROGRESS;
      }
    }
    if (outgoing_result == CNET_SCTP_STREAM_RESET_REQUEST_IN_PROGRESS) {
      status = encode_reset_response_pair(&staged,
          outgoing_sequence, outgoing_result,
          incoming_sequence, incoming_result,
          response, &response_size);
      if (status != CNET_SCTP_ASSOC_OK) return status;
      if (response_size > cap) return CNET_SCTP_ASSOC_CAPACITY;
      memcpy(output, response, response_size);
      *written = response_size;
      staged.last_time_ms = now;
      memcpy(s, &staged, sizeof(staged));
      return CNET_SCTP_ASSOC_OK;
    }

    write_reset_response(
        outgoing_response, outgoing_sequence, outgoing_result);
    if (incoming_result == CNET_SCTP_STREAM_RESET_PERFORMED &&
        (staged.outgoing_reset_events & outgoing_streams) == 0u &&
        cnet_sctp_data_outbound_drained(&staged.data)) {
      status = start_local_reset_with_prefix(&staged, now,
          outgoing_streams, 1, incoming_sequence,
          outgoing_response, sizeof(outgoing_response),
          response, sizeof(response), &response_size);
      if (status != CNET_SCTP_ASSOC_OK) return status;
      started_corresponding = 1;
    } else if (incoming_result == CNET_SCTP_STREAM_RESET_PERFORMED) {
      status = encode_reset_response(&staged,
          outgoing_sequence, outgoing_result,
          response, &response_size);
      if (status != CNET_SCTP_ASSOC_OK) return status;
    } else {
      status = encode_reset_response_pair(&staged,
          outgoing_sequence, outgoing_result,
          incoming_sequence, incoming_result,
          response, &response_size);
      if (status != CNET_SCTP_ASSOC_OK) return status;
    }
    if (response_size > cap) return CNET_SCTP_ASSOC_CAPACITY;

    staged.last_peer_reset_request = incoming_sequence;
    staged.expected_peer_reset_request = incoming_sequence + 1u;
    memcpy(staged.last_reset_response, response, response_size);
    staged.last_reset_response_size = response_size;
    if (outgoing_result == CNET_SCTP_STREAM_RESET_PERFORMED) {
      apply_peer_reset(&staged, incoming_mask, 0u);
    } else if (outgoing_result == CNET_SCTP_STREAM_RESET_IN_PROGRESS) {
      staged.deferred_peer_reset_active = 1u;
      staged.deferred_peer_reset_streams = incoming_mask;
      staged.deferred_peer_reset_request = outgoing_sequence;
      staged.deferred_peer_reset_last_tsn = last_assigned;
    }
    if (incoming_result == CNET_SCTP_STREAM_RESET_PERFORMED &&
        !started_corresponding) {
      staged.peer_incoming_reset_pending = 1u;
      staged.peer_incoming_reset_streams = outgoing_streams;
      staged.peer_incoming_reset_request = incoming_sequence;
      staged.reciprocal_reset_pending |= outgoing_streams;
    }
    memcpy(output, response, response_size);
    *written = response_size;
    staged.last_time_ms = now;
    memcpy(s, &staged, sizeof(staged));
    return CNET_SCTP_ASSOC_OK;
  }
  if (outgoing_request) {
    const uint32_t sequence = rd32(outgoing_request->bytes + 4u);
    const uint32_t response_sequence = rd32(outgoing_request->bytes + 8u);
    const uint32_t last_assigned = rd32(outgoing_request->bytes + 12u);
    cnet_sctp_assoc_stream_reset_result result;
    uint16_t requested_streams = 0u;
    uint16_t mask = 0u;
    int answers_incoming = 0;
    int answers_active_incoming = 0;
    int answers_paired_incoming = 0;
    if (sequence == staged.last_peer_reset_request &&
        staged.last_reset_response_size != 0u) {
      if (staged.last_reset_response_size > cap)
        return CNET_SCTP_ASSOC_CAPACITY;
      memcpy(output, staged.last_reset_response,
             staged.last_reset_response_size);
      *written = staged.last_reset_response_size;
      staged.last_time_ms = now;
      memcpy(s, &staged, sizeof(staged));
      return CNET_SCTP_ASSOC_OK;
    }
    if (staged.reset_active && staged.reset_requests_incoming &&
        response_sequence == staged.reset_request) {
      answers_incoming = 1;
      answers_active_incoming = 1;
      requested_streams = staged.reset_streams;
    } else if (staged.awaiting_local_incoming_reset != 0u &&
               response_sequence ==
                   staged.awaiting_local_incoming_reset_request) {
      answers_incoming = 1;
      requested_streams = staged.awaiting_local_incoming_reset;
    } else if (staged.reset_active &&
               staged.reset_has_paired_incoming &&
               !staged.reset_paired_incoming_complete &&
               response_sequence ==
                   staged.reset_paired_incoming_request) {
      answers_incoming = 1;
      answers_paired_incoming = 1;
      requested_streams = staged.reset_paired_incoming_streams;
    }
    if (sequence != staged.expected_peer_reset_request) {
      result = CNET_SCTP_STREAM_RESET_BAD_SEQUENCE;
    } else if (staged.deferred_peer_reset_active ||
               staged.peer_incoming_reset_pending) {
      result = CNET_SCTP_STREAM_RESET_REQUEST_IN_PROGRESS;
    } else {
      result = evaluate_peer_reset(&staged, outgoing_request, &mask);
    }
    status = encode_reset_response(&staged, sequence, result,
        response, &response_size);
    if (status != CNET_SCTP_ASSOC_OK) return status;
    if (response_size > cap) return CNET_SCTP_ASSOC_CAPACITY;
    /* RFC 6525 E1: the corresponding Outgoing Reset acknowledges our
     * Incoming Reset even when its TSN barrier is still in progress. */
    if (answers_active_incoming)
      acknowledge_local_incoming_reset(&staged);
    if (sequence == staged.expected_peer_reset_request &&
        result != CNET_SCTP_STREAM_RESET_REQUEST_IN_PROGRESS) {
      const uint16_t suppress_reciprocal = answers_incoming ?
          (uint16_t)(requested_streams & mask) : 0u;
      staged.last_peer_reset_request = sequence;
      ++staged.expected_peer_reset_request;
      memcpy(staged.last_reset_response, response, response_size);
      staged.last_reset_response_size = response_size;
      if (result == CNET_SCTP_STREAM_RESET_PERFORMED) {
        apply_peer_reset(&staged, mask, suppress_reciprocal);
      } else if (result == CNET_SCTP_STREAM_RESET_IN_PROGRESS) {
        staged.deferred_peer_reset_active = 1u;
        staged.deferred_peer_reset_streams = mask;
        staged.deferred_peer_reset_suppress_reciprocal =
            suppress_reciprocal;
        staged.deferred_peer_reset_answers_incoming =
            suppress_reciprocal != 0u && !answers_paired_incoming;
        staged.deferred_peer_reset_answers_pair =
            answers_paired_incoming != 0;
        staged.deferred_peer_reset_request = sequence;
        staged.deferred_peer_reset_last_tsn = last_assigned;
      }
    }
    if (answers_incoming) {
      if (result == CNET_SCTP_STREAM_RESET_PERFORMED) {
        if (answers_paired_incoming)
          finish_paired_incoming_reset(&staged, mask,
              CNET_SCTP_STREAM_RESET_DENIED);
        else
          finish_awaiting_local_incoming_reset(&staged, mask,
              CNET_SCTP_STREAM_RESET_DENIED);
      } else if (result == CNET_SCTP_STREAM_RESET_IN_PROGRESS) {
        if ((requested_streams & mask) == 0u) {
          if (answers_paired_incoming)
            finish_paired_incoming_reset(&staged, 0u,
                CNET_SCTP_STREAM_RESET_DENIED);
          else
            finish_awaiting_local_incoming_reset(&staged, 0u,
                CNET_SCTP_STREAM_RESET_DENIED);
        }
      } else if (result != CNET_SCTP_STREAM_RESET_REQUEST_IN_PROGRESS) {
        if (answers_paired_incoming)
          finish_paired_incoming_reset(&staged, 0u, result);
        else
          finish_awaiting_local_incoming_reset(&staged, 0u, result);
      }
    }
    memcpy(output, response, response_size);
    *written = response_size;
  }
  if (incoming_request) {
    const uint32_t sequence = rd32(incoming_request->bytes + 4u);
    cnet_sctp_assoc_stream_reset_result result;
    uint16_t streams = 0u;
    if (sequence == staged.last_peer_reset_request) {
      if (staged.last_reset_response_size != 0u) {
        if (staged.last_reset_response_size > cap)
          return CNET_SCTP_ASSOC_CAPACITY;
        memcpy(output, staged.last_reset_response,
               staged.last_reset_response_size);
        *written = staged.last_reset_response_size;
      } else if (!(staged.peer_incoming_reset_pending &&
                   staged.peer_incoming_reset_request == sequence)) {
        return CNET_SCTP_ASSOC_PROTOCOL;
      }
      staged.last_time_ms = now;
      memcpy(s, &staged, sizeof(staged));
      return CNET_SCTP_ASSOC_OK;
    }
    if (sequence != staged.expected_peer_reset_request) {
      result = CNET_SCTP_STREAM_RESET_BAD_SEQUENCE;
    } else {
      result = evaluate_incoming_reset(&staged, incoming_request, &streams);
    }
    if (result == CNET_SCTP_STREAM_RESET_PERFORMED) {
      staged.last_peer_reset_request = sequence;
      ++staged.expected_peer_reset_request;
      staged.last_reset_response_size = 0u;
      memset(staged.last_reset_response, 0,
             sizeof(staged.last_reset_response));
      if (!staged.reset_active &&
          (staged.outgoing_reset_events & streams) == 0u &&
          cnet_sctp_data_outbound_drained(&staged.data)) {
        status = start_local_reset(&staged, now, streams, 1, sequence,
            response, sizeof(response), &response_size);
        if (status != CNET_SCTP_ASSOC_OK) return status;
        if (response_size > cap) return CNET_SCTP_ASSOC_CAPACITY;
        memcpy(staged.last_reset_response, response, response_size);
        staged.last_reset_response_size = response_size;
        memcpy(output, response, response_size);
        *written = response_size;
      } else {
        staged.peer_incoming_reset_pending = 1u;
        staged.peer_incoming_reset_streams = streams;
        staged.peer_incoming_reset_request = sequence;
        staged.reciprocal_reset_pending |= streams;
      }
    } else {
      status = encode_reset_response(&staged, sequence, result,
          response, &response_size);
      if (status != CNET_SCTP_ASSOC_OK) return status;
      if (response_size > cap) return CNET_SCTP_ASSOC_CAPACITY;
      if (sequence == staged.expected_peer_reset_request &&
          result != CNET_SCTP_STREAM_RESET_REQUEST_IN_PROGRESS) {
        staged.last_peer_reset_request = sequence;
        ++staged.expected_peer_reset_request;
        memcpy(staged.last_reset_response, response, response_size);
        staged.last_reset_response_size = response_size;
      }
      memcpy(output, response, response_size);
      *written = response_size;
    }
  }
  staged.last_time_ms = now;
  memcpy(s, &staged, sizeof(staged));
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
  clear_deferred_peer_reset(s);
  clear_peer_incoming_reset(s);
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
    if (s->reset_active)
      (void)finish_active_reconfig(s, CNET_SCTP_STREAM_RESET_DENIED);
    s->reciprocal_reset_pending = 0u;
    s->awaiting_peer_reciprocal = 0u;
    s->reciprocal_reset_failed = 0u;
    clear_deferred_peer_reset(s);
    clear_peer_incoming_reset(s);
    s->state = CNET_SCTP_ASSOC_SHUTDOWN_RECEIVED;
    s->last_time_ms = now;
    return CNET_SCTP_ASSOC_OK;
  }
  status = begin_shutdown_control(s, now, 1, out, cap, written);
  if (status != CNET_SCTP_ASSOC_OK) return status;
  memcpy(&s->data, &candidate, sizeof(candidate));
  if (s->reset_active)
    (void)finish_active_reconfig(s, CNET_SCTP_STREAM_RESET_DENIED);
  s->reciprocal_reset_pending = 0u;
  s->awaiting_peer_reciprocal = 0u;
  s->reciprocal_reset_failed = 0u;
  clear_deferred_peer_reset(s);
  clear_peer_incoming_reset(s);
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
  if (written) *written = 0u;
  cnet_sctp_wire_header header;
  cnet_sctp_wire_decoder decoder;
  cnet_sctp_wire_decoder preflight_decoder;
  cnet_sctp_wire_decoder classify_decoder;
  cnet_sctp_wire_chunk chunk;
  cnet_sctp_wire_chunk second;
  cnet_sctp_wire_chunk terminal_chunk = {0};
  cnet_sctp_wire_decoder terminal_cursor;
  cnet_sctp_assoc_status status;
  uint64_t prior_time;
  size_t i, terminal_index = 0u;
  int all_unknown, terminal_abort = 0;
  if (written) *written = 0u;
  status = guard(s);
  if (status != CNET_SCTP_ASSOC_OK) return status;
  if (!packet || !output || !written) return CNET_SCTP_ASSOC_INVALID;
  if (s->state == CNET_SCTP_ASSOC_STOPPED || s->state == CNET_SCTP_ASSOC_FAILED)
    return CNET_SCTP_ASSOC_CLOSED_ERROR;
  if ((status = check_time(s, now)) != CNET_SCTP_ASSOC_OK)
    return status;
  /* Local egress and retained control packets stay at 1200 bytes. WebRTC
   * peers may deliver a bounded 1280-byte DATA packet after DTLS framing is
   * removed; no larger packet can reach the public Wire decoder here. */
  if (packet_size > CNET_SCTP_ASSOC_MAX_INGRESS_PACKET)
    return CNET_SCTP_ASSOC_CAPACITY;
  if (cnet_sctp_wire_decode(packet, packet_size, &header, &decoder) !=
      CNET_SCTP_WIRE_OK) return CNET_SCTP_ASSOC_PROTOCOL;
  if (packet_size > CNET_SCTP_ASSOC_MAX_PACKET) {
    cnet_sctp_wire_decoder ingress = decoder;
    int has_data = 0;
    for (i = 0u; i < header.chunk_count; ++i) {
      cnet_sctp_wire_chunk incoming;
      if (cnet_sctp_wire_next_chunk(&ingress, &incoming) !=
          CNET_SCTP_WIRE_OK)
        return CNET_SCTP_ASSOC_PROTOCOL;
      if (incoming.type == 0u || incoming.type == SCTP_TYPE_I_DATA) {
        has_data = 1;
      } else if (i != 0u || incoming.type != 3u) {
        /* Do not expand retained control, unknown-Chunk reporting or ABORT
         * reason bounds merely because peer DATA ingress is larger. */
        return CNET_SCTP_ASSOC_CAPACITY;
      }
    }
    if (!has_data) return CNET_SCTP_ASSOC_CAPACITY;
  }
  if (header.source_port != s->config.remote_port ||
      header.destination_port != s->config.local_port)
    return CNET_SCTP_ASSOC_PROTOCOL;
  preflight_decoder = decoder;
  classify_decoder = decoder;
  if (cnet_sctp_wire_next_chunk(&decoder, &chunk) != CNET_SCTP_WIRE_OK)
    return CNET_SCTP_ASSOC_PROTOCOL;
  /* Find the first ABORT. Its index defines which Chunks are processed;
   * later Chunks are framing-checked by Wire but semantically ignored. */
  terminal_cursor = preflight_decoder;
  for (i = 0u; i < header.chunk_count; ++i) {
    cnet_sctp_wire_chunk candidate;
    if (cnet_sctp_wire_next_chunk(&terminal_cursor, &candidate) !=
        CNET_SCTP_WIRE_OK)
      return CNET_SCTP_ASSOC_PROTOCOL;
    if (candidate.type == SCTP_TYPE_ABORT) {
      terminal_chunk = candidate;
      terminal_index = i;
      terminal_abort = 1;
      break;
    }
  }
  /* INIT uses tag zero. Plain ABORT uses our local tag; reflected T=1
   * ABORT uses the nonzero Peer Tag. All other packet types use Local Tag. */
  if (chunk.type == SCTP_TYPE_INIT) {
    if (header.verification_tag != 0u)
      return CNET_SCTP_ASSOC_PROTOCOL;
  } else if (terminal_abort) {
    const uint32_t expected_tag = (terminal_chunk.flags & 1u) ?
        s->peer_tag : s->local_tag;
    if (expected_tag == 0u || header.verification_tag != expected_tag)
      return CNET_SCTP_ASSOC_PROTOCOL;
  } else if (header.verification_tag != s->local_tag) {
    return CNET_SCTP_ASSOC_PROTOCOL;
  }
  if (terminal_abort) {
    if (!assoc_abort_bundle_preflight(&preflight_decoder,
        header.chunk_count, terminal_index))
      return CNET_SCTP_ASSOC_PROTOCOL;
    if (s->state == CNET_SCTP_ASSOC_CLOSED)
      return CNET_SCTP_ASSOC_PROTOCOL;
    /* Capture metadata before its DATA TCB is destroyed. Accepted
     * prefix-SACK progress is projected from an owned candidate, not by
     * mutating the live TCB or sending an ACK to the aborting peer. */
    record_peer_abort(s, &terminal_chunk);
    /* RFC 9260 §8.5.1 B permits reflected-tag T=1 ABORT, but its packet
     * verification tag is the PEER tag, not our ordinary LOCAL tag. Any
     * preceding SACK in that reflected packet is not admitted as normal
     * association traffic and MUST NOT credit a real DATA ticket. Still
     * accept the terminal ABORT without inventing cumulative success. */
    if (s->data_ready && terminal_index != 0u &&
        (terminal_chunk.flags & 1u) == 0u)
      s->peer_abort_info.last_acked_tsn =
          preview_abort_prefix_last_ack(s, now, &preflight_decoder,
              terminal_index);
    if (s->data_ready) {
      if (s->reset_active)
        (void)finish_active_reconfig(s, CNET_SCTP_STREAM_RESET_DENIED);
      s->reciprocal_reset_pending = 0u;
      s->awaiting_peer_reciprocal = 0u;
      s->reciprocal_reset_failed = 0u;
      clear_deferred_peer_reset(s);
      clear_peer_incoming_reset(s);
      (void)cnet_sctp_data_stop(&s->data);
      memset(&s->data, 0, sizeof(s->data));
      s->data_ready = 0u;
    }
    s->state = CNET_SCTP_ASSOC_FAILED;
    s->deadline_ms = 0u;
    s->t5_shutdown_deadline_ms = 0u;
    s->saved_size = 0u;
    memset(s->saved_packet, 0, sizeof(s->saved_packet));
    s->last_time_ms = now;
    return CNET_SCTP_ASSOC_OK;
  }
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
               chunk.type != 0u && chunk.type != SCTP_TYPE_I_DATA &&
               chunk.type != 3u) {
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
  /* RFC 8260 2.2.1/2.3.1 forbids mixing the legacy and interleaved data
   * families after negotiation. A well-framed violation on a live TCB is
   * terminal and receives an ABORT; short output capacity commits nothing. */
  if (s->data_ready &&
      packet_violates_interleaving_mode(&preflight_decoder,
          header.chunk_count, s->peer_interleaving_supported != 0u))
    return respond_abort(s, s->peer_tag, 1, output, cap, written);
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
    case SCTP_TYPE_I_DATA:
    case 3u: /* SACK */
    case SCTP_TYPE_FORWARD_TSN:
    case SCTP_TYPE_I_FORWARD_TSN:
      if (!s->data_ready ||
          (header.chunk_count != 1u &&
           s->state != CNET_SCTP_ASSOC_ESTABLISHED &&
           s->state != CNET_SCTP_ASSOC_SHUTDOWN_PENDING &&
           s->state != CNET_SCTP_ASSOC_SHUTDOWN_SENT &&
           s->state != CNET_SCTP_ASSOC_SHUTDOWN_RECEIVED)) {
        s->last_time_ms = prior_time;
        return CNET_SCTP_ASSOC_PROTOCOL;
      }
      if (chunk.type == 0u || chunk.type == SCTP_TYPE_I_DATA) {
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
      /* Admitted single ABORT was handled before packet-triggered T5.
       * Retain fail-closed behavior if a new bundle class reaches dispatch. */
      s->last_time_ms = prior_time;
      return CNET_SCTP_ASSOC_PROTOCOL;
    case SCTP_TYPE_COOKIE_ACK:
      return dispatch_result(s, handle_cookie_ack(s, now, &header, &chunk),
          prior_time);
    case SCTP_TYPE_RECONFIG:
      if (header.chunk_count != 1u) {
        s->last_time_ms = prior_time;
        return CNET_SCTP_ASSOC_PROTOCOL;
      }
      return dispatch_result(s, handle_reconfig(s, now, &header, &chunk,
          output, cap, written), prior_time);
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
  if (s->state == CNET_SCTP_ASSOC_ESTABLISHED && s->data_ready &&
      deferred_peer_reset_ready(s))
    return complete_deferred_peer_reset(s, now, output, cap, written);
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
    uint16_t reciprocal_streams;
    if (s->state == CNET_SCTP_ASSOC_ESTABLISHED && !s->reset_active &&
        cnet_sctp_data_outbound_drained(&s->data) &&
        next_reciprocal_streams(s, &reciprocal_streams)) {
      const int requested = s->peer_incoming_reset_pending &&
          s->peer_incoming_reset_streams == reciprocal_streams;
      const uint32_t response_sequence = requested ?
          s->peer_incoming_reset_request :
          s->expected_peer_reset_request - 1u;
      status = start_local_reset(s, now, reciprocal_streams, 1,
          response_sequence, output, cap, written);
      if (status == CNET_SCTP_ASSOC_OK && requested) {
        memcpy(s->last_reset_response, output, *written);
        s->last_reset_response_size = *written;
        clear_peer_incoming_reset(s);
      }
      return status;
    }
    if (s->state == CNET_SCTP_ASSOC_ESTABLISHED && s->reset_active &&
        s->reset_needs_initial_publish) {
      if (s->reset_packet_size > cap) return CNET_SCTP_ASSOC_CAPACITY;
      if (now > UINT64_MAX - s->reset_rto_ms)
        return CNET_SCTP_ASSOC_CLOCK;
      memcpy(output, s->reset_packet, s->reset_packet_size);
      *written = s->reset_packet_size;
      s->reset_needs_initial_publish = 0u;
      s->reset_retransmits = 0u;
      s->reset_deadline_ms = now + s->reset_rto_ms;
      s->last_time_ms = now;
      return CNET_SCTP_ASSOC_OK;
    }
    if (s->state == CNET_SCTP_ASSOC_ESTABLISHED && s->reset_active &&
        now >= s->reset_deadline_ms) {
      if (s->reset_retransmits >= s->config.max_retransmits) {
        /* RFC 6525 does not count an "In progress" probe as a path error.
         * This fixed-memory prototype still bounds Owner-visible retries;
         * timeout completes only the reset operation, not the association. */
        if (s->reconfig_kind == CNET_SCTP_RECONFIG_ADD_OUTGOING ||
            s->reconfig_kind == CNET_SCTP_RECONFIG_ADD_INCOMING ||
            s->reconfig_kind == CNET_SCTP_RECONFIG_ADD_STREAMS ||
            s->reconfig_kind == CNET_SCTP_RECONFIG_SSN_TSN_RESET) {
          status = finish_active_reconfig(
              s, CNET_SCTP_STREAM_RESET_TIMEOUT);
          if (status != CNET_SCTP_ASSOC_OK) return status;
        } else if (s->reset_has_paired_incoming) {
          if (!s->reset_outgoing_complete)
            finish_local_reset(s, CNET_SCTP_STREAM_RESET_TIMEOUT);
          if (s->reset_active && !s->reset_paired_incoming_complete)
            finish_paired_incoming_reset(s, 0u,
                CNET_SCTP_STREAM_RESET_TIMEOUT);
          if (s->reset_active)
            clear_active_reset(s, 0);
        } else {
          finish_local_reset(s, CNET_SCTP_STREAM_RESET_TIMEOUT);
        }
        s->last_time_ms = now;
        return CNET_SCTP_ASSOC_OK;
      }
      if (s->reset_packet_size > cap) return CNET_SCTP_ASSOC_CAPACITY;
      next_rto = s->reset_rto_ms;
      if (next_rto > s->config.t1_max_ms - next_rto)
        next_rto = s->config.t1_max_ms;
      else
        next_rto *= 2u;
      if (next_rto > s->config.t1_max_ms)
        next_rto = s->config.t1_max_ms;
      if (now > UINT64_MAX - next_rto) return CNET_SCTP_ASSOC_CLOCK;
      memcpy(output, s->reset_packet, s->reset_packet_size);
      *written = s->reset_packet_size;
      s->reset_rto_ms = next_rto;
      s->reset_deadline_ms = now + next_rto;
      ++s->reset_retransmits;
      s->last_time_ms = now;
      return CNET_SCTP_ASSOC_OK;
    }
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
       s->state == CNET_SCTP_ASSOC_SHUTDOWN_RECEIVED) && s->data_ready) {
    status = data_result(cnet_sctp_data_deadline(&s->data,
                                                deadline_ms, has_deadline));
    if (status != CNET_SCTP_ASSOC_OK) return status;
    if (s->state == CNET_SCTP_ASSOC_ESTABLISHED &&
        deferred_peer_reset_ready(s) &&
        (!*has_deadline || s->last_time_ms < *deadline_ms)) {
      *has_deadline = 1;
      *deadline_ms = s->last_time_ms;
    }
    if (s->state == CNET_SCTP_ASSOC_ESTABLISHED && s->reset_active &&
        (!*has_deadline || s->reset_deadline_ms < *deadline_ms)) {
      *has_deadline = 1;
      *deadline_ms = s->reset_deadline_ms;
    }
    if (s->state == CNET_SCTP_ASSOC_ESTABLISHED && !s->reset_active &&
        s->reciprocal_reset_pending != 0u &&
        cnet_sctp_data_outbound_drained(&s->data)) {
      uint16_t reciprocal_streams;
      if (next_reciprocal_streams(s, &reciprocal_streams) &&
          (!*has_deadline || s->last_time_ms < *deadline_ms)) {
        *has_deadline = 1;
        *deadline_ms = s->last_time_ms;
      }
    }
    return CNET_SCTP_ASSOC_OK;
  }
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

cnet_sctp_assoc_status cnet_sctp_assoc_reset_stream(cnet_sctp_assoc *s,
    uint64_t now, uint16_t stream, void *out, size_t cap, size_t *written) {
  uint16_t bit, streams;
  int reciprocal_retry;
  cnet_sctp_assoc_status status;
  if (written) *written = 0u;
  status = guard(s);
  if (status != CNET_SCTP_ASSOC_OK) return status;
  if (!out || !written) return CNET_SCTP_ASSOC_INVALID;
  if (s->state != CNET_SCTP_ASSOC_ESTABLISHED || !s->data_ready)
    return CNET_SCTP_ASSOC_CLOSED_ERROR;
  if ((status = check_time(s, now)) != CNET_SCTP_ASSOC_OK) return status;
  if (!s->peer_reconfig_supported)
    return CNET_SCTP_ASSOC_PROTOCOL;
  if (stream >= s->data.config.outbound_streams)
    return CNET_SCTP_ASSOC_INVALID;
  bit = reset_stream_bit(stream);
  reciprocal_retry = (s->reciprocal_reset_failed & bit) != 0u;
  streams = reciprocal_retry ? s->reset_streams : bit;
  if (s->reset_active ||
      (!reciprocal_retry && (s->reciprocal_reset_pending != 0u ||
                             s->reciprocal_reset_failed != 0u)) ||
      (s->awaiting_peer_reciprocal & bit) != 0u ||
      (s->outgoing_reset_events & bit) != 0u)
    return CNET_SCTP_ASSOC_BUSY;
  return start_local_reset(s, now, streams, reciprocal_retry,
      s->expected_peer_reset_request - 1u,
      out, cap, written);
}

cnet_sctp_assoc_status cnet_sctp_assoc_reset_incoming_streams(
    cnet_sctp_assoc *s, uint64_t now,
    const uint16_t *stream_ids, size_t stream_count,
    void *out, size_t cap, size_t *written) {
  uint16_t streams = 0u;
  size_t i;
  cnet_sctp_assoc_status status;
  if (written) *written = 0u;
  status = guard(s);
  if (status != CNET_SCTP_ASSOC_OK) return status;
  if (!out || !written || stream_count > CNET_SCTP_DATA_STREAMS ||
      (stream_count != 0u && !stream_ids))
    return CNET_SCTP_ASSOC_INVALID;
  if (s->state != CNET_SCTP_ASSOC_ESTABLISHED || !s->data_ready)
    return CNET_SCTP_ASSOC_CLOSED_ERROR;
  if ((status = check_time(s, now)) != CNET_SCTP_ASSOC_OK) return status;
  if (!s->peer_reconfig_supported)
    return CNET_SCTP_ASSOC_PROTOCOL;
  if (stream_count == 0u) {
    streams = reset_stream_mask(s->data.config.inbound_streams);
  } else {
    for (i = 0u; i < stream_count; ++i) {
      const uint16_t bit = reset_stream_bit(stream_ids[i]);
      if (stream_ids[i] >= s->data.config.inbound_streams || bit == 0u ||
          (streams & bit) != 0u)
        return CNET_SCTP_ASSOC_INVALID;
      streams |= bit;
    }
  }
  return start_local_incoming_reset(
      s, now, streams, stream_count == 0u, out, cap, written);
}

cnet_sctp_assoc_status cnet_sctp_assoc_reset_streams_bidirectional(
    cnet_sctp_assoc *s, uint64_t now,
    const uint16_t *stream_ids, size_t stream_count,
    void *out, size_t cap, size_t *written) {
  uint16_t outgoing_streams = 0u, incoming_streams = 0u;
  size_t i;
  cnet_sctp_assoc_status status;
  if (written) *written = 0u;
  status = guard(s);
  if (status != CNET_SCTP_ASSOC_OK) return status;
  if (!out || !written || stream_count > CNET_SCTP_DATA_STREAMS ||
      (stream_count != 0u && !stream_ids))
    return CNET_SCTP_ASSOC_INVALID;
  if (s->state != CNET_SCTP_ASSOC_ESTABLISHED || !s->data_ready)
    return CNET_SCTP_ASSOC_CLOSED_ERROR;
  if ((status = check_time(s, now)) != CNET_SCTP_ASSOC_OK) return status;
  if (!s->peer_reconfig_supported)
    return CNET_SCTP_ASSOC_PROTOCOL;
  if (stream_count == 0u) {
    outgoing_streams = reset_stream_mask(s->data.config.outbound_streams);
    incoming_streams = reset_stream_mask(s->data.config.inbound_streams);
  } else {
    for (i = 0u; i < stream_count; ++i) {
      const uint16_t bit = reset_stream_bit(stream_ids[i]);
      if (stream_ids[i] >= s->data.config.outbound_streams ||
          stream_ids[i] >= s->data.config.inbound_streams || bit == 0u ||
          (outgoing_streams & bit) != 0u)
        return CNET_SCTP_ASSOC_INVALID;
      outgoing_streams |= bit;
      incoming_streams |= bit;
    }
  }
  return start_local_paired_reset(s, now,
      outgoing_streams, incoming_streams, stream_count == 0u,
      out, cap, written);
}

cnet_sctp_assoc_status cnet_sctp_assoc_add_outgoing_streams(
    cnet_sctp_assoc *s, uint64_t now, uint16_t stream_count,
    void *out, size_t cap, size_t *written) {
  cnet_sctp_assoc_status status;
  if (written) *written = 0u;
  status = guard(s);
  if (status != CNET_SCTP_ASSOC_OK) return status;
  if (!out || !written || stream_count == 0u)
    return CNET_SCTP_ASSOC_INVALID;
  if (s->state != CNET_SCTP_ASSOC_ESTABLISHED || !s->data_ready)
    return CNET_SCTP_ASSOC_CLOSED_ERROR;
  if ((status = check_time(s, now)) != CNET_SCTP_ASSOC_OK) return status;
  if (!s->peer_reconfig_supported)
    return CNET_SCTP_ASSOC_PROTOCOL;
  if (stream_count >
      CNET_SCTP_DATA_STREAMS - s->data.config.outbound_streams)
    return CNET_SCTP_ASSOC_CAPACITY;
  return start_local_add_outgoing(
      s, now, stream_count, 0, out, cap, written);
}

cnet_sctp_assoc_status cnet_sctp_assoc_add_incoming_streams(
    cnet_sctp_assoc *s, uint64_t now, uint16_t stream_count,
    void *out, size_t cap, size_t *written) {
  cnet_sctp_assoc_status status;
  if (written) *written = 0u;
  status = guard(s);
  if (status != CNET_SCTP_ASSOC_OK) return status;
  if (!out || !written || stream_count == 0u)
    return CNET_SCTP_ASSOC_INVALID;
  if (s->state != CNET_SCTP_ASSOC_ESTABLISHED || !s->data_ready)
    return CNET_SCTP_ASSOC_CLOSED_ERROR;
  if ((status = check_time(s, now)) != CNET_SCTP_ASSOC_OK) return status;
  if (!s->peer_reconfig_supported)
    return CNET_SCTP_ASSOC_PROTOCOL;
  if (stream_count >
      CNET_SCTP_DATA_STREAMS - s->data.config.inbound_streams)
    return CNET_SCTP_ASSOC_CAPACITY;
  return start_local_add_incoming(
      s, now, stream_count, out, cap, written);
}

cnet_sctp_assoc_status cnet_sctp_assoc_add_streams(
    cnet_sctp_assoc *s, uint64_t now,
    uint16_t incoming_stream_count, uint16_t outgoing_stream_count,
    void *out, size_t cap, size_t *written) {
  cnet_sctp_assoc_status status;
  if (written) *written = 0u;
  status = guard(s);
  if (status != CNET_SCTP_ASSOC_OK) return status;
  if (!out || !written || incoming_stream_count == 0u ||
      outgoing_stream_count == 0u)
    return CNET_SCTP_ASSOC_INVALID;
  if (s->state != CNET_SCTP_ASSOC_ESTABLISHED || !s->data_ready)
    return CNET_SCTP_ASSOC_CLOSED_ERROR;
  if ((status = check_time(s, now)) != CNET_SCTP_ASSOC_OK) return status;
  if (!s->peer_reconfig_supported)
    return CNET_SCTP_ASSOC_PROTOCOL;
  if (incoming_stream_count >
          CNET_SCTP_DATA_STREAMS - s->data.config.inbound_streams ||
      outgoing_stream_count >
          CNET_SCTP_DATA_STREAMS - s->data.config.outbound_streams)
    return CNET_SCTP_ASSOC_CAPACITY;
  return start_local_add_streams(s, now,
      incoming_stream_count, outgoing_stream_count,
      out, cap, written);
}

cnet_sctp_assoc_status cnet_sctp_assoc_reset_association(
    cnet_sctp_assoc *s, uint64_t now,
    void *out, size_t cap, size_t *written) {
  cnet_sctp_assoc_status status;
  if (written) *written = 0u;
  status = guard(s);
  if (status != CNET_SCTP_ASSOC_OK) return status;
  if (!out || !written) return CNET_SCTP_ASSOC_INVALID;
  if (s->state != CNET_SCTP_ASSOC_ESTABLISHED || !s->data_ready)
    return CNET_SCTP_ASSOC_CLOSED_ERROR;
  if ((status = check_time(s, now)) != CNET_SCTP_ASSOC_OK) return status;
  if (!s->peer_reconfig_supported)
    return CNET_SCTP_ASSOC_PROTOCOL;
  return start_local_association_reset(s, now, out, cap, written);
}

cnet_sctp_assoc_status cnet_sctp_assoc_take_association_reset(
    cnet_sctp_assoc *s, cnet_sctp_assoc_reset_event *event) {
  cnet_sctp_assoc_status status = guard(s);
  if (status != CNET_SCTP_ASSOC_OK) return status;
  if (!event) return CNET_SCTP_ASSOC_INVALID;
  if (!s->association_reset_pending) return CNET_SCTP_ASSOC_EMPTY;
  *event = s->association_reset;
  memset(&s->association_reset, 0, sizeof(s->association_reset));
  s->association_reset_pending = 0u;
  return CNET_SCTP_ASSOC_OK;
}

cnet_sctp_assoc_status cnet_sctp_assoc_take_stream_change(
    cnet_sctp_assoc *s, cnet_sctp_assoc_stream_change_event *event) {
  cnet_sctp_assoc_status status = guard(s);
  if (status != CNET_SCTP_ASSOC_OK) return status;
  if (!event) return CNET_SCTP_ASSOC_INVALID;
  if (s->incoming_stream_change_pending) {
    *event = s->incoming_stream_change;
    memset(&s->incoming_stream_change, 0,
        sizeof(s->incoming_stream_change));
    s->incoming_stream_change_pending = 0u;
    return CNET_SCTP_ASSOC_OK;
  }
  if (s->outgoing_stream_change_pending) {
    *event = s->outgoing_stream_change;
    memset(&s->outgoing_stream_change, 0,
        sizeof(s->outgoing_stream_change));
    s->outgoing_stream_change_pending = 0u;
    return CNET_SCTP_ASSOC_OK;
  }
  return CNET_SCTP_ASSOC_EMPTY;
}

cnet_sctp_assoc_status cnet_sctp_assoc_take_stream_reset(cnet_sctp_assoc *s,
    cnet_sctp_assoc_stream_reset_event *event) {
  uint16_t stream;
  cnet_sctp_assoc_status status = guard(s);
  if (status != CNET_SCTP_ASSOC_OK) return status;
  if (!event) return CNET_SCTP_ASSOC_INVALID;
  for (stream = 0u; stream < CNET_SCTP_DATA_STREAMS; ++stream) {
    const uint16_t bit = (uint16_t)(UINT16_C(1) << stream);
    if ((s->incoming_reset_events & bit) != 0u) {
      event->stream_id = stream;
      event->incoming = 1u;
      event->result = (cnet_sctp_assoc_stream_reset_result)
          s->incoming_reset_results[stream];
      s->incoming_reset_events &= (uint16_t)~bit;
      s->incoming_reset_results[stream] = 0u;
      return CNET_SCTP_ASSOC_OK;
    }
    if ((s->outgoing_reset_events & bit) != 0u) {
      event->stream_id = stream;
      event->incoming = 0u;
      event->result = (cnet_sctp_assoc_stream_reset_result)
          s->outgoing_reset_results[stream];
      s->outgoing_reset_events &= (uint16_t)~bit;
      s->outgoing_reset_results[stream] = 0u;
      return CNET_SCTP_ASSOC_OK;
    }
  }
  return CNET_SCTP_ASSOC_EMPTY;
}

cnet_sctp_assoc_status cnet_sctp_assoc_send_message(cnet_sctp_assoc *s,
    uint64_t now, uint16_t stream_id, uint32_t ppid, int unordered,
    const void *payload, size_t n, void *packet, size_t cap,
    size_t *written) {
  const cnet_sctp_data_reliability reliable = {
    CNET_SCTP_DATA_RELIABLE, 0u
  };
  return cnet_sctp_assoc_send_message_pr(s, now, stream_id, ppid, unordered,
      &reliable, payload, n, packet, cap, written);
}

cnet_sctp_assoc_status cnet_sctp_assoc_send_message_pr(cnet_sctp_assoc *s,
    uint64_t now, uint16_t stream_id, uint32_t ppid, int unordered,
    const cnet_sctp_data_reliability *reliability,
    const void *payload, size_t n, void *packet, size_t cap,
    size_t *written) {
  return cnet_sctp_assoc_send_message_pr_bound(s, now, stream_id, ppid,
      unordered, reliability, payload, n, packet, cap, written, 0u);
}
cnet_sctp_assoc_status cnet_sctp_assoc_send_message_pr_bound(
    cnet_sctp_assoc *s, uint64_t now, uint16_t stream_id, uint32_t ppid,
    int unordered, const cnet_sctp_data_reliability *reliability,
    const void *payload, size_t n, void *packet, size_t cap,
    size_t *written, uint64_t admission_generation) {
  if (written) *written = 0u;
  cnet_sctp_assoc_status status = guard(s);
  if (status != CNET_SCTP_ASSOC_OK) return status;
  if (!reliability || !payload || !packet || !written)
    return CNET_SCTP_ASSOC_INVALID;
  if (s->state != CNET_SCTP_ASSOC_ESTABLISHED || !s->data_ready)
    return CNET_SCTP_ASSOC_CLOSED_ERROR;
  if (association_reset_blocks_data(s) ||
      (reset_blocked_streams(s) & reset_stream_bit(stream_id)) != 0u)
    return CNET_SCTP_ASSOC_BUSY;
  if ((status = check_time(s, now)) != CNET_SCTP_ASSOC_OK) return status;
  status = data_result(cnet_sctp_data_send_pr_bound(&s->data, now,
      stream_id, ppid, unordered, reliability, payload, n,
      packet, cap, written, admission_generation));
  if (status == CNET_SCTP_ASSOC_OK) s->last_time_ms = now;
  return status;
}

cnet_sctp_assoc_status cnet_sctp_assoc_take_message(cnet_sctp_assoc *s,
    cnet_sctp_data_message *message, void *payload,
    size_t cap, size_t *written) {
  if (written) *written = 0u;
  cnet_sctp_assoc_status status = guard(s);
  if (status != CNET_SCTP_ASSOC_OK) return status;
  if (!message || !payload || !written) return CNET_SCTP_ASSOC_INVALID;
  if ((s->state != CNET_SCTP_ASSOC_ESTABLISHED &&
       s->state != CNET_SCTP_ASSOC_SHUTDOWN_PENDING &&
       s->state != CNET_SCTP_ASSOC_SHUTDOWN_SENT &&
       s->state != CNET_SCTP_ASSOC_SHUTDOWN_RECEIVED &&
       s->state != CNET_SCTP_ASSOC_STOPPED) || !s->data_ready)
    return CNET_SCTP_ASSOC_CLOSED_ERROR;
  if (s->deferred_peer_reset_active) {
    status = data_result(cnet_sctp_data_take_before_reset(&s->data,
        s->deferred_peer_reset_streams, s->deferred_peer_reset_last_tsn,
        message, payload, cap, written));
  } else {
    status = data_result(cnet_sctp_data_take(&s->data, message,
        payload, cap, written));
  }
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
  if (written) *written = 0u;
  cnet_sctp_assoc_status status = guard(s);
  if (status != CNET_SCTP_ASSOC_OK) return status;
  if (!items || !out || !written) return CNET_SCTP_ASSOC_INVALID;
  if (s->state != CNET_SCTP_ASSOC_ESTABLISHED || !s->data_ready)
    return CNET_SCTP_ASSOC_CLOSED_ERROR;
  if (association_reset_blocks_data(s))
    return CNET_SCTP_ASSOC_BUSY;
  {
    size_t i;
    const uint16_t blocked = reset_blocked_streams(s);
    for (i = 0u; i < count; ++i)
      if ((blocked & reset_stream_bit(items[i].stream_id)) != 0u)
        return CNET_SCTP_ASSOC_BUSY;
  }
  if ((status = check_time(s, now)) != CNET_SCTP_ASSOC_OK) return status;
  status = data_result(cnet_sctp_data_send_bundle(
      &s->data, now, items, count, out, cap, written));
  if (status == CNET_SCTP_ASSOC_OK) s->last_time_ms = now;
  return status;
}

cnet_sctp_assoc_status cnet_sctp_assoc_queue_message(cnet_sctp_assoc *s,
    uint64_t now, uint16_t stream, uint32_t ppid, int unordered,
    const void *payload, size_t n) {
  const cnet_sctp_data_reliability reliable = {
    CNET_SCTP_DATA_RELIABLE, 0u
  };
  return cnet_sctp_assoc_queue_message_pr(s, now, stream, ppid, unordered,
      &reliable, payload, n);
}

cnet_sctp_assoc_status cnet_sctp_assoc_queue_message_pr(cnet_sctp_assoc *s,
    uint64_t now, uint16_t stream, uint32_t ppid, int unordered,
    const cnet_sctp_data_reliability *reliability,
    const void *payload, size_t n) {
  return cnet_sctp_assoc_queue_message_pr_bound(s, now, stream, ppid,
      unordered, reliability, payload, n, 0u);
}
cnet_sctp_assoc_status cnet_sctp_assoc_queue_message_pr_bound(
    cnet_sctp_assoc *s, uint64_t now, uint16_t stream, uint32_t ppid,
    int unordered, const cnet_sctp_data_reliability *reliability,
    const void *payload, size_t n, uint64_t admission_generation) {
  cnet_sctp_assoc_status status = guard(s);
  if (status != CNET_SCTP_ASSOC_OK) return status;
  if (!reliability || !payload) return CNET_SCTP_ASSOC_INVALID;
  if (s->state != CNET_SCTP_ASSOC_ESTABLISHED || !s->data_ready)
    return CNET_SCTP_ASSOC_CLOSED_ERROR;
  if (association_reset_blocks_data(s) ||
      (reset_blocked_streams(s) & reset_stream_bit(stream)) != 0u)
    return CNET_SCTP_ASSOC_BUSY;
  if ((status = check_time(s, now)) != CNET_SCTP_ASSOC_OK) return status;
  status = data_result(cnet_sctp_data_queue_message_pr_bound(&s->data,
      now, stream, ppid, unordered, reliability, payload, n,
      admission_generation));
  if (status == CNET_SCTP_ASSOC_OK) s->last_time_ms = now;
  return status;
}

/* Association Owner validates the clock before the DATA layer atomically
 * releases a never-sent, fully-copied message. No protocol packet is emitted. */
cnet_sctp_assoc_status cnet_sctp_assoc_cancel_unsent_message(
    cnet_sctp_assoc *s, uint64_t now, uint16_t stream_id) {
  cnet_sctp_assoc_status status = guard(s);
  if (status != CNET_SCTP_ASSOC_OK) return status;
  if (s->state != CNET_SCTP_ASSOC_ESTABLISHED || !s->data_ready)
    return CNET_SCTP_ASSOC_CLOSED_ERROR;
  if ((status = check_time(s, now)) != CNET_SCTP_ASSOC_OK) return status;
  status = data_result(cnet_sctp_data_cancel_unsent(&s->data,
      now, stream_id));
  if (status == CNET_SCTP_ASSOC_OK) s->last_time_ms = now;
  return status;
}

cnet_sctp_assoc_status cnet_sctp_assoc_take_abandoned(cnet_sctp_assoc *s,
    cnet_sctp_data_abandon_event *event) {
  cnet_sctp_assoc_status status = guard(s);
  if (status != CNET_SCTP_ASSOC_OK) return status;
  if (!event) return CNET_SCTP_ASSOC_INVALID;
  /* A LOCAL retry-budget EXPIRED state changes the Association to FAILED,
   * but does NOT clear its existing DATA TCB or buffered abandonment FIFO.
   * Allow this strictly read-only Owner take after local FAILED, so an
   * unpolled PR event can still retain its ORIGINAL DCEP generation.
   * Peer ABORT is different: its DATA state has already been destroyed. */
  if ((s->state != CNET_SCTP_ASSOC_ESTABLISHED &&
       s->state != CNET_SCTP_ASSOC_SHUTDOWN_PENDING &&
       s->state != CNET_SCTP_ASSOC_SHUTDOWN_SENT &&
       s->state != CNET_SCTP_ASSOC_SHUTDOWN_RECEIVED &&
       s->state != CNET_SCTP_ASSOC_FAILED) || !s->data_ready)
    return CNET_SCTP_ASSOC_CLOSED_ERROR;
  return data_result(cnet_sctp_data_take_abandoned(&s->data, event));
}

cnet_sctp_assoc_status cnet_sctp_assoc_pump_message(cnet_sctp_assoc *s,
    uint64_t now, void *out, size_t cap, size_t *written) {
  if (written) *written = 0u;
  cnet_sctp_assoc_status status = guard(s);
  if (status != CNET_SCTP_ASSOC_OK) return status;
  if (!out || !written) return CNET_SCTP_ASSOC_INVALID;
  if ((s->state != CNET_SCTP_ASSOC_ESTABLISHED &&
       s->state != CNET_SCTP_ASSOC_SHUTDOWN_PENDING &&
       s->state != CNET_SCTP_ASSOC_SHUTDOWN_RECEIVED) || !s->data_ready)
    return CNET_SCTP_ASSOC_CLOSED_ERROR;
  if (association_reset_blocks_data(s))
    return CNET_SCTP_ASSOC_BUSY;
  if ((status = check_time(s, now)) != CNET_SCTP_ASSOC_OK) return status;
  status = data_result(cnet_sctp_data_pump(&s->data, now, out, cap, written));
  if (status == CNET_SCTP_ASSOC_OK) s->last_time_ms = now;
  return status;
}

/* Initiate a graceful close only after the old TX tickets and pending
 * copied message drain. No new application data is admitted afterward. */
cnet_sctp_assoc_status cnet_sctp_assoc_shutdown(cnet_sctp_assoc *s,
    uint64_t now, void *out, size_t cap, size_t *written) {
  if (written) *written = 0u;
  cnet_sctp_assoc_status status = guard(s);
  if (status != CNET_SCTP_ASSOC_OK) return status;
  if (!out || !written) return CNET_SCTP_ASSOC_INVALID;
  if (s->state != CNET_SCTP_ASSOC_ESTABLISHED || !s->data_ready)
    return CNET_SCTP_ASSOC_CLOSED_ERROR;
  if (s->reset_active || s->deferred_peer_reset_active ||
      s->awaiting_local_incoming_reset != 0u ||
      s->reciprocal_reset_pending != 0u ||
      s->awaiting_peer_reciprocal != 0u ||
      s->reciprocal_reset_failed != 0u)
    return CNET_SCTP_ASSOC_BUSY;
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
  if (written) *written = 0u;
  cnet_sctp_assoc_status status = guard(s);
  if (status != CNET_SCTP_ASSOC_OK) return status;
  if (!out || !written) return CNET_SCTP_ASSOC_INVALID;
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

cnet_sctp_assoc_status cnet_sctp_assoc_take_peer_abort(
    cnet_sctp_assoc *s, cnet_sctp_assoc_peer_abort_info *info,
    void *reason, size_t capacity, size_t *written) {
  cnet_sctp_assoc_status status;
  if (written) *written = 0u;
  status = guard(s);
  if (status != CNET_SCTP_ASSOC_OK) return status;
  if (!info || !written) return CNET_SCTP_ASSOC_INVALID;
  if (!s->peer_abort_pending) return CNET_SCTP_ASSOC_EMPTY;
  if (s->peer_abort_reason_size > capacity)
    return CNET_SCTP_ASSOC_CAPACITY;
  if (s->peer_abort_reason_size != 0u && !reason)
    return CNET_SCTP_ASSOC_INVALID;
  if (s->peer_abort_reason_size != 0u)
    memcpy(reason, s->peer_abort_reason, s->peer_abort_reason_size);
  *info = s->peer_abort_info;
  *written = s->peer_abort_reason_size;
  s->peer_abort_pending = 0u;
  s->peer_abort_reason_size = 0u;
  memset(s->peer_abort_reason, 0, sizeof(s->peer_abort_reason));
  memset(&s->peer_abort_info, 0, sizeof(s->peer_abort_info));
  return CNET_SCTP_ASSOC_OK;
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
  clear_active_reset(s, 0);
  s->last_reset_response_size = 0u;
  s->reciprocal_reset_pending = 0u;
  s->awaiting_peer_reciprocal = 0u;
  s->awaiting_local_incoming_reset = 0u;
  s->awaiting_local_incoming_reset_request = 0u;
  s->reciprocal_reset_failed = 0u;
  s->incoming_reset_events = 0u;
  s->outgoing_reset_events = 0u;
  s->incoming_stream_change_pending = 0u;
  s->outgoing_stream_change_pending = 0u;
  s->association_reset_pending = 0u;
  clear_deferred_peer_reset(s);
  clear_peer_incoming_reset(s);
  memset(s->reset_packet, 0, sizeof(s->reset_packet));
  memset(s->last_reset_response, 0, sizeof(s->last_reset_response));
  memset(s->incoming_reset_results, 0, sizeof(s->incoming_reset_results));
  memset(s->outgoing_reset_results, 0, sizeof(s->outgoing_reset_results));
  memset(&s->incoming_stream_change, 0,
      sizeof(s->incoming_stream_change));
  memset(&s->outgoing_stream_change, 0,
      sizeof(s->outgoing_stream_change));
  memset(&s->association_reset, 0, sizeof(s->association_reset));
  /* Explicit stop discards any unread communication-lost event. */
  s->peer_abort_pending = 0u;
  s->peer_abort_reason_size = 0u;
  memset(s->peer_abort_reason, 0, sizeof(s->peer_abort_reason));
  memset(&s->peer_abort_info, 0, sizeof(s->peer_abort_info));
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
