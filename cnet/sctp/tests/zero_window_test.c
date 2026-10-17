#include "sctp_data_internal.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* CRC-correct wire tests; no TX/RX state injection or external SCTP engine. */
static int owned(void *p) { return *(int *)p; }
static void wr16(uint8_t *p, uint16_t n) {
  p[0] = (uint8_t)(n >> 8); p[1] = (uint8_t)n;
}
static void wr32(uint8_t *p, uint32_t n) {
  p[0] = (uint8_t)(n >> 24); p[1] = (uint8_t)(n >> 16);
  p[2] = (uint8_t)(n >> 8); p[3] = (uint8_t)n;
}
static uint32_t rd32(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
      ((uint32_t)p[2] << 8) | p[3];
}
static void init(cnet_sctp_data *d, int *owner, int receiver) {
  cnet_sctp_data_config c = {0};
  memset(d, 0, sizeof(*d)); *owner = 1;
  c.local_port = receiver ? 5001u : 5000u;
  c.remote_port = receiver ? 5000u : 5001u;
  c.local_tag = receiver ? 456u : 123u;
  c.peer_tag = receiver ? 123u : 456u;
  c.local_initial_tsn = receiver ? 900u : 100u;
  c.peer_initial_tsn = receiver ? 100u : 900u;
  c.inbound_streams = c.outbound_streams = 2u;
  c.peer_receive_window = CNET_SCTP_DATA_RECEIVE_CEILING;
  c.t3_initial_ms = 100u;
  c.t3_max_ms = 400u; c.max_retransmits = 3u;
  c.is_owner = owned; c.owner_context = owner;
  assert(cnet_sctp_data_init(d, &c, 0u) == CNET_SCTP_DATA_OK);
}
static size_t make_sack(const cnet_sctp_data *d, uint32_t cum,
    uint32_t rwnd, uint8_t *out) {
  uint8_t value[12] = {0};
  cnet_sctp_wire_chunk chunk = {0};
  size_t n = 0u;
  wr32(value, cum); wr32(value + 4u, rwnd);
  chunk.type = 3u; chunk.value = value; chunk.value_size = sizeof(value);
  assert(cnet_sctp_wire_encode(d->config.remote_port,
      d->config.local_port, d->config.local_tag, &chunk, 1u,
      out, CNET_SCTP_DATA_MTU, &n) == CNET_SCTP_WIRE_OK);
  return n;
}
static void ack(cnet_sctp_data *d, uint64_t now, uint32_t cum,
    uint32_t rwnd) {
  uint8_t wire[CNET_SCTP_DATA_MTU], out[CNET_SCTP_DATA_MTU];
  size_t n = make_sack(d, cum, rwnd, wire), written = 99u;
  assert(cnet_sctp_data_input(d, now, wire, n, out,
      sizeof(out), &written) == CNET_SCTP_DATA_OK);
  assert(written == 0u);
}
static void zero_probe_backoff_recovery(void) {
  cnet_sctp_data a, before;
  int owner;
  const uint8_t message[3] = {'a', 'b', 'c'};
  uint8_t first[CNET_SCTP_DATA_MTU], out[CNET_SCTP_DATA_MTU];
  size_t n = 0u, nfirst = 0u;
  uint64_t due = 0u;
  uint32_t cwnd;
  int has_due = 0;
  init(&a, &owner, 0);
  ack(&a, 10u, 99u, 0u);
  assert(a.peer_rwnd == 0u && !a.in_flight);
  assert(cnet_sctp_data_send(&a, 11u, 0u, 53u, 0, message,
      sizeof(message), out, sizeof(out), &n) == CNET_SCTP_DATA_CAPACITY);
  assert(n == 0u && a.next_tsn == 100u);
  assert(cnet_sctp_data_queue_message(&a, 11u, 0u, 53u, 0, message,
      sizeof(message)) == CNET_SCTP_DATA_OK);
  assert(cnet_sctp_data_deadline(&a, &due, &has_due) == CNET_SCTP_DATA_OK);
  assert(has_due && due == 111u);
  assert(cnet_sctp_data_advance(&a, 110u, out, sizeof(out), &n) ==
      CNET_SCTP_DATA_OK);
  assert(n == 0u && a.next_tsn == 100u);
  memcpy(&before, &a, sizeof(before));
  assert(cnet_sctp_data_advance(&a, 111u, out, 0u, &n) ==
      CNET_SCTP_DATA_CAPACITY);
  assert(n == 0u && memcmp(&before, &a, sizeof(before)) == 0);
  assert(cnet_sctp_data_advance(&a, 111u, first,
      sizeof(first), &nfirst) == CNET_SCTP_DATA_OK);
  assert(nfirst > 0u && a.probe_active && a.probe_tsn == 100u);
  assert(a.pending.active && a.pending.offset == 1u && a.next_tsn == 101u);
  assert(a.probe_deadline_ms == 311u);
  cwnd = a.cwnd;
  ack(&a, 112u, 99u, 0u); /* live, closed-window receiver */
  assert(a.probe_deadline_ms == 311u);
  memcpy(&before, &a, sizeof(before));
  assert(cnet_sctp_data_advance(&a, 311u, out, 0u, &n) ==
      CNET_SCTP_DATA_CAPACITY);
  assert(n == 0u && memcmp(&before, &a, sizeof(before)) == 0);
  assert(cnet_sctp_data_advance(&a, 311u, out,
      sizeof(out), &n) == CNET_SCTP_DATA_OK);
  assert(n == nfirst && memcmp(out, first, n) == 0);
  assert(a.cwnd == cwnd && a.probe_deadline_ms == 711u &&
      a.tx[0].retransmits == 0u && a.tx[0].probe_retransmitted);
  ack(&a, 312u, 99u, 0u);
  assert(cnet_sctp_data_deadline(&a, &due, &has_due) == CNET_SCTP_DATA_OK);
  assert(has_due && due == 711u);
  ack(&a, 400u, 100u, 1024u); /* accepted probe, reopened peer window */
  assert(!a.probe_active && !a.probe_deadline_ms &&
      a.pending.active && a.pending.offset == 1u && !a.rtt_valid);
  assert(cnet_sctp_data_pump(&a, 401u, out, sizeof(out), &n) ==
      CNET_SCTP_DATA_OK);
  assert(n > 0u && !a.pending.active && a.next_tsn == 102u);
  ack(&a, 402u, 101u, 1024u);
  assert(a.in_flight == 0u && a.t3_deadline_ms == 0u);
  assert(cnet_sctp_data_stop(&a) == CNET_SCTP_DATA_OK);
}

