#include "sctp_data_internal.h"

#ifdef NDEBUG
#undef NDEBUG /* Runtime C11 conformance stays enabled in Release. */
#endif
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct test_owner { int admitted; } test_owner;
static int owned(void *context) { return ((test_owner *)context)->admitted; }

static void initialize_pair(cnet_sctp_data *a, cnet_sctp_data *b,
                            test_owner *oa, test_owner *ob,
                            uint32_t sender_tsn) {
  cnet_sctp_data_config ca = {0}, cb = {0};
  memset(a, 0, sizeof(*a));
  memset(b, 0, sizeof(*b));
  oa->admitted = ob->admitted = 1;
  ca.local_port = 5000u; ca.remote_port = 5001u;
  ca.local_tag = UINT32_C(0x10203040);
  ca.peer_tag = UINT32_C(0x50607080);
  ca.local_initial_tsn = sender_tsn;
  ca.peer_initial_tsn = 700u;
  ca.inbound_streams = ca.outbound_streams = 4u;
  ca.peer_receive_window = CNET_SCTP_DATA_RECEIVE_CEILING;
  ca.t3_initial_ms = 100u; ca.t3_max_ms = 400u;
  ca.max_retransmits = 3u;
  ca.is_owner = owned; ca.owner_context = oa;
  cb = ca;
  cb.local_port = ca.remote_port; cb.remote_port = ca.local_port;
  cb.local_tag = ca.peer_tag; cb.peer_tag = ca.local_tag;
  cb.local_initial_tsn = ca.peer_initial_tsn;
  cb.peer_initial_tsn = ca.local_initial_tsn;
  cb.owner_context = ob;
  assert(cnet_sctp_data_init(a, &ca, 0u) == CNET_SCTP_DATA_OK);
  assert(cnet_sctp_data_init(b, &cb, 0u) == CNET_SCTP_DATA_OK);
}

