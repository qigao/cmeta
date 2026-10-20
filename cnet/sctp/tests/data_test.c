#include "sctp_data_internal.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct test_owner { int admitted; } test_owner;
static int owner_check(void *v) { return ((test_owner *)v)->admitted; }

static cnet_sctp_data_config config(int initiator, test_owner *owner,
                                    uint32_t a_tsn, uint32_t b_tsn) {
  cnet_sctp_data_config cfg = {0};
  cfg.local_port = initiator ? 5000u : 5001u;
  cfg.remote_port = initiator ? 5001u : 5000u;
  cfg.local_tag = initiator ? UINT32_C(0x11223344) : UINT32_C(0x55667788);
  cfg.peer_tag = initiator ? UINT32_C(0x55667788) : UINT32_C(0x11223344);
  cfg.local_initial_tsn = initiator ? a_tsn : b_tsn;
  cfg.peer_initial_tsn = initiator ? b_tsn : a_tsn;
  cfg.outbound_streams = 4u;
  cfg.inbound_streams = 4u;
  cfg.peer_receive_window = 32768u;
  cfg.t3_initial_ms = 100u;
  cfg.t3_max_ms = 400u;
  cfg.max_retransmits = 3u;
  cfg.is_owner = owner_check;
  cfg.owner_context = owner;
  return cfg;
}
static void pair_init(cnet_sctp_data *a, cnet_sctp_data *b,
                      test_owner *oa, test_owner *ob,
                      uint32_t a_tsn, uint32_t b_tsn) {
  cnet_sctp_data_config ca, cb;
  memset(a, 0, sizeof(*a));
  memset(b, 0, sizeof(*b));
  oa->admitted = ob->admitted = 1;
  ca = config(1, oa, a_tsn, b_tsn);
  cb = config(0, ob, a_tsn, b_tsn);
  assert(cnet_sctp_data_init(a, &ca, 0u) == CNET_SCTP_DATA_OK);
  assert(cnet_sctp_data_init(b, &cb, 0u) == CNET_SCTP_DATA_OK);
}
static void inspect(const uint8_t *packet, size_t n,
                    uint8_t type, uint32_t verification_tag,
                    cnet_sctp_wire_chunk *value) {
  cnet_sctp_wire_header header = {0};
  cnet_sctp_wire_decoder decoder = {0};
  assert(cnet_sctp_wire_decode(packet, n, &header, &decoder) == CNET_SCTP_WIRE_OK);
  assert(header.chunk_count == 1u && header.verification_tag == verification_tag);
  assert(cnet_sctp_wire_next_chunk(&decoder, value) == CNET_SCTP_WIRE_OK);
  assert(value->type == type);
}
static uint32_t rd32(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
         ((uint32_t)p[2] << 8) | p[3];
}
static uint16_t rd16(const uint8_t *p) {
  return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}
static void reseal(uint8_t *packet, size_t n) {
  uint32_t crc;
  memset(packet + 8u, 0, 4u);
  crc = cnet_sctp_wire_crc32c(packet, n);
  packet[8] = (uint8_t)crc;
  packet[9] = (uint8_t)(crc >> 8);
  packet[10] = (uint8_t)(crc >> 16);
  packet[11] = (uint8_t)(crc >> 24);
}

