#include "sctp_dcep_internal.h"

#include <string.h>

#define DCEP_MAGIC UINT32_C(0x44434550)
enum { DCEP_ACK = 2, DCEP_OPEN = 3 };
static void wr16(uint8_t *p, uint16_t x) {
  p[0] = (uint8_t)(x >> 8); p[1] = (uint8_t)x;
}
static void wr32(uint8_t *p, uint32_t x) {
  p[0] = (uint8_t)(x >> 24); p[1] = (uint8_t)(x >> 16);
  p[2] = (uint8_t)(x >> 8); p[3] = (uint8_t)x;
}
static uint16_t rd16(const uint8_t *p) {
  return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}
static uint32_t rd32(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
         ((uint32_t)p[2] << 8) | p[3];
}
static cnet_sctp_dcep_status guard(cnet_sctp_dcep *d) {
  if (!d || d->magic != DCEP_MAGIC || !d->is_owner)
    return CNET_SCTP_DCEP_INVALID;
  if (!d->is_owner(d->context)) return CNET_SCTP_DCEP_OWNER;
  return d->stopped ? CNET_SCTP_DCEP_CLOSED : CNET_SCTP_DCEP_OK;
}

/* Strict UTF-8: reject overlong forms, isolated continuation code points,
 * UTF-16 surrogate ranges and values beyond Unicode scalar U+10FFFF. */
static int valid_utf8(const uint8_t *s, size_t n) {
  size_t i = 0u;
  while (i < n) {
    unsigned int need;
    uint8_t c = s[i];
    if (c <= 0x7fu) { ++i; continue; }
    if (c >= 0xc2u && c <= 0xdfu) need = 1u;
    else if (c >= 0xe0u && c <= 0xefu) need = 2u;
    else if (c >= 0xf0u && c <= 0xf4u) need = 3u;
    else return 0;
    if (need >= n - i) return 0;
    if (s[i+1u] < 0x80u || s[i+1u] > 0xbfu) return 0;
    if (need >= 2u) {
      if (s[i+2u] < 0x80u || s[i+2u] > 0xbfu) return 0;
      if ((c == 0xe0u && s[i+1u] < 0xa0u) ||
          (c == 0xedu && s[i+1u] > 0x9fu)) return 0;
    }
    if (need == 3u) {
      if (s[i+3u] < 0x80u || s[i+3u] > 0xbfu) return 0;
      if ((c == 0xf0u && s[i+1u] < 0x90u) ||
          (c == 0xf4u && s[i+1u] > 0x8fu)) return 0;
    }
    i += need + 1u;
  }
  return 1;
}

static cnet_sctp_dcep_status validate_type(uint8_t channel, uint32_t reliability) {
  switch (channel) {
    case 0x00u: case 0x80u:
      /* RFC 8832: nonzero received reliable params are ignored, not used. */
      (void)reliability;
      return CNET_SCTP_DCEP_OK;
    case 0x01u: case 0x81u: case 0x02u: case 0x82u:
      return CNET_SCTP_DCEP_UNSUPPORTED; /* PR-SCTP not implemented yet */
    default: return CNET_SCTP_DCEP_PROTOCOL;
  }
}

cnet_sctp_dcep_status cnet_sctp_dcep_init(cnet_sctp_dcep *d,
    uint16_t streams, int is_dtls_client,
    cnet_sctp_dcep_owner_fn is_owner, void *context) {
  if (!d || !is_owner || !is_owner(context) ||
      streams == 0u || streams > CNET_SCTP_DCEP_STREAMS ||
      (is_dtls_client != 0 && is_dtls_client != 1))
    return CNET_SCTP_DCEP_INVALID;
  if (d->magic == DCEP_MAGIC) return CNET_SCTP_DCEP_BUSY;
  memset(d, 0, sizeof(*d));
  d->streams = streams;
  d->is_dtls_client = (uint8_t)is_dtls_client;
  d->is_owner = is_owner;
  d->context = context;
  d->magic = DCEP_MAGIC;
  return CNET_SCTP_DCEP_OK;
}

