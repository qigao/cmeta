#include "sctp_data_internal.h"

#include <limits.h>
#include <string.h>

/* C11-only fixed-capacity SCTP DATA/SACK sub-engine, no automatic I/O. */
#define DATA_MAGIC UINT32_C(0x53444331)
enum { CHUNK_DATA = 0, CHUNK_SACK = 3, DATA_HEADER = 12 };
enum { DATA_B = 2, DATA_E = 1, DATA_U = 4 };

static uint16_t rd16(const uint8_t *p) {
  return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}
static uint32_t rd32(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
         ((uint32_t)p[2] << 8) | p[3];
}
static void wr16(uint8_t *p, uint16_t n) {
  p[0] = (uint8_t)(n >> 8);
  p[1] = (uint8_t)n;
}
static void wr32(uint8_t *p, uint32_t n) {
  p[0] = (uint8_t)(n >> 24);
  p[1] = (uint8_t)(n >> 16);
  p[2] = (uint8_t)(n >> 8);
  p[3] = (uint8_t)n;
}
/* RFC 9260 serial arithmetic: compare in the unsigned half-range.
 * Converting wrapped uint32_t subtraction to int32_t is implementation-
 * defined in C11, so keep this portable across all supported toolchains. */
static int ahead(uint32_t a, uint32_t b) {
  const uint32_t delta = a - b;
  return delta != 0u && delta < UINT32_C(0x80000000);
}
static cnet_sctp_data_status guard(cnet_sctp_data *d) {
  if (!d || d->magic != DATA_MAGIC) return CNET_SCTP_DATA_INVALID;
  if (!d->config.is_owner(d->config.owner_context)) return CNET_SCTP_DATA_OWNER;
  if (d->closed) return CNET_SCTP_DATA_CLOSED;
  return CNET_SCTP_DATA_OK;
}
static cnet_sctp_data_status clock_check(const cnet_sctp_data *d, uint64_t now) {
  return now < d->last_time_ms ? CNET_SCTP_DATA_CLOCK : CNET_SCTP_DATA_OK;
}
/* Retained gap-acknowledged tickets are not outstanding for T3/rwnd.
 * They still occupy their original bounded TX slots until cumulative ACK. */
static cnet_sctp_data_tx *oldest_outstanding(cnet_sctp_data *d) {
  cnet_sctp_data_tx *oldest = NULL;
  unsigned int i;
  for (i = 0u; i < CNET_SCTP_DATA_TX_SLOTS; ++i) {
    cnet_sctp_data_tx *slot = &d->tx[i];
    if (slot->used && !slot->gap_acked &&
        (!oldest || ahead(oldest->tsn, slot->tsn))) oldest = slot;
  }
  return oldest;
}
/* The private one-chunk packet profile has a 12-byte common header.
 * rwnd/in_flight account for user-buffer bytes; congestion control accounts
 * for DATA header + payload + padding (RFC 9260 7.2). Derive this from the
 * retained wire tickets, avoiding another counter that can drift on reneging.
 * PMTU/DTLS negotiation and retransmission scheduling remain separate gates. */
#define CC_QUANTUM (CNET_SCTP_DATA_MTU - 12u)
#define CC_LIMIT (16u * CNET_SCTP_DATA_MTU)
/* A pending Fast Retransmit is an Owner-visible transmission obligation.
 * Do not allocate a fresh DATA TSN ahead of an eligible retained packet.
 * Pure intent does not reduce the actual flight or grant cwnd credit. */
static int has_pending_retransmit(const cnet_sctp_data *d) {
  unsigned int i;
  for (i = 0u; i < CNET_SCTP_DATA_TX_SLOTS; ++i) {
    const cnet_sctp_data_tx *tx = &d->tx[i];
    if (tx->used && !tx->gap_acked && !tx->fast_retransmitted &&
        tx->pending_retransmit) return 1;
  }
  return 0;
}
static int has_retained_tx(const cnet_sctp_data *d) {
  unsigned int i;
  for (i = 0u; i < CNET_SCTP_DATA_TX_SLOTS; ++i)
    if (d->tx[i].used) return 1;
  return 0;
}
static cnet_sctp_data_tx *find_retained(cnet_sctp_data *d, uint32_t tsn) {
  unsigned int i;
  for (i = 0u; i < CNET_SCTP_DATA_TX_SLOTS; ++i)
    if (d->tx[i].used && d->tx[i].tsn == tsn) return &d->tx[i];
  return NULL;
}
static uint64_t next_probe_interval(const cnet_sctp_data *d) {
  const uint64_t prior = d->probe_interval_ms ?
      d->probe_interval_ms : d->rto_ms;
  return prior >= d->config.t3_max_ms / 2u ?
      d->config.t3_max_ms : prior * 2u;
}
static size_t flight_bytes(const cnet_sctp_data *d) {
  size_t bytes = 0u;
  unsigned int i;
  for (i = 0u; i < CNET_SCTP_DATA_TX_SLOTS; ++i)
    if (d->tx[i].used && d->tx[i].flight_accounted)
      bytes += d->tx[i].packet_size - 12u;
  return bytes;
}

/* L=1 slow start and bounded congestion-avoidance byte credit. No credit
 * is taken for duplicate TSN reports or re-ACKs: this is deliberately more
 * conservative than the optional duplicate-byte credit in 7.2.2. Preserve
 * the existing no-gap-only-growth and recovery-exit-ACK protections. */
static void congestion_ack(cnet_sctp_data *d, size_t before,
    size_t newly_acked, size_t outstanding, uint32_t progress,
    int was_recovering) {
  uint32_t increment = 0u;
  if (!was_recovering) {
    if (d->cwnd <= d->ssthresh) {
      d->partial_bytes_acked = 0u;
      if (progress && before >= d->cwnd)
        increment = newly_acked < CC_QUANTUM ?
            (uint32_t)newly_acked : CC_QUANTUM;
    } else {
      /* Fixed 16-ticket ceiling bounds one ACK to 16 * 1040 bytes.
       * Cap retained credit as well, including gap-only ACK sequences. */
      /* Only cumulative TSN progress contributes CA byte credit.
       * Revocable Gap ACKs must not prepay future cwnd growth. */
      if (progress) d->partial_bytes_acked += (uint32_t)newly_acked;
      if (d->partial_bytes_acked > 2u * CC_LIMIT)
        d->partial_bytes_acked = 2u * CC_LIMIT;
      if (before < d->cwnd && d->partial_bytes_acked > d->cwnd)
        d->partial_bytes_acked = d->cwnd;
      if (progress && newly_acked && before >= d->cwnd &&
          d->partial_bytes_acked >= d->cwnd) {
        d->partial_bytes_acked -= d->cwnd;
        increment = CC_QUANTUM;
      }
    }
    if (d->cwnd >= CC_LIMIT) increment = 0u;
    else if (increment > CC_LIMIT - d->cwnd) increment = CC_LIMIT - d->cwnd;
    d->cwnd += increment;
  }
  if (!outstanding) d->partial_bytes_acked = 0u;
}

static size_t rx_available(const cnet_sctp_data *d) {
  size_t by_bytes = CNET_SCTP_DATA_RECEIVE_CEILING - d->buffered_rx;
  size_t free_slots = 0u;
  size_t i;
  for (i = 0u; i < CNET_SCTP_DATA_RX_SLOTS; ++i)
    if (!d->rx[i].used) ++free_slots;
  if (by_bytes > free_slots * CNET_SCTP_DATA_PAYLOAD_MAX)
    by_bytes = free_slots * CNET_SCTP_DATA_PAYLOAD_MAX;
  return by_bytes;
}
/* The prospective SACK rwnd must count the RX ticket consumed by this
 * DATA chunk, not merely subtract its user bytes.  A one-byte DATA still
 * occupies an entire bounded receive slot until take(). */
