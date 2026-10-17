#include "sctp_data_internal.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Original strict C11 wire tests of SCTP 6.9: CRC-correct multi-DATA,
 * SACK-before-DATA and transactional rejection of malformed trailing DATA.
 * All traffic uses the actual fixed-capacity DATA engine and encoder. */
typedef struct pair {
  cnet_sctp_data a, b;
  int admitted;
} pair;
static int is_owner(void *v) { return *(int *)v; }
static cnet_sctp_data_config cfg(pair *p, int a) {
  cnet_sctp_data_config c = {0};
  c.local_port = a ? 5000u : 5001u;
  c.remote_port = a ? 5001u : 5000u;
  c.local_tag = a ? 111u : 222u;
  c.peer_tag = a ? 222u : 111u;
  c.local_initial_tsn = a ? 100u : 900u;
  c.peer_initial_tsn = a ? 900u : 100u;
  c.outbound_streams = c.inbound_streams = 2u;
  c.peer_receive_window = CNET_SCTP_DATA_RECEIVE_CEILING;
  c.t3_initial_ms = 100u; c.t3_max_ms = 400u;
  c.max_retransmits = 3u;
  c.is_owner = is_owner; c.owner_context = &p->admitted;
  return c;
}
static void init(pair *p) {
  cnet_sctp_data_config a, b;
  memset(p, 0, sizeof(*p)); p->admitted = 1;
  a = cfg(p, 1); b = cfg(p, 0);
  assert(cnet_sctp_data_init(&p->a, &a, 0u) == CNET_SCTP_DATA_OK);
  assert(cnet_sctp_data_init(&p->b, &b, 0u) == CNET_SCTP_DATA_OK);
}
static cnet_sctp_wire_chunk one(const uint8_t *packet, size_t n,
    uint8_t expected) {
  cnet_sctp_wire_header h = {0};
  cnet_sctp_wire_decoder dec = {0};
  cnet_sctp_wire_chunk ch = {0};
  assert(cnet_sctp_wire_decode(packet, n, &h, &dec) == CNET_SCTP_WIRE_OK);
  assert(h.chunk_count == 1u);
  assert(cnet_sctp_wire_next_chunk(&dec, &ch) == CNET_SCTP_WIRE_OK);
  assert(ch.type == expected);
  return ch;
}
static size_t pack(const cnet_sctp_data *d, int outbound,
    const cnet_sctp_wire_chunk *chunks, size_t count,
    uint8_t *out, size_t cap) {
  size_t n = 0u;
  const uint16_t source = outbound ? d->config.local_port : d->config.remote_port;
  const uint16_t dest = outbound ? d->config.remote_port : d->config.local_port;
  const uint32_t tag = outbound ? d->config.peer_tag : d->config.local_tag;
  assert(cnet_sctp_wire_encode(source, dest, tag, chunks, count,
      out, cap, &n) == CNET_SCTP_WIRE_OK);
  return n;
}
static void multi_data_atomicity(void) {
  pair p;
  uint8_t original[3][CNET_SCTP_DATA_MTU];
  uint8_t combined[CNET_SCTP_DATA_MTU], sack[CNET_SCTP_DATA_MTU];
  uint8_t payload[3] = {'a', 'b', 'c'}, body[8];
  size_t sent[3], n, ns, nr, nm, i;
  cnet_sctp_wire_chunk chunks[3], corrupt[3], reported;
  cnet_sctp_data_message meta = {0};
  cnet_sctp_data before;
  init(&p);
  for (i = 0u; i < 3u; ++i) {
    assert(cnet_sctp_data_send(&p.a, i + 1u, 0u, 53u, 0,
        &payload[i], 1u, original[i], sizeof(original[i]),
        &sent[i]) == CNET_SCTP_DATA_OK);
    chunks[i] = one(original[i], sent[i], 0u);
  }
  n = pack(&p.a, 1, chunks, 3u, combined, sizeof(combined));
  memcpy(corrupt, chunks, sizeof(corrupt));
  corrupt[2].flags = 0x80u;
  ns = pack(&p.a, 1, corrupt, 3u, combined, sizeof(combined));
  memcpy(&before, &p.b, sizeof(before));
  nr = 0u;
  assert(cnet_sctp_data_input(&p.b, 10u, combined, ns, sack,
      sizeof(sack), &nr) == CNET_SCTP_DATA_PROTOCOL);
  assert(nr == 0u && memcmp(&before, &p.b, sizeof(before)) == 0);
  n = pack(&p.a, 1, chunks, 3u, combined, sizeof(combined));
  assert(cnet_sctp_data_input(&p.b, 10u, combined, n, sack, 8u,
      &nr) == CNET_SCTP_DATA_CAPACITY);
  assert(nr == 0u && memcmp(&before, &p.b, sizeof(before)) == 0);
  assert(cnet_sctp_data_input(&p.b, 10u, combined, n, sack,
      sizeof(sack), &nr) == CNET_SCTP_DATA_OK);
  reported = one(sack, nr, 3u);
  assert(reported.value_size >= 12u && reported.value[3] == 102u);
  assert(p.b.buffered_rx == 3u && p.b.cumulative_rx == 102u);
  assert(cnet_sctp_data_input(&p.a, 11u, sack, nr, combined,
      sizeof(combined), &ns) == CNET_SCTP_DATA_OK);
  assert(ns == 0u && p.a.in_flight == 0u);
  for (i = 0u; i < 3u; ++i) {
    assert(cnet_sctp_data_take(&p.b, &meta, body,
        sizeof(body), &nm) == CNET_SCTP_DATA_OK);
    assert(nm == 1u && body[0] == payload[i] &&
        meta.stream_sequence == (uint16_t)i);
  }
  assert(cnet_sctp_data_stop(&p.a) == CNET_SCTP_DATA_OK);
  assert(cnet_sctp_data_stop(&p.b) == CNET_SCTP_DATA_OK);
}
static void sack_before_data_and_bad_order(void) {
  pair p;
  const uint8_t request = 0x22u, reply = 0x33u;
  uint8_t data_a[CNET_SCTP_DATA_MTU], data_b[CNET_SCTP_DATA_MTU];
  uint8_t ack_a[CNET_SCTP_DATA_MTU], combined[CNET_SCTP_DATA_MTU];
  uint8_t response[CNET_SCTP_DATA_MTU], body[8];
  size_t na, nb, ns, np, nr, nm;
  cnet_sctp_wire_chunk good[2], bad[2], sack;
  cnet_sctp_data_message meta = {0};
  cnet_sctp_data before;
  init(&p);
  assert(cnet_sctp_data_send(&p.a, 1u, 0u, 53u, 0,
      &request, 1u, data_a, sizeof(data_a), &na) == CNET_SCTP_DATA_OK);
  assert(cnet_sctp_data_input(&p.b, 2u, data_a, na, ack_a,
      sizeof(ack_a), &ns) == CNET_SCTP_DATA_OK);
  assert(cnet_sctp_data_send(&p.b, 3u, 0u, 53u, 0,
      &reply, 1u, data_b, sizeof(data_b), &nb) == CNET_SCTP_DATA_OK);
  sack = one(ack_a, ns, 3u);
  good[0] = sack;
  good[1] = one(data_b, nb, 0u);
  bad[0] = good[1]; bad[1] = good[0];
  np = pack(&p.b, 1, bad, 2u, combined, sizeof(combined));
  memcpy(&before, &p.a, sizeof(before));
  assert(cnet_sctp_data_input(&p.a, 10u, combined, np,
      response, sizeof(response), &nr) == CNET_SCTP_DATA_PROTOCOL);
  assert(nr == 0u && memcmp(&before, &p.a, sizeof(before)) == 0);
  np = pack(&p.b, 1, good, 2u, combined, sizeof(combined));
  assert(cnet_sctp_data_input(&p.a, 10u, combined, np,
      response, sizeof(response), &nr) == CNET_SCTP_DATA_OK);
  assert(one(response, nr, 3u).type == 3u);
  assert(p.a.in_flight == 0u && p.a.buffered_rx == 1u);
  assert(cnet_sctp_data_input(&p.b, 11u, response, nr,
      combined, sizeof(combined), &ns) == CNET_SCTP_DATA_OK);
  assert(ns == 0u && p.b.in_flight == 0u);
  assert(cnet_sctp_data_take(&p.a, &meta, body,
      sizeof(body), &nm) == CNET_SCTP_DATA_OK);
  assert(nm == 1u && body[0] == reply);
  assert(cnet_sctp_data_stop(&p.a) == CNET_SCTP_DATA_OK);
  assert(cnet_sctp_data_stop(&p.b) == CNET_SCTP_DATA_OK);
}
/* The emitted bundle has 3 chunks, but the one-Owner T3 schedule keeps
 * three separately retransmittable single-DATA tickets when it is lost. */
