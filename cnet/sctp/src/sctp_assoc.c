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
static cnet_sctp_assoc_status publish_packet(uint8_t *output, size_t cap,
    size_t *written, const uint8_t *scratch, size_t size) {
  if (size > cap) return CNET_SCTP_ASSOC_CAPACITY;
  memcpy(output, scratch, size);
  *written = size;
  return CNET_SCTP_ASSOC_OK;
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
  if (now > UINT64_MAX - s->config.t1_ms) return CNET_SCTP_ASSOC_CLOCK;
  if (!acknowledgement) wr32(value, s->data.cumulative_rx);
  status = encode_one(s, s->peer_tag,
      acknowledgement ? SCTP_TYPE_SHUTDOWN_ACK : SCTP_TYPE_SHUTDOWN,
      acknowledgement ? NULL : value, acknowledgement ? 0u : sizeof(value),
      scratch, sizeof(scratch), &n);
  if (status != CNET_SCTP_ASSOC_OK) return status;
  return begin_packet(s, now, scratch, n,
      acknowledgement ? CNET_SCTP_ASSOC_SHUTDOWN_ACK_SENT :
          CNET_SCTP_ASSOC_SHUTDOWN_SENT, out, cap, written);
}
/* A graceful terminal retains bounded RX tickets until the Owner takes
 * every fully received ULP record. STOPPED blocks further transport I/O,
 * but drained()/destroy() remain BUSY until those bytes are delivered. */