static size_t rx_window_after_admit(const cnet_sctp_data *d, size_t bytes) {
  size_t free_slots = 0u, space;
  unsigned int i;
  if (bytes > CNET_SCTP_DATA_RECEIVE_CEILING - d->buffered_rx) return 0u;
  for (i = 0u; i < CNET_SCTP_DATA_RX_SLOTS; ++i)
    if (!d->rx[i].used) ++free_slots;
  if (bytes != 0u) {
    if (free_slots == 0u) return 0u;
    --free_slots;
  }
  space = CNET_SCTP_DATA_RECEIVE_CEILING - d->buffered_rx - bytes;
  if (space > free_slots * CNET_SCTP_DATA_PAYLOAD_MAX)
    space = free_slots * CNET_SCTP_DATA_PAYLOAD_MAX;
  return space;
}
static cnet_sctp_data_status encode_one(cnet_sctp_data *d, uint8_t type,
    uint8_t flags, const uint8_t *value, size_t value_size,
    void *output, size_t cap, size_t *written) {
  cnet_sctp_wire_chunk ch;
  cnet_sctp_wire_status wire;
  ch.type = type;
  ch.flags = flags;
  ch.value = value;
  ch.value_size = value_size;
  wire = cnet_sctp_wire_encode(d->config.local_port,
      d->config.remote_port, d->config.peer_tag, &ch, 1u,
      output, cap, written);
  if (wire == CNET_SCTP_WIRE_CAPACITY) return CNET_SCTP_DATA_CAPACITY;
  return wire == CNET_SCTP_WIRE_OK ? CNET_SCTP_DATA_OK : CNET_SCTP_DATA_PROTOCOL;
}

cnet_sctp_data_status cnet_sctp_data_init(cnet_sctp_data *d,
    const cnet_sctp_data_config *cfg, uint64_t now) {
  if (!d || !cfg || !cfg->is_owner || !cfg->is_owner(cfg->owner_context) ||
      cfg->local_port == 0u || cfg->remote_port == 0u ||
      cfg->local_tag == 0u || cfg->peer_tag == 0u ||
      cfg->inbound_streams == 0u || cfg->outbound_streams == 0u ||
      cfg->inbound_streams > CNET_SCTP_DATA_STREAMS ||
      cfg->outbound_streams > CNET_SCTP_DATA_STREAMS ||
      cfg->peer_receive_window < CNET_SCTP_DATA_PAYLOAD_MAX ||
      cfg->t3_initial_ms == 0u ||
      cfg->t3_max_ms < cfg->t3_initial_ms ||
      cfg->t3_max_ms > UINT64_MAX - now ||
      cfg->max_retransmits == 0u)
    return CNET_SCTP_DATA_INVALID;
  if (d->magic == DATA_MAGIC) return CNET_SCTP_DATA_CLOSED;
  memset(d, 0, sizeof(*d));
  d->config = *cfg;
  d->next_tsn = cfg->local_initial_tsn;
  d->last_ack = cfg->local_initial_tsn - 1u;
  d->cumulative_rx = cfg->peer_initial_tsn - 1u;
  d->peer_rwnd = cfg->peer_receive_window;
  d->last_advertised_rwnd = CNET_SCTP_DATA_RECEIVE_CEILING;
  /* No IP-family metadata is admitted by this plaintext prototype yet.
   * Use the lower IPv6 initial bound for both families (7.2.1). */
  d->cwnd = 4u * CC_QUANTUM < 4344u ? 4u * CC_QUANTUM : 4344u;
  d->ssthresh = 16u * CNET_SCTP_DATA_MTU;
  d->rto_ms = cfg->t3_initial_ms;
  d->last_time_ms = now;
  d->magic = DATA_MAGIC;
  return CNET_SCTP_DATA_OK;
}

/* One DATA chunk is admitted only after the output packet and TX ticket
 * both fit. The caller fixes stream SSN and B/E flags for a fragmented
 * message; no implicit per-fragment SSN increment or borrowed views. */
static cnet_sctp_data_status send_frame(cnet_sctp_data *d, uint64_t now,
    uint16_t stream, uint16_t ssn, uint32_t ppid, uint8_t flags,
    const uint8_t *payload, size_t n, void *output, size_t cap,
    size_t *written, int zero_probe) {
  cnet_sctp_data_tx *slot = NULL;
  uint8_t value[DATA_HEADER + CNET_SCTP_DATA_PAYLOAD_MAX];
  size_t packet_bytes = 0u, i;
  cnet_sctp_data_status status;
  if (n == 0u || n > CNET_SCTP_DATA_PAYLOAD_MAX)
    return CNET_SCTP_DATA_INVALID;
  /* RFC 9260 6.1(B): one new packet may cross cwnd only while flight
   * was below it. Each supported DATA chunk is <= CC_QUANTUM, so this
   * admits at most cwnd + CC_QUANTUM - 1 padded bytes, never another packet.
   * The receiver-window check remains independent and is never overbooked. */
  if (d->t3_wait_ack || flight_bytes(d) >= d->cwnd)
    return CNET_SCTP_DATA_CAPACITY;
  /* One-byte DATA is the only exception to a closed receive window,
   * and only if no previous ticket is retained (RFC 9260 6.1(A)). */
  if (zero_probe) {
    if (n != 1u || d->peer_rwnd != 0u || has_retained_tx(d))
      return CNET_SCTP_DATA_CAPACITY;
  } else if (n > d->peer_rwnd || d->in_flight > d->peer_rwnd - n) {
    return CNET_SCTP_DATA_CAPACITY;
  }
  /* RFC 9260 6.1(C): first run Owner advance() for queued loss repair.
   * Both direct sends and fragmented pump() share this admission point. */
  if (has_pending_retransmit(d))
    return CNET_SCTP_DATA_RETRANSMIT_PENDING;
  for (i = 0u; i < CNET_SCTP_DATA_TX_SLOTS; ++i)
    if (!d->tx[i].used) { slot = &d->tx[i]; break; }
  if (!slot) return CNET_SCTP_DATA_CAPACITY;
  if (d->t3_deadline_ms == 0u && now > UINT64_MAX - d->rto_ms)
    return CNET_SCTP_DATA_CLOCK;
  wr32(value, d->next_tsn);
  wr16(value + 4u, stream);
  wr16(value + 6u, ssn);
  wr32(value + 8u, ppid);
  memcpy(value + DATA_HEADER, payload, n);
  status = encode_one(d, CHUNK_DATA, flags, value, DATA_HEADER + n,
                       output, cap, &packet_bytes);
  if (status != CNET_SCTP_DATA_OK) return status;
  memcpy(slot->packet, output, packet_bytes);
  slot->packet_size = packet_bytes;
  slot->user_size = n;
  slot->tsn = d->next_tsn++;
  slot->first_sent_ms = now;
  slot->retransmits = 0u;
  slot->gap_acked = 0u;
  slot->flight_accounted = 1u;
  slot->pending_retransmit = 0u;
  slot->ack_counted = 0u;
  slot->used = 1u;
  d->in_flight += n;
  if (d->t3_deadline_ms == 0u) d->t3_deadline_ms = now + d->rto_ms;
  d->last_time_ms = now;
  *written = packet_bytes;
  return CNET_SCTP_DATA_OK;
}

