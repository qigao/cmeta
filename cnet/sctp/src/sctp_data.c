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
static int ahead(uint32_t a, uint32_t b) {
  return (int32_t)(a - b) > 0;
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
static unsigned int tx_count(const cnet_sctp_data *d) {
  unsigned int i, count = 0u;
  for (i = 0u; i < CNET_SCTP_DATA_TX_SLOTS; ++i)
    if (d->tx[i].used) ++count;
  return count;
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
  d->cwnd = 4u * CNET_SCTP_DATA_MTU; /* bounded first-path slow start */
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
    size_t *written) {
  cnet_sctp_data_tx *slot = NULL;
  uint8_t value[DATA_HEADER + CNET_SCTP_DATA_PAYLOAD_MAX];
  size_t packet_bytes = 0u, i;
  cnet_sctp_data_status status;
  if (n == 0u || n > CNET_SCTP_DATA_PAYLOAD_MAX)
    return CNET_SCTP_DATA_INVALID;
  if (n > d->peer_rwnd || d->in_flight > d->peer_rwnd - n ||
      n > d->cwnd || d->in_flight > d->cwnd - n)
    return CNET_SCTP_DATA_CAPACITY;
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
  slot->retransmits = 0u;
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
      (const uint8_t *)payload, n, output, cap, written);
  if (status == CNET_SCTP_DATA_OK && !unordered) ++d->tx_ssn[stream];
  return status;
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
      n, output, cap, written);
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
  distance = (int32_t)(tsn - prospective_cum);
  if (distance <= 0) duplicate = 1;
  else if (distance > 32) return CNET_SCTP_DATA_CAPACITY;
  else if ((mask & (UINT32_C(1) << ((unsigned int)distance - 1u))) != 0u)
    duplicate = 1;
  if (!duplicate) {
    if (bytes > rx_available(d)) return CNET_SCTP_DATA_CAPACITY;
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
  status = sack_packet(d, prospective_cum, mask,
      rx_available(d) - (duplicate ? 0u : bytes), duplicate, tsn,
      out, cap, written);
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
  d->last_time_ms = now;
  return CNET_SCTP_DATA_OK;
}

static cnet_sctp_data_status input_sack(cnet_sctp_data *d, uint64_t now,
    const cnet_sctp_wire_chunk *ch) {
  uint32_t cumulative, last_tsn, peer_window;
  uint16_t blocks, duplicates, prev_end = 0u;
  size_t i, acked_bytes = 0u;
  if (ch->flags != 0u || ch->value_size < 12u)
    return CNET_SCTP_DATA_PROTOCOL;
  cumulative = rd32(ch->value);
  peer_window = rd32(ch->value + 4u);
  blocks = rd16(ch->value + 8u);
  duplicates = rd16(ch->value + 10u);
  if (blocks > 16u || duplicates > CNET_SCTP_DATA_RX_SLOTS ||
      ch->value_size != 12u + 4u * ((size_t)blocks + duplicates))
    return CNET_SCTP_DATA_PROTOCOL;
  last_tsn = d->next_tsn - 1u;
  if (ahead(cumulative, last_tsn) || ahead(d->last_ack, cumulative))
    return CNET_SCTP_DATA_PROTOCOL;
  for (i = 0u; i < blocks; ++i) {
    const uint8_t *p = ch->value + 12u + 4u * i;
    uint16_t first = rd16(p), end = rd16(p + 2u);
    if (!first || first <= prev_end || end < first ||
        ahead(cumulative + end, last_tsn))
      return CNET_SCTP_DATA_PROTOCOL;
    prev_end = end;
  }
  /* Validate all gap blocks and rescheduling overflow BEFORE changing
   * outstanding tickets. A bad SACK cannot partially settle the sender. */
  if (now > UINT64_MAX - d->config.t3_max_ms)
    return CNET_SCTP_DATA_CLOCK;
  for (i = 0u; i < CNET_SCTP_DATA_TX_SLOTS; ++i) {
    cnet_sctp_data_tx *slot = &d->tx[i];
    size_t j;
    int covered;
    if (!slot->used) continue;
    covered = !ahead(slot->tsn, cumulative);
    for (j = 0u; !covered && j < blocks; ++j) {
      const uint8_t *p = ch->value + 12u + 4u * j;
      const uint32_t start = cumulative + rd16(p);
      const uint32_t end = cumulative + rd16(p + 2u);
      if (!ahead(start, slot->tsn) && !ahead(slot->tsn, end))
        covered = 1;
    }
    if (covered) {
      acked_bytes += slot->user_size;
      d->in_flight -= slot->user_size;
      memset(slot, 0, sizeof(*slot));
    }
  }
  d->last_ack = cumulative;
  d->peer_rwnd = peer_window;
  if (acked_bytes) {
    const uint32_t increment = acked_bytes > CNET_SCTP_DATA_MTU ?
        CNET_SCTP_DATA_MTU : (uint32_t)acked_bytes;
    d->cwnd = d->cwnd + increment > 16u * CNET_SCTP_DATA_MTU ?
        16u * CNET_SCTP_DATA_MTU : d->cwnd + increment;
    d->rto_ms = d->config.t3_initial_ms;
    d->t3_deadline_ms = tx_count(d) ? now + d->rto_ms : 0u;
  }
  d->last_time_ms = now;
  return CNET_SCTP_DATA_OK;
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
      header.chunk_count != 1u ||
      cnet_sctp_wire_next_chunk(&parser, &chunk) != CNET_SCTP_WIRE_OK)
    return CNET_SCTP_DATA_PROTOCOL;
  if (chunk.type == CHUNK_DATA)
    return input_data(d, now, &chunk, out, cap, written);
  if (chunk.type == CHUNK_SACK)
    return input_sack(d, now, &chunk);
  return CNET_SCTP_DATA_PROTOCOL;
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
    /* All capacity and coherency checks above precede any RX mutation. */
    {
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
      meta->stream_id = start->stream_id; /* start was just erased! */
      meta->stream_sequence = start->ssn;
      meta->ppid = start->ppid;
      meta->tsn = start->tsn;
      meta->size = copied;
      meta->unordered = start->unordered;
      *written = copied;
    }
    if (!start->unordered) ++d->rx_ssn[start->stream_id];
    return CNET_SCTP_DATA_OK;
  }
  return CNET_SCTP_DATA_EMPTY;
}