static void gap_sack_wrap_t3_ordering(void) {
  cnet_sctp_data a, b;
  test_owner oa = {0}, ob = {0};
  uint8_t packets[3][1200], sacks[3][1200], retransmit[1200], msg[1024];
  const uint8_t a0[] = {'a','a','0'}, a1[] = {'a','a','1'}, a2[] = {'a','a','2'};
  const uint8_t *inputs[] = {a0, a1, a2};
  cnet_sctp_data_message info = {0};
  cnet_sctp_wire_chunk ch = {0};
  size_t np[3] = {0}, ns[3] = {0}, nr = 0u, nm = 0u;
  uint64_t deadline = 0u;
  int has_deadline = 0;
  unsigned int i;
  pair_init(&a, &b, &oa, &ob, UINT32_C(0xfffffffe), 700u);
  for (i = 0u; i < 3u; ++i) {
    assert(cnet_sctp_data_send(&a, 1u + i, 0u, 51u, 0,
             inputs[i], 3u, packets[i], sizeof(packets[i]), &np[i]) ==
           CNET_SCTP_DATA_OK);
    inspect(packets[i], np[i], 0u, b.config.local_tag, &ch);
    assert(ch.value_size == 15u && ch.flags == 3u);
    assert(rd32(ch.value) == UINT32_C(0xfffffffe) + i);
    assert(rd16(ch.value + 6u) == (uint16_t)i);
  }
  assert(cnet_sctp_data_deadline(&a, &deadline, &has_deadline) == CNET_SCTP_DATA_OK);
  assert(has_deadline && deadline == 101u);
  assert(cnet_sctp_data_input(&b, 10u, packets[1], np[1],
                             sacks[1], sizeof(sacks[1]), &ns[1]) == CNET_SCTP_DATA_OK);
  inspect(sacks[1], ns[1], 3u, a.config.local_tag, &ch);
  assert(rd32(ch.value) == UINT32_C(0xfffffffd));
  assert(rd16(ch.value + 8u) == 1u);
  assert(rd16(ch.value + 12u) == 2u && rd16(ch.value + 14u) == 2u);
  assert(cnet_sctp_data_input(&a, 11u, sacks[1], ns[1],
                             retransmit, sizeof(retransmit), &nr) == CNET_SCTP_DATA_OK);
  assert(nr == 0u && a.in_flight == 6u); /* gap ACK retired DATA 1 */
  assert(cnet_sctp_data_input(&b, 12u, packets[2], np[2],
                             sacks[2], sizeof(sacks[2]), &ns[2]) == CNET_SCTP_DATA_OK);
  inspect(sacks[2], ns[2], 3u, a.config.local_tag, &ch);
  assert(rd16(ch.value + 12u) == 2u && rd16(ch.value + 14u) == 3u);
  assert(cnet_sctp_data_input(&a, 13u, sacks[2], ns[2],
                             retransmit, sizeof(retransmit), &nr) == CNET_SCTP_DATA_OK);
  assert(a.in_flight == 3u);
  assert(cnet_sctp_data_take(&b, &info, msg, sizeof(msg), &nm) == CNET_SCTP_DATA_EMPTY);

  assert(cnet_sctp_data_advance(&a, 210u, retransmit,
                                sizeof(retransmit), &nr) == CNET_SCTP_DATA_OK);
  assert(nr == np[0] && memcmp(retransmit, packets[0], nr) == 0);
  assert(a.cwnd == CNET_SCTP_DATA_MTU);
  assert(cnet_sctp_data_input(&b, 211u, retransmit, nr,
                             sacks[0], sizeof(sacks[0]), &ns[0]) == CNET_SCTP_DATA_OK);
  inspect(sacks[0], ns[0], 3u, a.config.local_tag, &ch);
  assert(rd32(ch.value) == 0u && rd16(ch.value + 8u) == 0u);
  assert(cnet_sctp_data_input(&a, 212u, sacks[0], ns[0],
                             retransmit, sizeof(retransmit), &nr) == CNET_SCTP_DATA_OK);
  assert(a.in_flight == 0u && a.t3_deadline_ms == 0u);
  for (i = 0u; i < 3u; ++i) {
    assert(cnet_sctp_data_take(&b, &info, msg, sizeof(msg), &nm) == CNET_SCTP_DATA_OK);
    assert(info.stream_id == 0u && info.stream_sequence == (uint16_t)i);
    assert(info.ppid == 51u && !info.unordered && nm == 3u);
    assert(memcmp(msg, inputs[i], 3u) == 0);
  }
  assert(cnet_sctp_data_take(&b, &info, msg, sizeof(msg), &nm) == CNET_SCTP_DATA_EMPTY);
  assert(cnet_sctp_data_input(&b, 213u, packets[0], np[0],
                             sacks[0], sizeof(sacks[0]), &ns[0]) == CNET_SCTP_DATA_OK);
  inspect(sacks[0], ns[0], 3u, a.config.local_tag, &ch);
  assert(rd16(ch.value + 10u) == 1u && rd32(ch.value + 12u) == UINT32_C(0xfffffffe));
  assert(cnet_sctp_data_take(&b, &info, msg, sizeof(msg), &nm) == CNET_SCTP_DATA_EMPTY);
  assert(cnet_sctp_data_stop(&a) == CNET_SCTP_DATA_OK);
  assert(cnet_sctp_data_stop(&b) == CNET_SCTP_DATA_OK);
}