cnet_sctp_data_status cnet_sctp_data_send(cnet_sctp_data *d, uint64_t now,
    uint16_t stream, uint32_t ppid, int unordered, const void *payload,
    size_t n, void *output, size_t cap, size_t *written) {
  cnet_sctp_data_status status = guard(d);
  uint16_t seq;
  if (status != CNET_SCTP_DATA_OK) return status;
  if (!payload || !output || !written || n == 0u ||
      stream >= d->config.outbound_streams) return CNET_SCTP_DATA_INVALID;
  *written = 0u;
  if ((status = clock_check(d, now)) != CNET_SCTP_DATA_OK) return status;
  if (d->pending.active) return CNET_SCTP_DATA_CAPACITY;
  if (n > CNET_SCTP_DATA_PAYLOAD_MAX) return CNET_SCTP_DATA_CAPACITY;
  seq = unordered ? 0u : d->tx_ssn[stream];
  status = send_frame(d, now, stream, seq, ppid,
      (uint8_t)(DATA_B | DATA_E | (unordered ? DATA_U : 0u)),
      (const uint8_t *)payload, n, output, cap, written, 0);
  if (status == CNET_SCTP_DATA_OK && !unordered) ++d->tx_ssn[stream];
  return status;
}

/* Aggregate up to four DATA chunks without introducing any new lifetime
 * or retransmission ownership: each fragment is first admitted using the
 * ordinary per-TSN send path on a private bounded candidate.  The actual
 * output is committed as one CRC-correct wire packet only after all four
 * potential tickets and the final destination capacity have been checked. */
cnet_sctp_data_status cnet_sctp_data_send_bundle(cnet_sctp_data *d,
    uint64_t now, const cnet_sctp_data_bundle_item *items, size_t count,
    void *output, size_t cap, size_t *written) {
  cnet_sctp_data_status status = guard(d);
  cnet_sctp_data staged;
  uint8_t singles[CNET_SCTP_DATA_BUNDLE_MAX][CNET_SCTP_DATA_MTU];
  cnet_sctp_wire_chunk chunks[CNET_SCTP_DATA_BUNDLE_MAX];
  size_t i, packet_cap, retained_tx = 0u;
  if (status != CNET_SCTP_DATA_OK) return status;
  if (!items || !output || !written || count == 0u)
    return CNET_SCTP_DATA_INVALID;
  *written = 0u;
  if (count > CNET_SCTP_DATA_BUNDLE_MAX) return CNET_SCTP_DATA_CAPACITY;
  /* Retained tickets are the source of truth; there is intentionally no
   * shadow tx_count counter that could drift after SACK reneging/cleanup.
   * The staged ordinary send path below separately enforces byte rwnd and
   * congestion-window admission for each actual DATA chunk. */
  for (i = 0u; i < CNET_SCTP_DATA_TX_SLOTS; ++i)
    if (d->tx[i].used) ++retained_tx;
  if (count > CNET_SCTP_DATA_TX_SLOTS - retained_tx)
    return CNET_SCTP_DATA_CAPACITY;
  if ((status = clock_check(d, now)) != CNET_SCTP_DATA_OK) return status;
  memcpy(&staged, d, sizeof(staged));
  for (i = 0u; i < count; ++i) {
    cnet_sctp_wire_header h = {0};
    cnet_sctp_wire_decoder decoder = {0};
    size_t packet_size = 0u;
    status = cnet_sctp_data_send(&staged, now, items[i].stream_id,
        items[i].ppid, items[i].unordered, items[i].payload,
        items[i].payload_size, singles[i], sizeof(singles[i]),
        &packet_size);
    if (status != CNET_SCTP_DATA_OK) return status;
    if (cnet_sctp_wire_decode(singles[i], packet_size,
        &h, &decoder) != CNET_SCTP_WIRE_OK || h.chunk_count != 1u ||
        cnet_sctp_wire_next_chunk(&decoder, &chunks[i]) != CNET_SCTP_WIRE_OK ||
        chunks[i].type != CHUNK_DATA)
      return CNET_SCTP_DATA_PROTOCOL;
  }
  packet_cap = cap < CNET_SCTP_DATA_MTU ? cap : CNET_SCTP_DATA_MTU;
  /* Avoid leaving bytes in caller memory on any failure. Encode into a
   * private packet and publish only after the state candidate is accepted. */
  {
    uint8_t packet[CNET_SCTP_DATA_MTU];
    size_t packet_size = 0u;
    cnet_sctp_wire_status result = cnet_sctp_wire_encode(
        d->config.local_port, d->config.remote_port, d->config.peer_tag,
        chunks, count, packet, packet_cap, &packet_size);
    if (result == CNET_SCTP_WIRE_CAPACITY) return CNET_SCTP_DATA_CAPACITY;
    if (result != CNET_SCTP_WIRE_OK) return CNET_SCTP_DATA_PROTOCOL;
    memcpy(output, packet, packet_size);
    *written = packet_size;
  }
  memcpy(d, &staged, sizeof(staged));
  return CNET_SCTP_DATA_OK;
}

cnet_sctp_data_status cnet_sctp_data_queue_message(cnet_sctp_data *d,
    uint64_t now, uint16_t stream, uint32_t ppid, int unordered,
    const void *payload, size_t n) {
  cnet_sctp_data_status status = guard(d);
  if (status != CNET_SCTP_DATA_OK) return status;
  if (!payload || stream >= d->config.outbound_streams || n == 0u)
    return CNET_SCTP_DATA_INVALID;
  if ((status = clock_check(d, now)) != CNET_SCTP_DATA_OK) return status;
  if (d->pending.active || n > CNET_SCTP_DATA_MESSAGE_MAX)
    return CNET_SCTP_DATA_CAPACITY;
  /* A queued COPY supplies bytes for a later probe. Direct send remains
   * backpressured at rwnd=0; no raw input pointer is retained. */
  if (d->peer_rwnd == 0u && !has_retained_tx(d) &&
      now > UINT64_MAX - d->rto_ms) return CNET_SCTP_DATA_CLOCK;
  /* The entire user message is owned locally before any fragment leaves.
   * Caller may immediately free/reuse its input buffer. */
  memcpy(d->pending.bytes, payload, n);
  d->pending.size = n;
  d->pending.offset = 0u;
  d->pending.stream_id = stream;
  d->pending.ssn = unordered ? 0u : d->tx_ssn[stream];
  d->pending.ppid = ppid;
  d->pending.unordered = unordered != 0;
  d->pending.active = 1u;
  if (d->peer_rwnd == 0u && !has_retained_tx(d)) {
    d->probe_interval_ms = d->rto_ms;
    d->probe_deadline_ms = now + d->rto_ms;
  }
  d->last_time_ms = now;
  return CNET_SCTP_DATA_OK;
}

cnet_sctp_data_status cnet_sctp_data_pump(cnet_sctp_data *d,
    uint64_t now, void *output, size_t cap, size_t *written) {
  cnet_sctp_data_status status = guard(d);
  size_t n;
  uint8_t flags;
  if (status != CNET_SCTP_DATA_OK) return status;
  if (!output || !written) return CNET_SCTP_DATA_INVALID;
  *written = 0u;
  if ((status = clock_check(d, now)) != CNET_SCTP_DATA_OK) return status;
  if (!d->pending.active) return CNET_SCTP_DATA_EMPTY;
  n = d->pending.size - d->pending.offset;
  if (n > CNET_SCTP_DATA_PAYLOAD_MAX) n = CNET_SCTP_DATA_PAYLOAD_MAX;
  flags = (uint8_t)((d->pending.offset == 0u ? DATA_B : 0u) |
      (d->pending.offset + n == d->pending.size ? DATA_E : 0u) |
      (d->pending.unordered ? DATA_U : 0u));
  status = send_frame(d, now, d->pending.stream_id, d->pending.ssn,
      d->pending.ppid, flags, d->pending.bytes + d->pending.offset,
      n, output, cap, written, 0);
  if (status != CNET_SCTP_DATA_OK) return status;
  d->pending.offset += n;
  if (d->pending.offset == d->pending.size) {
    if (!d->pending.unordered) ++d->tx_ssn[d->pending.stream_id];
    /* No message body survives successful admission of the final fragment.
     * Retransmit tickets own independent full-wire copies. */
    memset(&d->pending, 0, sizeof(d->pending));
  }
  return CNET_SCTP_DATA_OK;
}