static void finish_shutdown(cnet_sctp_assoc *s) {
  if (s->data_ready && s->data.buffered_rx == 0u) {
    (void)cnet_sctp_data_stop(&s->data);
    memset(&s->data, 0, sizeof(s->data));
    s->data_ready = 0u;
  }
  s->state = CNET_SCTP_ASSOC_STOPPED;
  s->deadline_ms = 0u;
  s->saved_size = 0u;
  s->current_rto_ms = 0u;
  s->retransmits = 0u;
  memset(s->saved_packet, 0, sizeof(s->saved_packet));
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
  if (header->verification_tag != s->local_tag || chunk->flags != 0u ||
      chunk->value_size != 4u || !s->data_ready)
    return CNET_SCTP_ASSOC_PROTOCOL;
  if (s->state == CNET_SCTP_ASSOC_SHUTDOWN_ACK_SENT)
    return publish_packet(out, cap, written, s->saved_packet, s->saved_size);
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
      chunk->flags != 0u || chunk->value_size != 0u ||
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

cnet_sctp_assoc_status cnet_sctp_assoc_input(cnet_sctp_assoc *s,
    uint64_t now, const uint8_t *packet, size_t packet_size,
    uint8_t *output, size_t cap, size_t *written) {
  cnet_sctp_wire_header header;
  cnet_sctp_wire_decoder decoder;
  cnet_sctp_wire_chunk chunk;
  cnet_sctp_wire_chunk second;
  cnet_sctp_assoc_status status = guard(s);
  if (status != CNET_SCTP_ASSOC_OK) return status;
  if (!packet || !output || !written) return CNET_SCTP_ASSOC_INVALID;
  *written = 0u;
  if (s->state == CNET_SCTP_ASSOC_STOPPED || s->state == CNET_SCTP_ASSOC_FAILED)
    return CNET_SCTP_ASSOC_CLOSED_ERROR;
  if ((status = check_time(s, now)) != CNET_SCTP_ASSOC_OK)
    return status;
  /* The external clock advances even when incoming bytes are rejected: an
   * invalid/expired cookie must not enable a later clock rollback replay. */
  s->last_time_ms = now;
  if (cnet_sctp_wire_decode(packet, packet_size, &header, &decoder) !=
      CNET_SCTP_WIRE_OK) return CNET_SCTP_ASSOC_PROTOCOL;
  if (header.source_port != s->config.remote_port ||
      header.destination_port != s->config.local_port ||
      cnet_sctp_wire_next_chunk(&decoder, &chunk) != CNET_SCTP_WIRE_OK)
    return CNET_SCTP_ASSOC_PROTOCOL;
  if (header.chunk_count == 2u && chunk.type == SCTP_TYPE_COOKIE_ECHO) {
    /* COOKIE ECHO + advisory ERROR has its own bounded handshake rule. */
    if (cnet_sctp_wire_next_chunk(&decoder, &second) != CNET_SCTP_WIRE_OK ||
        !valid_cookie_echo_error(&second))
      return CNET_SCTP_ASSOC_PROTOCOL;
  } else if (header.chunk_count > 1u &&
             chunk.type != 0u && chunk.type != 3u) {
    /* No arbitrary control chunks can be bundled with INIT, Cookie,
     * SHUTDOWN or ABORT in the private Association profile. */
    return CNET_SCTP_ASSOC_PROTOCOL;
  }
  switch (chunk.type) {
    case 0u: /* DATA */
    case 3u: /* SACK */
      if (!s->data_ready ||
          (header.chunk_count != 1u &&
           s->state != CNET_SCTP_ASSOC_ESTABLISHED &&
           s->state != CNET_SCTP_ASSOC_SHUTDOWN_PENDING))
        return CNET_SCTP_ASSOC_PROTOCOL;
      if (chunk.type == 0u) {
        if (s->state != CNET_SCTP_ASSOC_ESTABLISHED &&
            s->state != CNET_SCTP_ASSOC_SHUTDOWN_PENDING &&
            s->state != CNET_SCTP_ASSOC_SHUTDOWN_SENT)
          return CNET_SCTP_ASSOC_PROTOCOL;
      } else if (s->state != CNET_SCTP_ASSOC_ESTABLISHED &&
                 s->state != CNET_SCTP_ASSOC_SHUTDOWN_PENDING &&
                 s->state != CNET_SCTP_ASSOC_SHUTDOWN_RECEIVED)
        return CNET_SCTP_ASSOC_PROTOCOL;
      return data_result(cnet_sctp_data_input(&s->data, now, packet,
          packet_size, output, cap, written));
    case SCTP_TYPE_INIT:
      return handle_init(s, now, &header, &chunk, output, cap, written);
    case SCTP_TYPE_INIT_ACK:
      return handle_init_ack(s, now, &header, &chunk, output, cap, written);
    case SCTP_TYPE_COOKIE_ECHO:
      return handle_cookie_echo(s, now, &header, &chunk, output, cap, written);
    case SCTP_TYPE_ABORT:
      /* This private M2 path handles only a plain ABORT using the
       * expected tag. It never responds to ABORT with another ABORT. */
      if (header.verification_tag != s->local_tag || chunk.flags != 0u ||
          chunk.value_size != 0u || s->state == CNET_SCTP_ASSOC_CLOSED)
        return CNET_SCTP_ASSOC_PROTOCOL;
      if (s->data_ready) {
        (void)cnet_sctp_data_stop(&s->data);
        memset(&s->data, 0, sizeof(s->data));
        s->data_ready = 0u;
      }
      s->state = CNET_SCTP_ASSOC_FAILED;
      s->deadline_ms = 0u;
      s->saved_size = 0u;
      memset(s->saved_packet, 0, sizeof(s->saved_packet));
      return CNET_SCTP_ASSOC_OK;
    case SCTP_TYPE_COOKIE_ACK:
      return handle_cookie_ack(s, now, &header, &chunk);
    case SCTP_TYPE_SHUTDOWN:
      if (header.chunk_count != 1u) return CNET_SCTP_ASSOC_PROTOCOL;
      return handle_shutdown(s, now, &header, &chunk, output, cap, written);
    case SCTP_TYPE_SHUTDOWN_ACK:
      if (header.chunk_count != 1u) return CNET_SCTP_ASSOC_PROTOCOL;
      return handle_shutdown_ack(s, now, &header, &chunk, output, cap, written);
    case SCTP_TYPE_SHUTDOWN_COMPLETE:
      if (header.chunk_count != 1u ||
          header.verification_tag != s->local_tag || chunk.flags != 0u ||
          chunk.value_size != 0u ||
          s->state != CNET_SCTP_ASSOC_SHUTDOWN_ACK_SENT)
        return CNET_SCTP_ASSOC_PROTOCOL;
      finish_shutdown(s);
      s->last_time_ms = now;
      return CNET_SCTP_ASSOC_OK;
    default:
      return CNET_SCTP_ASSOC_PROTOCOL; /* unsupported control chunk */
  }
}

cnet_sctp_assoc_status cnet_sctp_assoc_advance(cnet_sctp_assoc *s,
    uint64_t now, uint8_t *output, size_t cap, size_t *written) {
  cnet_sctp_assoc_status status = guard(s);
  uint64_t next_rto;
  if (status != CNET_SCTP_ASSOC_OK) return status;
  if (!output || !written) return CNET_SCTP_ASSOC_INVALID;
  *written = 0u;
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
  if (now < s->deadline_ms) {
    s->last_time_ms = now;
    return CNET_SCTP_ASSOC_OK;
  }
  if (s->retransmits >= s->config.max_retransmits) {
    s->state = CNET_SCTP_ASSOC_FAILED;
    s->deadline_ms = 0u;
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
