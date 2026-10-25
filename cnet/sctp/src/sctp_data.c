#include "sctp_data_internal.h"

#include <limits.h>
#include <string.h>

/* C11-only fixed-capacity SCTP DATA/SACK sub-engine, no automatic I/O. */
#define DATA_MAGIC UINT32_C(0x53444331)
enum {
  CHUNK_DATA = 0,
  CHUNK_I_DATA = 64,
  CHUNK_SACK = 3,
  CHUNK_FORWARD_TSN = 192,
  CHUNK_I_FORWARD_TSN = 194,
  DATA_HEADER = 12,
  I_DATA_HEADER = 16
};
enum { DATA_B = 2, DATA_E = 1, DATA_U = 4 };
enum {
  PR_NO_LOSS = 0,
  PR_FAST_LOSS = 1,
  PR_T3_LOSS = 2,
  PR_PROBE_RETRANSMIT = 3
};
static const cnet_sctp_data_reliability RELIABLE_POLICY = {
  CNET_SCTP_DATA_RELIABLE, 0u
};

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
static int reliability_valid(const cnet_sctp_data *d,
    const cnet_sctp_data_reliability *reliability) {
  return reliability &&
      reliability->mode >= CNET_SCTP_DATA_RELIABLE &&
      reliability->mode <= CNET_SCTP_DATA_RELIABILITY_TIMED &&
      (reliability->mode != CNET_SCTP_DATA_RELIABLE ||
       reliability->value == 0u) &&
      (reliability->mode == CNET_SCTP_DATA_RELIABLE ||
       d->config.forward_tsn_supported) &&
      (reliability->mode != CNET_SCTP_DATA_RELIABILITY_TIMED ||
       reliability->value != 0u);
}
static int pr_retransmits_exhausted(const cnet_sctp_data_tx *tx) {
  return tx->reliability_mode == CNET_SCTP_DATA_RELIABILITY_RETRANSMITS &&
      (uint64_t)tx->retransmits + tx->probe_retransmits >=
          tx->reliability_value;
}
/* Retained gap-acknowledged tickets are not outstanding for T3/rwnd.
 * They still occupy their original bounded TX slots until cumulative ACK. */
