#include "sctp_assoc_internal.h"

#include <limits.h>
#include <string.h>

#define SCTP_ASSOC_MAGIC UINT32_C(0x53314341)
enum {
  SCTP_TYPE_INIT = 1,
  SCTP_TYPE_INIT_ACK = 2,
  SCTP_TYPE_COOKIE_ECHO = 10,
  SCTP_TYPE_COOKIE_ACK = 11,
  SCTP_PARAM_COOKIE = 7,
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
  if (issued > now || now - issued > s->config.cookie_lifetime_ms)
    return CNET_SCTP_ASSOC_EXPIRED;
  return CNET_SCTP_ASSOC_OK;
}

static cnet_sctp_assoc_status handle_init(cnet_sctp_assoc *s, uint64_t now,
    const cnet_sctp_wire_header *header, const cnet_sctp_wire_chunk *chunk,
    uint8_t *output, size_t cap, size_t *written) {
  uint8_t value[16u + 4u + CNET_SCTP_ASSOC_COOKIE_BYTES];
  uint8_t scratch[CNET_SCTP_ASSOC_MAX_PACKET];
  size_t size = 0u;
  cnet_sctp_assoc_status status;
  if (s->state != CNET_SCTP_ASSOC_CLOSED || header->verification_tag != 0u ||
      chunk->value_size != 16u || !good_init_fields(chunk->value, chunk->value_size))
    return CNET_SCTP_ASSOC_PROTOCOL;
  prepare_init(s, value);
  wr16(value + 16u, SCTP_PARAM_COOKIE);
  wr16(value + 18u, (uint16_t)(4u + CNET_SCTP_ASSOC_COOKIE_BYTES));
  status = make_cookie(s, now, chunk->value, value + 20u);
  if (status != CNET_SCTP_ASSOC_OK) return status;
  status = encode_one(s, rd32(chunk->value), SCTP_TYPE_INIT_ACK,
      value, sizeof(value), scratch, sizeof(scratch), &size);
  if (status != CNET_SCTP_ASSOC_OK) return status;
  /* Stateless responder: CLOSED remains unchanged until valid COOKIE ECHO. */
  status = publish_packet(output, cap, written, scratch, size);
  if (status == CNET_SCTP_ASSOC_OK) s->last_time_ms = now;
  return status;
}