cnet_sctp_dcep_status cnet_sctp_dcep_open(cnet_sctp_dcep *d,
    uint16_t sid, uint8_t type, uint16_t priority, uint32_t reliability,
    const void *label, size_t label_len, const void *protocol, size_t proto_len,
    void *output, size_t cap, size_t *written) {
  cnet_sctp_dcep_status status = guard(d);
  cnet_sctp_dcep_channel *channel;
  uint8_t *bytes = (uint8_t *)output;
  size_t n = 12u + label_len + proto_len;
  if (status != CNET_SCTP_DCEP_OK) return status;
  if (!output || !written || (label_len && !label) ||
      (proto_len && !protocol) || sid >= d->streams)
    return CNET_SCTP_DCEP_INVALID;
  *written = 0u;
  if (((sid & 1u) == 0u) != (d->is_dtls_client != 0u))
    return CNET_SCTP_DCEP_PROTOCOL;
  channel = &d->channels[sid];
  if (channel->state) return CNET_SCTP_DCEP_BUSY;
  status = validate_type(type, reliability);
  if (status != CNET_SCTP_DCEP_OK) return status;
  if (label_len > CNET_SCTP_DCEP_TEXT_MAX ||
      proto_len > CNET_SCTP_DCEP_TEXT_MAX || n > cap)
    return CNET_SCTP_DCEP_CAPACITY;
  if (!valid_utf8((const uint8_t *)label, label_len) ||
      !valid_utf8((const uint8_t *)protocol, proto_len))
    return CNET_SCTP_DCEP_PROTOCOL;
  bytes[0] = DCEP_OPEN;
  bytes[1] = type;
  wr16(bytes + 2u, priority);
  /* Reliable OPEN must use 0, while received reliable params are ignored. */
  wr32(bytes + 4u, 0u);
  wr16(bytes + 8u, (uint16_t)label_len);
  wr16(bytes + 10u, (uint16_t)proto_len);
  if (label_len) memcpy(bytes + 12u, label, label_len);
  if (proto_len) memcpy(bytes + 12u + label_len, protocol, proto_len);
  channel->sid = sid;
  channel->channel_type = type;
  channel->priority = priority;
  channel->reliability = 0u;
  channel->label_size = label_len;
  channel->protocol_size = proto_len;
  if (label_len) memcpy(channel->label, label, label_len);
  if (proto_len) memcpy(channel->protocol, protocol, proto_len);
  channel->state = 1u;
  *written = n;
  return CNET_SCTP_DCEP_OK;
}

cnet_sctp_dcep_status cnet_sctp_dcep_input(cnet_sctp_dcep *d,
    uint16_t sid, const void *wire, size_t n,
    void *reply, size_t cap, size_t *written) {
  cnet_sctp_dcep_status status = guard(d);
  const uint8_t *bytes = (const uint8_t *)wire;
  cnet_sctp_dcep_channel *channel;
  size_t label_len, proto_len;
  if (status != CNET_SCTP_DCEP_OK) return status;
  if (!wire || !reply || !written || sid >= d->streams || n == 0u)
    return CNET_SCTP_DCEP_INVALID;
  *written = 0u;
  channel = &d->channels[sid];
  if (bytes[0] == DCEP_ACK) {
    if (n != 1u || (channel->state != 1u && channel->state != 2u))
      return CNET_SCTP_DCEP_PROTOCOL;
    channel->state = 2u;
    return CNET_SCTP_DCEP_OK;
  }
  if (bytes[0] != DCEP_OPEN || n < 12u)
    return CNET_SCTP_DCEP_PROTOCOL;
  /* The remote DTLS role must use the opposite SID parity. */
  /* RFC 8832: the remote DTLS client sends OPEN on even SIDs and
   * the remote DTLS server on odd SIDs. Compare with remote, not local role. */
  if (((sid & 1u) != 0u) != (d->is_dtls_client != 0u))
    return CNET_SCTP_DCEP_PROTOCOL;
  if (channel->state) return CNET_SCTP_DCEP_BUSY;
  status = validate_type(bytes[1], rd32(bytes + 4u));
  if (status != CNET_SCTP_DCEP_OK) return status;
  label_len = rd16(bytes + 8u);
  proto_len = rd16(bytes + 10u);
  if (label_len > CNET_SCTP_DCEP_TEXT_MAX ||
      proto_len > CNET_SCTP_DCEP_TEXT_MAX)
    return CNET_SCTP_DCEP_CAPACITY;
  if (n != 12u + label_len + proto_len ||
      !valid_utf8(bytes + 12u, label_len) ||
      !valid_utf8(bytes + 12u + label_len, proto_len))
    return CNET_SCTP_DCEP_PROTOCOL;
  if (cap < 1u) return CNET_SCTP_DCEP_CAPACITY;
  ((uint8_t *)reply)[0] = DCEP_ACK;
  *written = 1u;
  channel->sid = sid;
  channel->channel_type = bytes[1];
  channel->priority = rd16(bytes + 2u);
  channel->reliability = 0u;
  channel->label_size = label_len;
  channel->protocol_size = proto_len;
  if (label_len) memcpy(channel->label, bytes + 12u, label_len);
  if (proto_len) memcpy(channel->protocol, bytes + 12u + label_len, proto_len);
  channel->state = 2u;
  return CNET_SCTP_DCEP_OK;
}

int cnet_sctp_dcep_ready(const cnet_sctp_dcep *d, uint16_t stream) {
  if (!d || d->magic != DCEP_MAGIC || d->stopped ||
      !d->is_owner(d->context) || stream >= d->streams) return 0;
  return d->channels[stream].state == 2u;
}

cnet_sctp_dcep_status cnet_sctp_dcep_stop(cnet_sctp_dcep *d) {
  cnet_sctp_dcep_status status = guard(d);
  if (status != CNET_SCTP_DCEP_OK) return status;
  memset(d->channels, 0, sizeof(d->channels));
  d->stopped = 1u;
  return CNET_SCTP_DCEP_OK;
}