/* Creates RFC 9260 SACK with consecutive gap-ack blocks and, optionally,
 * one explicitly observed duplicate TSN. Caller supplies a *prospective*
 * cumulative TSN and gap bitmap, enabling capacity-before-state-change.
 */
static cnet_sctp_data_status sack_packet(cnet_sctp_data *d,
    uint32_t cumulative, uint32_t gaps, size_t window,
    int duplicate, uint32_t duplicate_tsn,
    void *out, size_t cap, size_t *written) {
  uint8_t value[12u + 16u * 4u + 4u] = {0};
  uint16_t runs = 0u;
  size_t offset = 12u, i = 0u;
  while (i < 32u) {
    size_t start, end;
    if ((gaps & (UINT32_C(1) << i)) == 0u) { ++i; continue; }
    start = i + 1u;
    while (i < 32u && (gaps & (UINT32_C(1) << i)) != 0u) ++i;
    end = i;
    /* A 32-bit bitmap has at most 16 alternating gap runs. */
    wr16(value + offset, (uint16_t)start);
    wr16(value + offset + 2u, (uint16_t)end);
    offset += 4u;
    ++runs;
  }
  wr32(value, cumulative);
  wr32(value + 4u, (uint32_t)window);
  wr16(value + 8u, runs);
  wr16(value + 10u, duplicate ? 1u : 0u);
  if (duplicate) {
    wr32(value + offset, duplicate_tsn);
    offset += 4u;
  }
  return encode_one(d, CHUNK_SACK, 0u, value, offset, out, cap, written);
}

static cnet_sctp_data_status input_data(cnet_sctp_data *d, uint64_t now,
    const cnet_sctp_wire_chunk *ch, void *out, size_t cap, size_t *written) {
  uint32_t tsn, prospective_cum, mask;
  int32_t distance;
  uint16_t stream, ssn;
  size_t bytes;
  cnet_sctp_data_rx *slot = NULL;
  cnet_sctp_data_status status;
  unsigned int i;
  int duplicate = 0;
  if ((ch->flags & (uint8_t)~(DATA_B | DATA_E | DATA_U)) != 0u ||
      ch->value_size < DATA_HEADER + 1u)
    return CNET_SCTP_DATA_PROTOCOL; /* fragments legal, empty RFC DATA not */
  bytes = ch->value_size - DATA_HEADER;
  if (bytes > CNET_SCTP_DATA_PAYLOAD_MAX) return CNET_SCTP_DATA_CAPACITY;
  tsn = rd32(ch->value);
  stream = rd16(ch->value + 4u);
  ssn = rd16(ch->value + 6u);
  if (stream >= d->config.inbound_streams) return CNET_SCTP_DATA_PROTOCOL;
  if ((ch->flags & DATA_U) != 0u && ssn != 0u)
    return CNET_SCTP_DATA_PROTOCOL;
  mask = d->rx_gaps;
  prospective_cum = d->cumulative_rx;
  /* Unsigned RFC serial arithmetic also rejects the ambiguous half-wrap. */
  if (!ahead(tsn, prospective_cum)) duplicate = 1;
  else if (tsn - prospective_cum > 32u) return CNET_SCTP_DATA_CAPACITY;
  distance = duplicate ? 0 : (int32_t)(tsn - prospective_cum);
  if (!duplicate &&
      (mask & (UINT32_C(1) << ((unsigned int)distance - 1u))) != 0u)
    duplicate = 1;
  if (!duplicate) {
    if (bytes > rx_available(d)) {
      /* RFC 9260 6.2: zero-window DATA is dropped without advancing the
       * receive TSN, but answered by a current-window SACK. */
      status = sack_packet(d, d->cumulative_rx, d->rx_gaps,
          rx_available(d), 0, 0u, out, cap, written);
      if (status == CNET_SCTP_DATA_OK) {
        d->last_advertised_rwnd = (uint32_t)rx_available(d);
        d->window_update_pending = 0u;
        d->last_time_ms = now;
      }
      return status;
    }
    for (i = 0u; i < CNET_SCTP_DATA_RX_SLOTS; ++i) {
      if (!d->rx[i].used) {
        if (!slot) slot = &d->rx[i];
        continue;
      }
      /* All fragments share an SSN. Reject two distinct ordered starts
       * with the same stream/SSN, but admit contiguous middle/end chunks.
       * The TSN bitmap already rejects exact duplicate fragments. */
      if ((ch->flags & DATA_B) && !(ch->flags & DATA_U) &&
          !d->rx[i].unordered && (d->rx[i].flags & DATA_B) &&
          d->rx[i].stream_id == stream && d->rx[i].ssn == ssn)
        return CNET_SCTP_DATA_PROTOCOL;
    }
    if (!slot) return CNET_SCTP_DATA_CAPACITY;
    mask |= UINT32_C(1) << ((unsigned int)distance - 1u);
    while ((mask & 1u) != 0u) {
      ++prospective_cum;
      mask >>= 1u;
    }
  }
  const size_t offered = rx_window_after_admit(
      d, duplicate ? 0u : bytes);
  status = sack_packet(d, prospective_cum, mask,
      offered, duplicate, tsn, out, cap, written);
  if (status != CNET_SCTP_DATA_OK) return status;
  if (!duplicate) {
    memcpy(slot->payload, ch->value + DATA_HEADER, bytes);
    slot->size = bytes;
    slot->tsn = tsn;
    slot->stream_id = stream;
    slot->ssn = ssn;
    slot->ppid = rd32(ch->value + 8u);
    slot->unordered = (ch->flags & DATA_U) != 0u;
    slot->flags = ch->flags;
    slot->used = 1u;
    d->buffered_rx += bytes;
    d->cumulative_rx = prospective_cum;
    d->rx_gaps = mask;
  }
  d->last_advertised_rwnd = (uint32_t)offered;
  d->window_update_pending = 0u;
  d->last_time_ms = now;
  return CNET_SCTP_DATA_OK;
}

/* RFC 6298-style fixed-point estimator; Karn excludes retransmissions
 * and previously gap-acknowledged tickets. All arithmetic is bounded. */
static void update_rtt(cnet_sctp_data *d, uint64_t sample) {
  uint64_t srtt, variation, delta, rto;
  if (sample == 0u) sample = 1u;
  if (sample > d->config.t3_max_ms) sample = d->config.t3_max_ms;
  /* 7*SRTT_x8 + 8*sample must fit uint64_t on every C11 target.
   * A 64th of UINT64_MAX still exceeds any practical transport RTO.
   * Clamp before the first sample and before fixed-point updates. */
  if (sample > UINT64_MAX / 64u) sample = UINT64_MAX / 64u;
  if (!d->rtt_valid) {
    d->srtt_x8_ms = sample * 8u;
    d->rttvar_x4_ms = sample * 2u;
    d->rtt_valid = 1u;
  } else {
    srtt = d->srtt_x8_ms / 8u;
    delta = srtt > sample ? srtt - sample : sample - srtt;
    d->rttvar_x4_ms = (3u * d->rttvar_x4_ms + 4u * delta) / 4u;
    d->srtt_x8_ms = (7u * d->srtt_x8_ms + 8u * sample) / 8u;
  }
  srtt = d->srtt_x8_ms / 8u;
  variation = d->rttvar_x4_ms;
  rto = srtt + (variation > 1u ? variation : 1u);
  if (rto < d->config.t3_initial_ms) rto = d->config.t3_initial_ms;
  if (rto > d->config.t3_max_ms) rto = d->config.t3_max_ms;
  d->rto_ms = rto;
}