static void unordered_priority_and_capacity(void) {
  cnet_sctp_data a, b;
  test_owner oa = {0}, ob = {0};
  uint8_t out[1200], wire0[1200], wire1[1200], ack[1200], event[1024];
  const uint8_t one[] = {'o'}, two[] = {'u'};
  cnet_sctp_data_message info = {0};
  size_t n0 = 0u, n1 = 0u, na = 0u, nmsg = 0u;
  pair_init(&a, &b, &oa, &ob, 100u, 300u);
  assert(cnet_sctp_data_send(&a, 1u, 0u, 53u, 0,
                             one, 1u, wire0, 15u, &n0) == CNET_SCTP_DATA_CAPACITY);
  assert(a.next_tsn == 100u && a.in_flight == 0u);
  assert(cnet_sctp_data_send(&a, 1u, 0u, 53u, 0,
                             one, 0u, wire0, sizeof(wire0), &n0) == CNET_SCTP_DATA_INVALID);
  assert(cnet_sctp_data_send(&a, 1u, 5u, 53u, 0,
                             one, 1u, wire0, sizeof(wire0), &n0) == CNET_SCTP_DATA_INVALID);
  assert(cnet_sctp_data_send(&a, 1u, 0u, 53u, 0,
                             one, 1u, wire0, sizeof(wire0), &n0) == CNET_SCTP_DATA_OK);
  assert(cnet_sctp_data_send(&a, 2u, 1u, 54u, 1,
                             two, 1u, wire1, sizeof(wire1), &n1) == CNET_SCTP_DATA_OK);
  assert(cnet_sctp_data_input(&b, 3u, wire1, n1,
                             ack, 15u, &na) == CNET_SCTP_DATA_CAPACITY);
  assert(b.buffered_rx == 0u && b.rx_gaps == 0u);
  assert(cnet_sctp_data_input(&b, 3u, wire1, n1,
                             ack, sizeof(ack), &na) == CNET_SCTP_DATA_OK);
  assert(cnet_sctp_data_take(&b, &info, event, 0u, &nmsg) == CNET_SCTP_DATA_CAPACITY);
  assert(cnet_sctp_data_take(&b, &info, event, sizeof(event), &nmsg) == CNET_SCTP_DATA_OK);
  assert(info.stream_id == 1u && info.unordered && info.ppid == 54u);
  assert(event[0] == 'u');
  assert(cnet_sctp_data_input(&b, 4u, wire0, n0,
                             ack, sizeof(ack), &na) == CNET_SCTP_DATA_OK);
  assert(cnet_sctp_data_take(&b, &info, event, sizeof(event), &nmsg) == CNET_SCTP_DATA_OK);
  assert(info.stream_id == 0u && !info.unordered && info.stream_sequence == 0u);
  assert(event[0] == 'o');
  /* Owner refusal preserves entire pending transport state. */
  oa.admitted = 0;
  assert(cnet_sctp_data_send(&a, 5u, 0u, 53u, 0,
                             one, 1u, out, sizeof(out), &na) == CNET_SCTP_DATA_OWNER);
  oa.admitted = 1;
  assert(cnet_sctp_data_stop(&a) == CNET_SCTP_DATA_OK);
  assert(cnet_sctp_data_stop(&b) == CNET_SCTP_DATA_OK);
}

static void invalid_sack_and_t3_budget(void) {
  cnet_sctp_data a, b;
  test_owner oa = {0}, ob = {0};
  uint8_t packet[1200], sack[1200], forged[1200], reply[1200];
  const uint8_t msg[] = {'x'};
  size_t np = 0u, ns = 0u, nr = 0u;
  pair_init(&a, &b, &oa, &ob, 10u, 200u);
  assert(cnet_sctp_data_send(&a, 1u, 0u, 51u, 0,
                             msg, 1u, packet, sizeof(packet), &np) == CNET_SCTP_DATA_OK);
  assert(cnet_sctp_data_input(&b, 2u, packet, np,
                             sack, sizeof(sack), &ns) == CNET_SCTP_DATA_OK);
  memcpy(forged, sack, ns);
  /* Cumulative ACK claims an unsent future TSN, but CRC is valid. */
  forged[19u] = 50u;
  reseal(forged, ns);
  assert(cnet_sctp_data_input(&a, 3u, forged, ns,
                             reply, sizeof(reply), &nr) == CNET_SCTP_DATA_PROTOCOL);
  assert(a.in_flight == 1u && a.last_ack == 9u);
  assert(cnet_sctp_data_input(&a, 4u, sack, ns,
                             reply, sizeof(reply), &nr) == CNET_SCTP_DATA_OK);
  assert(a.in_flight == 0u);
  assert(cnet_sctp_data_send(&a, 5u, 0u, 51u, 0,
                             msg, 1u, packet, sizeof(packet), &np) == CNET_SCTP_DATA_OK);
  assert(cnet_sctp_data_advance(&a, 105u, reply, 4u, &nr) == CNET_SCTP_DATA_CAPACITY);
  assert(a.tx[0].retransmits == 0u && a.t3_deadline_ms == 105u);
  assert(cnet_sctp_data_advance(&a, 105u, reply,
                                sizeof(reply), &nr) == CNET_SCTP_DATA_OK);
  assert(nr == np && memcmp(reply, packet, np) == 0);
  assert(cnet_sctp_data_deadline(&a, NULL, NULL) == CNET_SCTP_DATA_INVALID);
  assert(cnet_sctp_data_advance(&a, 305u, reply,
                                sizeof(reply), &nr) == CNET_SCTP_DATA_OK);
  assert(cnet_sctp_data_advance(&a, 705u, reply,
                                sizeof(reply), &nr) == CNET_SCTP_DATA_OK);
  assert(cnet_sctp_data_advance(&a, 1105u, reply,
                                sizeof(reply), &nr) == CNET_SCTP_DATA_EXPIRED);
  assert(a.closed);
  assert(cnet_sctp_data_send(&a, 1106u, 0u, 51u, 0,
                             msg, 1u, packet, sizeof(packet), &np) == CNET_SCTP_DATA_CLOSED);
  assert(cnet_sctp_data_stop(&b) == CNET_SCTP_DATA_OK);
}

/* Half-range TSN difference has no ordering under RFC serial arithmetic.
 * Treat it as non-admissible instead of depending on signed narrowing. */