/* A received INIT ACK can include several optional TLVs; cookie is mandatory. */
static cnet_sctp_assoc_status find_cookie_parameter(
    const cnet_sctp_wire_chunk *chunk, const uint8_t **cookie, size_t *len) {
  size_t position = 16u;
  const uint8_t *value = chunk->value;
  *cookie = NULL;
  *len = 0u;
  if (!good_init_fields(value, chunk->value_size)) return CNET_SCTP_ASSOC_PROTOCOL;
  while (position < chunk->value_size) {
    size_t available = chunk->value_size - position;
    size_t parameter_size;
    size_t padded;
    uint16_t type;
    if (available < 4u) return CNET_SCTP_ASSOC_PROTOCOL;
    type = rd16(value + position);
    parameter_size = rd16(value + position + 2u);
    if (parameter_size < 4u || parameter_size > available) return CNET_SCTP_ASSOC_PROTOCOL;
    padded = (parameter_size + 3u) & ~(size_t)3u;
    if (padded > available) {
      if (parameter_size != available) return CNET_SCTP_ASSOC_PROTOCOL;
      padded = available;
    }
    if (type == SCTP_PARAM_COOKIE) {
      if (*cookie || parameter_size == 4u) return CNET_SCTP_ASSOC_PROTOCOL;
      *cookie = value + position + 4u;
      *len = parameter_size - 4u;
    }
    position += padded;
  }
  return *cookie ? CNET_SCTP_ASSOC_OK : CNET_SCTP_ASSOC_PROTOCOL;
}
static cnet_sctp_assoc_status handle_init_ack(cnet_sctp_assoc *s,
    uint64_t now, const cnet_sctp_wire_header *header,
    const cnet_sctp_wire_chunk *chunk,
    uint8_t *output, size_t cap, size_t *written) {
  const uint8_t *cookie = NULL;
  size_t cookie_size = 0u;
  uint8_t scratch[CNET_SCTP_ASSOC_MAX_PACKET];
  size_t size = 0u;
  uint32_t peer_tag;
  cnet_sctp_assoc_status status;
  if (s->state != CNET_SCTP_ASSOC_COOKIE_WAIT ||
      header->verification_tag != s->local_tag) return CNET_SCTP_ASSOC_PROTOCOL;
  status = find_cookie_parameter(chunk, &cookie, &cookie_size);
  if (status != CNET_SCTP_ASSOC_OK) return status;
  if (cookie_size > CNET_SCTP_ASSOC_MAX_PACKET - 16u)
    return CNET_SCTP_ASSOC_CAPACITY;
  peer_tag = rd32(chunk->value);
  status = encode_one(s, peer_tag, SCTP_TYPE_COOKIE_ECHO,
      cookie, cookie_size, scratch, sizeof(scratch), &size);
  if (status != CNET_SCTP_ASSOC_OK) return status;
  if (now > UINT64_MAX - s->config.t1_ms) return CNET_SCTP_ASSOC_CLOCK;
  status = begin_packet(s, now, scratch, size, CNET_SCTP_ASSOC_COOKIE_ECHOED,
      output, cap, written);
  if (status == CNET_SCTP_ASSOC_OK) {
    s->peer_tag = peer_tag;
    s->peer_initial_tsn = rd32(chunk->value + 12u);
    s->peer_outbound_streams = rd16(chunk->value + 8u);
    s->peer_inbound_streams = rd16(chunk->value + 10u);
  }
  return status;
}
static cnet_sctp_assoc_status handle_cookie_echo(cnet_sctp_assoc *s,
    uint64_t now, const cnet_sctp_wire_header *header,
    const cnet_sctp_wire_chunk *chunk,
    uint8_t *output, size_t cap, size_t *written) {
  uint8_t scratch[CNET_SCTP_ASSOC_MAX_PACKET];
  size_t size = 0u;
  cnet_sctp_assoc_status status;
  if ((s->state != CNET_SCTP_ASSOC_CLOSED &&
       s->state != CNET_SCTP_ASSOC_ESTABLISHED) ||
      header->verification_tag != s->local_tag)
    return CNET_SCTP_ASSOC_PROTOCOL;
  status = verify_cookie(s, now, chunk->value, chunk->value_size);
  if (status != CNET_SCTP_ASSOC_OK) return status;
  if (s->state == CNET_SCTP_ASSOC_ESTABLISHED &&
      (s->peer_tag != rd32(chunk->value + 16u) ||
       s->peer_initial_tsn != rd32(chunk->value + 24u)))
    return CNET_SCTP_ASSOC_AUTH;
  status = encode_one(s, rd32(chunk->value + 16u), SCTP_TYPE_COOKIE_ACK,
      NULL, 0u, scratch, sizeof(scratch), &size);
  if (status != CNET_SCTP_ASSOC_OK) return status;
  status = publish_packet(output, cap, written, scratch, size);
  if (status != CNET_SCTP_ASSOC_OK) return status;
  if (s->state == CNET_SCTP_ASSOC_CLOSED) {
    s->peer_tag = rd32(chunk->value + 16u);
    s->peer_initial_tsn = rd32(chunk->value + 24u);
    s->peer_outbound_streams = rd16(chunk->value + 32u);
    s->peer_inbound_streams = rd16(chunk->value + 34u);
    s->state = CNET_SCTP_ASSOC_ESTABLISHED;
  }
  s->last_time_ms = now;
  return CNET_SCTP_ASSOC_OK;
}
static cnet_sctp_assoc_status handle_cookie_ack(cnet_sctp_assoc *s,
    uint64_t now, const cnet_sctp_wire_header *header,
    const cnet_sctp_wire_chunk *chunk) {
  if (s->state != CNET_SCTP_ASSOC_COOKIE_ECHOED ||
      header->verification_tag != s->local_tag ||
      chunk->value_size != 0u) return CNET_SCTP_ASSOC_PROTOCOL;
  s->state = CNET_SCTP_ASSOC_ESTABLISHED;
  s->saved_size = 0u;
  memset(s->saved_packet, 0, sizeof(s->saved_packet));
  s->deadline_ms = 0u;
  s->last_time_ms = now;
  return CNET_SCTP_ASSOC_OK;
}