static cnet_sctp_data_status input_sack(cnet_sctp_data *d, uint64_t now,
    const cnet_sctp_wire_chunk *ch) {
  uint32_t cumulative, peer_window, progress, sent_span;
  uint16_t blocks, duplicates, prev_end = 0u;
  /* 0=outstanding, 1=revocable gap ACK, 2=cumulative ACK (release). */
  uint8_t coverage[CNET_SCTP_DATA_TX_SLOTS] = {0};
  cnet_sctp_data_tx *oldest;
  size_t i, acked_bytes = 0u, outstanding_bytes = 0u;
  size_t retained_after = 0u;
  const size_t previous_flight = flight_bytes(d);
  uint64_t deadline;
  int earliest_acked = 0;
  uint64_t rtt_sample = 0u;
  int has_rtt_sample = 0;
  int t3_acknowledged = 0;
  int probe_cumul_acked = 0;
  const int was_probing = d->probe_active != 0u;
  if (ch->flags != 0u || ch->value_size < 12u)
    return CNET_SCTP_DATA_PROTOCOL;
  cumulative = rd32(ch->value);
  peer_window = rd32(ch->value + 4u);
  blocks = rd16(ch->value + 8u);
  duplicates = rd16(ch->value + 10u);
  if (blocks > 16u || duplicates > CNET_SCTP_DATA_RX_SLOTS ||
      ch->value_size != 12u + 4u * ((size_t)blocks + duplicates))
    return CNET_SCTP_DATA_PROTOCOL;
  progress = cumulative - d->last_ack;
  /* RFC 9260 6.2.1 D(i): an older SACK is dropped, not a transport error.
   * Exactly half a sequence space has no ordering and is rejected. */
  if (progress == UINT32_C(0x80000000)) return CNET_SCTP_DATA_PROTOCOL;
  if (progress > UINT32_C(0x80000000)) return CNET_SCTP_DATA_OK;
  sent_span = (d->next_tsn - 1u) - d->last_ack;
  if (progress > sent_span) return CNET_SCTP_DATA_PROTOCOL;
  for (i = 0u; i < blocks; ++i) {
    const uint8_t *p = ch->value + 12u + 4u * i;
    uint16_t first = rd16(p), end = rd16(p + 2u);
    if (!first || first <= prev_end || end < first ||
        (uint32_t)end > sent_span - progress)
      return CNET_SCTP_DATA_PROTOCOL;
    prev_end = end;
  }
  oldest = oldest_outstanding(d);
  /* Preview all ticket states and the new timer before mutating anything.
   * Invalid SACKs and timer-overflow refusals cannot partially settle TX. */
  for (i = 0u; i < CNET_SCTP_DATA_TX_SLOTS; ++i) {
    const cnet_sctp_data_tx *slot = &d->tx[i];
    uint32_t offset;
    size_t j;
    if (!slot->used) continue;
    if (slot->tsn - d->last_ack <= progress) coverage[i] = 2u;
    offset = slot->tsn - cumulative;
    for (j = 0u; !coverage[i] && j < blocks; ++j) {
      const uint8_t *p = ch->value + 12u + 4u * j;
      if (offset >= rd16(p) && offset <= rd16(p + 2u)) coverage[i] = 1u;
    }
    if (coverage[i] != 2u) ++retained_after;
    if (coverage[i] == 2u && d->probe_active &&
        slot->tsn == d->probe_tsn) probe_cumul_acked = 1;
    if (coverage[i]) {
      /* A revocable Gap ACK cannot release post-T3 send suppression.
       * Wait for irrevocable cumulative confirmation of the retransmit. */
      if (d->t3_wait_ack && coverage[i] == 2u &&
          slot->tsn == d->t3_wait_tsn)
        t3_acknowledged = 1;
      if (!has_rtt_sample && coverage[i] == 2u && !slot->gap_acked &&
          !slot->ack_counted && slot->retransmits == 0u &&
          !slot->probe_retransmitted && now >= slot->first_sent_ms) {
        rtt_sample = now - slot->first_sent_ms;
        has_rtt_sample = 1;
      }
      if (!slot->ack_counted) acked_bytes += slot->packet_size - 12u;
      if (slot == oldest) earliest_acked = 1;
    } else {
      /* Also restores bytes previously gap-acked but now omitted (reneged). */
      outstanding_bytes += slot->user_size;
    }
  }
  /* Check deadline arithmetic before committing ANY SACK state. */
  if (((peer_window == 0u && d->pending.active && retained_after == 0u) ||
       (peer_window != 0u && was_probing && retained_after != 0u)) &&
      now > UINT64_MAX - d->config.t3_max_ms)
    return CNET_SCTP_DATA_CLOCK;
  deadline = d->t3_deadline_ms;
  if (outstanding_bytes == 0u) {
    deadline = 0u; /* RFC 9260 6.3.2 R2, even if gap tickets are retained. */
  } else if (deadline == 0u || earliest_acked) {
    /* R3: restart only for the earliest outstanding TSN, not any later ACK.
     * R4: a retracted Gap ACK restarts a stopped timer. Preserve RTO backoff;
     * an ACK alone is NOT an unambiguous RTT sample (6.3.1 C5 / 6.3.3). */
    if (now > UINT64_MAX - d->rto_ms) return CNET_SCTP_DATA_CLOCK;
    deadline = now + d->rto_ms;
  }
  /* A newly sampled RTT can raise RTO after the earlier timer preview.
   * Reserve the maximum permitted deadline before settling any tickets. */
  if (has_rtt_sample && outstanding_bytes &&
      (earliest_acked || deadline == 0u) &&
      now > UINT64_MAX - d->config.t3_max_ms)
    return CNET_SCTP_DATA_CLOCK;
  /* RFC 9260 7.2.4: HTNA normally supplies miss indications. During
   * Fast Recovery, a cumulative advance also reports every remaining hole
   * below this SACK's highest Gap ACK, even without a newly ACKed higher
   * TSN. Do not infer loss for an unreported tail beyond all Gap ACKs.
   * 6.2.1 D(iii): withdrawing a previous Gap ACK is one miss indication.
   * Evaluate old ticket state before committing any ACK flags. */
  {
    uint32_t highest_new = 0u;
    int has_higher = 0;
    for (i = 0u; i < CNET_SCTP_DATA_TX_SLOTS; ++i) {
      const cnet_sctp_data_tx *slot = &d->tx[i];
      if (!slot->used || !coverage[i] || slot->ack_counted) continue;
      if (!has_higher || ahead(slot->tsn, highest_new)) {
        highest_new = slot->tsn;
        has_higher = 1;
      }
    }
    for (i = 0u; i < CNET_SCTP_DATA_TX_SLOTS; ++i) {
      cnet_sctp_data_tx *slot = &d->tx[i];
      const int recovery_hole = d->recovery_active && progress != 0u &&
          blocks != 0u && ahead(cumulative + prev_end, slot->tsn);
      if (slot->used && coverage[i] == 0u &&
          (slot->gap_acked || recovery_hole ||
           (has_higher && ahead(highest_new, slot->tsn))) &&
          slot->missing_reports < 3u)
        ++slot->missing_reports;
    }
  }
  for (i = 0u; i < CNET_SCTP_DATA_TX_SLOTS; ++i) {
    cnet_sctp_data_tx *slot = &d->tx[i];
    if (!slot->used) continue;
    if (!coverage[i] && slot->missing_reports >= 3u &&
        !slot->fast_retransmitted) slot->pending_retransmit = 1u;
    if (coverage[i] == 2u) {
      memset(slot, 0, sizeof(*slot));
    } else {
      slot->gap_acked = coverage[i];
      slot->flight_accounted = coverage[i] == 0u;
      if (coverage[i]) slot->pending_retransmit = 0u;
      if (coverage[i]) {
        slot->ack_counted = 1u;
        slot->missing_reports = 0u; /* a received TSN breaks the miss streak */
      }
    }
  }
  /* RTT observations from ACKs during fast recovery must not inflate
   * the congestion window when that same ACK exits the recovery epoch. */
  const int was_recovering = d->recovery_active != 0u;
  if (has_rtt_sample) {
    update_rtt(d, rtt_sample);
    if (outstanding_bytes && (earliest_acked || deadline == 0u))
      deadline = now + d->rto_ms;
  }
  if (t3_acknowledged) d->t3_wait_ack = 0u;
  d->last_ack = cumulative;
  if (d->recovery_active && !ahead(d->recovery_exit_tsn, cumulative))
    d->recovery_active = 0u;
  d->peer_rwnd = peer_window;
  d->in_flight = outstanding_bytes;
  d->t3_deadline_ms = deadline;
  if (peer_window == 0u) {
    if (was_probing) d->probe_unanswered = 0u;
    if (probe_cumul_acked) {
      d->probe_active = 0u;
      d->probe_deadline_ms = 0u;
      d->probe_interval_ms = 0u;
    }
    if (!d->probe_active && d->pending.active && retained_after == 0u) {
      /* Identical zero-window SACKs must NOT indefinitely postpone the
       * first probe. Restart only after a cumulatively ACKed probe. */
      if (d->probe_deadline_ms == 0u) {
        d->probe_interval_ms = d->rto_ms;
        d->probe_deadline_ms = now + d->rto_ms;
      }
    } else if (!d->probe_active) {
      d->probe_interval_ms = 0u;
      d->probe_deadline_ms = 0u;
    }
  } else {
    /* Window reopening while a probe is unacknowledged returns the same
     * retained TSN to ordinary T3; no extra DATA flight is allocated. */
    d->probe_active = 0u;
    d->probe_deadline_ms = 0u;
    d->probe_interval_ms = 0u;
    d->probe_unanswered = 0u;
    if (was_probing && retained_after != 0u)
      d->t3_deadline_ms = now + d->rto_ms;
  }
  congestion_ack(d, previous_flight, acked_bytes, outstanding_bytes,
                 progress, was_recovering);
  d->last_time_ms = now;
  return CNET_SCTP_DATA_OK;
}