static void ambiguous_half_wrap_tsn(void) {
  cnet_sctp_data a, b;
  test_owner oa = {0}, ob = {0};
  uint8_t packet[1200], sack[1200], message[8];
  const uint8_t value = 'h';
  cnet_sctp_data_message info = {0};
  cnet_sctp_wire_chunk chunk = {0};
  size_t np = 0u, ns = 0u, nm = 0u;
  pair_init(&a, &b, &oa, &ob, 100u, 200u);
  /* Deliberately inject an invalid half-wrap receiver baseline. */
  b.cumulative_rx = a.next_tsn - UINT32_C(0x80000000);
  assert(cnet_sctp_data_send(&a, 1u, 0u, 51u, 0, &value, 1u,
                             packet, sizeof(packet), &np) == CNET_SCTP_DATA_OK);
  assert(cnet_sctp_data_input(&b, 2u, packet, np,
                              sack, sizeof(sack), &ns) == CNET_SCTP_DATA_OK);
  inspect(sack, ns, 3u, a.config.local_tag, &chunk);
  assert(rd16(chunk.value + 10u) == 1u);
  assert(rd32(chunk.value + 12u) == 100u);
  assert(b.buffered_rx == 0u && b.rx_gaps == 0u);
  assert(cnet_sctp_data_take(&b, &info, message, sizeof(message), &nm) ==
         CNET_SCTP_DATA_EMPTY);
  assert(cnet_sctp_data_stop(&a) == CNET_SCTP_DATA_OK);
  assert(cnet_sctp_data_stop(&b) == CNET_SCTP_DATA_OK);
}

static void karn_rtt_first_sample(void) {
  cnet_sctp_data a, b;
  test_owner oa = {0}, ob = {0};
  uint8_t packet[1200], sack[1200], reply[1200];
  const uint8_t payload = 'r';
  size_t np = 0u, ns = 0u, nr = 0u;
  pair_init(&a, &b, &oa, &ob, 100u, 300u);
  assert(cnet_sctp_data_send(&a, 1u, 0u, 51u, 0, &payload, 1u,
      packet, sizeof(packet), &np) == CNET_SCTP_DATA_OK);
  assert(cnet_sctp_data_input(&b, 51u, packet, np,
      sack, sizeof(sack), &ns) == CNET_SCTP_DATA_OK);
  assert(cnet_sctp_data_input(&a, 51u, sack, ns,
      reply, sizeof(reply), &nr) == CNET_SCTP_DATA_OK);
  assert(a.rtt_valid && a.srtt_x8_ms == 400u);
  assert(a.rto_ms == 150u);
  assert(cnet_sctp_data_send(&a, 52u, 0u, 51u, 0, &payload, 1u,
      packet, sizeof(packet), &np) == CNET_SCTP_DATA_OK);
  assert(cnet_sctp_data_advance(&a, 202u, reply,
      sizeof(reply), &nr) == CNET_SCTP_DATA_OK);
  assert(a.tx[0].retransmits == 1u);
  assert(cnet_sctp_data_input(&b, 203u, packet, np,
      sack, sizeof(sack), &ns) == CNET_SCTP_DATA_OK);
  assert(cnet_sctp_data_input(&a, 204u, sack, ns,
      reply, sizeof(reply), &nr) == CNET_SCTP_DATA_OK);
  assert(a.srtt_x8_ms == 400u); /* Karn: retransmitted TSN excluded */
  assert(cnet_sctp_data_stop(&a) == CNET_SCTP_DATA_OK);
  assert(cnet_sctp_data_stop(&b) == CNET_SCTP_DATA_OK);
}

/* A cumulative ACK that samples RTT must not commit any ticket if its
 * newly computed RTO deadline could overflow the caller's clock. */
static void rtt_deadline_overflow_atomicity(void) {
  cnet_sctp_data a, b;
  test_owner oa = {0}, ob = {0};
  uint8_t packet[1200], sack[1200], reply[1200];
  const uint8_t payload = 'o';
  size_t np = 0u, ns = 0u, nr = 0u;
  const uint64_t near_end = UINT64_MAX - 200u;
  pair_init(&a, &b, &oa, &ob, 100u, 300u);
  assert(cnet_sctp_data_send(&a, 1u, 0u, 51u, 0, &payload, 1u,
      packet, sizeof(packet), &np) == CNET_SCTP_DATA_OK);
  assert(cnet_sctp_data_send(&a, 2u, 0u, 51u, 0, &payload, 1u,
      reply, sizeof(reply), &nr) == CNET_SCTP_DATA_OK);
  assert(cnet_sctp_data_input(&b, 3u, packet, np,
      sack, sizeof(sack), &ns) == CNET_SCTP_DATA_OK);
  assert(cnet_sctp_data_input(&a, near_end, sack, ns,
      reply, sizeof(reply), &nr) == CNET_SCTP_DATA_CLOCK);
  assert(a.last_ack == 99u && a.in_flight == 2u);
  assert(a.tx[0].used && a.tx[1].used && !a.rtt_valid);
  assert(cnet_sctp_data_stop(&a) == CNET_SCTP_DATA_OK);
  assert(cnet_sctp_data_stop(&b) == CNET_SCTP_DATA_OK);
}