static uint32_t be32(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
         ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void test_twenty_kib_loss_gap_ack_reassembly(void) {
  enum { MESSAGE_SIZE = 20480, FRAGMENTS = 20 };
  cnet_sctp_data a, b;
  test_owner oa = {0}, ob = {0};
  uint8_t payload[MESSAGE_SIZE], expected[MESSAGE_SIZE], decoded[MESSAGE_SIZE];
  uint8_t packet[CNET_SCTP_DATA_MTU], sack[CNET_SCTP_DATA_MTU];
  uint8_t reply[CNET_SCTP_DATA_MTU];
  cnet_sctp_wire_header header = {0};
  cnet_sctp_wire_decoder decoder = {0};
  cnet_sctp_wire_chunk ch = {0};
  cnet_sctp_data_message message = {0};
  size_t n = 0u, ns = 0u, nr = 0u;
  uint64_t now = 2u, deadline = 0u;
  int has_deadline = 0;
  unsigned int i;
  const uint32_t first_tsn = UINT32_C(0xfffffff8);
  initialize_pair(&a, &b, &oa, &ob, first_tsn);
  for (i = 0u; i < MESSAGE_SIZE; ++i)
    payload[i] = (uint8_t)(i * 37u + 11u);
  memcpy(expected, payload, MESSAGE_SIZE);
  assert(cnet_sctp_data_queue_message(&a, 1u, 0u, 53u, 0,
                                      payload, MESSAGE_SIZE) == CNET_SCTP_DATA_OK);
  memset(payload, 0xff, sizeof(payload)); /* no borrowed app lifetime */
  assert(cnet_sctp_data_queue_message(&a, 1u, 0u, 53u, 0,
                                      expected, MESSAGE_SIZE) == CNET_SCTP_DATA_CAPACITY);
  assert(cnet_sctp_data_pump(&a, 2u, packet, 16u, &n) == CNET_SCTP_DATA_CAPACITY);
  assert(n == 0u && a.pending.offset == 0u && a.next_tsn == first_tsn);
  assert(a.tx_ssn[0] == 0u);
  assert(cnet_sctp_data_pump(&a, 2u, packet, sizeof(packet), &n) == CNET_SCTP_DATA_OK);
  assert(cnet_sctp_wire_decode(packet, n, &header, &decoder) == CNET_SCTP_WIRE_OK);
  assert(cnet_sctp_wire_next_chunk(&decoder, &ch) == CNET_SCTP_WIRE_OK);
  assert(ch.type == 0u && ch.flags == 2u); /* first B=1 E=0 */
  assert(be32(ch.value) == first_tsn);
  /* First fragment lost. Gap ACKs keep owned TX tickets until
   * cumulative ACK; recover the missing first fragment before pumping more. */
  for (i = 1u; i < FRAGMENTS; ++i) {
    now += 3u;
    if (i == 4u) {
      /* Three higher Gap ACKs mark the lost B for Fast Retransmit.
       * Owner advance() must transmit the retained TSN first. */
      const size_t offset = a.pending.offset;
      assert(cnet_sctp_data_pump(&a, now, packet,
          sizeof(packet), &n) == CNET_SCTP_DATA_RETRANSMIT_PENDING);
      assert(n == 0u && offset == i * CNET_SCTP_DATA_PAYLOAD_MAX);
      assert(cnet_sctp_data_deadline(&a, &deadline, &has_deadline) ==
             CNET_SCTP_DATA_OK);
      assert(has_deadline && deadline <= now);
      assert(cnet_sctp_data_advance(&a, now, packet,
          sizeof(packet), &n) == CNET_SCTP_DATA_OK);
      assert(cnet_sctp_wire_decode(packet, n, &header, &decoder) ==
             CNET_SCTP_WIRE_OK);
      assert(cnet_sctp_wire_next_chunk(&decoder, &ch) == CNET_SCTP_WIRE_OK);
      assert(ch.flags == 2u && be32(ch.value) == first_tsn);
      assert(cnet_sctp_data_input(&b, now + 1u, packet, n,
          sack, sizeof(sack), &ns) == CNET_SCTP_DATA_OK);
      assert(cnet_sctp_data_input(&a, now + 2u, sack, ns,
          reply, sizeof(reply), &nr) == CNET_SCTP_DATA_OK);
      assert(nr == 0u && a.in_flight == 0u && a.pending.offset == offset);
      now += 3u;
    }
    assert(cnet_sctp_data_pump(&a, now, packet,
                               sizeof(packet), &n) == CNET_SCTP_DATA_OK);
    assert(cnet_sctp_wire_decode(packet, n, &header, &decoder) == CNET_SCTP_WIRE_OK);
    assert(cnet_sctp_wire_next_chunk(&decoder, &ch) == CNET_SCTP_WIRE_OK);
    assert(ch.type == 0u && ch.value_size == 1036u);
    assert(be32(ch.value) == first_tsn + i);
    assert(((ch.flags & 2u) != 0u) == 0);
    assert(((ch.flags & 1u) != 0u) == (i == FRAGMENTS - 1u));
    assert(((ch.flags & 4u) != 0u) == 0);
    assert(a.tx_ssn[0] == (i == FRAGMENTS - 1u ? 1u : 0u));
    assert(cnet_sctp_data_input(&b, now + 1u, packet, n,
                                sack, sizeof(sack), &ns) == CNET_SCTP_DATA_OK);
    assert(cnet_sctp_data_input(&a, now + 2u, sack, ns,
                                reply, sizeof(reply), &nr) == CNET_SCTP_DATA_OK);
    assert(nr == 0u);
    if (i + 1u < FRAGMENTS)
      assert(cnet_sctp_data_take(&b, &message, decoded,
                                 sizeof(decoded), &nr) == CNET_SCTP_DATA_EMPTY);
  }
  assert(!a.pending.active && a.next_tsn == first_tsn + FRAGMENTS);
  assert(cnet_sctp_data_pump(&a, now + 3u, packet, sizeof(packet), &n) ==
         CNET_SCTP_DATA_EMPTY);
  assert(n == 0u && a.in_flight == 0u);
  assert(b.buffered_rx == MESSAGE_SIZE);
  assert(cnet_sctp_data_deadline(&a, &deadline, &has_deadline) == CNET_SCTP_DATA_OK);
  assert(!has_deadline);
  assert(cnet_sctp_data_take(&b, &message, decoded, 1024u, &nr) ==
         CNET_SCTP_DATA_CAPACITY);
  assert(b.buffered_rx == MESSAGE_SIZE && nr == 0u);
  assert(cnet_sctp_data_take(&b, &message, decoded,
                             sizeof(decoded), &nr) == CNET_SCTP_DATA_OK);
  assert(nr == MESSAGE_SIZE && message.size == MESSAGE_SIZE);
  assert(message.stream_id == 0u && message.stream_sequence == 0u &&
         message.ppid == 53u && message.tsn == first_tsn &&
         !message.unordered);
  assert(memcmp(decoded, expected, MESSAGE_SIZE) == 0);
  assert(b.buffered_rx == 0u && b.rx_ssn[0] == 1u);
  assert(a.in_flight == 0u && a.t3_deadline_ms == 0u);
  assert(cnet_sctp_data_stop(&a) == CNET_SCTP_DATA_OK);
  assert(cnet_sctp_data_stop(&b) == CNET_SCTP_DATA_OK);
}

static void test_bounded_queue_and_unordered_fragments(void) {
  cnet_sctp_data a, b;
  test_owner oa = {0}, ob = {0};
  uint8_t payload[3072], wire[CNET_SCTP_DATA_MTU], sack[CNET_SCTP_DATA_MTU];
  uint8_t event[3072], response[CNET_SCTP_DATA_MTU];
  cnet_sctp_data_message meta = {0};
  size_t n = 0u, ns = 0u, nr = 0u;
  unsigned int i;
  initialize_pair(&a, &b, &oa, &ob, 100u);
  for (i = 0u; i < sizeof(payload); ++i) payload[i] = (uint8_t)i;
  assert(cnet_sctp_data_queue_message(&a, 1u, 1u, 53u, 1,
      payload, sizeof(payload)) == CNET_SCTP_DATA_OK);
  assert(cnet_sctp_data_send(&a, 2u, 0u, 51u, 0, payload, 1u,
      wire, sizeof(wire), &n) == CNET_SCTP_DATA_CAPACITY);
  assert(cnet_sctp_data_queue_message(&a, 2u, 1u, 53u, 1,
      payload, sizeof(payload)) == CNET_SCTP_DATA_CAPACITY);
  for (i = 0u; i < 3u; ++i) {
    assert(cnet_sctp_data_pump(&a, 2u + i, wire, sizeof(wire), &n) ==
           CNET_SCTP_DATA_OK);
    assert(cnet_sctp_data_input(&b, 2u + i, wire, n,
                                sack, sizeof(sack), &ns) == CNET_SCTP_DATA_OK);
    assert(cnet_sctp_data_input(&a, 2u + i, sack, ns,
                                response, sizeof(response), &nr) == CNET_SCTP_DATA_OK);
  }
  assert(!a.pending.active && a.tx_ssn[1] == 0u);
  assert(cnet_sctp_data_take(&b, &meta, event, sizeof(event), &nr) == CNET_SCTP_DATA_OK);
  assert(nr == sizeof(payload) && meta.unordered && meta.stream_id == 1u &&
         meta.ppid == 53u && meta.stream_sequence == 0u);
  assert(memcmp(event, payload, sizeof(payload)) == 0);
  assert(cnet_sctp_data_stop(&a) == CNET_SCTP_DATA_OK);
  assert(cnet_sctp_data_stop(&b) == CNET_SCTP_DATA_OK);
}

int main(void) {
  test_twenty_kib_loss_gap_ack_reassembly();
  test_bounded_queue_and_unordered_fragments();
  puts("C11 SCTP 20KiB DATA B/E TSN reassembly, T3 and backpressure PASS");
  return 0;
}