/* RFC 9260 SHUTDOWN carries the peer's cumulative TSN acknowledgement.
 * Reuse exactly the validated SACK settlement/reneging/T3 path without
 * allocating a second TX owner or fabricating a new packet on the wire. */
cnet_sctp_data_status cnet_sctp_data_shutdown_cumack(cnet_sctp_data *d,
    uint64_t now, uint32_t cumulative) {
  cnet_sctp_data_status status = guard(d);
  cnet_sctp_wire_chunk sack = {0};
  uint8_t fields[12] = {0};
  if (status != CNET_SCTP_DATA_OK) return status;
  if ((status = clock_check(d, now)) != CNET_SCTP_DATA_OK) return status;
  wr32(fields, cumulative);
  wr32(fields + 4u, d->peer_rwnd);
  sack.type = CHUNK_SACK;
  sack.value = fields;
  sack.value_size = sizeof(fields);
  return input_sack(d, now, &sack);
}
int cnet_sctp_data_outbound_drained(const cnet_sctp_data *d) {
  unsigned int i;
  if (!d || d->magic != DATA_MAGIC || d->pending.active) return 0;
  for (i = 0u; i < CNET_SCTP_DATA_TX_SLOTS; ++i)
    if (d->tx[i].used) return 0;
  return 1;
}

cnet_sctp_data_status cnet_sctp_data_input(cnet_sctp_data *d, uint64_t now,
    const void *packet, size_t packet_size, void *out, size_t cap,
    size_t *written) {
  cnet_sctp_data_status status = guard(d);
  cnet_sctp_wire_header header;
  cnet_sctp_wire_decoder parser;
  cnet_sctp_wire_chunk chunk;
  if (status != CNET_SCTP_DATA_OK) return status;
  if (!packet || !out || !written) return CNET_SCTP_DATA_INVALID;
  *written = 0u;
  if ((status = clock_check(d, now)) != CNET_SCTP_DATA_OK) return status;
  if (cnet_sctp_wire_decode(packet, packet_size, &header, &parser) !=
      CNET_SCTP_WIRE_OK ||
      header.source_port != d->config.remote_port ||
      header.destination_port != d->config.local_port ||
      header.verification_tag != d->config.local_tag ||
      cnet_sctp_wire_next_chunk(&parser, &chunk) != CNET_SCTP_WIRE_OK)
    return CNET_SCTP_DATA_PROTOCOL;
  if (header.chunk_count == 1u) {
    if (chunk.type == CHUNK_DATA)
      return input_data(d, now, &chunk, out, cap, written);
    if (chunk.type == CHUNK_SACK)
      return input_sack(d, now, &chunk);
    return CNET_SCTP_DATA_PROTOCOL;
  }
  /* RFC 9260 6.9: receive multiple DATA chunks and at most one
   * leading SACK. The sender need not bundle; the receiver must process
   * legal bundling. Keep the existing 1200-byte private path MTU.
   * Every chunk is validated on a bounded state snapshot, including the
   * later fragments; an invalid final chunk or too-small SACK output
   * cannot partially admit any preceding DATA or settle TX tickets. */
  if (packet_size > CNET_SCTP_DATA_MTU)
    return CNET_SCTP_DATA_CAPACITY;
  {
    cnet_sctp_data staged;
    uint8_t response[CNET_SCTP_DATA_MTU];
    size_t reply_size = 0u, i;
    memcpy(&staged, d, sizeof(staged));
    for (i = 0u; i < header.chunk_count; ++i) {
      if (i != 0u &&
          cnet_sctp_wire_next_chunk(&parser, &chunk) != CNET_SCTP_WIRE_OK)
        return CNET_SCTP_DATA_PROTOCOL;
      if (chunk.type == CHUNK_SACK) {
        /* Control chunks precede DATA. A second SACK is not a legal
         * extension of this single-path private profile. */
        if (i != 0u) return CNET_SCTP_DATA_PROTOCOL;
        status = input_sack(&staged, now, &chunk);
      } else if (chunk.type == CHUNK_DATA) {
        size_t packet_bytes = 0u;
        status = input_data(&staged, now, &chunk,
            response, sizeof(response), &packet_bytes);
        if (status == CNET_SCTP_DATA_OK) reply_size = packet_bytes;
      } else {
        return CNET_SCTP_DATA_PROTOCOL;
      }
      if (status != CNET_SCTP_DATA_OK) return status;
    }
    if (reply_size > cap) return CNET_SCTP_DATA_CAPACITY;
    if (reply_size != 0u) memcpy(out, response, reply_size);
    memcpy(d, &staged, sizeof(staged));
    *written = reply_size;
    return CNET_SCTP_DATA_OK;
  }
}

static cnet_sctp_data_rx *find_rx_tsn(cnet_sctp_data *d, uint32_t tsn) {
  size_t i;
  for (i = 0u; i < CNET_SCTP_DATA_RX_SLOTS; ++i)
    if (d->rx[i].used && d->rx[i].tsn == tsn) return &d->rx[i];
  return NULL;
}