static void distinct_gap_sack_missing_evidence(void) {
  cnet_sctp_data a, b;
  test_owner oa = {0}, ob = {0};
  uint8_t packets[4][1200], sack[1200], reply[1200];
  const uint8_t payload = 'g';
  size_t np[4] = {0}, ns = 0u, nr = 0u;
  unsigned int i;
  pair_init(&a, &b, &oa, &ob, 100u, 300u);
  for (i = 0u; i < 4u; ++i)
    assert(cnet_sctp_data_send(&a, 1u + i, 0u, 51u, 0,
        &payload, 1u, packets[i], sizeof(packets[i]), &np[i]) ==
        CNET_SCTP_DATA_OK);
  for (i = 1u; i < 4u; ++i) {
    assert(cnet_sctp_data_input(&b, 10u + i, packets[i], np[i],
        sack, sizeof(sack), &ns) == CNET_SCTP_DATA_OK);
    assert(cnet_sctp_data_input(&a, 20u + i, sack, ns,
        reply, sizeof(reply), &nr) == CNET_SCTP_DATA_OK);
    assert(a.tx[0].missing_reports == i);
    assert(cnet_sctp_data_input(&a, 20u + i, sack, ns,
        reply, sizeof(reply), &nr) == CNET_SCTP_DATA_OK);
    assert(a.tx[0].missing_reports == i);
  }
  assert(cnet_sctp_data_stop(&a) == CNET_SCTP_DATA_OK);
  assert(cnet_sctp_data_stop(&b) == CNET_SCTP_DATA_OK);
}

static void fast_retransmit_once(void) {
  cnet_sctp_data a, b;
  test_owner oa = {0}, ob = {0};
  uint8_t packets[4][1200], sack[1200], reply[1200];
  const uint8_t payload = 'f';
  size_t np[4] = {0}, ns = 0u, nr = 0u;
  unsigned int i;
  pair_init(&a, &b, &oa, &ob, 100u, 300u);
  for (i = 0u; i < 4u; ++i)
    assert(cnet_sctp_data_send(&a, 1u + i, 0u, 51u, 0,
        &payload, 1u, packets[i], sizeof(packets[i]), &np[i]) ==
        CNET_SCTP_DATA_OK);
  for (i = 1u; i < 4u; ++i) {
    assert(cnet_sctp_data_input(&b, 10u + i, packets[i], np[i],
        sack, sizeof(sack), &ns) == CNET_SCTP_DATA_OK);
    assert(cnet_sctp_data_input(&a, 20u + i, sack, ns,
        reply, sizeof(reply), &nr) == CNET_SCTP_DATA_OK);
  }
  assert(a.tx[0].missing_reports == 3u);
  assert(cnet_sctp_data_advance(&a, 24u, reply, 8u, &nr) ==
         CNET_SCTP_DATA_CAPACITY);
  assert(!a.tx[0].fast_retransmitted);
  assert(cnet_sctp_data_advance(&a, 24u, reply, sizeof(reply), &nr) ==
         CNET_SCTP_DATA_OK);
  assert(nr == np[0] && memcmp(reply, packets[0], nr) == 0);
  assert(a.tx[0].fast_retransmitted && a.tx[0].retransmits == 1u);
  assert(a.recovery_active && a.recovery_exit_tsn == 103u);
  assert(cnet_sctp_data_advance(&a, 25u, reply, sizeof(reply), &nr) ==
         CNET_SCTP_DATA_OK);
  assert(nr == 0u);
  /* A cumulative ACK through the recovery point exits exactly once. */
  assert(cnet_sctp_data_input(&b, 26u, packets[0], np[0],
      sack, sizeof(sack), &ns) == CNET_SCTP_DATA_OK);
  {
    const uint32_t recovery_cwnd = a.cwnd;
    assert(cnet_sctp_data_input(&a, 27u, sack, ns,
        reply, sizeof(reply), &nr) == CNET_SCTP_DATA_OK);
    assert(!a.recovery_active && a.in_flight == 0u);
    assert(a.cwnd == recovery_cwnd); /* exit ACK is not a growth ACK */
  }
  assert(cnet_sctp_data_stop(&a) == CNET_SCTP_DATA_OK);
  assert(cnet_sctp_data_stop(&b) == CNET_SCTP_DATA_OK);
}