static void receiver_zero_window_sacks_dropped_data(void) {
  cnet_sctp_data a, b;
  int oa, ob;
  uint8_t payload[1024] = {0};
  uint8_t frame[CNET_SCTP_DATA_MTU], sack[CNET_SCTP_DATA_MTU];
  uint8_t out[CNET_SCTP_DATA_MTU], body[1024], value[13] = {0};
  cnet_sctp_data_message meta = {0};
  cnet_sctp_wire_chunk chunk = {0};
  size_t n, ns, nw;
  unsigned int i;
  init(&a, &oa, 0); init(&b, &ob, 1);
  for (i = 0u; i < 32u; ++i) {
    assert(cnet_sctp_data_send(&a, 1u, 0u, 53u, 0,
        payload, sizeof(payload), frame, sizeof(frame), &n) ==
        CNET_SCTP_DATA_OK);
    assert(cnet_sctp_data_input(&b, 1u, frame, n,
        sack, sizeof(sack), &ns) == CNET_SCTP_DATA_OK);
    assert(cnet_sctp_data_input(&a, 1u, sack, ns,
        out, sizeof(out), &nw) == CNET_SCTP_DATA_OK);
  }
  assert(b.buffered_rx == CNET_SCTP_DATA_RECEIVE_CEILING &&
      b.cumulative_rx == 131u && a.peer_rwnd == 0u);
  wr32(value, 132u); wr16(value + 4u, 0u); wr16(value + 6u, 32u);
  wr32(value + 8u, 53u); value[12] = 'Z';
  chunk.type = 0u; chunk.flags = 3u;
  chunk.value = value; chunk.value_size = sizeof(value);
  assert(cnet_sctp_wire_encode(a.config.local_port, a.config.remote_port,
      b.config.local_tag, &chunk, 1u,
      frame, sizeof(frame), &n) == CNET_SCTP_WIRE_OK);
  assert(cnet_sctp_data_input(&b, 2u, frame, n,
      sack, sizeof(sack), &ns) == CNET_SCTP_DATA_OK);
  assert(ns >= 28u && rd32(sack + 16u) == 131u &&
      rd32(sack + 20u) == 0u);
  assert(b.buffered_rx == CNET_SCTP_DATA_RECEIVE_CEILING);
  for (i = 0u; i < 2u; ++i) {
    assert(cnet_sctp_data_take(&b, &meta, body, sizeof(body), &nw) ==
        CNET_SCTP_DATA_OK);
    assert(nw == sizeof(body));
  }
  assert(b.buffered_rx == 30u * 1024u);
  assert(cnet_sctp_data_input(&b, 3u, frame, n,
      sack, sizeof(sack), &ns) == CNET_SCTP_DATA_OK);
  /* One consumed RX slot remains free after accepting the probe. */
  assert(rd32(sack + 16u) == 132u && rd32(sack + 20u) == 1024u);
  assert(b.buffered_rx == 30u * 1024u + 1u);
  assert(cnet_sctp_data_stop(&a) == CNET_SCTP_DATA_OK);
  assert(cnet_sctp_data_stop(&b) == CNET_SCTP_DATA_OK);
}
int main(void) {
  zero_probe_backoff_recovery();
  receiver_zero_window_sacks_dropped_data();
  puts("SCTP zero-window Owner-driven DATA probe and recovery PASS");
  return 0;
}