cnet_sctp_data_status cnet_sctp_data_take(cnet_sctp_data *d,
    cnet_sctp_data_message *meta, void *out, size_t cap,
    size_t *written) {
  cnet_sctp_data_status status = guard(d);
  unsigned int i;
  if (status != CNET_SCTP_DATA_OK) return status;
  if (!meta || !out || !written) return CNET_SCTP_DATA_INVALID;
  *written = 0u;
  for (i = 0u; i < CNET_SCTP_DATA_RX_SLOTS; ++i) {
    cnet_sctp_data_rx *start = &d->rx[i];
    uint32_t tsn;
    size_t total = 0u, count;
    int complete = 0;
    if (!start->used || (start->flags & DATA_B) == 0u ||
        (!start->unordered && start->ssn != d->rx_ssn[start->stream_id]))
      continue;
    /* RFC 9260 DATA fragments of one message have contiguous TSNs and
     * identical SID/SSN/PPID/U; they may arrive out of order. Scan only
     * the bounded RX slots. No partial user delivery or borrowed pointer. */
    tsn = start->tsn;
    for (count = 0u; count < CNET_SCTP_DATA_RX_SLOTS; ++count, ++tsn) {
      cnet_sctp_data_rx *frag = find_rx_tsn(d, tsn);
      if (!frag) break; /* incomplete: gap missing */
      if (frag->stream_id != start->stream_id ||
          frag->ssn != start->ssn || frag->ppid != start->ppid ||
          frag->unordered != start->unordered ||
          (count != 0u && (frag->flags & DATA_B) != 0u))
        return CNET_SCTP_DATA_PROTOCOL; /* no legacy interleaving */
      if (total > CNET_SCTP_DATA_MESSAGE_MAX - frag->size)
        return CNET_SCTP_DATA_CAPACITY;
      total += frag->size;
      if (frag->flags & DATA_E) { complete = 1; break; }
    }
    if (!complete) continue;
    if (total > cap) return CNET_SCTP_DATA_CAPACITY;
    /* Snapshot metadata before freeing its first fragment. The entire
     * message is committed at once, never exposing erased RX storage. */
    {
      const uint16_t stream_id = start->stream_id;
      const uint16_t stream_ssn = start->ssn;
      const uint32_t ppid = start->ppid;
      const uint32_t start_tsn = start->tsn;
      const uint8_t unordered = start->unordered;
      size_t copied = 0u;
      uint32_t consume_tsn = start->tsn;
      uint8_t last_flags;
      do {
        cnet_sctp_data_rx *frag = find_rx_tsn(d, consume_tsn);
        last_flags = frag->flags;
        memcpy((uint8_t *)out + copied, frag->payload, frag->size);
        copied += frag->size;
        d->buffered_rx -= frag->size;
        memset(frag, 0, sizeof(*frag));
        ++consume_tsn;
      } while ((last_flags & DATA_E) == 0u);
      meta->stream_id = stream_id;
      meta->stream_sequence = stream_ssn;
      meta->ppid = ppid;
      meta->tsn = start_tsn;
      meta->size = copied;
      meta->unordered = unordered;
      *written = copied;
      if (!unordered) ++d->rx_ssn[stream_id];
      /* ULP released RX tickets: schedule an explicit SACK when a closed
       * advertised window has reopened by at least one full payload slot. */
      if (d->last_advertised_rwnd == 0u &&
          rx_available(d) >= CNET_SCTP_DATA_PAYLOAD_MAX)
        d->window_update_pending = 1u;
    }
    return CNET_SCTP_DATA_OK;
  }
  return CNET_SCTP_DATA_EMPTY;
}

/* RFC 9260 6.2: Owner explicitly emits a reopened-window SACK, not a
 * timer/callback. Capacity refusal must preserve all receive state. */
cnet_sctp_data_status cnet_sctp_data_window_update(cnet_sctp_data *d,
    uint64_t now, void *out, size_t cap, size_t *written) {
  cnet_sctp_data_status status = guard(d);
  size_t available;
  if (status != CNET_SCTP_DATA_OK) return status;
  if (!out || !written) return CNET_SCTP_DATA_INVALID;
  *written = 0u;
  if ((status = clock_check(d, now)) != CNET_SCTP_DATA_OK) return status;
  if (!d->window_update_pending) return CNET_SCTP_DATA_EMPTY;
  available = rx_available(d);
  if (available < CNET_SCTP_DATA_PAYLOAD_MAX)
    return CNET_SCTP_DATA_EMPTY;
  status = sack_packet(d, d->cumulative_rx, d->rx_gaps, available,
      0, 0u, out, cap, written);
  if (status != CNET_SCTP_DATA_OK) return status;
  d->last_advertised_rwnd = (uint32_t)available;
  d->window_update_pending = 0u;
  d->last_time_ms = now;
  return CNET_SCTP_DATA_OK;
}