static void fast_retransmit_budget_exhaustion(void) {
  cnet_sctp_data a, b;
  test_owner oa = {0}, ob = {0};
  uint8_t packets[4][1200], sack[1200], reply[1200];
  const uint8_t payload = 'e';
  size_t np[4] = {0}, ns = 0u, nr = 0u;
  unsigned int i;
  pair_init(&a, &b, &oa, &ob, 100u, 300u);
  for (i = 0u; i < 4u; ++i)
    assert(cnet_sctp_data_send(&a, 1u + i, 0u, 51u, 0,
        &payload, 1u, packets[i], sizeof(packets[i]), &np[i]) ==
        CNET_SCTP_DATA_OK);
  for (i = 1u; i < 4u; ++i) {
    assert(cnet_sctp_data_input(&b, 10u + i, packets[i], np[i],
        sack, sizeof(sack), &ns) == CNET_SCTP_DATA_OK);
    assert(cnet_sctp_data_input(&a, 20u + i, sack, ns,
        reply, sizeof(reply), &nr) == CNET_SCTP_DATA_OK);
  }
  assert(a.tx[0].missing_reports == 3u);
  a.tx[0].retransmits = a.config.max_retransmits;
  assert(cnet_sctp_data_advance(&a, 24u, reply, sizeof(reply), &nr) ==
         CNET_SCTP_DATA_EXPIRED);
  assert(a.closed && !a.tx[0].fast_retransmitted && nr == 0u);
  assert(cnet_sctp_data_stop(&b) == CNET_SCTP_DATA_OK);
}

static void t3_precedes_fast_retransmit(void) {
  cnet_sctp_data a, b;
  test_owner oa = {0}, ob = {0};
  uint8_t packets[4][1200], sack[1200], reply[1200];
  const uint8_t payload = 't';
  size_t np[4] = {0}, ns = 0u, nr = 0u;
  unsigned int i;
  pair_init(&a, &b, &oa, &ob, 100u, 300u);
  for (i = 0u; i < 4u; ++i)
    assert(cnet_sctp_data_send(&a, 1u + i, 0u, 51u, 0,
        &payload, 1u, packets[i], sizeof(packets[i]), &np[i]) ==
        CNET_SCTP_DATA_OK);
  for (i = 1u; i < 4u; ++i) {
    assert(cnet_sctp_data_input(&b, 10u + i, packets[i], np[i],
        sack, sizeof(sack), &ns) == CNET_SCTP_DATA_OK);
    assert(cnet_sctp_data_input(&a, 20u + i, sack, ns,
        reply, sizeof(reply), &nr) == CNET_SCTP_DATA_OK);
  }
  assert(a.tx[0].missing_reports == 3u);
  assert(cnet_sctp_data_advance(&a, a.t3_deadline_ms, reply, sizeof(reply), &nr) ==
         CNET_SCTP_DATA_OK);
  assert(nr == np[0] && memcmp(reply, packets[0], nr) == 0);
  assert(a.tx[0].retransmits == 1u && !a.tx[0].fast_retransmitted);
  assert(a.tx[0].missing_reports == 0u);
  assert(!a.recovery_active && a.cwnd == CNET_SCTP_DATA_MTU);
  assert(cnet_sctp_data_advance(&a, a.last_time_ms + 1u,
      reply, sizeof(reply), &nr) == CNET_SCTP_DATA_OK);
  assert(nr == 0u && a.tx[0].retransmits == 1u);
  assert(cnet_sctp_data_stop(&a) == CNET_SCTP_DATA_OK);
  assert(cnet_sctp_data_stop(&b) == CNET_SCTP_DATA_OK);
}

/* Karn regression: cumulative ACK after fast retransmit cannot sample
 * that retransmitted TSN, even when it exits the recovery epoch. */
static void karn_fast_retransmit_exclusion(void) {
  cnet_sctp_data a, b;
  test_owner oa = {0}, ob = {0};
  uint8_t packets[4][1200], sack[1200], reply[1200];
  const uint8_t payload = 'k';
  size_t np[4] = {0}, ns = 0u, nr = 0u;
  unsigned int i;
  pair_init(&a, &b, &oa, &ob, 100u, 300u);
  for (i = 0u; i < 4u; ++i)
    assert(cnet_sctp_data_send(&a, 1u + i, 0u, 51u, 0,
        &payload, 1u, packets[i], sizeof(packets[i]), &np[i]) ==
        CNET_SCTP_DATA_OK);
  for (i = 1u; i < 4u; ++i) {
    assert(cnet_sctp_data_input(&b, 10u + i, packets[i], np[i],
        sack, sizeof(sack), &ns) == CNET_SCTP_DATA_OK);
    assert(cnet_sctp_data_input(&a, 20u + i, sack, ns,
        reply, sizeof(reply), &nr) == CNET_SCTP_DATA_OK);
  }
  assert(!a.rtt_valid && a.tx[0].missing_reports == 3u);
  assert(cnet_sctp_data_advance(&a, 24u, reply, sizeof(reply), &nr) ==
         CNET_SCTP_DATA_OK);
  assert(a.tx[0].fast_retransmitted && a.tx[0].retransmits == 1u);
  assert(cnet_sctp_data_input(&b, 25u, reply, nr,
      sack, sizeof(sack), &ns) == CNET_SCTP_DATA_OK);
  assert(cnet_sctp_data_input(&a, 26u, sack, ns,
      reply, sizeof(reply), &nr) == CNET_SCTP_DATA_OK);
  assert(!a.rtt_valid && a.srtt_x8_ms == 0u);
  assert(!a.recovery_active && a.in_flight == 0u);
  assert(cnet_sctp_data_stop(&a) == CNET_SCTP_DATA_OK);
  assert(cnet_sctp_data_stop(&b) == CNET_SCTP_DATA_OK);
}