cnet_sctp_assoc_status cnet_sctp_assoc_input(cnet_sctp_assoc *s,
    uint64_t now, const uint8_t *packet, size_t packet_size,
    uint8_t *output, size_t cap, size_t *written) {
  cnet_sctp_wire_header header;
  cnet_sctp_wire_decoder decoder;
  cnet_sctp_wire_chunk chunk;
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
      header.chunk_count != 1u ||
      cnet_sctp_wire_next_chunk(&decoder, &chunk) != CNET_SCTP_WIRE_OK)
    return CNET_SCTP_ASSOC_PROTOCOL;
  switch (chunk.type) {
    case SCTP_TYPE_INIT:
      return handle_init(s, now, &header, &chunk, output, cap, written);
    case SCTP_TYPE_INIT_ACK:
      return handle_init_ack(s, now, &header, &chunk, output, cap, written);
    case SCTP_TYPE_COOKIE_ECHO:
      return handle_cookie_echo(s, now, &header, &chunk, output, cap, written);
    case SCTP_TYPE_COOKIE_ACK:
      return handle_cookie_ack(s, now, &header, &chunk);
    default:
      return CNET_SCTP_ASSOC_PROTOCOL; /* DATA, shutdown, SACK not implemented */
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
  s->last_time_ms = now;
  if (s->state == CNET_SCTP_ASSOC_STOPPED || s->state == CNET_SCTP_ASSOC_FAILED)
    return CNET_SCTP_ASSOC_CLOSED_ERROR;
  if (s->state != CNET_SCTP_ASSOC_COOKIE_WAIT &&
      s->state != CNET_SCTP_ASSOC_COOKIE_ECHOED) {
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
  if (s->saved_size > cap) return CNET_SCTP_ASSOC_CAPACITY;
  memcpy(output, s->saved_packet, s->saved_size);
  *written = s->saved_size;
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
  *has_deadline = s->state == CNET_SCTP_ASSOC_COOKIE_WAIT ||
      s->state == CNET_SCTP_ASSOC_COOKIE_ECHOED;
  *deadline_ms = *has_deadline ? s->deadline_ms : 0u;
  return CNET_SCTP_ASSOC_OK;
}

cnet_sctp_assoc_status cnet_sctp_assoc_stop(cnet_sctp_assoc *s) {
  cnet_sctp_assoc_status status = guard(s);
  if (status != CNET_SCTP_ASSOC_OK) return status;
  s->state = CNET_SCTP_ASSOC_STOPPED;
  s->deadline_ms = 0u;
  s->saved_size = 0u;
  memset(s->saved_packet, 0, sizeof(s->saved_packet));
  return CNET_SCTP_ASSOC_OK;
}

cnet_sctp_assoc_status cnet_sctp_assoc_destroy(cnet_sctp_assoc *s) {
  cnet_sctp_assoc_status status = guard(s);
  if (status != CNET_SCTP_ASSOC_OK) return status;
  if (s->state != CNET_SCTP_ASSOC_STOPPED) return CNET_SCTP_ASSOC_BUSY;
  memset(s, 0, sizeof(*s));
  return CNET_SCTP_ASSOC_OK;
}

cnet_sctp_assoc_state cnet_sctp_assoc_state_get(const cnet_sctp_assoc *s) {
  return s && s->magic == SCTP_ASSOC_MAGIC ? s->state : CNET_SCTP_ASSOC_STOPPED;
}

int cnet_sctp_assoc_drained(const cnet_sctp_assoc *s) {
  return s && s->magic == SCTP_ASSOC_MAGIC &&
      s->state == CNET_SCTP_ASSOC_STOPPED && !s->in_progress;
}