cnet_sctp_data_status cnet_sctp_data_advance(cnet_sctp_data *d, uint64_t now,
    void *out, size_t cap, size_t *written) {
  cnet_sctp_data_status status = guard(d);
  cnet_sctp_data_tx *oldest;
  if (status != CNET_SCTP_DATA_OK) return status;
  if (!out || !written) return CNET_SCTP_DATA_INVALID;
  *written = 0u;
  if ((status = clock_check(d, now)) != CNET_SCTP_DATA_OK) return status;
  /* RFC 9260 6.1(A): Owner-driven zero-window probing after one RTO,
   * exponentially bounded. No T3 cwnd reduction or retransmit-budget
   * consumption while the peer continues advertising rwnd=0. */
  if (d->peer_rwnd == 0u && d->probe_deadline_ms != 0u) {
    if (now < d->probe_deadline_ms) {
      d->last_time_ms = now;
      return CNET_SCTP_DATA_OK;
    }
    {
      const uint64_t interval = next_probe_interval(d);
      if (now > UINT64_MAX - interval) return CNET_SCTP_DATA_CLOCK;
      if (d->probe_active) {
        cnet_sctp_data_tx *probe = find_retained(d, d->probe_tsn);
        if (!probe) return CNET_SCTP_DATA_PROTOCOL;
        /* If NO SACK arrives across bounded retries, the peer may be
         * dead rather than persistently window-closed.  Responsive peers
         * reset this counter on every accepted SACK, even with rwnd=0. */
        if (d->probe_unanswered >= d->config.max_retransmits) {
          d->closed = 1u;
          return CNET_SCTP_DATA_EXPIRED;
        }
        if (probe->packet_size > cap) return CNET_SCTP_DATA_CAPACITY;
        memcpy(out, probe->packet, probe->packet_size);
        *written = probe->packet_size;
        /* Retried zero-window probes are Karn-ambiguous without consuming
         * ordinary loss/error retransmission budget. */
        probe->probe_retransmitted = 1u;
        ++d->probe_unanswered;
        d->probe_interval_ms = interval;
        d->probe_deadline_ms = now + interval;
        d->last_time_ms = now;
        return CNET_SCTP_DATA_OK;
      }
      if (d->pending.active && !has_retained_tx(d)) {
        const uint8_t flags = (uint8_t)(
            (d->pending.offset == 0u ? DATA_B : 0u) |
            (d->pending.offset + 1u == d->pending.size ? DATA_E : 0u) |
            (d->pending.unordered ? DATA_U : 0u));
        status = send_frame(d, now, d->pending.stream_id, d->pending.ssn,
            d->pending.ppid, flags, d->pending.bytes + d->pending.offset,
            1u, out, cap, written, 1);
        if (status != CNET_SCTP_DATA_OK) return status;
        d->probe_tsn = d->next_tsn - 1u;
        d->probe_active = 1u;
        d->probe_unanswered = 0u;
        ++d->pending.offset;
        if (d->pending.offset == d->pending.size) {
          if (!d->pending.unordered) ++d->tx_ssn[d->pending.stream_id];
          memset(&d->pending, 0, sizeof(d->pending));
        }
        d->probe_interval_ms = interval;
        d->probe_deadline_ms = now + interval;
        return CNET_SCTP_DATA_OK;
      }
      d->probe_deadline_ms = 0u;
      d->probe_interval_ms = 0u;
    }
  }
  /* A due T3 timeout owns congestion recovery. Do not allow a newly
   * eligible fast retransmission to postpone an already-expired T3. */
  if (d->t3_deadline_ms && now >= d->t3_deadline_ms)
    goto t3_due;
  if (d->t3_wait_ack) {
    d->last_time_ms = now;
    return CNET_SCTP_DATA_OK;
  }
  /* Fast retransmit is emitted only by the Owner's explicit advance().
   * No hidden send, callback, or additional retransmit buffer is created. */
  {
    cnet_sctp_data_tx *fast = NULL;
    unsigned int i;
    for (i = 0u; i < CNET_SCTP_DATA_TX_SLOTS; ++i) {
      cnet_sctp_data_tx *slot = &d->tx[i];
      if (slot->used && !slot->gap_acked && !slot->fast_retransmitted &&
          slot->pending_retransmit &&
          (!fast || ahead(fast->tsn, slot->tsn)))
        fast = slot;
    }
    if (fast) {
      /* RFC 9260 7.2.4 permits the first fast retransmission to cross
       * the reduced cwnd, but subsequent retransmissions must wait until
       * the actual outstanding flight has drained to the new window.
       * Pending loss intent does not itself grant congestion credit. */
      if (d->recovery_active && flight_bytes(d) > d->cwnd)
        return CNET_SCTP_DATA_CAPACITY;
      /* 7.2.4(4): restart only when retransmitting the first outstanding
       * DATA chunk (or starting a stopped timer). Later losses must not
       * postpone that chunk's deadline. Preflight before any TX mutation. */
      const int restart_t3 = !d->t3_deadline_ms ||
          fast == oldest_outstanding(d);
      /* Fast retransmission consumes the same bounded retransmit budget as
       * T3; exhaustion must not silently emit an additional packet. */
      if (fast->retransmits >= d->config.max_retransmits) {
        d->closed = 1u;
        return CNET_SCTP_DATA_EXPIRED;
      }
      if (fast->packet_size > cap) return CNET_SCTP_DATA_CAPACITY;
      if (restart_t3 && now > UINT64_MAX - d->rto_ms)
        return CNET_SCTP_DATA_CLOCK;
      memcpy(out, fast->packet, fast->packet_size);
      *written = fast->packet_size;
      fast->fast_retransmitted = 1u;
      fast->pending_retransmit = 0u;
      fast->flight_accounted = 1u;
      ++fast->retransmits;
      /* Enter one recovery epoch, bounded by the highest TSN already
       * sent. Additional losses in that epoch reuse its window reduction. */
      if (!d->recovery_active) {
        d->ssthresh = d->cwnd / 2u;
        if (d->ssthresh < 4u * CNET_SCTP_DATA_MTU)
          d->ssthresh = 4u * CNET_SCTP_DATA_MTU;
        d->cwnd = d->ssthresh;
        d->partial_bytes_acked = 0u;
        d->recovery_exit_tsn = d->next_tsn - 1u;
        d->recovery_active = 1u;
      }
      if (restart_t3) d->t3_deadline_ms = now + d->rto_ms;
      d->last_time_ms = now;
      return CNET_SCTP_DATA_OK;
    }
  }
  if (!d->t3_deadline_ms || now < d->t3_deadline_ms) {
    d->last_time_ms = now;
    return CNET_SCTP_DATA_OK;
  }
t3_due:
  oldest = oldest_outstanding(d);
  if (!oldest) {
    d->t3_deadline_ms = 0u;
    d->last_time_ms = now;
    return CNET_SCTP_DATA_OK;
  }
  if (oldest->retransmits >= d->config.max_retransmits) {
    d->closed = 1u; /* terminal is observable, no background retry */
    return CNET_SCTP_DATA_EXPIRED;
  }
  if (oldest->packet_size > cap) return CNET_SCTP_DATA_CAPACITY;
  if (now > UINT64_MAX - d->config.t3_max_ms) return CNET_SCTP_DATA_CLOCK;
  memcpy(out, oldest->packet, oldest->packet_size);
  *written = oldest->packet_size;
  ++oldest->retransmits;
  oldest->pending_retransmit = 0u;
  oldest->flight_accounted = 1u;
  d->t3_wait_tsn = oldest->tsn;
  d->t3_wait_ack = 1u;
  /* Timeout starts a new loss epoch. Old SACK-derived evidence cannot
   * trigger a second fast retransmission without fresh higher-TSN ACKs. */
  {
    unsigned int i;
    for (i = 0u; i < CNET_SCTP_DATA_TX_SLOTS; ++i)
      if (d->tx[i].used) {
        d->tx[i].missing_reports = 0u;
        d->tx[i].pending_retransmit = 0u;
      }
  }
  d->ssthresh = d->cwnd / 2u;
  if (d->ssthresh < 4u * CNET_SCTP_DATA_MTU)
    d->ssthresh = 4u * CNET_SCTP_DATA_MTU;
  d->cwnd = CNET_SCTP_DATA_MTU; /* timeout recovery, not a full cwnd model */
  d->recovery_active = 0u; /* T3 owns the next congestion response. */
  d->partial_bytes_acked = 0u;
  d->rto_ms = d->rto_ms > d->config.t3_max_ms / 2u ?
      d->config.t3_max_ms : d->rto_ms * 2u;
  d->t3_deadline_ms = now + d->rto_ms;
  d->last_time_ms = now;
  return CNET_SCTP_DATA_OK;
}

cnet_sctp_data_status cnet_sctp_data_deadline(cnet_sctp_data *d,
    uint64_t *deadline, int *has_deadline) {
  cnet_sctp_data_status status = guard(d);
  if (status != CNET_SCTP_DATA_OK) return status;
  if (!deadline || !has_deadline) return CNET_SCTP_DATA_INVALID;
  *has_deadline = d->t3_deadline_ms != 0u;
  *deadline = d->t3_deadline_ms;
  if (d->peer_rwnd == 0u && d->probe_deadline_ms &&
      (d->probe_active || !has_retained_tx(d))) {
    *has_deadline = 1;
    *deadline = d->probe_deadline_ms;
  }
  /* A pending fast retransmission is runnable now under the existing Owner,
   * unless recovery cwnd actually prevents emission.  The application
   * need not wait for T3 before it calls advance(). */
  if (!d->t3_wait_ack && has_pending_retransmit(d) &&
      (!d->recovery_active || flight_bytes(d) <= d->cwnd) &&
      (!*has_deadline || d->last_time_ms < *deadline)) {
    *has_deadline = 1;
    *deadline = d->last_time_ms;
  }
  return CNET_SCTP_DATA_OK;
}

cnet_sctp_data_status cnet_sctp_data_stop(cnet_sctp_data *d) {
  cnet_sctp_data_status status = guard(d);
  if (status != CNET_SCTP_DATA_OK) return status;
  memset(d->tx, 0, sizeof(d->tx));
  memset(d->rx, 0, sizeof(d->rx));
  memset(&d->pending, 0, sizeof(d->pending));
  d->in_flight = 0u;
  d->t3_wait_ack = 0u;
  d->partial_bytes_acked = 0u;
  d->buffered_rx = 0u;
  d->rx_gaps = 0u;
  d->last_advertised_rwnd = 0u;
  d->window_update_pending = 0u;
  d->t3_deadline_ms = 0u;
  d->probe_deadline_ms = 0u;
  d->probe_interval_ms = 0u;
  d->probe_active = 0u;
  d->probe_unanswered = 0u;
  d->closed = 1u;
  return CNET_SCTP_DATA_OK;
}