/* A TSN previously acknowledged through a revocable Gap ACK cannot
 * later provide a Karn RTT sample when the cumulative ACK catches up. */
static void karn_gap_ack_then_cumulative(void) {
  cnet_sctp_data a, b;
  test_owner oa = {0}, ob = {0};
  uint8_t packets[2][1200], sack[1200], reply[1200];
  const uint8_t payload = 'g';
  size_t np[2] = {0}, ns = 0u, nr = 0u;
  unsigned int i;
  pair_init(&a, &b, &oa, &ob, 100u, 300u);
  for (i = 0u; i < 2u; ++i)
    assert(cnet_sctp_data_send(&a, 1u + i, 0u, 51u, 0,
        &payload, 1u, packets[i], sizeof(packets[i]), &np[i]) ==
        CNET_SCTP_DATA_OK);
  assert(cnet_sctp_data_input(&b, 10u, packets[1], np[1],
      sack, sizeof(sack), &ns) == CNET_SCTP_DATA_OK);
  assert(cnet_sctp_data_input(&a, 11u, sack, ns,
      reply, sizeof(reply), &nr) == CNET_SCTP_DATA_OK);
  assert(a.tx[1].gap_acked && a.tx[1].ack_counted && !a.rtt_valid);
  /* Exclude TSN 100 from sampling so only the previously gap-acked
   * TSN 101 could contribute an RTT sample on cumulative confirmation. */
  a.tx[0].retransmits = 1u;
  assert(cnet_sctp_data_input(&b, 12u, packets[0], np[0],
      sack, sizeof(sack), &ns) == CNET_SCTP_DATA_OK);
  assert(cnet_sctp_data_input(&a, 13u, sack, ns,
      reply, sizeof(reply), &nr) == CNET_SCTP_DATA_OK);
  assert(!a.rtt_valid && a.in_flight == 0u);
  assert(cnet_sctp_data_stop(&a) == CNET_SCTP_DATA_OK);
  assert(cnet_sctp_data_stop(&b) == CNET_SCTP_DATA_OK);
}

/* RFC 9260 reneging: a Gap ACK can be withdrawn and later restored.
 * The retained TX ticket remains owned and must not produce an RTT sample. */
static void karn_gap_reneging_restore(void) {
  cnet_sctp_data a, b;
  test_owner oa = {0}, ob = {0};
  uint8_t packets[2][1200], sack[1200], reneged[1200], reply[1200];
  const uint8_t payload = 'n';
  size_t np[2] = {0}, ns = 0u, nr = 0u, nrn = 0u;
  cnet_sctp_wire_chunk chunk = {0};
  unsigned int i;
  pair_init(&a, &b, &oa, &ob, 100u, 300u);
  for (i = 0u; i < 2u; ++i)
    assert(cnet_sctp_data_send(&a, 1u + i, 0u, 51u, 0,
        &payload, 1u, packets[i], sizeof(packets[i]), &np[i]) ==
        CNET_SCTP_DATA_OK);
  assert(cnet_sctp_data_input(&b, 10u, packets[1], np[1],
      sack, sizeof(sack), &ns) == CNET_SCTP_DATA_OK);
  assert(cnet_sctp_data_input(&a, 11u, sack, ns,
      reply, sizeof(reply), &nr) == CNET_SCTP_DATA_OK);
  assert(a.tx[1].gap_acked && a.tx[1].ack_counted && !a.rtt_valid);
  /* Produce a CRC-valid SACK with identical cumulative TSN but no gaps. */
  {
    uint8_t value[12] = {0};
    cnet_sctp_wire_chunk outgoing = {0};
    value[3] = 99u;
    value[6] = 0x80u; /* advertised receive window = 32768 */
    outgoing.type = 3u;
    outgoing.flags = 0u;
    outgoing.value = value;
    outgoing.value_size = sizeof(value);
    assert(cnet_sctp_wire_encode(b.config.local_port, b.config.remote_port,
        a.config.local_tag, &outgoing, 1u, reneged, sizeof(reneged),
        &nrn) == CNET_SCTP_WIRE_OK);
    inspect(reneged, nrn, 3u, a.config.local_tag, &chunk);
  }
  assert(cnet_sctp_data_input(&a, 12u, reneged, nrn,
      reply, sizeof(reply), &nr) == CNET_SCTP_DATA_OK);
  assert(!a.tx[1].gap_acked && a.tx[1].ack_counted);
  assert(a.in_flight == 2u && !a.rtt_valid);
  assert(cnet_sctp_data_input(&a, 13u, sack, ns,
      reply, sizeof(reply), &nr) == CNET_SCTP_DATA_OK);
  assert(a.tx[1].gap_acked && a.tx[1].ack_counted && !a.rtt_valid);
  assert(cnet_sctp_data_stop(&a) == CNET_SCTP_DATA_OK);
  assert(cnet_sctp_data_stop(&b) == CNET_SCTP_DATA_OK);
}

/* Two missing TSNs must not cause repeated congestion-window reductions
 * inside the same recovery epoch. Each retransmit remains Owner-driven. */