cnet_sctp_data_status cnet_sctp_data_advance(cnet_sctp_data *d, uint64_t now,
    void *out, size_t cap, size_t *written) {
  cnet_sctp_data_status status = guard(d);
  cnet_sctp_data_tx *oldest = NULL;
  unsigned int i;
  if (status != CNET_SCTP_DATA_OK) return status;
  if (!out || !written) return CNET_SCTP_DATA_INVALID;
  *written = 0u;
  if ((status = clock_check(d, now)) != CNET_SCTP_DATA_OK) return status;
  if (!d->t3_deadline_ms || now < d->t3_deadline_ms) {
    d->last_time_ms = now;
    return CNET_SCTP_DATA_OK;
  }
  for (i = 0u; i < CNET_SCTP_DATA_TX_SLOTS; ++i) {
    if (d->tx[i].used && (!oldest ||
        ahead(oldest->tsn, d->tx[i].tsn))) oldest = &d->tx[i];
  }
  if (!oldest) { d->t3_deadline_ms = 0u; return CNET_SCTP_DATA_OK; }
  if (oldest->retransmits >= d->config.max_retransmits) {
    d->closed = 1u; /* terminal is observable, no background retry */
    return CNET_SCTP_DATA_EXPIRED;
  }
  if (oldest->packet_size > cap) return CNET_SCTP_DATA_CAPACITY;
  if (now > UINT64_MAX - d->config.t3_max_ms) return CNET_SCTP_DATA_CLOCK;
  memcpy(out, oldest->packet, oldest->packet_size);
  *written = oldest->packet_size;
  ++oldest->retransmits;
  d->ssthresh = d->cwnd / 2u;
  if (d->ssthresh < 4u * CNET_SCTP_DATA_MTU)
    d->ssthresh = 4u * CNET_SCTP_DATA_MTU;
  d->cwnd = CNET_SCTP_DATA_MTU; /* timeout recovery, not a full cwnd model */
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
  return CNET_SCTP_DATA_OK;
}

cnet_sctp_data_status cnet_sctp_data_stop(cnet_sctp_data *d) {
  cnet_sctp_data_status status = guard(d);
  if (status != CNET_SCTP_DATA_OK) return status;
  memset(d->tx, 0, sizeof(d->tx));
  memset(d->rx, 0, sizeof(d->rx));
  d->in_flight = 0u;
  d->buffered_rx = 0u;
  d->rx_gaps = 0u;
  d->t3_deadline_ms = 0u;
  d->closed = 1u;
  return CNET_SCTP_DATA_OK;
}