static void outbound_bundle_and_per_tsn_t3(void) {
  pair p;
  cnet_sctp_data_bundle_item items[5] = {0};
  const uint8_t payload[5] = {'a', 'b', 'c', 'd', 'e'};
  uint8_t combined[CNET_SCTP_DATA_MTU], single[CNET_SCTP_DATA_MTU];
  uint8_t sack[CNET_SCTP_DATA_MTU], response[CNET_SCTP_DATA_MTU];
  uint8_t body[8];
  size_t n = 0u, ns = 0u, nr = 0u, nm = 0u, i;
  uint64_t due = 0u;
  int has_due = 0;
  cnet_sctp_data before;
  cnet_sctp_data_message meta = {0};
  cnet_sctp_wire_header header = {0};
  cnet_sctp_wire_decoder decoder = {0};
  cnet_sctp_wire_chunk chunk = {0};
  init(&p);
  for (i = 0u; i < 5u; ++i) {
    items[i].stream_id = 0u;
    items[i].ppid = 53u;
    items[i].payload = &payload[i];
    items[i].payload_size = 1u;
  }
  memcpy(&before, &p.a, sizeof(before));
  assert(cnet_sctp_data_send_bundle(&p.a, 1u, items, 5u,
      combined, sizeof(combined), &n) == CNET_SCTP_DATA_CAPACITY);
  assert(n == 0u && memcmp(&before, &p.a, sizeof(before)) == 0);
  memset(combined, 0xA5, sizeof(combined));
  assert(cnet_sctp_data_send_bundle(&p.a, 1u, items, 3u,
      combined, 10u, &n) == CNET_SCTP_DATA_CAPACITY);
  assert(n == 0u && combined[0] == 0xA5 &&
      memcmp(&before, &p.a, sizeof(before)) == 0);
  p.admitted = 0;
  assert(cnet_sctp_data_send_bundle(&p.a, 1u, items, 3u,
      combined, sizeof(combined), &n) == CNET_SCTP_DATA_OWNER);
  p.admitted = 1;
  assert(memcmp(&before, &p.a, sizeof(before)) == 0);
  assert(cnet_sctp_data_send_bundle(&p.a, 1u, items, 3u,
      combined, sizeof(combined), &n) == CNET_SCTP_DATA_OK);
  assert(n > 0u && p.a.next_tsn == 103u && p.a.in_flight == 3u);
  assert(cnet_sctp_wire_decode(combined, n, &header, &decoder) ==
      CNET_SCTP_WIRE_OK);
  assert(header.chunk_count == 3u);
  for (i = 0u; i < 3u; ++i) {
    assert(cnet_sctp_wire_next_chunk(&decoder, &chunk) == CNET_SCTP_WIRE_OK);
    assert(chunk.type == 0u && chunk.value_size == 13u &&
        chunk.value[3] == (uint8_t)(100u + i));
  }
  /* Drop the complete outbound bundle: T3 MUST resend original TSNs
   * separately, not replay the combined packet or duplicate any ticket. */
  for (i = 0u; i < 3u; ++i) {
    assert(cnet_sctp_data_deadline(&p.a, &due, &has_due) == CNET_SCTP_DATA_OK);
    assert(has_due);
    assert(cnet_sctp_data_advance(&p.a, due, single,
        sizeof(single), &nr) == CNET_SCTP_DATA_OK);
    chunk = one(single, nr, 0u);
    assert(chunk.value_size == 13u &&
        chunk.value[3] == (uint8_t)(100u + i));
    assert(cnet_sctp_data_input(&p.b, due + 1u, single, nr,
        sack, sizeof(sack), &ns) == CNET_SCTP_DATA_OK);
    assert(cnet_sctp_data_input(&p.a, due + 2u, sack, ns,
        response, sizeof(response), &n) == CNET_SCTP_DATA_OK);
    assert(n == 0u && p.a.in_flight == 2u - i);
  }
  assert(p.a.next_tsn == 103u && p.a.t3_deadline_ms == 0u);
  for (i = 0u; i < 3u; ++i) {
    assert(cnet_sctp_data_take(&p.b, &meta, body,
        sizeof(body), &nm) == CNET_SCTP_DATA_OK);
    assert(nm == 1u && body[0] == payload[i] &&
        meta.stream_sequence == (uint16_t)i);
  }
  assert(cnet_sctp_data_stop(&p.a) == CNET_SCTP_DATA_OK);
  assert(cnet_sctp_data_stop(&p.b) == CNET_SCTP_DATA_OK);
}
static void outbound_bundle_respects_peer_window(void) {
  pair p;
  cnet_sctp_data_bundle_item items[2] = {{0}};
  uint8_t packet[CNET_SCTP_DATA_MTU];
  uint8_t body[2] = {'x', 'y'};
  size_t written = 99u;
  cnet_sctp_data before;
  init(&p);
  items[0].ppid = items[1].ppid = 53u;
  items[0].payload = &body[0]; items[1].payload = &body[1];
  items[0].payload_size = items[1].payload_size = 1u;
  p.a.config.peer_receive_window = 1u;
  assert(cnet_sctp_data_send_bundle(&p.a, 1u, items, 1u,
      packet, sizeof(packet), &written) == CNET_SCTP_DATA_OK);
  assert(written > 0u && p.a.in_flight == 1u);
  memcpy(&before, &p.a, sizeof(before));
  memset(packet, 0xA5, sizeof(packet)); written = 99u;
  assert(cnet_sctp_data_send_bundle(&p.a, 2u, items, 2u,
      packet, sizeof(packet), &written) == CNET_SCTP_DATA_CAPACITY);
  assert(written == 0u && packet[0] == 0xA5 &&
      memcmp(&before, &p.a, sizeof(before)) == 0);
  assert(cnet_sctp_data_stop(&p.a) == CNET_SCTP_DATA_OK);
  assert(cnet_sctp_data_stop(&p.b) == CNET_SCTP_DATA_OK);
}
int main(void) {
  outbound_bundle_respects_peer_window();
  outbound_bundle_and_per_tsn_t3();
  multi_data_atomicity();
  sack_before_data_and_bad_order();
  puts("C11 SCTP bounded bundled DATA / leading SACK atomicity PASS");
  return 0;
}