static void multi_loss_one_recovery_epoch(void) {
  cnet_sctp_data a, b;
  test_owner oa = {0}, ob = {0};
  uint8_t packets[5][1200], sack[1200], reply[1200];
  const uint8_t payload = 'm';
  size_t np[5] = {0}, ns = 0u, nr = 0u;
  uint32_t reduced_cwnd;
  unsigned int i;
  pair_init(&a, &b, &oa, &ob, 100u, 300u);
  for (i = 0u; i < 5u; ++i)
    assert(cnet_sctp_data_send(&a, 1u + i, 0u, 51u, 0,
        &payload, 1u, packets[i], sizeof(packets[i]), &np[i]) ==
        CNET_SCTP_DATA_OK);
  /* Lose TSN 100 and 101; three higher Gap ACKs provide evidence. */
  for (i = 2u; i < 5u; ++i) {
    assert(cnet_sctp_data_input(&b, 10u + i, packets[i], np[i],
        sack, sizeof(sack), &ns) == CNET_SCTP_DATA_OK);
    assert(cnet_sctp_data_input(&a, 20u + i, sack, ns,
        reply, sizeof(reply), &nr) == CNET_SCTP_DATA_OK);
  }
  assert(a.tx[0].missing_reports == 3u);
  assert(a.tx[1].missing_reports == 3u);
  assert(cnet_sctp_data_advance(&a, 25u, reply, sizeof(reply), &nr) ==
         CNET_SCTP_DATA_OK);
  assert(nr == np[0] && memcmp(reply, packets[0], nr) == 0);
  assert(a.recovery_active && a.recovery_exit_tsn == 104u);
  reduced_cwnd = a.cwnd;
  assert(cnet_sctp_data_advance(&a, 26u, reply, sizeof(reply), &nr) ==
         CNET_SCTP_DATA_OK);
  assert(nr == np[1] && memcmp(reply, packets[1], nr) == 0);
  assert(a.cwnd == reduced_cwnd && a.recovery_active);
  assert(a.tx[0].fast_retransmitted && a.tx[1].fast_retransmitted);
  assert(cnet_sctp_data_advance(&a, 27u, reply, sizeof(reply), &nr) ==
         CNET_SCTP_DATA_OK);
  assert(nr == 0u && a.cwnd == reduced_cwnd);
  assert(cnet_sctp_data_stop(&a) == CNET_SCTP_DATA_OK);
  assert(cnet_sctp_data_stop(&b) == CNET_SCTP_DATA_OK);
}

static void output_count_zeroed_before_owner_guard(void) {
  cnet_sctp_data a, b;
  test_owner oa = {1}, ob = {1};
  uint8_t packet[1200] = {0}, payload[4] = {1u, 2u, 3u, 4u};
  cnet_sctp_data_message message = {0};
  size_t written;
  pair_init(&a, &b, &oa, &ob, 100u, 300u);
  oa.admitted = 0;
  written = 99u;
  assert(cnet_sctp_data_send(&a, 1u, 0u, 51u, 0, payload, sizeof(payload),
      packet, sizeof(packet), &written) == CNET_SCTP_DATA_OWNER);
  assert(written == 0u);
  written = 99u;
  assert(cnet_sctp_data_send_bundle(&a, 1u, NULL, 1u, packet, sizeof(packet),
      &written) == CNET_SCTP_DATA_OWNER);
  assert(written == 0u);
  written = 99u;
  assert(cnet_sctp_data_input(&a, 1u, packet, sizeof(packet), packet,
      sizeof(packet), &written) == CNET_SCTP_DATA_OWNER);
  assert(written == 0u);
  written = 99u;
  assert(cnet_sctp_data_take(&a, &message, payload, sizeof(payload),
      &written) == CNET_SCTP_DATA_OWNER);
  assert(written == 0u);
  written = 99u;
  assert(cnet_sctp_data_advance(&a, 1u, packet, sizeof(packet),
      &written) == CNET_SCTP_DATA_OWNER);
  assert(written == 0u);
  oa.admitted = 1;
  assert(cnet_sctp_data_stop(&a) == CNET_SCTP_DATA_OK);
  assert(cnet_sctp_data_stop(&b) == CNET_SCTP_DATA_OK);
}

int main(void) {
  output_count_zeroed_before_owner_guard();
  multi_loss_one_recovery_epoch();
  karn_gap_reneging_restore();
  karn_gap_ack_then_cumulative();
  karn_fast_retransmit_exclusion();
  t3_precedes_fast_retransmit();
  fast_retransmit_budget_exhaustion();
  fast_retransmit_once();
  distinct_gap_sack_missing_evidence();
  rtt_deadline_overflow_atomicity();
  karn_rtt_first_sample();
  ambiguous_half_wrap_tsn();
  gap_sack_wrap_t3_ordering();
  unordered_priority_and_capacity();
  invalid_sack_and_t3_budget();
  puts("SCTP C11 DATA/TSN/GAP-SACK/ordered-unordered/T3 bounded tests PASS");
  return 0;
}
