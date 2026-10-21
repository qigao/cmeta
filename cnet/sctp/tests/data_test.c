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

/* RFC 9260 3.3.1: reserved DATA flags are ignored on receive, the I
 * bit is accepted (this bounded profile emits SACK synchronously), and
 * an unordered DATA wire SSN is not an admission constraint. Mutate
 * checksum-correct, retained sender packets rather than changing the
 * sender's normal zero-reserved-bit encoding. */
static void data_reserved_i_and_unordered_ssn_are_ignored(void) {
  cnet_sctp_data a, b;
  test_owner oa = {0}, ob = {0};
  uint8_t ordered[1200], unordered[1200], sack[1200], reply[1200], msg[8];
  const uint8_t o = 'o', u = 'u';
  cnet_sctp_data_message meta = {0};
  cnet_sctp_wire_chunk ch = {0};
  size_t no = 0u, nu = 0u, ns = 0u, nr = 0u, nm = 0u;

  pair_init(&a, &b, &oa, &ob, 100u, 300u);
  assert(cnet_sctp_data_send(&a, 1u, 0u, 53u, 0,
      &o, 1u, ordered, sizeof(ordered), &no) == CNET_SCTP_DATA_OK);
  assert(cnet_sctp_data_send(&a, 2u, 1u, 54u, 1,
      &u, 1u, unordered, sizeof(unordered), &nu) == CNET_SCTP_DATA_OK);
  assert(ordered[13] == 3u && unordered[13] == 7u);
  /* DATA B/E/U are low three bits; 0xF0 are ignored reserved bits
   * and 0x08 is the optional I bit. */
  ordered[13] |= 0xF8u;
  unordered[13] |= 0xF8u;
  /* DATA SSN field sits at packet offsets 22-23; U=1 ignores it. */
  unordered[22] = 0xBEu;
  unordered[23] = 0xEFu;
  reseal(ordered, no);
  reseal(unordered, nu);
  inspect(ordered, no, 0u, b.config.local_tag, &ch);
  assert(ch.flags == 0xFBu && rd16(ch.value + 6u) == 0u);
  inspect(unordered, nu, 0u, b.config.local_tag, &ch);
  assert(ch.flags == 0xFFu && rd16(ch.value + 6u) == 0xBEEFu);

  /* A later unordered TSN arrives first and is delivered immediately. */
  assert(cnet_sctp_data_input(&b, 3u, unordered, nu,
      sack, sizeof(sack), &ns) == CNET_SCTP_DATA_OK);
  inspect(sack, ns, 3u, a.config.local_tag, &ch);
  assert(ch.flags == 0u && rd32(ch.value) == 99u);
  assert(rd16(ch.value + 8u) == 1u && rd16(ch.value + 10u) == 0u);
  assert(rd16(ch.value + 12u) == 2u && rd16(ch.value + 14u) == 2u);
  assert(cnet_sctp_data_take(&b, &meta, msg, sizeof(msg), &nm) ==
      CNET_SCTP_DATA_OK);
  assert(meta.unordered && meta.stream_id == 1u &&
      meta.stream_sequence == 0u && meta.ppid == 54u);
  assert(nm == 1u && msg[0] == u);
  assert(cnet_sctp_data_take(&b, &meta, msg, sizeof(msg), &nm) ==
      CNET_SCTP_DATA_EMPTY);
  assert(cnet_sctp_data_input(&a, 4u, sack, ns,
      reply, sizeof(reply), &nr) == CNET_SCTP_DATA_OK);
  assert(nr == 0u && a.in_flight == 1u && a.tx[1].gap_acked);

  /* Filling the lower TSN closes the gap without double-delivery. */
  assert(cnet_sctp_data_input(&b, 5u, ordered, no,
      sack, sizeof(sack), &ns) == CNET_SCTP_DATA_OK);
  inspect(sack, ns, 3u, a.config.local_tag, &ch);
  assert(ch.flags == 0u && rd32(ch.value) == 101u);
  assert(rd16(ch.value + 8u) == 0u && rd16(ch.value + 10u) == 0u);
  assert(cnet_sctp_data_take(&b, &meta, msg, sizeof(msg), &nm) ==
      CNET_SCTP_DATA_OK);
  assert(!meta.unordered && meta.stream_id == 0u &&
      meta.stream_sequence == 0u && meta.ppid == 53u);
  assert(nm == 1u && msg[0] == o);
  assert(cnet_sctp_data_input(&a, 6u, sack, ns,
      reply, sizeof(reply), &nr) == CNET_SCTP_DATA_OK);
  assert(nr == 0u && a.last_ack == 101u && a.in_flight == 0u &&
      a.t3_deadline_ms == 0u && cnet_sctp_data_outbound_drained(&a));

  assert(cnet_sctp_data_input(&b, 7u, unordered, nu,
      sack, sizeof(sack), &ns) == CNET_SCTP_DATA_OK);
  inspect(sack, ns, 3u, a.config.local_tag, &ch);
  assert(ch.flags == 0u && ch.value_size == 16u);
  assert(rd32(ch.value) == 101u && rd16(ch.value + 8u) == 0u &&
      rd16(ch.value + 10u) == 1u && rd32(ch.value + 12u) == 101u);
  assert(cnet_sctp_data_take(&b, &meta, msg, sizeof(msg), &nm) ==
      CNET_SCTP_DATA_EMPTY);
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

/* SACK body bytes below are authored independently of the SCTP DATA
 * peer, rather than round-tripped through its packet generator. A separate
 * fixed-wire known-answer pair also exercises checksum, length and TSN
 * handling without calling the prototype wire encoder. */
static void sack_test_be16(uint8_t *p, uint16_t n) {
  p[0] = (uint8_t)(n >> 8);
  p[1] = (uint8_t)n;
}
static void sack_test_be32(uint8_t *p, uint32_t n) {
  p[0] = (uint8_t)(n >> 24);
  p[1] = (uint8_t)(n >> 16);
  p[2] = (uint8_t)(n >> 8);
  p[3] = (uint8_t)n;
}

static void hand_authored_sack_rejections_preserve_t3(void) {
  cnet_sctp_data a, b, before;
  test_owner oa = {0}, ob = {0};
  const uint8_t values[] = {'a', 'b', 'c'};
  uint8_t packets[3][1200], wire[256], response[1200];
  size_t np[3] = {0}, packet_n = 0u, response_n = 0u, i, edge;
  cnet_sctp_wire_chunk ch = {0};
  cnet_sctp_wire_header header = {0};
  cnet_sctp_wire_decoder decoder = {0};
  /* These exact 32/28-byte wire fixtures were constructed separately,
   * using SCTP common header 5001->5000, VTag 0x11223344 and reflected
   * CRC32C with a zero checksum field. The embedded checksum bytes are
   * in SCTP little-endian order. No local peer or encoder generates them.
   * Gap: cumulative=100, rwnd=32768, Gap Ack Block=[2,2], flags=0x5A
   * (RFC 9260 3.3.4 requires receivers to ignore SACK flag bits).
   * Final: cumulative=102, rwnd=32768, zero gaps/duplicates.
   * Stale: cumulative=99, rwnd=32768, zero gaps/duplicates. */
  static const uint8_t gap_sack_wire[] = {
    0x13u,0x89u,0x13u,0x88u,0x11u,0x22u,0x33u,0x44u,
    0xEDu,0x07u,0xE2u,0x38u,0x03u,0x5Au,0x00u,0x14u,
    0x00u,0x00u,0x00u,0x64u,0x00u,0x00u,0x80u,0x00u,
    0x00u,0x01u,0x00u,0x00u,0x00u,0x02u,0x00u,0x02u
  };
  static const uint8_t final_sack_wire[] = {
    0x13u,0x89u,0x13u,0x88u,0x11u,0x22u,0x33u,0x44u,
    0xF7u,0x48u,0x6Bu,0x1Au,0x03u,0x00u,0x00u,0x10u,
    0x00u,0x00u,0x00u,0x66u,0x00u,0x00u,0x80u,0x00u,
    0x00u,0x00u,0x00u,0x00u
  };
  static const uint8_t stale_sack_wire[] = {
    0x13u,0x89u,0x13u,0x88u,0x11u,0x22u,0x33u,0x44u,
    0x8Cu,0x5Cu,0x9Au,0x30u,0x03u,0x00u,0x00u,0x10u,
    0x00u,0x00u,0x00u,0x63u,0x00u,0x00u,0x80u,0x00u,
    0x00u,0x00u,0x00u,0x00u
  };
  static const struct {
    uint32_t cumulative;
    uint16_t blocks, duplicates;
    uint16_t start0, end0, start1, end1;
    size_t payload_bytes;
    uint8_t flags;
  } invalid[] = {
    {103u,0u,0u,0u,0u,0u,0u,12u,0u}, /* ACK of unsent TSN */
    {99u,1u,0u,0u,1u,0u,0u,16u,0u}, /* gap starts at zero */
    {99u,1u,0u,2u,1u,0u,0u,16u,0u}, /* reversed gap */
    {99u,1u,0u,4u,4u,0u,0u,16u,0u}, /* gap beyond sent TSN */
    {99u,2u,0u,1u,2u,2u,3u,20u,0u}, /* overlapping gaps */
    {99u,2u,0u,2u,3u,1u,1u,20u,0u}, /* unordered gaps */
    {99u,1u,0u,1u,1u,0u,0u,12u,0u}, /* missing gap bytes */
    {99u,0u,0u,0u,0u,0u,0u,16u,0u}, /* extra unclaimed bytes */
    {99u,17u,0u,1u,1u,0u,0u,80u,0u}, /* >16 gap blocks */
    {99u,0u,1u,0u,0u,0u,0u,12u,0u}, /* missing duplicate TSN */
    {99u,1u,0u,0u,1u,0u,0u,16u,0xA5u},
      /* invalid zero-offset gap remains invalid when SACK flags are ignored */
    {UINT32_C(0x80000063),0u,0u,0u,0u,0u,0u,12u,0u},
      /* ambiguous serial-number half-wrap */
    {102u,1u,0u,1u,1u,0u,0u,16u,0u}
      /* gap refers past already cumulative-ACKed sent span */
  };

  pair_init(&a, &b, &oa, &ob, 100u, 300u);
  for (i = 0u; i < 3u; ++i)
    assert(cnet_sctp_data_send(&a, 1u + i, 0u, 51u, 0,
        values + i, 1u, packets[i], sizeof(packets[i]), &np[i]) ==
        CNET_SCTP_DATA_OK);
  assert(a.last_ack == 99u && a.in_flight == 3u);
  assert(a.t3_deadline_ms == 101u && a.rto_ms == 100u);

  assert(cnet_sctp_wire_decode(gap_sack_wire, sizeof(gap_sack_wire),
      &header, &decoder) == CNET_SCTP_WIRE_OK);
  assert(header.source_port == 5001u && header.destination_port == 5000u &&
      header.verification_tag == a.config.local_tag && header.chunk_count == 1u);
  assert(cnet_sctp_wire_next_chunk(&decoder, &ch) == CNET_SCTP_WIRE_OK);
  assert(ch.type == 3u && ch.flags == 0x5Au && ch.value_size == 16u &&
      rd32(ch.value) == 100u && rd16(ch.value + 12u) == 2u &&
      rd16(ch.value + 14u) == 2u);
  assert(cnet_sctp_wire_decode(final_sack_wire, sizeof(final_sack_wire),
      &header, &decoder) == CNET_SCTP_WIRE_OK);
  assert(cnet_sctp_wire_decode(stale_sack_wire, sizeof(stale_sack_wire),
      &header, &decoder) == CNET_SCTP_WIRE_OK);

  /* Every malformed-but-CRC-valid SACK is rejected before processing T3,
   * both one tick before expiry and exactly on the deadline. */
  for (edge = 0u; edge < 2u; ++edge) {
    for (i = 0u; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
      uint8_t body[12u + 33u * 4u] = {0};
      cnet_sctp_wire_chunk sack = {0};
      size_t j;
      sack_test_be32(body, invalid[i].cumulative);
      sack_test_be32(body + 4u, 32768u);
      sack_test_be16(body + 8u, invalid[i].blocks);
      sack_test_be16(body + 10u, invalid[i].duplicates);
      sack_test_be16(body + 12u, invalid[i].start0);
      sack_test_be16(body + 14u, invalid[i].end0);
      sack_test_be16(body + 16u, invalid[i].start1);
      sack_test_be16(body + 18u, invalid[i].end1);
      sack.type = 3u;
      sack.flags = invalid[i].flags;
      sack.value = body;
      sack.value_size = invalid[i].payload_bytes;
      assert(cnet_sctp_wire_encode(5001u, 5000u, a.config.local_tag,
          &sack, 1u, wire, sizeof(wire), &packet_n) == CNET_SCTP_WIRE_OK);
      /* Isolate SACK semantic rejection from mere checksum failure. */
      assert(cnet_sctp_wire_decode(wire, packet_n, &header, &decoder) ==
          CNET_SCTP_WIRE_OK);
      memcpy(&before, &a, sizeof(before));
      memset(response, 0xCC, sizeof(response));
      response_n = 123u;
      assert(cnet_sctp_data_input(&a, edge ? 101u : 100u,
          wire, packet_n, response, sizeof(response), &response_n) ==
          CNET_SCTP_DATA_PROTOCOL);
      assert(response_n == 0u && memcmp(&a, &before, sizeof(before)) == 0);
      for (j = 0u; j < sizeof(response); ++j)
        assert(response[j] == 0xCCu);
    }
  }

  /* Corrupt a fixed-wire known-answer packet WITHOUT recomputing CRC. */
  {
    uint8_t corrupted[sizeof(gap_sack_wire)];
    memcpy(corrupted, gap_sack_wire, sizeof(corrupted));
    corrupted[19] ^= 0x01u;
    assert(cnet_sctp_wire_decode(corrupted, sizeof(corrupted),
        &header, &decoder) == CNET_SCTP_WIRE_CHECKSUM);
    memcpy(&before, &a, sizeof(before));
    memset(response, 0xCC, sizeof(response));
    response_n = 123u;
    assert(cnet_sctp_data_input(&a, 101u, corrupted, sizeof(corrupted),
        response, sizeof(response), &response_n) == CNET_SCTP_DATA_PROTOCOL);
    assert(response_n == 0u && response[0] == 0xCCu &&
        memcmp(&a, &before, sizeof(before)) == 0);
  }

  /* Only explicit Owner advance may emit the due T3 retransmission.
   * A too-small destination at the exact deadline must be retryable. */
  memcpy(&before, &a, sizeof(before));
  memset(response, 0xCC, sizeof(response));
  response_n = 123u;
  assert(cnet_sctp_data_advance(&a, 101u, response, np[0] - 1u,
      &response_n) == CNET_SCTP_DATA_CAPACITY);
  assert(response_n == 0u && response[0] == 0xCCu &&
      memcmp(&a, &before, sizeof(before)) == 0);
  assert(cnet_sctp_data_advance(&a, 101u, response, sizeof(response),
      &response_n) == CNET_SCTP_DATA_OK);
  assert(response_n == np[0] &&
      memcmp(response, packets[0], np[0]) == 0);
  assert(a.tx[0].retransmits == 1u && a.t3_wait_ack);
  assert(a.rto_ms == 200u && a.t3_deadline_ms == 301u);

  /* Positives use only the fixed, independently checksumed wire fixtures.
   * A cumulative ACK for the T3 retransmit clears its admission barrier;
   * a later Gap ACK does not prematurely release the retained TSN 102. */
  memset(response, 0xCC, sizeof(response));
  assert(cnet_sctp_data_input(&a, 102u, gap_sack_wire,
      sizeof(gap_sack_wire), response, sizeof(response), &response_n) ==
      CNET_SCTP_DATA_OK);
  assert(response_n == 0u && response[0] == 0xCCu);
  assert(a.last_ack == 100u && !a.tx[0].used && a.tx[1].used &&
      a.tx[2].used && a.tx[2].gap_acked && a.tx[2].ack_counted);
  assert(!a.t3_wait_ack && a.in_flight == 1u);
  assert(a.t3_deadline_ms == 302u && a.rto_ms == 200u);
  assert(cnet_sctp_data_input(&a, 103u, final_sack_wire,
      sizeof(final_sack_wire), response, sizeof(response), &response_n) ==
      CNET_SCTP_DATA_OK);
  assert(response_n == 0u && a.last_ack == 102u);
  assert(a.in_flight == 0u && a.t3_deadline_ms == 0u);
  assert(cnet_sctp_data_outbound_drained(&a));

  /* A valid but outdated SACK is ignored instead of rewinding the
   * cumulative acknowledgement, T3 or monotonic Owner clock. */
  memcpy(&before, &a, sizeof(before));
  memset(response, 0xCC, sizeof(response));
  response_n = 123u;
  assert(cnet_sctp_data_input(&a, 104u, stale_sack_wire,
      sizeof(stale_sack_wire), response, sizeof(response), &response_n) ==
      CNET_SCTP_DATA_OK);
  assert(response_n == 0u && response[0] == 0xCCu &&
      memcmp(&a, &before, sizeof(before)) == 0);
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

/* Deterministic network adversary: lose two lower TSNs, deliver three
 * higher TSNs out of order, duplicate each DATA and replay its SACK.
 * The expected Gap/duplicate fields are authored independently of the
 * receiver's SACK builder; repeated evidence must not count toward Fast
 * Retransmit or postpone T3. Then recover both losses and deliver once. */
static void reorder_duplicate_sacks_require_distinct_loss_evidence(void) {
  cnet_sctp_data a, b;
  test_owner oa = {0}, ob = {0};
  uint8_t packets[5][1200], sack[1200], reply[1200];
  uint8_t retransmit0[1200], retransmit1[1200], message[1024];
  const uint8_t payloads[5] = {'a', 'b', 'c', 'd', 'e'};
  cnet_sctp_data_message meta = {0};
  cnet_sctp_wire_chunk ch = {0};
  size_t np[5] = {0}, ns = 0u, nr = 0u;
  size_t nret0 = 0u, nret1 = 0u, nm = 0u;
  uint64_t deadline = 0u;
  int has_deadline = 0;
  unsigned int i, stage;

  pair_init(&a, &b, &oa, &ob, 100u, 300u);
  for (i = 0u; i < 5u; ++i) {
    assert(cnet_sctp_data_send(&a, 1u + i, 0u, 51u, 0,
        payloads + i, 1u, packets[i], sizeof(packets[i]), &np[i]) ==
        CNET_SCTP_DATA_OK);
    inspect(packets[i], np[i], 0u, b.config.local_tag, &ch);
    assert(rd32(ch.value) == 100u + i);
  }
  assert(cnet_sctp_data_deadline(&a, &deadline, &has_deadline) ==
      CNET_SCTP_DATA_OK);
  assert(has_deadline && deadline == 101u);

  /* TSNs 100 and 101 are lost on the wire. Only NEW Gap ACKs of
   * 102, 103 and 104 supply the three distinct missing indications. */
  for (stage = 0u; stage < 3u; ++stage) {
    const unsigned int index = 2u + stage;
    const uint64_t received_at = 10u + stage * 10u;
    const uint32_t expected_tsn = 102u + stage;
    const uint16_t expected_gap_end = (uint16_t)(3u + stage);
    const uint32_t expected_mask =
        (UINT32_C(1) << (stage + 3u)) - 4u;
    const uint32_t expected_window =
        (uint32_t)((CNET_SCTP_DATA_RX_SLOTS - (stage + 1u)) *
                   CNET_SCTP_DATA_PAYLOAD_MAX);

    assert(cnet_sctp_data_input(&b, received_at, packets[index], np[index],
        sack, sizeof(sack), &ns) == CNET_SCTP_DATA_OK);
    inspect(sack, ns, 3u, a.config.local_tag, &ch);
    assert(ch.value_size == 16u && rd32(ch.value) == 99u);
    assert(rd32(ch.value + 4u) == expected_window);
    assert(rd16(ch.value + 8u) == 1u && rd16(ch.value + 10u) == 0u);
    assert(rd16(ch.value + 12u) == 3u &&
        rd16(ch.value + 14u) == expected_gap_end);
    assert(b.cumulative_rx == 99u && b.rx_gaps == expected_mask);
    assert(b.buffered_rx == stage + 1u);

    memset(reply, 0xCC, sizeof(reply));
    assert(cnet_sctp_data_input(&a, received_at + 1u, sack, ns,
        reply, sizeof(reply), &nr) == CNET_SCTP_DATA_OK);
    assert(nr == 0u && reply[0] == 0xCCu);
    assert(a.tx[0].missing_reports == stage + 1u);
    assert(a.tx[1].missing_reports == stage + 1u);
    assert(a.in_flight == 4u - stage);

    /* A duplicate DATA also owes a duplicate SACK. If its response
     * cannot fit, input and advertised window must remain uncommitted;
     * the same clock tick and packet can then be retried without loss. */
    {
      uint8_t tiny[1] = {0xCCu};
      size_t refused = 123u;
      uint64_t before = b.last_time_ms;
      assert(cnet_sctp_data_input(&b, received_at + 2u,
          packets[index], np[index], tiny, sizeof(tiny), &refused) ==
          CNET_SCTP_DATA_CAPACITY);
      assert(refused == 0u && tiny[0] == 0xCCu);
      assert(b.last_time_ms == before);
      assert(b.cumulative_rx == 99u && b.rx_gaps == expected_mask);
      assert(b.buffered_rx == stage + 1u);
    }
    assert(cnet_sctp_data_input(&b, received_at + 2u,
        packets[index], np[index], sack, sizeof(sack), &ns) ==
        CNET_SCTP_DATA_OK);
    inspect(sack, ns, 3u, a.config.local_tag, &ch);
    assert(ch.value_size == 20u && rd32(ch.value) == 99u);
    assert(rd32(ch.value + 4u) == expected_window);
    assert(rd16(ch.value + 8u) == 1u && rd16(ch.value + 10u) == 1u);
    assert(rd16(ch.value + 12u) == 3u &&
        rd16(ch.value + 14u) == expected_gap_end);
    assert(rd32(ch.value + 16u) == expected_tsn);
    assert(b.cumulative_rx == 99u && b.rx_gaps == expected_mask);
    assert(b.buffered_rx == stage + 1u);

    /* Deliver the duplicate report AND replay the identical SACK.
     * Neither arrival can manufacture another missing indication. */
    for (i = 0u; i < 2u; ++i) {
      memset(reply, 0xCC, sizeof(reply));
      assert(cnet_sctp_data_input(&a, received_at + 3u + i, sack, ns,
          reply, sizeof(reply), &nr) == CNET_SCTP_DATA_OK);
      assert(nr == 0u && reply[0] == 0xCCu);
      assert(a.tx[0].missing_reports == stage + 1u);
      assert(a.tx[1].missing_reports == stage + 1u);
      assert(a.in_flight == 4u - stage);
      assert(a.t3_deadline_ms == 101u);
      assert(!a.recovery_active);
      assert(a.tx[0].pending_retransmit == (stage == 2u));
      assert(a.tx[1].pending_retransmit == (stage == 2u));
    }
  }
  /* Three distinct higher ACKs permit Owner-pulled fast retransmission
   * before T3; replays did not reset that original 101ms timer. */
  assert(cnet_sctp_data_deadline(&a, &deadline, &has_deadline) ==
      CNET_SCTP_DATA_OK);
  assert(has_deadline && deadline == a.last_time_ms);
  assert(cnet_sctp_data_advance(&a, 35u, retransmit0,
      sizeof(retransmit0), &nret0) == CNET_SCTP_DATA_OK);
  assert(nret0 == np[0] &&
      memcmp(retransmit0, packets[0], nret0) == 0);
  assert(a.recovery_active && a.recovery_exit_tsn == 104u);
  assert(a.t3_deadline_ms == 135u);
  assert(cnet_sctp_data_advance(&a, 36u, retransmit1,
      sizeof(retransmit1), &nret1) == CNET_SCTP_DATA_OK);
  assert(nret1 == np[1] &&
      memcmp(retransmit1, packets[1], nret1) == 0);
  assert(a.t3_deadline_ms == 135u); /* later loss does not restart T3 */
  assert(cnet_sctp_data_advance(&a, 37u, reply, sizeof(reply), &nr) ==
      CNET_SCTP_DATA_OK);
  assert(nr == 0u && a.t3_deadline_ms == 135u);

  /* Fill TSN 100, then 101. The receiver must emit a narrower Gap ACK,
   * then one cumulative acknowledgement of all five unique DATA TSNs. */
  assert(cnet_sctp_data_input(&b, 40u, retransmit0, nret0,
      sack, sizeof(sack), &ns) == CNET_SCTP_DATA_OK);
  inspect(sack, ns, 3u, a.config.local_tag, &ch);
  assert(ch.value_size == 16u && rd32(ch.value) == 100u);
  assert(rd16(ch.value + 8u) == 1u && rd16(ch.value + 10u) == 0u);
  assert(rd16(ch.value + 12u) == 2u && rd16(ch.value + 14u) == 4u);
  assert(cnet_sctp_data_input(&a, 41u, sack, ns, reply,
      sizeof(reply), &nr) == CNET_SCTP_DATA_OK);
  assert(nr == 0u && !a.tx[0].used && a.tx[1].used);

  assert(cnet_sctp_data_input(&b, 42u, retransmit1, nret1,
      sack, sizeof(sack), &ns) == CNET_SCTP_DATA_OK);
  inspect(sack, ns, 3u, a.config.local_tag, &ch);
  assert(ch.value_size == 12u && rd32(ch.value) == 104u);
  assert(rd16(ch.value + 8u) == 0u && rd16(ch.value + 10u) == 0u);
  assert(cnet_sctp_data_input(&a, 43u, sack, ns, reply,
      sizeof(reply), &nr) == CNET_SCTP_DATA_OK);
  assert(nr == 0u && a.last_ack == 104u && a.in_flight == 0u);
  assert(a.t3_deadline_ms == 0u && !a.recovery_active);
  assert(cnet_sctp_data_outbound_drained(&a));
  assert(b.cumulative_rx == 104u && b.rx_gaps == 0u);
  assert(b.buffered_rx == 5u);

  for (i = 0u; i < 5u; ++i) {
    assert(cnet_sctp_data_take(&b, &meta, message, sizeof(message), &nm) ==
        CNET_SCTP_DATA_OK);
    assert(meta.tsn == 100u + i && meta.stream_sequence == (uint16_t)i);
    assert(meta.stream_id == 0u && meta.ppid == 51u && !meta.unordered);
    assert(nm == 1u && message[0] == payloads[i]);
  }
  assert(b.buffered_rx == 0u);
  /* Late duplicates after complete delivery can only produce duplicate
   * SACKs; they must not redeliver to the application or restart T3. */
  assert(cnet_sctp_data_input(&b, 50u, packets[2], np[2],
      sack, sizeof(sack), &ns) == CNET_SCTP_DATA_OK);
  inspect(sack, ns, 3u, a.config.local_tag, &ch);
  assert(ch.value_size == 16u && rd32(ch.value) == 104u);
  assert(rd16(ch.value + 8u) == 0u && rd16(ch.value + 10u) == 1u);
  assert(rd32(ch.value + 12u) == 102u);
  assert(cnet_sctp_data_input(&a, 51u, sack, ns, reply,
      sizeof(reply), &nr) == CNET_SCTP_DATA_OK);
  assert(nr == 0u && a.in_flight == 0u && a.t3_deadline_ms == 0u);
  assert(cnet_sctp_data_take(&b, &meta, message, sizeof(message), &nm) ==
      CNET_SCTP_DATA_EMPTY);

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

/* A SACK's Duplicate TSN list is bounded by the encoded Chunk length, not
 * by the number of DATA chunks this endpoint can buffer concurrently. */
static void sack_duplicate_list_can_exceed_rx_slots(void) {
  cnet_sctp_data a, b;
  test_owner oa = {0}, ob = {0};
  const uint8_t payload = 'd';
  uint8_t data_packet[1200], body[12u + 4u * (CNET_SCTP_DATA_RX_SLOTS + 1u)];
  uint8_t sack_packet[256], reply[1200];
  cnet_sctp_wire_chunk sack = {0};
  size_t data_size = 0u, sack_size = 0u, reply_size = 123u, i;

  pair_init(&a, &b, &oa, &ob, 100u, 300u);
  assert(cnet_sctp_data_send(&a, 1u, 0u, 51u, 0, &payload, 1u,
      data_packet, sizeof(data_packet), &data_size) == CNET_SCTP_DATA_OK);
  assert(data_size != 0u && a.tx[0].used && a.tx[0].tsn == 100u);

  memset(body, 0, sizeof(body));
  sack_test_be32(body, 99u);       /* no cumulative acknowledgement */
  sack_test_be32(body + 4u, 32768u);
  sack_test_be16(body + 8u, 0u);  /* no Gap Ack Blocks */
  sack_test_be16(body + 10u,
      (uint16_t)(CNET_SCTP_DATA_RX_SLOTS + 1u));
  for (i = 0u; i < CNET_SCTP_DATA_RX_SLOTS + 1u; ++i)
    sack_test_be32(body + 12u + 4u * i, 100u); /* repeated duplicate TSN */

  sack.type = 3u;
  sack.flags = 0xA5u; /* RFC 9260: SACK flags are ignored on receipt. */
  sack.value = body;
  sack.value_size = sizeof(body);
  assert(cnet_sctp_wire_encode(b.config.local_port, b.config.remote_port,
      a.config.local_tag, &sack, 1u, sack_packet, sizeof(sack_packet),
      &sack_size) == CNET_SCTP_WIRE_OK);
  assert(cnet_sctp_data_input(&a, 2u, sack_packet, sack_size, reply,
      sizeof(reply), &reply_size) == CNET_SCTP_DATA_OK);
  assert(reply_size == 0u && a.last_time_ms == 2u);
  assert(a.last_ack == 99u && a.peer_rwnd == 32768u);
  assert(a.in_flight == 1u && a.tx[0].used && a.tx[0].tsn == 100u);
  assert(a.t3_deadline_ms == 101u); /* a duplicate report does not restart T3 */

  assert(cnet_sctp_data_stop(&a) == CNET_SCTP_DATA_OK);
  assert(cnet_sctp_data_stop(&b) == CNET_SCTP_DATA_OK);
}

int main(void) {
  data_reserved_i_and_unordered_ssn_are_ignored();
  hand_authored_sack_rejections_preserve_t3();
  sack_duplicate_list_can_exceed_rx_slots();
  reorder_duplicate_sacks_require_distinct_loss_evidence();
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