static cnet_sctp_data_tx *oldest_outstanding(cnet_sctp_data *d) {
  cnet_sctp_data_tx *oldest = NULL;
  unsigned int i;
  for (i = 0u; i < CNET_SCTP_DATA_TX_SLOTS; ++i) {
    cnet_sctp_data_tx *slot = &d->tx[i];
    if (slot->used && !slot->abandoned && !slot->gap_acked &&
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
static int same_tx_message(const cnet_sctp_data_tx *a,
    const cnet_sctp_data_tx *b) {
  return a->used && b->used &&
      a->stream_id == b->stream_id &&
      a->unordered == b->unordered &&
      a->interleaved == b->interleaved &&
      a->message_first_tsn == b->message_first_tsn;
}
static size_t active_pr_messages(const cnet_sctp_data *d) {
  size_t count = 0u, i, j;
  for (i = 0u; i < CNET_SCTP_DATA_TX_SLOTS; ++i) {
    const cnet_sctp_data_tx *tx = &d->tx[i];
    int seen = 0;
    if (!tx->used || tx->reliability_mode == CNET_SCTP_DATA_RELIABLE)
      continue;
    for (j = 0u; j < i; ++j)
      if (same_tx_message(tx, &d->tx[j])) { seen = 1; break; }
    if (!seen) ++count;
  }
  if (d->pending.active &&
      d->pending.reliability.mode != CNET_SCTP_DATA_RELIABLE) {
    int represented = 0;
    if (d->pending.has_tsn) {
      for (i = 0u; i < CNET_SCTP_DATA_TX_SLOTS; ++i)
        if (d->tx[i].used &&
            d->tx[i].message_first_tsn == d->pending.first_tsn) {
          represented = 1;
          break;
        }
    }
    if (!represented) ++count;
  }
  return count;
}
static int has_pr_event_capacity(const cnet_sctp_data *d) {
  return (size_t)d->abandon_event_count + active_pr_messages(d) <
      CNET_SCTP_DATA_TX_SLOTS;
}
static cnet_sctp_data_status push_abandon_event(cnet_sctp_data *d,
    const cnet_sctp_data_abandon_event *event) {
  size_t tail;
  if (d->abandon_event_count >= CNET_SCTP_DATA_TX_SLOTS)
    return CNET_SCTP_DATA_CAPACITY;
  tail = ((size_t)d->abandon_event_head + d->abandon_event_count) %
      CNET_SCTP_DATA_TX_SLOTS;
  d->abandon_events[tail] = *event;
  ++d->abandon_event_count;
  return CNET_SCTP_DATA_OK;
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
    if (d->tx[i].used && !d->tx[i].abandoned &&
        d->tx[i].flight_accounted)
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
  /* Advertise the conservative per-ticket budget used by the sender even
   * though one peer fragment may occupy the larger full-PMTU RX slot. */
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
      cfg->forward_tsn_supported > 1u ||
      cfg->interleaving_supported > 1u ||
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
    uint16_t stream, uint32_t message_id, uint32_t fsn, uint32_t ppid,
    uint8_t flags,
    const uint8_t *payload, size_t n, void *output, size_t cap,
    size_t *written, int zero_probe,
    const cnet_sctp_data_reliability *reliability,
    size_t message_size, uint32_t message_first_tsn,
    uint64_t expires_at_ms) {
  cnet_sctp_data_tx *slot = NULL;
  uint8_t value[I_DATA_HEADER + CNET_SCTP_DATA_PAYLOAD_MAX];
  const size_t header_size = d->config.interleaving_supported ?
      I_DATA_HEADER : DATA_HEADER;
  size_t packet_bytes = 0u, i;
  cnet_sctp_data_status status;
  if (n == 0u || n > CNET_SCTP_DATA_PAYLOAD_MAX ||
      message_size < n || !reliability_valid(d, reliability) ||
      (reliability->mode == CNET_SCTP_DATA_RELIABILITY_TIMED &&
       expires_at_ms < now))
    return CNET_SCTP_DATA_INVALID;
  if (d->forward_active) return CNET_SCTP_DATA_CAPACITY;
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
  if (d->config.interleaving_supported) {
    wr16(value + 6u, 0u);
    wr32(value + 8u, message_id);
    wr32(value + 12u, (flags & DATA_B) != 0u ? ppid : fsn);
  } else {
    wr16(value + 6u, (uint16_t)message_id);
    wr32(value + 8u, ppid);
  }
  memcpy(value + header_size, payload, n);
  status = encode_one(d, d->config.interleaving_supported ?
      CHUNK_I_DATA : CHUNK_DATA, flags, value, header_size + n,
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
  slot->stream_id = stream;
  slot->ssn = (uint16_t)message_id;
  slot->mid = message_id;
  slot->fsn = fsn;
  slot->ppid = ppid;
  slot->message_first_tsn = message_first_tsn;
  slot->message_size = message_size;
  slot->unordered = (flags & DATA_U) != 0u;
  slot->interleaved = d->config.interleaving_supported;
  slot->reliability_mode = (uint8_t)reliability->mode;
  slot->reliability_value = reliability->value;
  slot->expires_at_ms = reliability->mode == CNET_SCTP_DATA_RELIABILITY_TIMED ?
      expires_at_ms : 0u;
  slot->abandoned = 0u;
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
  return cnet_sctp_data_send_pr(d, now, stream, ppid, unordered,
      &RELIABLE_POLICY, payload, n, output, cap, written);
}

cnet_sctp_data_status cnet_sctp_data_send_pr(cnet_sctp_data *d, uint64_t now,
    uint16_t stream, uint32_t ppid, int unordered,
    const cnet_sctp_data_reliability *reliability, const void *payload,
    size_t n, void *output, size_t cap, size_t *written) {
  if (written) *written = 0u;
  cnet_sctp_data_status status = guard(d);
  uint32_t sequence;
  if (status != CNET_SCTP_DATA_OK) return status;
  if (!payload || !output || !written || n == 0u ||
      stream >= d->config.outbound_streams) return CNET_SCTP_DATA_INVALID;
  if ((status = clock_check(d, now)) != CNET_SCTP_DATA_OK) return status;
  if (!reliability_valid(d, reliability))
    return CNET_SCTP_DATA_INVALID;
  if (reliability->mode == CNET_SCTP_DATA_RELIABILITY_TIMED &&
      now > UINT64_MAX - reliability->value)
    return CNET_SCTP_DATA_INVALID;
  if (d->pending.active) return CNET_SCTP_DATA_CAPACITY;
  if (n > CNET_SCTP_DATA_PAYLOAD_MAX) return CNET_SCTP_DATA_CAPACITY;
  if (reliability->mode != CNET_SCTP_DATA_RELIABLE &&
      !has_pr_event_capacity(d)) return CNET_SCTP_DATA_CAPACITY;
  sequence = d->config.interleaving_supported ?
      (unordered ? d->tx_mid_unordered[stream] :
       d->tx_mid_ordered[stream]) :
      (unordered ? 0u : d->tx_ssn[stream]);
  status = send_frame(d, now, stream, sequence, 0u, ppid,
      (uint8_t)(DATA_B | DATA_E | (unordered ? DATA_U : 0u)),
      (const uint8_t *)payload, n, output, cap, written, 0, reliability,
      n, d->next_tsn,
      reliability->mode == CNET_SCTP_DATA_RELIABILITY_TIMED ?
          now + reliability->value : 0u);
  if (status == CNET_SCTP_DATA_OK) {
    if (d->config.interleaving_supported) {
      if (unordered) ++d->tx_mid_unordered[stream];
      else {
        ++d->tx_mid_ordered[stream];
        ++d->tx_ssn[stream]; /* lower-16 compatibility projection */
      }
    } else if (!unordered) {
      ++d->tx_ssn[stream];
    }
  }
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
  if (written) *written = 0u;
  cnet_sctp_data_status status = guard(d);
  cnet_sctp_data staged;
  uint8_t singles[CNET_SCTP_DATA_BUNDLE_MAX][CNET_SCTP_DATA_MTU];
  cnet_sctp_wire_chunk chunks[CNET_SCTP_DATA_BUNDLE_MAX];
  size_t i, packet_cap, retained_tx = 0u;
  if (status != CNET_SCTP_DATA_OK) return status;
  if (!items || !output || !written || count == 0u)
    return CNET_SCTP_DATA_INVALID;
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
        chunks[i].type != (staged.config.interleaving_supported ?
            CHUNK_I_DATA : CHUNK_DATA))
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

static void finish_pending_message(cnet_sctp_data *d) {
  if (d->pending.interleaved) {
    if (d->pending.unordered)
      ++d->tx_mid_unordered[d->pending.stream_id];
    else {
      ++d->tx_mid_ordered[d->pending.stream_id];
      ++d->tx_ssn[d->pending.stream_id];
    }
  } else if (!d->pending.unordered) {
    ++d->tx_ssn[d->pending.stream_id];
  }
  memset(&d->pending, 0, sizeof(d->pending));
}

cnet_sctp_data_status cnet_sctp_data_queue_message(cnet_sctp_data *d,
    uint64_t now, uint16_t stream, uint32_t ppid, int unordered,
    const void *payload, size_t n) {
  return cnet_sctp_data_queue_message_pr(d, now, stream, ppid, unordered,
      &RELIABLE_POLICY, payload, n);
}

cnet_sctp_data_status cnet_sctp_data_queue_message_pr(cnet_sctp_data *d,
    uint64_t now, uint16_t stream, uint32_t ppid, int unordered,
    const cnet_sctp_data_reliability *reliability,
    const void *payload, size_t n) {
  cnet_sctp_data_status status = guard(d);
  if (status != CNET_SCTP_DATA_OK) return status;
  if (!payload || stream >= d->config.outbound_streams || n == 0u)
    return CNET_SCTP_DATA_INVALID;
  if ((status = clock_check(d, now)) != CNET_SCTP_DATA_OK) return status;
  if (!reliability_valid(d, reliability))
    return CNET_SCTP_DATA_INVALID;
  if (reliability->mode == CNET_SCTP_DATA_RELIABILITY_TIMED &&
      now > UINT64_MAX - reliability->value)
    return CNET_SCTP_DATA_INVALID;
  if (d->pending.active || n > CNET_SCTP_DATA_MESSAGE_MAX)
    return CNET_SCTP_DATA_CAPACITY;
  if (reliability->mode != CNET_SCTP_DATA_RELIABLE &&
      !has_pr_event_capacity(d)) return CNET_SCTP_DATA_CAPACITY;
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
  d->pending.mid = unordered ? d->tx_mid_unordered[stream] :
      d->tx_mid_ordered[stream];
  d->pending.next_fsn = 0u;
  d->pending.ppid = ppid;
  d->pending.reliability = *reliability;
  d->pending.expires_at_ms =
      reliability->mode == CNET_SCTP_DATA_RELIABILITY_TIMED ?
          now + reliability->value : 0u;
  d->pending.unordered = unordered != 0;
  d->pending.interleaved = d->config.interleaving_supported;
  d->pending.active = 1u;
  if (d->peer_rwnd == 0u && !has_retained_tx(d)) {
    d->probe_interval_ms = d->rto_ms;
    d->probe_deadline_ms = now + d->rto_ms;
  }
  d->last_time_ms = now;
  return CNET_SCTP_DATA_OK;
}

/* Remove only a copied message that has NO allocated DATA TSN. The sender
 * has not consumed SSN/MID, so no RFC3758 FORWARD nor fictitious peer ACK
 * is necessary. A previous DATA fragment/probe or due TIMED expiry must
 * never be reclassified as a safe local cancellation. */
cnet_sctp_data_status cnet_sctp_data_cancel_unsent(cnet_sctp_data *d,
    uint64_t now, uint16_t stream_id) {
  cnet_sctp_data_status status = guard(d);
  if (status != CNET_SCTP_DATA_OK) return status;
  if (stream_id >= d->config.outbound_streams)
    return CNET_SCTP_DATA_INVALID;
  if ((status = clock_check(d, now)) != CNET_SCTP_DATA_OK) return status;
  if (!d->pending.active || d->pending.stream_id != stream_id)
    return CNET_SCTP_DATA_EMPTY;
  if (d->pending.has_tsn || d->pending.offset != 0u ||
      (d->pending.reliability.mode == CNET_SCTP_DATA_RELIABILITY_TIMED &&
       now >= d->pending.expires_at_ms))
    return CNET_SCTP_DATA_RETRANSMIT_PENDING;
  memset(&d->pending, 0, sizeof(d->pending));
  if (d->peer_rwnd == 0u && !d->probe_active && !has_retained_tx(d)) {
    d->probe_deadline_ms = 0u;
    d->probe_interval_ms = 0u;
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
  /* A queued lifetime starts at admission, not at the next fragment pump.
   * The Owner must publish the corresponding abandonment through advance(). */
  if (d->pending.reliability.mode == CNET_SCTP_DATA_RELIABILITY_TIMED &&
      now >= d->pending.expires_at_ms)
    return CNET_SCTP_DATA_RETRANSMIT_PENDING;
  n = d->pending.size - d->pending.offset;
  if (n > CNET_SCTP_DATA_PAYLOAD_MAX) n = CNET_SCTP_DATA_PAYLOAD_MAX;
  flags = (uint8_t)((d->pending.offset == 0u ? DATA_B : 0u) |
      (d->pending.offset + n == d->pending.size ? DATA_E : 0u) |
      (d->pending.unordered ? DATA_U : 0u));
  status = send_frame(d, now, d->pending.stream_id,
      d->pending.interleaved ? d->pending.mid : d->pending.ssn,
      d->pending.next_fsn, d->pending.ppid, flags,
      d->pending.bytes + d->pending.offset,
      n, output, cap, written, 0, &d->pending.reliability,
      d->pending.size,
      d->pending.has_tsn ? d->pending.first_tsn : d->next_tsn,
      d->pending.expires_at_ms);
  if (status != CNET_SCTP_DATA_OK) return status;
  if (!d->pending.has_tsn) {
    d->pending.first_tsn = d->next_tsn - 1u;
    d->pending.has_tsn = 1u;
  }
  d->pending.offset += n;
  ++d->pending.next_fsn;
  if (d->pending.offset == d->pending.size)
    /* No message body survives successful admission of the final fragment.
     * Retransmit tickets own independent full-wire copies. */
    finish_pending_message(d);
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

static int same_rx_message(const cnet_sctp_data_rx *a,
    const cnet_sctp_data_rx *b) {
  if (!a || !b || a->interleaved != b->interleaved ||
      a->stream_id != b->stream_id || a->unordered != b->unordered)
    return 0;
  return a->interleaved ? a->mid == b->mid :
      a->ssn == b->ssn && a->ppid == b->ppid;
}

static cnet_sctp_data_rx *find_rx_fragment(cnet_sctp_data *d,
    const cnet_sctp_data_rx *start, uint32_t index) {
  size_t i;
  for (i = 0u; i < CNET_SCTP_DATA_RX_SLOTS; ++i) {
    cnet_sctp_data_rx *candidate = &d->rx[i];
    if (!candidate->used || !same_rx_message(start, candidate)) continue;
    if ((start->interleaved && candidate->fsn == index) ||
        (!start->interleaved && candidate->tsn == start->tsn + index))
      return candidate;
  }
  return NULL;
}

static int interleaved_message_forwarded(const cnet_sctp_data *d,
    const cnet_sctp_data_rx *message) {
  const uint16_t bit = (uint16_t)(UINT16_C(1) << message->stream_id);
  uint32_t ceiling;
  uint16_t streams;
  if (!message->interleaved) return 0;
  if (message->unordered) {
    streams = d->rx_forward_mid_unordered_streams;
    ceiling = d->rx_forward_mid_unordered[message->stream_id];
  } else {
    streams = d->rx_forward_mid_ordered_streams;
    ceiling = d->rx_forward_mid_ordered[message->stream_id];
  }
  return (streams & bit) != 0u &&
      !ahead(message->mid, ceiling);
}

static void discard_forwarded_partial_messages(cnet_sctp_data *d,
    uint32_t cumulative) {
  uint8_t complete[CNET_SCTP_DATA_RX_SLOTS] = {0};
  size_t i, j;
  /* First preserve every fully received B..E run. A FORWARD-TSN may advance
   * across Gap-ACKed DATA; those complete messages still belong to the ULP. */
  for (i = 0u; i < CNET_SCTP_DATA_RX_SLOTS; ++i) {
    const cnet_sctp_data_rx *start = &d->rx[i];
    size_t members[CNET_SCTP_DATA_RX_SLOTS];
    size_t count;
    if (!start->used || (start->flags & DATA_B) == 0u) continue;
    for (count = 0u; count < CNET_SCTP_DATA_RX_SLOTS; ++count) {
      const cnet_sctp_data_rx *fragment =
          find_rx_fragment(d, start, (uint32_t)count);
      size_t index;
      if (!fragment ||
          (count != 0u && (fragment->flags & DATA_B) != 0u))
        break;
      index = (size_t)(fragment - d->rx);
      members[count] = index;
      if ((fragment->flags & DATA_E) != 0u) {
        size_t member;
        for (member = 0u; member <= count; ++member)
          complete[members[member]] = 1u;
        break;
      }
    }
  }
  /* If any retained fragment of an incomplete message lies at/below the
   * forwarded point, the missing part can never arrive. Remove every
   * retained fragment with the same message identity as one transaction. */
  for (i = 0u; i < CNET_SCTP_DATA_RX_SLOTS; ++i) {
    const cnet_sctp_data_rx key = d->rx[i];
    if (!key.used || complete[i] ||
        (ahead(key.tsn, cumulative) &&
         !interleaved_message_forwarded(d, &key))) continue;
    for (j = 0u; j < CNET_SCTP_DATA_RX_SLOTS; ++j) {
      cnet_sctp_data_rx *fragment = &d->rx[j];
      if (!fragment->used || complete[j] ||
          !same_rx_message(&key, fragment))
        continue;
      d->buffered_rx -= fragment->size;
      memset(fragment, 0, sizeof(*fragment));
    }
  }
}

static int rx_message_complete(cnet_sctp_data *d,
    const cnet_sctp_data_rx *start) {
  size_t count;
  if (!start || !start->used || (start->flags & DATA_B) == 0u) return 0;
  for (count = 0u; count < CNET_SCTP_DATA_RX_SLOTS; ++count) {
    const cnet_sctp_data_rx *fragment =
        find_rx_fragment(d, start, (uint32_t)count);
    if (!fragment ||
        (count != 0u && (fragment->flags & DATA_B) != 0u))
      return 0;
    if ((fragment->flags & DATA_E) != 0u) return 1;
  }
  return 0;
}

/* Preserve already received ordered messages at/below the peer's skip
 * ceiling. Select the nearest complete SSN for Owner-pulled delivery; once
 * it is taken, this helper advances across the next missing range. */
static void advance_forward_stream(cnet_sctp_data *d, uint16_t stream) {
  const uint16_t bit = (uint16_t)(UINT16_C(1) << stream);
  const uint16_t current = d->rx_ssn[stream];
  const uint16_t ceiling = d->rx_forward_ssn[stream];
  const uint16_t span = (uint16_t)(ceiling - current);
  uint16_t nearest = 0u;
  uint16_t nearest_delta = 0u;
  size_t i;
  int found = 0;
  if ((d->rx_forward_streams & bit) == 0u) return;
  if (span >= UINT16_C(0x8000)) {
    d->rx_forward_streams &= (uint16_t)~bit;
    return;
  }
  for (i = 0u; i < CNET_SCTP_DATA_RX_SLOTS; ++i) {
    const cnet_sctp_data_rx *start = &d->rx[i];
    uint16_t delta;
    if (!start->used || start->unordered || start->stream_id != stream ||
        (start->flags & DATA_B) == 0u || !rx_message_complete(d, start))
      continue;
    delta = (uint16_t)(start->ssn - current);
    if (delta <= span && (!found || delta < nearest_delta)) {
      found = 1;
      nearest = start->ssn;
      nearest_delta = delta;
    }
  }
  if (found) {
    d->rx_ssn[stream] = nearest;
  } else {
    d->rx_ssn[stream] = (uint16_t)(ceiling + 1u);
    d->rx_forward_streams &= (uint16_t)~bit;
  }
}

/* RFC 8260 applies the skip ceiling independently to ordered and unordered
 * MID spaces. Ordered delivery pauses on any complete message at or below
 * the ceiling; otherwise it advances to the first MID after the skipped
 * range. Unordered delivery has no ordering gate, but retains a floor so
 * late fragments of an explicitly skipped message cannot be resurrected. */
static void advance_forward_mid(cnet_sctp_data *d, uint16_t stream,
    int unordered) {
  const uint16_t bit = (uint16_t)(UINT16_C(1) << stream);
  uint16_t *streams = unordered ? &d->rx_forward_mid_unordered_streams :
      &d->rx_forward_mid_ordered_streams;
  uint32_t *ceiling_slot = unordered ?
      &d->rx_forward_mid_unordered[stream] :
      &d->rx_forward_mid_ordered[stream];
  uint32_t *current = unordered ? &d->rx_mid_unordered_floor[stream] :
      &d->rx_mid_ordered[stream];
  uint32_t nearest = 0u, nearest_delta = 0u;
  size_t i;
  int found = 0;
  if ((*streams & bit) == 0u) return;
  if (*ceiling_slot - *current >= UINT32_C(0x80000000)) {
    *streams &= (uint16_t)~bit;
    return;
  }
  if (unordered) {
    *current = *ceiling_slot + 1u;
    return;
  }
  for (i = 0u; i < CNET_SCTP_DATA_RX_SLOTS; ++i) {
    cnet_sctp_data_rx *start = &d->rx[i];
    uint32_t delta;
    if (!start->used || !start->interleaved || start->unordered ||
        start->stream_id != stream || (start->flags & DATA_B) == 0u ||
        !rx_message_complete(d, start))
      continue;
    delta = start->mid - *current;
    if (delta <= *ceiling_slot - *current &&
        (!found || delta < nearest_delta)) {
      found = 1;
      nearest = start->mid;
      nearest_delta = delta;
    }
  }
  if (found) {
    *current = nearest;
  } else {
    *current = *ceiling_slot + 1u;
    *streams &= (uint16_t)~bit;
  }
  d->rx_ssn[stream] = (uint16_t)*current;
}

/* RFC 3758 3.6: advance the peer cumulative TSN and ordered-stream SSNs in
 * one bounded transaction. Skipped partial messages are discarded before
 * any ULP view can escape; output-capacity failure leaves the live TCB intact. */
static cnet_sctp_data_status input_forward_tsn(cnet_sctp_data *d,
    uint64_t now, const cnet_sctp_wire_chunk *ch, int interleaved,
    void *out, size_t cap, size_t *written) {
  cnet_sctp_data staged;
  uint32_t cumulative;
  size_t position;
  const size_t entry_size = interleaved ? 8u : 4u;
  uint16_t touched_ordered = 0u, touched_unordered = 0u;
  cnet_sctp_data_status status;
  if (!d->config.forward_tsn_supported ||
      interleaved != (d->config.interleaving_supported != 0u) ||
      ch->value_size < 4u ||
      (ch->value_size - 4u) % entry_size != 0u)
    return CNET_SCTP_DATA_PROTOCOL;
  cumulative = rd32(ch->value);
  memcpy(&staged, d, sizeof(staged));
  if (cumulative - staged.cumulative_rx == UINT32_C(0x80000000))
    return CNET_SCTP_DATA_PROTOCOL;
  if (ahead(cumulative, staged.cumulative_rx)) {
    const uint32_t distance = cumulative - staged.cumulative_rx;
    staged.rx_gaps = distance >= 32u ? 0u : staged.rx_gaps >> distance;
    staged.cumulative_rx = cumulative;
    while ((staged.rx_gaps & 1u) != 0u) {
      ++staged.cumulative_rx;
      staged.rx_gaps >>= 1u;
    }
    for (position = 4u; position < ch->value_size;
         position += entry_size) {
      const uint16_t stream = rd16(ch->value + position);
      const uint16_t bit = stream < CNET_SCTP_DATA_STREAMS ?
          (uint16_t)(UINT16_C(1) << stream) : 0u;
      if (stream >= staged.config.inbound_streams || bit == 0u)
        return CNET_SCTP_DATA_PROTOCOL;
      if (interleaved) {
        const int unordered = (rd16(ch->value + position + 2u) & 1u) != 0u;
        const uint32_t mid = rd32(ch->value + position + 4u);
        uint16_t *streams = unordered ?
            &staged.rx_forward_mid_unordered_streams :
            &staged.rx_forward_mid_ordered_streams;
        uint32_t *ceiling = unordered ?
            &staged.rx_forward_mid_unordered[stream] :
            &staged.rx_forward_mid_ordered[stream];
        const uint32_t delta = mid - *ceiling;
        if ((*streams & bit) != 0u && delta == UINT32_C(0x80000000))
          return CNET_SCTP_DATA_PROTOCOL;
        if ((*streams & bit) == 0u || delta < UINT32_C(0x80000000)) {
          *streams |= bit;
          *ceiling = mid;
        }
        if (unordered) touched_unordered |= bit;
        else touched_ordered |= bit;
      } else {
        const uint16_t ssn = rd16(ch->value + position + 2u);
        if ((staged.rx_forward_streams & bit) == 0u ||
            (uint16_t)(ssn - staged.rx_forward_ssn[stream]) <
                UINT16_C(0x8000)) {
          staged.rx_forward_streams |= bit;
          staged.rx_forward_ssn[stream] = ssn;
        }
        advance_forward_stream(&staged, stream);
      }
    }
    discard_forwarded_partial_messages(&staged, staged.cumulative_rx);
    if (interleaved) {
      for (position = 0u; position < CNET_SCTP_DATA_STREAMS; ++position) {
        const uint16_t bit = (uint16_t)(UINT16_C(1) << position);
        if ((touched_ordered & bit) != 0u)
          advance_forward_mid(&staged, (uint16_t)position, 0);
        if ((touched_unordered & bit) != 0u)
          advance_forward_mid(&staged, (uint16_t)position, 1);
      }
    }
  } else {
    /* Duplicate/old FORWARD-TSN is acknowledged but cannot rewind SSNs. */
    for (position = 4u; position < ch->value_size;
         position += entry_size) {
      const uint16_t stream = rd16(ch->value + position);
      const uint16_t bit = stream < CNET_SCTP_DATA_STREAMS ?
          (uint16_t)(UINT16_C(1) << stream) : 0u;
      if (stream >= staged.config.inbound_streams || bit == 0u)
        return CNET_SCTP_DATA_PROTOCOL;
    }
  }
  status = sack_packet(&staged, staged.cumulative_rx, staged.rx_gaps,
      rx_available(&staged), 0, 0u, out, cap, written);
  if (status != CNET_SCTP_DATA_OK) return status;
  staged.last_advertised_rwnd = (uint32_t)rx_available(&staged);
  staged.window_update_pending = 0u;
  staged.last_time_ms = now;
  memcpy(d, &staged, sizeof(*d));
  return CNET_SCTP_DATA_OK;
}

static cnet_sctp_data_status input_data(cnet_sctp_data *d, uint64_t now,
    const cnet_sctp_wire_chunk *ch, void *out, size_t cap, size_t *written) {
  uint32_t tsn, prospective_cum, mask;
  uint32_t mid = 0u, fsn = 0u, ppid = 0u;
  int32_t distance;
  uint16_t stream, ssn;
  size_t bytes;
  const int interleaved = ch->type == CHUNK_I_DATA;
  const size_t header_size = interleaved ? I_DATA_HEADER : DATA_HEADER;
  cnet_sctp_data_rx *slot = NULL;
  cnet_sctp_data_status status;
  unsigned int i;
  int duplicate = 0;
  int drop_payload = 0;
  /* RFC 9260 3.3.1: ignore reserved DATA bits, and accept the I bit.
   * This owner-local receiver already emits an immediate SACK for DATA. */
  if (interleaved != (d->config.interleaving_supported != 0u) ||
      ch->value_size < header_size + 1u)
    return CNET_SCTP_DATA_PROTOCOL; /* fragments legal, empty RFC DATA not */
  bytes = ch->value_size - header_size;
  if (bytes > CNET_SCTP_DATA_RX_PAYLOAD_MAX)
    return CNET_SCTP_DATA_CAPACITY;
  tsn = rd32(ch->value);
  stream = rd16(ch->value + 4u);
  /* With U=1 the wire SSN is explicitly ignored by the receiver. */
  ssn = interleaved || (ch->flags & DATA_U) != 0u ?
      0u : rd16(ch->value + 6u);
  if (stream >= d->config.inbound_streams) return CNET_SCTP_DATA_PROTOCOL;
  if (interleaved) {
    const uint32_t floor = (ch->flags & DATA_U) != 0u ?
        d->rx_mid_unordered_floor[stream] : d->rx_mid_ordered[stream];
    uint32_t delta;
    mid = rd32(ch->value + 8u);
    if ((ch->flags & DATA_B) != 0u) {
      ppid = rd32(ch->value + 12u);
    } else {
      fsn = rd32(ch->value + 12u);
      if (fsn == 0u) return CNET_SCTP_DATA_PROTOCOL;
      if (fsn >= CNET_SCTP_DATA_RX_SLOTS)
        return CNET_SCTP_DATA_CAPACITY;
    }
    delta = mid - floor;
    if (delta == UINT32_C(0x80000000)) return CNET_SCTP_DATA_PROTOCOL;
    if (delta > UINT32_C(0x80000000)) drop_payload = 1;
  } else {
    ppid = rd32(ch->value + 8u);
  }
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
    if (!drop_payload && bytes > rx_available(d)) {
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
    for (i = 0u; !drop_payload && i < CNET_SCTP_DATA_RX_SLOTS; ++i) {
      if (!d->rx[i].used) {
        if (!slot) slot = &d->rx[i];
        continue;
      }
      /* All fragments share an SSN. Reject two distinct ordered starts
       * with the same stream/SSN, but admit contiguous middle/end chunks.
       * The TSN bitmap already rejects exact duplicate fragments. */
      if ((ch->flags & DATA_B) && !(ch->flags & DATA_U) &&
          !interleaved && !d->rx[i].unordered &&
          (d->rx[i].flags & DATA_B) &&
          d->rx[i].stream_id == stream && d->rx[i].ssn == ssn)
        return CNET_SCTP_DATA_PROTOCOL;
      if (interleaved && d->rx[i].interleaved &&
          d->rx[i].stream_id == stream &&
          d->rx[i].unordered == ((ch->flags & DATA_U) != 0u) &&
          d->rx[i].mid == mid) {
        if (d->rx[i].fsn == fsn ||
            ((d->rx[i].flags & DATA_E) != 0u && fsn > d->rx[i].fsn) ||
            ((ch->flags & DATA_E) != 0u && d->rx[i].fsn > fsn))
          return CNET_SCTP_DATA_PROTOCOL;
      }
    }
    if (!drop_payload && !slot) return CNET_SCTP_DATA_CAPACITY;
    mask |= UINT32_C(1) << ((unsigned int)distance - 1u);
    while ((mask & 1u) != 0u) {
      ++prospective_cum;
      mask >>= 1u;
    }
  }
  const size_t offered = rx_window_after_admit(
      d, duplicate || drop_payload ? 0u : bytes);
  status = sack_packet(d, prospective_cum, mask,
      offered, duplicate, tsn, out, cap, written);
  if (status != CNET_SCTP_DATA_OK) return status;
  if (!duplicate && !drop_payload) {
    memcpy(slot->payload, ch->value + header_size, bytes);
    slot->size = bytes;
    slot->tsn = tsn;
    slot->stream_id = stream;
    slot->ssn = ssn;
    slot->mid = mid;
    slot->fsn = fsn;
    slot->ppid = ppid;
    slot->unordered = (ch->flags & DATA_U) != 0u;
    slot->interleaved = interleaved != 0;
    /* Only U/B/E affect reassembly; never retain reserved/I bits as state. */
    slot->flags = (uint8_t)(ch->flags & (DATA_B | DATA_E | DATA_U));
    slot->used = 1u;
    d->buffered_rx += bytes;
  }
  if (!duplicate) {
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
  /* RFC 9260 3.3.4: SACK Chunk Flags are ignored on receipt. */
  if (ch->value_size < 12u)
    return CNET_SCTP_DATA_PROTOCOL;
  cumulative = rd32(ch->value);
  peer_window = rd32(ch->value + 4u);
  blocks = rd16(ch->value + 8u);
  duplicates = rd16(ch->value + 10u);
  /* Duplicate TSNs are an on-wire report list, not retained RX slots. The
   * Chunk length already bounds this list; do not reject a valid SACK merely
   * because it reports more duplicates than the receiver can buffer at once. */
  if (blocks > 16u ||
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
    if (slot->abandoned) {
      /* RFC 3758 A2 treats abandonment as final acknowledgement for
       * retransmission/congestion accounting. A later SACK may release the
       * retained ticket, but cannot credit bytes or resurrect its flight. */
      continue;
    }
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
      if (slot->used && !slot->abandoned && coverage[i] == 0u &&
          (slot->gap_acked || recovery_hole ||
           (has_higher && ahead(highest_new, slot->tsn))) &&
          slot->missing_reports < 3u)
        ++slot->missing_reports;
    }
  }
  for (i = 0u; i < CNET_SCTP_DATA_TX_SLOTS; ++i) {
    cnet_sctp_data_tx *slot = &d->tx[i];
    if (!slot->used) continue;
    if (!slot->abandoned && !coverage[i] && slot->missing_reports >= 3u &&
        !slot->fast_retransmitted) slot->pending_retransmit = 1u;
    if (coverage[i] == 2u) {
      memset(slot, 0, sizeof(*slot));
    } else if (slot->abandoned) {
      slot->gap_acked = coverage[i];
      slot->flight_accounted = 0u;
      slot->pending_retransmit = 0u;
      slot->missing_reports = 0u;
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
  if (d->forward_active &&
      !ahead(d->forward_cumulative_tsn, cumulative)) {
    d->forward_active = 0u;
    d->forward_cumulative_tsn = 0u;
    d->forward_deadline_ms = 0u;
    d->forward_retransmits = 0u;
    d->forward_packet_size = 0u;
    memset(d->forward_packet, 0, sizeof(d->forward_packet));
  }
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
  if (!d || d->magic != DATA_MAGIC || d->pending.active ||
      d->forward_active) return 0;
  for (i = 0u; i < CNET_SCTP_DATA_TX_SLOTS; ++i)
    if (d->tx[i].used) return 0;
  return 1;
}
int cnet_sctp_data_association_reset_ready(const cnet_sctp_data *d) {
  unsigned int i;
  if (!cnet_sctp_data_outbound_drained(d) || d->buffered_rx != 0u ||
      d->rx_gaps != 0u || d->abandon_event_count != 0u)
    return 0;
  for (i = 0u; i < CNET_SCTP_DATA_RX_SLOTS; ++i)
    if (d->rx[i].used) return 0;
  return 1;
}
cnet_sctp_data_status cnet_sctp_data_reset_association(cnet_sctp_data *d,
    uint64_t now, uint32_t local_next_tsn, uint32_t peer_next_tsn) {
  cnet_sctp_data_status status = guard(d);
  if (status != CNET_SCTP_DATA_OK) return status;
  if ((status = clock_check(d, now)) != CNET_SCTP_DATA_OK) return status;
  if (!cnet_sctp_data_association_reset_ready(d))
    return CNET_SCTP_DATA_INVALID;

  memset(d->tx, 0, sizeof(d->tx));
  memset(d->rx, 0, sizeof(d->rx));
  memset(&d->pending, 0, sizeof(d->pending));
  memset(d->abandon_events, 0, sizeof(d->abandon_events));
  d->abandon_event_head = 0u;
  d->abandon_event_count = 0u;
  memset(d->tx_ssn, 0, sizeof(d->tx_ssn));
  memset(d->rx_ssn, 0, sizeof(d->rx_ssn));
  memset(d->rx_forward_ssn, 0, sizeof(d->rx_forward_ssn));
  d->rx_forward_streams = 0u;
  memset(d->rx_forward_mid_ordered, 0,
      sizeof(d->rx_forward_mid_ordered));
  memset(d->rx_forward_mid_unordered, 0,
      sizeof(d->rx_forward_mid_unordered));
  d->rx_forward_mid_ordered_streams = 0u;
  d->rx_forward_mid_unordered_streams = 0u;
  memset(d->tx_mid_ordered, 0, sizeof(d->tx_mid_ordered));
  memset(d->tx_mid_unordered, 0, sizeof(d->tx_mid_unordered));
  memset(d->rx_mid_ordered, 0, sizeof(d->rx_mid_ordered));
  memset(d->rx_mid_unordered_floor, 0,
      sizeof(d->rx_mid_unordered_floor));
  memset(d->rx_forward_mid_ordered, 0,
      sizeof(d->rx_forward_mid_ordered));
  memset(d->rx_forward_mid_unordered, 0,
      sizeof(d->rx_forward_mid_unordered));
  d->rx_forward_mid_ordered_streams = 0u;
  d->rx_forward_mid_unordered_streams = 0u;
  d->next_tsn = local_next_tsn;
  d->last_ack = local_next_tsn - 1u;
  d->cumulative_rx = peer_next_tsn - 1u;
  d->rx_gaps = 0u;
  d->in_flight = 0u;
  d->buffered_rx = 0u;
  d->partial_bytes_acked = 0u;
  d->t3_deadline_ms = 0u;
  d->t3_wait_tsn = 0u;
  d->t3_wait_ack = 0u;
  d->recovery_exit_tsn = 0u;
  d->recovery_active = 0u;
  d->probe_deadline_ms = 0u;
  d->probe_interval_ms = 0u;
  d->probe_tsn = 0u;
  d->probe_active = 0u;
  d->probe_unanswered = 0u;
  d->last_advertised_rwnd = CNET_SCTP_DATA_RECEIVE_CEILING;
  d->window_update_pending = 0u;
  d->last_time_ms = now;
  return CNET_SCTP_DATA_OK;
}
int cnet_sctp_data_streams_through_tsn_drained(const cnet_sctp_data *d,
    uint16_t stream_mask, uint32_t last_tsn) {
  unsigned int i;
  if (!d || d->magic != DATA_MAGIC) return 0;
  for (i = 0u; i < CNET_SCTP_DATA_RX_SLOTS; ++i) {
    const cnet_sctp_data_rx *slot = &d->rx[i];
    if (slot->used && slot->stream_id < CNET_SCTP_DATA_STREAMS &&
        (stream_mask & (uint16_t)(UINT16_C(1) << slot->stream_id)) != 0u &&
        !ahead(slot->tsn, last_tsn))
      return 0;
  }
  return 1;
}
void cnet_sctp_data_reset_outbound_ssn(cnet_sctp_data *d,
    uint16_t stream_id) {
  if (d && d->magic == DATA_MAGIC &&
      stream_id < d->config.outbound_streams) {
    d->tx_ssn[stream_id] = 0u;
    d->tx_mid_ordered[stream_id] = 0u;
    d->tx_mid_unordered[stream_id] = 0u;
  }
}
void cnet_sctp_data_reset_inbound_ssn(cnet_sctp_data *d,
    uint16_t stream_id) {
  if (d && d->magic == DATA_MAGIC &&
      stream_id < d->config.inbound_streams) {
    d->rx_ssn[stream_id] = 0u;
    d->rx_mid_ordered[stream_id] = 0u;
    d->rx_mid_unordered_floor[stream_id] = 0u;
    d->rx_forward_ssn[stream_id] = 0u;
    d->rx_forward_mid_ordered[stream_id] = 0u;
    d->rx_forward_mid_unordered[stream_id] = 0u;
    d->rx_forward_streams &=
        (uint16_t)~(uint16_t)(UINT16_C(1) << stream_id);
    d->rx_forward_mid_ordered_streams &=
        (uint16_t)~(uint16_t)(UINT16_C(1) << stream_id);
    d->rx_forward_mid_unordered_streams &=
        (uint16_t)~(uint16_t)(UINT16_C(1) << stream_id);
  }
}

cnet_sctp_data_status cnet_sctp_data_input(cnet_sctp_data *d, uint64_t now,
    const void *packet, size_t packet_size, void *out, size_t cap,
    size_t *written) {
  if (written) *written = 0u;
  cnet_sctp_data_status status = guard(d);
  cnet_sctp_wire_header header;
  cnet_sctp_wire_decoder parser;
  cnet_sctp_wire_chunk chunk;
  if (status != CNET_SCTP_DATA_OK) return status;
  if (!packet || !out || !written) return CNET_SCTP_DATA_INVALID;
  if ((status = clock_check(d, now)) != CNET_SCTP_DATA_OK) return status;
  if (packet_size > CNET_SCTP_DATA_INGRESS_MTU)
    return CNET_SCTP_DATA_CAPACITY;
  if (cnet_sctp_wire_decode(packet, packet_size, &header, &parser) !=
      CNET_SCTP_WIRE_OK ||
      header.source_port != d->config.remote_port ||
      header.destination_port != d->config.local_port ||
      header.verification_tag != d->config.local_tag ||
      cnet_sctp_wire_next_chunk(&parser, &chunk) != CNET_SCTP_WIRE_OK)
    return CNET_SCTP_DATA_PROTOCOL;
  if (header.chunk_count == 1u) {
    if (chunk.type == CHUNK_DATA || chunk.type == CHUNK_I_DATA)
      return input_data(d, now, &chunk, out, cap, written);
    if (chunk.type == CHUNK_SACK)
      return input_sack(d, now, &chunk);
    if (chunk.type == CHUNK_FORWARD_TSN ||
        chunk.type == CHUNK_I_FORWARD_TSN)
      return input_forward_tsn(d, now, &chunk,
          chunk.type == CHUNK_I_FORWARD_TSN, out, cap, written);
    return CNET_SCTP_DATA_PROTOCOL;
  }
  /* RFC 9260 6.9: receive multiple DATA chunks and at most one
   * leading SACK. The sender need not bundle; the receiver must process
   * legal bundling. Peer ingress is bounded independently from the
   * conservative 1200-byte local egress MTU.
   * Every chunk is validated on a bounded state snapshot, including the
   * later fragments; an invalid final chunk or too-small SACK output
   * cannot partially admit any preceding DATA or settle TX tickets. */
  if (packet_size > CNET_SCTP_DATA_INGRESS_MTU)
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
      } else if (chunk.type == CHUNK_DATA || chunk.type == CHUNK_I_DATA) {
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

static cnet_sctp_data_status take_message(cnet_sctp_data *d,
    uint16_t held_streams, uint32_t last_tsn,
    cnet_sctp_data_message *meta, void *out, size_t cap, size_t *written) {
  if (written) *written = 0u;
  cnet_sctp_data_status status = guard(d);
  unsigned int i;
  if (status != CNET_SCTP_DATA_OK) return status;
  if (!meta || !out || !written) return CNET_SCTP_DATA_INVALID;
  for (i = 0u; i < CNET_SCTP_DATA_RX_SLOTS; ++i) {
    cnet_sctp_data_rx *start = &d->rx[i];
    size_t total = 0u, count;
    int complete = 0;
    int held = 0;
    if (!start->used || (start->flags & DATA_B) == 0u ||
        (!start->unordered &&
         (start->interleaved ?
          start->mid != d->rx_mid_ordered[start->stream_id] :
          start->ssn != d->rx_ssn[start->stream_id])))
      continue;
    if (start->stream_id < CNET_SCTP_DATA_STREAMS &&
        (held_streams &
         (uint16_t)(UINT16_C(1) << start->stream_id)) != 0u &&
        ahead(start->tsn, last_tsn))
      continue;
    /* Legacy DATA fragments use contiguous TSNs; I-DATA fragments use FSNs
     * and may be interleaved with other messages. Both are found through the
     * bounded RX slots. No partial user delivery or borrowed pointer. */
    for (count = 0u; count < CNET_SCTP_DATA_RX_SLOTS; ++count) {
      cnet_sctp_data_rx *frag =
          find_rx_fragment(d, start, (uint32_t)count);
      if (!frag) break; /* incomplete: gap missing */
      if (count != 0u && (frag->flags & DATA_B) != 0u)
        return CNET_SCTP_DATA_PROTOCOL; /* no legacy interleaving */
      if (frag->stream_id < CNET_SCTP_DATA_STREAMS &&
          (held_streams &
           (uint16_t)(UINT16_C(1) << frag->stream_id)) != 0u &&
          ahead(frag->tsn, last_tsn))
        held = 1;
      if (total > CNET_SCTP_DATA_MESSAGE_MAX - frag->size)
        return CNET_SCTP_DATA_CAPACITY;
      total += frag->size;
      if (frag->flags & DATA_E) { complete = 1; break; }
    }
    if (!complete) continue;
    if (held) continue;
    if (total > cap) return CNET_SCTP_DATA_CAPACITY;
    /* Snapshot metadata before freeing its first fragment. The entire
     * message is committed at once, never exposing erased RX storage. */
    {
      const cnet_sctp_data_rx key = *start;
      const uint16_t stream_id = start->stream_id;
      const uint16_t stream_ssn = start->ssn;
      const uint32_t message_id = start->mid;
      const uint32_t ppid = start->ppid;
      const uint32_t start_tsn = start->tsn;
      const uint8_t unordered = start->unordered;
      const uint8_t interleaved = start->interleaved;
      size_t copied = 0u;
      uint32_t consume_index = 0u;
      uint8_t last_flags;
      do {
        cnet_sctp_data_rx *frag =
            find_rx_fragment(d, &key, consume_index);
        last_flags = frag->flags;
        memcpy((uint8_t *)out + copied, frag->payload, frag->size);
        copied += frag->size;
        d->buffered_rx -= frag->size;
        memset(frag, 0, sizeof(*frag));
        ++consume_index;
      } while ((last_flags & DATA_E) == 0u);
      meta->stream_id = stream_id;
      meta->stream_sequence = interleaved ?
          (uint16_t)message_id : stream_ssn;
      meta->ppid = ppid;
      meta->tsn = start_tsn;
      meta->message_identifier = interleaved ? message_id : stream_ssn;
      meta->size = copied;
      meta->unordered = unordered;
      meta->interleaved = interleaved;
      *written = copied;
      if (!unordered) {
        if (interleaved) {
          ++d->rx_mid_ordered[stream_id];
          ++d->rx_ssn[stream_id];
          advance_forward_mid(d, stream_id, 0);
        } else {
          ++d->rx_ssn[stream_id];
          advance_forward_stream(d, stream_id);
        }
      }
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

cnet_sctp_data_status cnet_sctp_data_take(cnet_sctp_data *d,
    cnet_sctp_data_message *meta, void *out, size_t cap,
    size_t *written) {
  return take_message(d, 0u, 0u, meta, out, cap, written);
}

cnet_sctp_data_status cnet_sctp_data_take_before_reset(cnet_sctp_data *d,
    uint16_t stream_mask, uint32_t last_tsn,
    cnet_sctp_data_message *meta, void *out, size_t cap,
    size_t *written) {
  return take_message(d, stream_mask, last_tsn, meta, out, cap, written);
}

cnet_sctp_data_status cnet_sctp_data_take_abandoned(cnet_sctp_data *d,
    cnet_sctp_data_abandon_event *event) {
  /* A failed T3/FORWARD retransmission budget sets closed=1, but does NOT
   * discard this Owner-owned copied local-abandonment FIFO. Permit ONLY this
   * read-only retrieval after local FAILED; all DATA send/advance/ingress
   * APIs still reject closed TCBs through their ordinary guard(). */
  if (!d || d->magic != DATA_MAGIC || !d->config.is_owner)
    return CNET_SCTP_DATA_INVALID;
  if (!d->config.is_owner(d->config.owner_context))
    return CNET_SCTP_DATA_OWNER;
  if (!event) return CNET_SCTP_DATA_INVALID;
  if (d->abandon_event_count == 0u) return CNET_SCTP_DATA_EMPTY;
  *event = d->abandon_events[d->abandon_event_head];
  memset(&d->abandon_events[d->abandon_event_head], 0,
      sizeof(d->abandon_events[d->abandon_event_head]));
  d->abandon_event_head = (uint8_t)(
      (d->abandon_event_head + 1u) % CNET_SCTP_DATA_TX_SLOTS);
  --d->abandon_event_count;
  return CNET_SCTP_DATA_OK;
}

/* RFC 9260 6.2: Owner explicitly emits a reopened-window SACK, not a
 * timer/callback. Capacity refusal must preserve all receive state. */
cnet_sctp_data_status cnet_sctp_data_window_update(cnet_sctp_data *d,
    uint64_t now, void *out, size_t cap, size_t *written) {
  if (written) *written = 0u;
  cnet_sctp_data_status status = guard(d);
  size_t available;
  if (status != CNET_SCTP_DATA_OK) return status;
  if (!out || !written) return CNET_SCTP_DATA_INVALID;
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

static cnet_sctp_data_tx *find_tx(cnet_sctp_data *d, uint32_t tsn) {
  size_t i;
  for (i = 0u; i < CNET_SCTP_DATA_TX_SLOTS; ++i)
    if (d->tx[i].used && d->tx[i].tsn == tsn) return &d->tx[i];
  return NULL;
}

static void recompute_flight(cnet_sctp_data *d) {
  size_t i;
  d->in_flight = 0u;
  for (i = 0u; i < CNET_SCTP_DATA_TX_SLOTS; ++i)
    if (d->tx[i].used && !d->tx[i].abandoned && !d->tx[i].gap_acked)
      d->in_flight += d->tx[i].user_size;
}

static int pending_matches_tx(const cnet_sctp_data_pending *pending,
    const cnet_sctp_data_tx *tx) {
  return pending->active && pending->has_tsn && tx->used &&
      pending->stream_id == tx->stream_id &&
      pending->unordered == tx->unordered &&
      pending->interleaved == tx->interleaved &&
      pending->first_tsn == tx->message_first_tsn;
}

/* RFC 3758 3.5 A3: one fragment reaching its PR policy abandons every
 * assigned TSN in that user message. Any copied but unassigned suffix is
 * canceled in the same transaction, and one bounded ULP record is retained. */
static cnet_sctp_data_status abandon_tx_message(cnet_sctp_data *d,
    const cnet_sctp_data_tx *key, cnet_sctp_data_abandon_reason reason,
    uint32_t loss_tsn, int *loss_abandoned) {
  cnet_sctp_data_abandon_event event = {0};
  size_t i;
  event.stream_id = key->stream_id;
  event.ppid = key->ppid;
  event.message_identifier = key->interleaved ? key->mid : key->ssn;
  event.message_size = key->message_size;
  event.first_tsn = key->message_first_tsn;
  event.last_tsn = key->tsn;
  event.reliability.mode =
      (cnet_sctp_data_reliability_mode)key->reliability_mode;
  event.reliability.value = key->reliability_value;
  event.reason = reason;
  event.unordered = key->unordered;
  event.interleaved = key->interleaved;
  event.has_tsn = 1u;
  for (i = 0u; i < CNET_SCTP_DATA_TX_SLOTS; ++i) {
    const cnet_sctp_data_tx *tx = &d->tx[i];
    if (!same_tx_message(key, tx)) continue;
    if (ahead(tx->tsn, event.last_tsn)) event.last_tsn = tx->tsn;
    if (tx->tsn == loss_tsn) *loss_abandoned = 1;
  }
  if (pending_matches_tx(&d->pending, key)) {
    event.ppid = d->pending.ppid;
    event.message_size = d->pending.size;
    event.last_tsn = d->next_tsn - 1u;
  }
  if (push_abandon_event(d, &event) != CNET_SCTP_DATA_OK)
    return CNET_SCTP_DATA_CAPACITY;
  for (i = 0u; i < CNET_SCTP_DATA_TX_SLOTS; ++i) {
    cnet_sctp_data_tx *tx = &d->tx[i];
    if (!same_tx_message(key, tx)) continue;
    tx->abandoned = 1u;
    tx->flight_accounted = 0u;
    tx->pending_retransmit = 0u;
  }
  if (pending_matches_tx(&d->pending, key))
    finish_pending_message(d);
  return CNET_SCTP_DATA_OK;
}

static cnet_sctp_data_status abandon_unsent_pending(cnet_sctp_data *d) {
  cnet_sctp_data_abandon_event event = {0};
  event.stream_id = d->pending.stream_id;
  event.ppid = d->pending.ppid;
  event.message_identifier = d->pending.interleaved ?
      d->pending.mid : d->pending.ssn;
  event.message_size = d->pending.size;
  event.reliability = d->pending.reliability;
  event.reason = CNET_SCTP_DATA_ABANDON_TIMED;
  event.unordered = d->pending.unordered;
  event.interleaved = d->pending.interleaved;
  if (push_abandon_event(d, &event) != CNET_SCTP_DATA_OK)
    return CNET_SCTP_DATA_CAPACITY;
  memset(&d->pending, 0, sizeof(d->pending));
  d->probe_deadline_ms = 0u;
  d->probe_interval_ms = 0u;
  return CNET_SCTP_DATA_OK;
}

/* Mark due RFC 3758 records, then publish one cumulative FORWARD-TSN only
 * when every TSN from last_ack+1 is either abandoned or already Gap-ACKed.
 * The fixed stream array bounds the stream/SSN pairs and the copied packet
 * owns the retransmission ticket. */
static cnet_sctp_data_status emit_forward_tsn(cnet_sctp_data *d,
    uint64_t now, int loss_kind, uint32_t loss_tsn,
    void *out, size_t cap, size_t *written) {
  cnet_sctp_data staged;
  uint8_t value[4u + 16u * CNET_SCTP_DATA_STREAMS] = {0};
  uint16_t stream_ssn[CNET_SCTP_DATA_STREAMS] = {0};
  uint32_t stream_mid_ordered[CNET_SCTP_DATA_STREAMS] = {0};
  uint32_t stream_mid_unordered[CNET_SCTP_DATA_STREAMS] = {0};
  uint16_t streams = 0u;
  uint16_t unordered_streams = 0u;
  uint32_t cursor, cumulative;
  size_t i, value_size = 4u, packet_size = 0u;
  int abandoned_seen = 0;
  int loss_abandoned = 0;
  cnet_sctp_data_status status;
  if (!d->config.forward_tsn_supported) return CNET_SCTP_DATA_EMPTY;
  memcpy(&staged, d, sizeof(staged));
  for (i = 0u; i < CNET_SCTP_DATA_TX_SLOTS; ++i) {
    cnet_sctp_data_tx *tx = &staged.tx[i];
    cnet_sctp_data_abandon_reason reason;
    if (!tx->used || tx->abandoned ||
        tx->reliability_mode == CNET_SCTP_DATA_RELIABLE)
      continue;
    if ((tx->reliability_mode == CNET_SCTP_DATA_RELIABILITY_TIMED &&
         now >= tx->expires_at_ms) ||
        (loss_kind != PR_NO_LOSS && tx->tsn == loss_tsn &&
         pr_retransmits_exhausted(tx))) {
      reason = tx->reliability_mode == CNET_SCTP_DATA_RELIABILITY_TIMED ?
          CNET_SCTP_DATA_ABANDON_TIMED :
          CNET_SCTP_DATA_ABANDON_RETRANSMITS;
      status = abandon_tx_message(&staged, tx, reason, loss_tsn,
          &loss_abandoned);
      if (status != CNET_SCTP_DATA_OK) return status;
    }
  }
  cursor = staged.last_ack + 1u;
  cumulative = staged.last_ack;
  for (i = 0u; i < CNET_SCTP_DATA_TX_SLOTS; ++i, ++cursor) {
    cnet_sctp_data_tx *tx = find_tx(&staged, cursor);
    if (!tx || (!tx->abandoned && !tx->gap_acked)) break;
    cumulative = cursor;
    if (tx->abandoned) {
      abandoned_seen = 1;
      if (staged.config.interleaving_supported) {
        uint16_t *mask = tx->unordered ? &unordered_streams : &streams;
        uint32_t *mids = tx->unordered ?
            stream_mid_unordered : stream_mid_ordered;
        const uint16_t bit = (uint16_t)(UINT16_C(1) << tx->stream_id);
        if ((*mask & bit) == 0u ||
            tx->mid - mids[tx->stream_id] < UINT32_C(0x80000000))
          mids[tx->stream_id] = tx->mid;
        *mask |= bit;
      } else if (!tx->unordered) {
        const uint16_t bit = (uint16_t)(UINT16_C(1) << tx->stream_id);
        if ((streams & bit) == 0u ||
            (uint16_t)(tx->ssn - stream_ssn[tx->stream_id]) <
                UINT16_C(0x8000))
          stream_ssn[tx->stream_id] = tx->ssn;
        streams |= bit;
      }
    }
  }
  recompute_flight(&staged);
  if (!abandoned_seen) {
    memcpy(d, &staged, sizeof(*d));
    return CNET_SCTP_DATA_EMPTY;
  }
  /* RFC 3758 F5: abandoning a chunk at a point where ordinary SCTP would
   * mark it for fast/T3 retransmission keeps the corresponding congestion
   * response. Stage it with the FORWARD-TSN so short output is atomic. */
  if (loss_abandoned && loss_kind == PR_FAST_LOSS &&
      !staged.recovery_active) {
    staged.ssthresh = staged.cwnd / 2u;
    if (staged.ssthresh < 4u * CNET_SCTP_DATA_MTU)
      staged.ssthresh = 4u * CNET_SCTP_DATA_MTU;
    staged.cwnd = staged.ssthresh;
    staged.partial_bytes_acked = 0u;
    staged.recovery_exit_tsn = staged.next_tsn - 1u;
    staged.recovery_active = 1u;
  } else if (loss_abandoned && loss_kind == PR_T3_LOSS) {
    staged.ssthresh = staged.cwnd / 2u;
    if (staged.ssthresh < 4u * CNET_SCTP_DATA_MTU)
      staged.ssthresh = 4u * CNET_SCTP_DATA_MTU;
    staged.cwnd = CNET_SCTP_DATA_MTU;
    staged.recovery_active = 0u;
    staged.partial_bytes_acked = 0u;
    staged.rto_ms = staged.rto_ms > staged.config.t3_max_ms / 2u ?
        staged.config.t3_max_ms : staged.rto_ms * 2u;
    for (i = 0u; i < CNET_SCTP_DATA_TX_SLOTS; ++i) {
      if (!staged.tx[i].used) continue;
      staged.tx[i].missing_reports = 0u;
      staged.tx[i].pending_retransmit = 0u;
      staged.tx[i].fast_retransmitted = 0u;
    }
  }
  wr32(value, cumulative);
  for (i = 0u; i < CNET_SCTP_DATA_STREAMS; ++i) {
    if ((streams & (uint16_t)(UINT16_C(1) << i)) == 0u) continue;
    if (staged.config.interleaving_supported) {
      wr16(value + value_size, (uint16_t)i);
      wr16(value + value_size + 2u, 0u);
      wr32(value + value_size + 4u, stream_mid_ordered[i]);
      value_size += 8u;
    } else {
      wr16(value + value_size, (uint16_t)i);
      wr16(value + value_size + 2u, stream_ssn[i]);
      value_size += 4u;
    }
  }
  if (staged.config.interleaving_supported) {
    for (i = 0u; i < CNET_SCTP_DATA_STREAMS; ++i) {
      if ((unordered_streams & (uint16_t)(UINT16_C(1) << i)) == 0u)
        continue;
      wr16(value + value_size, (uint16_t)i);
      wr16(value + value_size + 2u, 1u);
      wr32(value + value_size + 4u, stream_mid_unordered[i]);
      value_size += 8u;
    }
  }
  if (now > UINT64_MAX - staged.rto_ms) return CNET_SCTP_DATA_CLOCK;
  status = encode_one(&staged, staged.config.interleaving_supported ?
      CHUNK_I_FORWARD_TSN : CHUNK_FORWARD_TSN, 0u, value, value_size,
      staged.forward_packet, sizeof(staged.forward_packet), &packet_size);
  if (status != CNET_SCTP_DATA_OK) return status;
  if (packet_size > cap) return CNET_SCTP_DATA_CAPACITY;
  for (i = 0u; i < CNET_SCTP_DATA_TX_SLOTS; ++i)
    if (staged.tx[i].used && !ahead(staged.tx[i].tsn, cumulative))
      memset(&staged.tx[i], 0, sizeof(staged.tx[i]));
  if (staged.probe_active && !find_tx(&staged, staged.probe_tsn)) {
    staged.probe_active = 0u;
    staged.probe_tsn = 0u;
    staged.probe_deadline_ms = 0u;
    staged.probe_interval_ms = 0u;
    staged.probe_unanswered = 0u;
  }
  recompute_flight(&staged);
  staged.forward_active = 1u;
  staged.forward_cumulative_tsn = cumulative;
  staged.forward_packet_size = packet_size;
  staged.forward_deadline_ms = now + staged.rto_ms;
  staged.forward_retransmits = 0u;
  staged.t3_deadline_ms = oldest_outstanding(&staged) ?
      now + staged.rto_ms : 0u;
  staged.t3_wait_ack = 0u;
  staged.last_time_ms = now;
  memcpy(out, staged.forward_packet, packet_size);
  *written = packet_size;
  memcpy(d, &staged, sizeof(*d));
  return CNET_SCTP_DATA_OK;
}

cnet_sctp_data_status cnet_sctp_data_advance(cnet_sctp_data *d, uint64_t now,
    void *out, size_t cap, size_t *written) {
  if (written) *written = 0u;
  cnet_sctp_data_status status = guard(d);
  cnet_sctp_data_tx *oldest;
  if (status != CNET_SCTP_DATA_OK) return status;
  if (!out || !written) return CNET_SCTP_DATA_INVALID;
  if ((status = clock_check(d, now)) != CNET_SCTP_DATA_OK) return status;
  if (d->pending.active && !d->pending.has_tsn &&
      d->pending.reliability.mode == CNET_SCTP_DATA_RELIABILITY_TIMED &&
      now >= d->pending.expires_at_ms) {
    status = abandon_unsent_pending(d);
    if (status != CNET_SCTP_DATA_OK) return status;
    d->last_time_ms = now;
    return CNET_SCTP_DATA_OK;
  }
  if (d->forward_active) {
    if (now < d->forward_deadline_ms) {
      d->last_time_ms = now;
      return CNET_SCTP_DATA_OK;
    }
    if (d->forward_retransmits >= d->config.max_retransmits) {
      d->closed = 1u;
      return CNET_SCTP_DATA_EXPIRED;
    }
    if (d->forward_packet_size > cap) return CNET_SCTP_DATA_CAPACITY;
    if (now > UINT64_MAX - d->rto_ms) return CNET_SCTP_DATA_CLOCK;
    memcpy(out, d->forward_packet, d->forward_packet_size);
    *written = d->forward_packet_size;
    ++d->forward_retransmits;
    d->forward_deadline_ms = now + d->rto_ms;
    d->last_time_ms = now;
    return CNET_SCTP_DATA_OK;
  }
  status = emit_forward_tsn(d, now, PR_NO_LOSS, 0u,
      out, cap, written);
  if (status == CNET_SCTP_DATA_OK || status == CNET_SCTP_DATA_CAPACITY ||
      status == CNET_SCTP_DATA_CLOCK)
    return status;
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
        if (pr_retransmits_exhausted(probe)) {
          status = emit_forward_tsn(d, now, PR_PROBE_RETRANSMIT,
              probe->tsn, out, cap, written);
          if (status != CNET_SCTP_DATA_EMPTY) return status;
          probe = find_retained(d, d->probe_tsn);
          if (!probe || probe->abandoned) {
            d->last_time_ms = now;
            return CNET_SCTP_DATA_OK;
          }
        }
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
        if (probe->probe_retransmits != UINT32_MAX)
          ++probe->probe_retransmits;
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
        status = send_frame(d, now, d->pending.stream_id,
            d->pending.interleaved ? d->pending.mid : d->pending.ssn,
            d->pending.next_fsn, d->pending.ppid, flags,
            d->pending.bytes + d->pending.offset,
            1u, out, cap, written, 1, &d->pending.reliability,
            d->pending.size,
            d->pending.has_tsn ? d->pending.first_tsn : d->next_tsn,
            d->pending.expires_at_ms);
        if (status != CNET_SCTP_DATA_OK) return status;
        if (!d->pending.has_tsn) {
          d->pending.first_tsn = d->next_tsn - 1u;
          d->pending.has_tsn = 1u;
        }
        d->probe_tsn = d->next_tsn - 1u;
        d->probe_active = 1u;
        d->probe_unanswered = 0u;
        ++d->pending.offset;
        ++d->pending.next_fsn;
        if (d->pending.offset == d->pending.size)
          finish_pending_message(d);
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
      if (slot->used && !slot->abandoned && !slot->gap_acked &&
          !slot->fast_retransmitted &&
          slot->pending_retransmit &&
          (!fast || ahead(fast->tsn, slot->tsn)))
        fast = slot;
    }
    if (fast) {
      if (pr_retransmits_exhausted(fast)) {
        status = emit_forward_tsn(d, now, PR_FAST_LOSS, fast->tsn,
            out, cap, written);
        if (status != CNET_SCTP_DATA_EMPTY) return status;
        if (fast->abandoned) {
          d->last_time_ms = now;
          return CNET_SCTP_DATA_OK;
        }
      }
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
  if (pr_retransmits_exhausted(oldest)) {
    status = emit_forward_tsn(d, now, PR_T3_LOSS, oldest->tsn,
        out, cap, written);
    if (status != CNET_SCTP_DATA_EMPTY) return status;
    oldest = oldest_outstanding(d);
    if (!oldest) {
      d->t3_deadline_ms = 0u;
      d->last_time_ms = now;
      return CNET_SCTP_DATA_OK;
    }
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
        /* T3 begins a new loss epoch. A TSN fast-retransmitted in the old
         * epoch may become eligible again after the T3 barrier is ACKed. */
        d->tx[i].fast_retransmitted = 0u;
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
  if (d->forward_active &&
      (!*has_deadline || d->forward_deadline_ms < *deadline)) {
    *has_deadline = 1;
    *deadline = d->forward_deadline_ms;
  }
  if (!d->forward_active && d->config.forward_tsn_supported) {
    uint32_t cursor = d->last_ack + 1u;
    size_t i;
    int abandoned = 0;
    for (i = 0u; i < CNET_SCTP_DATA_TX_SLOTS; ++i, ++cursor) {
      cnet_sctp_data_tx *tx = find_tx(d, cursor);
      if (!tx || (!tx->abandoned && !tx->gap_acked)) break;
      if (tx->abandoned) abandoned = 1;
    }
    if (abandoned && (!*has_deadline || d->last_time_ms < *deadline)) {
      *has_deadline = 1;
      *deadline = d->last_time_ms;
    }
  }
  /* An active zero-window probe owns retransmission timing instead of T3.
   * Establish its deadline first, then allow an earlier PR-message lifetime
   * below to preempt it. */
  if (d->peer_rwnd == 0u && d->probe_deadline_ms &&
      (d->probe_active || !has_retained_tx(d))) {
    *has_deadline = 1;
    *deadline = d->probe_deadline_ms;
  }
  if (d->pending.active &&
      d->pending.reliability.mode == CNET_SCTP_DATA_RELIABILITY_TIMED &&
      (!*has_deadline || d->pending.expires_at_ms < *deadline)) {
    *has_deadline = 1;
    *deadline = d->pending.expires_at_ms;
  }
  {
    size_t i;
    for (i = 0u; i < CNET_SCTP_DATA_TX_SLOTS; ++i) {
      const cnet_sctp_data_tx *tx = &d->tx[i];
      if (tx->used && !tx->abandoned &&
          tx->reliability_mode == CNET_SCTP_DATA_RELIABILITY_TIMED &&
          (!*has_deadline || tx->expires_at_ms < *deadline)) {
        *has_deadline = 1;
        *deadline = tx->expires_at_ms;
      }
    }
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
  memset(d->abandon_events, 0, sizeof(d->abandon_events));
  d->abandon_event_head = 0u;
  d->abandon_event_count = 0u;
  d->in_flight = 0u;
  d->t3_wait_ack = 0u;
  d->partial_bytes_acked = 0u;
  d->buffered_rx = 0u;
  d->rx_gaps = 0u;
  memset(d->rx_forward_ssn, 0, sizeof(d->rx_forward_ssn));
  d->rx_forward_streams = 0u;
  d->last_advertised_rwnd = 0u;
  d->window_update_pending = 0u;
  d->t3_deadline_ms = 0u;
  d->probe_deadline_ms = 0u;
  d->probe_interval_ms = 0u;
  d->probe_active = 0u;
  d->probe_unanswered = 0u;
  d->forward_active = 0u;
  d->forward_cumulative_tsn = 0u;
  d->forward_deadline_ms = 0u;
  d->forward_retransmits = 0u;
  d->forward_packet_size = 0u;
  memset(d->forward_packet, 0, sizeof(d->forward_packet));
  d->closed = 1u;
  return CNET_SCTP_DATA_OK;
}
