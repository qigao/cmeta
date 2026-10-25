#include "sctp_data_internal.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <string.h>

static int owner(void *context) {
  return *(const int *)context;
}

static cnet_sctp_data_config config(uint16_t local_port,
    uint16_t remote_port, uint32_t local_tag, uint32_t peer_tag,
    uint32_t local_tsn, uint32_t peer_tsn, int *owned, int forward_tsn) {
  cnet_sctp_data_config c = {0};
  c.local_port = local_port;
  c.remote_port = remote_port;
  c.local_tag = local_tag;
  c.peer_tag = peer_tag;
  c.local_initial_tsn = local_tsn;
  c.peer_initial_tsn = peer_tsn;
  c.outbound_streams = 4u;
  c.inbound_streams = 4u;
  c.peer_receive_window = 8192u;
  c.t3_initial_ms = 100u;
  c.t3_max_ms = 800u;
  c.max_retransmits = 3u;
  c.forward_tsn_supported = (uint8_t)forward_tsn;
  c.is_owner = owner;
  c.owner_context = owned;
  return c;
}

static cnet_sctp_wire_chunk one_chunk(const uint8_t *packet, size_t size) {
  cnet_sctp_wire_header header = {0};
  cnet_sctp_wire_decoder decoder = {0};
  cnet_sctp_wire_chunk chunk = {0};
  assert(cnet_sctp_wire_decode(packet, size, &header, &decoder) ==
      CNET_SCTP_WIRE_OK);
  assert(header.chunk_count == 1u);
  assert(cnet_sctp_wire_next_chunk(&decoder, &chunk) == CNET_SCTP_WIRE_OK);
  return chunk;
}

static size_t encode_forward(const cnet_sctp_data_config *receiver,
    const uint8_t *value, size_t value_size, uint8_t *packet, size_t cap) {
  cnet_sctp_wire_chunk chunk = {0};
  size_t written = 0u;
  chunk.type = 192u;
  chunk.value = value;
  chunk.value_size = value_size;
  assert(cnet_sctp_wire_encode(receiver->remote_port, receiver->local_port,
      receiver->local_tag, &chunk, 1u, packet, cap, &written) ==
      CNET_SCTP_WIRE_OK);
  return written;
}

static void timed_and_retransmit_abandonment(void) {
  int owned = 1;
  cnet_sctp_data a = {0}, b = {0};
  cnet_sctp_data_config ac = config(5000u, 5001u, 11u, 22u,
      100u, 200u, &owned, 1);
  cnet_sctp_data_config bc = config(5001u, 5000u, 22u, 11u,
      200u, 100u, &owned, 1);
  cnet_sctp_data_reliability timed = {
    CNET_SCTP_DATA_RELIABILITY_TIMED, 5u
  };
  cnet_sctp_data_reliability no_retry = {
    CNET_SCTP_DATA_RELIABILITY_RETRANSMITS, 0u
  };
  cnet_sctp_data_message message = {0};
  uint8_t data[1200], forward[1200], first_forward[1200], sack[1200],
      payload[16];
  size_t data_n = 0u, forward_n = 0u, sack_n = 0u, payload_n = 0u;
  uint64_t deadline = 0u;
  int has_deadline = 0;
  cnet_sctp_wire_chunk chunk;

  assert(cnet_sctp_data_init(&a, &ac, 0u) == CNET_SCTP_DATA_OK);
  assert(cnet_sctp_data_init(&b, &bc, 0u) == CNET_SCTP_DATA_OK);
  assert(cnet_sctp_data_send_pr(&a, 1u, 1u, 53u, 0, &timed,
      "lost", 4u, data, sizeof(data), &data_n) == CNET_SCTP_DATA_OK);
  assert(cnet_sctp_data_deadline(&a, &deadline, &has_deadline) ==
      CNET_SCTP_DATA_OK && has_deadline && deadline == 6u);
  assert(cnet_sctp_data_advance(&a, 6u, forward, 8u, &forward_n) ==
      CNET_SCTP_DATA_CAPACITY);
  assert(forward_n == 0u && a.tx[0].used && !a.tx[0].abandoned);
  assert(cnet_sctp_data_advance(&a, 6u, forward, sizeof(forward),
      &forward_n) == CNET_SCTP_DATA_OK);
  chunk = one_chunk(forward, forward_n);
  assert(chunk.type == 192u && chunk.value_size == 8u);
  assert(chunk.value[3] == 100u && chunk.value[5] == 1u &&
      chunk.value[7] == 0u);
  memcpy(first_forward, forward, forward_n);
  assert(cnet_sctp_data_deadline(&a, &deadline, &has_deadline) ==
      CNET_SCTP_DATA_OK && has_deadline && deadline == 106u);
  assert(cnet_sctp_data_advance(&a, 106u, forward, 8u, &forward_n) ==
      CNET_SCTP_DATA_CAPACITY);
  assert(forward_n == 0u && a.forward_retransmits == 0u);
  assert(cnet_sctp_data_advance(&a, 106u, forward, sizeof(forward),
      &forward_n) == CNET_SCTP_DATA_OK);
  assert(forward_n != 0u && memcmp(first_forward, forward, forward_n) == 0 &&
      a.forward_retransmits == 1u);
  assert(cnet_sctp_data_input(&b, 107u, first_forward, forward_n,
      sack, sizeof(sack), &sack_n) == CNET_SCTP_DATA_OK);
  assert(cnet_sctp_data_take(&b, &message, payload, sizeof(payload),
      &payload_n) == CNET_SCTP_DATA_EMPTY);
  assert(cnet_sctp_data_input(&b, 108u, forward, forward_n,
      data, sizeof(data), &data_n) == CNET_SCTP_DATA_OK);
  assert(b.cumulative_rx == 100u && b.rx_ssn[1] == 1u);
  assert(cnet_sctp_data_input(&a, 109u, sack, sack_n,
      forward, sizeof(forward), &forward_n) == CNET_SCTP_DATA_OK);
  assert(cnet_sctp_data_outbound_drained(&a));

  assert(cnet_sctp_data_send_pr(&a, 110u, 2u, 53u, 1, &no_retry,
      "drop", 4u, data, sizeof(data), &data_n) == CNET_SCTP_DATA_OK);
  assert(cnet_sctp_data_advance(&a, 210u, forward, sizeof(forward),
      &forward_n) == CNET_SCTP_DATA_OK);
  chunk = one_chunk(forward, forward_n);
  assert(chunk.type == 192u && chunk.value_size == 4u); /* unordered */
  assert(a.cwnd == CNET_SCTP_DATA_MTU && a.rto_ms == 200u);
  assert(cnet_sctp_data_input(&b, 211u, forward, forward_n,
      sack, sizeof(sack), &sack_n) == CNET_SCTP_DATA_OK);
  assert(cnet_sctp_data_input(&a, 212u, sack, sack_n,
      forward, sizeof(forward), &forward_n) == CNET_SCTP_DATA_OK);
  assert(cnet_sctp_data_outbound_drained(&a));
}

static void ordered_gap_releases_received_message(void) {
  int owned = 1;
  cnet_sctp_data a = {0}, b = {0}, before = {0};
  cnet_sctp_data_config ac = config(7000u, 7001u, 55u, 66u,
      100u, 200u, &owned, 1);
  cnet_sctp_data_config bc = config(7001u, 7000u, 66u, 55u,
      200u, 100u, &owned, 1);
  cnet_sctp_data_reliability timed = {
    CNET_SCTP_DATA_RELIABILITY_TIMED, 5u
  };
  cnet_sctp_data_message message = {0};
  uint8_t lost[1200], received[1200], sack[1200], forward[1200],
      payload[16];
  size_t lost_n = 0u, received_n = 0u, sack_n = 0u, forward_n = 0u,
      payload_n = 0u;
  cnet_sctp_wire_chunk chunk;

  assert(cnet_sctp_data_init(&a, &ac, 0u) == CNET_SCTP_DATA_OK);
  assert(cnet_sctp_data_init(&b, &bc, 0u) == CNET_SCTP_DATA_OK);
  assert(cnet_sctp_data_send_pr(&a, 1u, 1u, 53u, 0, &timed,
      "old", 3u, lost, sizeof(lost), &lost_n) == CNET_SCTP_DATA_OK);
  assert(cnet_sctp_data_send(&a, 2u, 1u, 53u, 0, "new", 3u,
      received, sizeof(received), &received_n) == CNET_SCTP_DATA_OK);
  assert(cnet_sctp_data_input(&b, 3u, received, received_n,
      sack, sizeof(sack), &sack_n) == CNET_SCTP_DATA_OK);
  assert(cnet_sctp_data_take(&b, &message, payload, sizeof(payload),
      &payload_n) == CNET_SCTP_DATA_EMPTY);
  assert(cnet_sctp_data_input(&a, 4u, sack, sack_n,
      forward, sizeof(forward), &forward_n) == CNET_SCTP_DATA_OK);
  assert(cnet_sctp_data_advance(&a, 6u, forward, sizeof(forward),
      &forward_n) == CNET_SCTP_DATA_OK);
  chunk = one_chunk(forward, forward_n);
  assert(chunk.type == 192u && chunk.value_size == 8u &&
      chunk.value[3] == 101u && chunk.value[5] == 1u &&
      chunk.value[7] == 0u);
  memcpy(&before, &b, sizeof(before));
  assert(cnet_sctp_data_input(&b, 7u, forward, forward_n,
      sack, 8u, &sack_n) == CNET_SCTP_DATA_CAPACITY);
  assert(sack_n == 0u && memcmp(&b, &before, sizeof(b)) == 0);
  assert(cnet_sctp_data_input(&b, 7u, forward, forward_n,
      sack, sizeof(sack), &sack_n) == CNET_SCTP_DATA_OK);
  assert(b.cumulative_rx == 101u && b.rx_gaps == 0u &&
      b.buffered_rx == 3u && b.rx_ssn[1] == 1u);
  assert(cnet_sctp_data_take(&b, &message, payload, sizeof(payload),
      &payload_n) == CNET_SCTP_DATA_OK);
  assert(payload_n == 3u && memcmp(payload, "new", 3u) == 0 &&
      message.stream_sequence == 1u && b.rx_ssn[1] == 2u);
}

static void reliable_predecessor_does_not_resurrect_abandoned_data(void) {
  int owned = 1;
  cnet_sctp_data a = {0}, b = {0};
  cnet_sctp_data_config ac = config(7100u, 7101u, 56u, 67u,
      100u, 200u, &owned, 1);
  cnet_sctp_data_config bc = config(7101u, 7100u, 67u, 56u,
      200u, 100u, &owned, 1);
  cnet_sctp_data_reliability timed = {
    CNET_SCTP_DATA_RELIABILITY_TIMED, 5u
  };
  uint8_t reliable[1200], partial[1200], sack[1200], forward[1200];
  size_t reliable_n = 0u, partial_n = 0u, sack_n = 0u, forward_n = 0u;
  uint64_t deadline = 0u;
  int has_deadline = 0;
  cnet_sctp_wire_chunk chunk;

  assert(cnet_sctp_data_init(&a, &ac, 0u) == CNET_SCTP_DATA_OK);
  assert(cnet_sctp_data_init(&b, &bc, 0u) == CNET_SCTP_DATA_OK);
  assert(cnet_sctp_data_send(&a, 1u, 0u, 53u, 0, "keep", 4u,
      reliable, sizeof(reliable), &reliable_n) == CNET_SCTP_DATA_OK);
  assert(cnet_sctp_data_send_pr(&a, 2u, 1u, 53u, 0, &timed,
      "drop", 4u, partial, sizeof(partial), &partial_n) ==
      CNET_SCTP_DATA_OK);
  assert(cnet_sctp_data_advance(&a, 7u, forward, sizeof(forward),
      &forward_n) == CNET_SCTP_DATA_OK);
  assert(forward_n == 0u && a.tx[1].used && a.tx[1].abandoned &&
      a.in_flight == 4u);
  assert(cnet_sctp_data_input(&b, 8u, reliable, reliable_n,
      sack, sizeof(sack), &sack_n) == CNET_SCTP_DATA_OK);
  assert(cnet_sctp_data_input(&a, 9u, sack, sack_n,
      forward, sizeof(forward), &forward_n) == CNET_SCTP_DATA_OK);
  assert(a.tx[1].used && a.tx[1].abandoned &&
      !a.tx[1].flight_accounted && a.in_flight == 0u);
  assert(cnet_sctp_data_deadline(&a, &deadline, &has_deadline) ==
      CNET_SCTP_DATA_OK && has_deadline && deadline == 9u);
  assert(cnet_sctp_data_advance(&a, 9u, forward, sizeof(forward),
      &forward_n) == CNET_SCTP_DATA_OK);
  chunk = one_chunk(forward, forward_n);
  assert(chunk.type == 192u && chunk.value_size == 8u &&
      chunk.value[3] == 101u);
  assert(cnet_sctp_data_input(&b, 10u, forward, forward_n,
      sack, sizeof(sack), &sack_n) == CNET_SCTP_DATA_OK);
  assert(cnet_sctp_data_input(&a, 11u, sack, sack_n,
      partial, sizeof(partial), &partial_n) == CNET_SCTP_DATA_OK);
  assert(cnet_sctp_data_outbound_drained(&a));
}

static void received_abandoned_sequence_remains_deliverable(void) {
  int owned = 1;
  cnet_sctp_data a = {0}, b = {0};
  cnet_sctp_data_config ac = config(7200u, 7201u, 57u, 68u,
      100u, 200u, &owned, 1);
  cnet_sctp_data_config bc = config(7201u, 7200u, 68u, 57u,
      200u, 100u, &owned, 1);
  cnet_sctp_data_reliability timed = {
    CNET_SCTP_DATA_RELIABILITY_TIMED, 5u
  };
  cnet_sctp_data_message message = {0};
  uint8_t first[1200], second[1200], sack[1200], forward[1200], payload[16];
  size_t first_n = 0u, second_n = 0u, sack_n = 0u, forward_n = 0u,
      payload_n = 0u;
  cnet_sctp_wire_chunk chunk;

  assert(cnet_sctp_data_init(&a, &ac, 0u) == CNET_SCTP_DATA_OK);
  assert(cnet_sctp_data_init(&b, &bc, 0u) == CNET_SCTP_DATA_OK);
  assert(cnet_sctp_data_send_pr(&a, 1u, 2u, 53u, 0, &timed,
      "missing", 7u, first, sizeof(first), &first_n) == CNET_SCTP_DATA_OK);
  assert(cnet_sctp_data_send_pr(&a, 2u, 2u, 53u, 0, &timed,
      "present", 7u, second, sizeof(second), &second_n) == CNET_SCTP_DATA_OK);
  assert(cnet_sctp_data_input(&b, 3u, second, second_n,
      sack, sizeof(sack), &sack_n) == CNET_SCTP_DATA_OK);
  /* Drop the Gap SACK. The sender is therefore allowed to abandon both
   * messages, while the receiver must retain the complete second message. */
  assert(cnet_sctp_data_advance(&a, 7u, forward, sizeof(forward),
      &forward_n) == CNET_SCTP_DATA_OK);
  chunk = one_chunk(forward, forward_n);
  assert(chunk.type == 192u && chunk.value_size == 8u &&
      chunk.value[3] == 101u && chunk.value[5] == 2u &&
      chunk.value[7] == 1u);
  assert(cnet_sctp_data_input(&b, 8u, forward, forward_n,
      sack, sizeof(sack), &sack_n) == CNET_SCTP_DATA_OK);
  assert(b.buffered_rx == 7u && b.rx_ssn[2] == 1u &&
      (b.rx_forward_streams & (UINT16_C(1) << 2u)) != 0u);
  assert(cnet_sctp_data_take(&b, &message, payload, sizeof(payload),
      &payload_n) == CNET_SCTP_DATA_OK);
  assert(payload_n == 7u && memcmp(payload, "present", 7u) == 0 &&
      message.stream_sequence == 1u && b.rx_ssn[2] == 2u &&
      (b.rx_forward_streams & (UINT16_C(1) << 2u)) == 0u);
}

static void negotiation_is_mandatory(void) {
  int owned = 1;
  cnet_sctp_data d = {0};
  cnet_sctp_data_config c = config(6000u, 6001u, 33u, 44u,
      1u, 9u, &owned, 0);
  cnet_sctp_data_reliability timed = {
    CNET_SCTP_DATA_RELIABILITY_TIMED, 1u
  };
  cnet_sctp_wire_chunk chunk = {0};
  uint8_t value[4] = {0, 0, 0, 9};
  uint8_t packet[1200], output[1200];
  size_t packet_n = 0u, output_n = 0u;
  assert(cnet_sctp_data_init(&d, &c, 0u) == CNET_SCTP_DATA_OK);
  assert(cnet_sctp_data_send_pr(&d, 1u, 0u, 53u, 0, &timed,
      "x", 1u, packet, sizeof(packet), &packet_n) ==
      CNET_SCTP_DATA_INVALID);
  chunk.type = 192u;
  chunk.value = value;
  chunk.value_size = sizeof(value);
  assert(cnet_sctp_wire_encode(c.remote_port, c.local_port, c.local_tag,
      &chunk, 1u, packet, sizeof(packet), &packet_n) == CNET_SCTP_WIRE_OK);
  assert(cnet_sctp_data_input(&d, 2u, packet, packet_n,
      output, sizeof(output), &output_n) == CNET_SCTP_DATA_PROTOCOL);
}

static void forward_input_validation_is_atomic(void) {
  int owned = 1;
  cnet_sctp_data d = {0}, before = {0};
  cnet_sctp_data_config c = config(8000u, 8001u, 77u, 88u,
      1u, 9u, &owned, 1);
  uint8_t value[12] = {0, 0, 0, 9, 0, 0, 0, 2, 0, 0, 0, 1};
  uint8_t packet[1200], output[1200];
  size_t packet_n, output_n = 0u;

  assert(cnet_sctp_data_init(&d, &c, 0u) == CNET_SCTP_DATA_OK);
  packet_n = encode_forward(&c, value, 5u, packet, sizeof(packet));
  memcpy(&before, &d, sizeof(before));
  assert(cnet_sctp_data_input(&d, 1u, packet, packet_n,
      output, sizeof(output), &output_n) == CNET_SCTP_DATA_PROTOCOL);
  assert(output_n == 0u && memcmp(&d, &before, sizeof(d)) == 0);

  value[3] = 9u;
  value[5] = 4u; /* outside negotiated inbound streams */
  packet_n = encode_forward(&c, value, 8u, packet, sizeof(packet));
  assert(cnet_sctp_data_input(&d, 1u, packet, packet_n,
      output, sizeof(output), &output_n) == CNET_SCTP_DATA_PROTOCOL);
  assert(output_n == 0u && memcmp(&d, &before, sizeof(d)) == 0);

  value[0] = 0x80u;
  value[1] = 0u;
  value[2] = 0u;
  value[3] = 8u; /* exactly half the TSN sequence space from cumulative 8 */
  packet_n = encode_forward(&c, value, 4u, packet, sizeof(packet));
  assert(cnet_sctp_data_input(&d, 1u, packet, packet_n,
      output, sizeof(output), &output_n) == CNET_SCTP_DATA_PROTOCOL);
  assert(output_n == 0u && memcmp(&d, &before, sizeof(d)) == 0);

  value[0] = value[1] = value[2] = 0u;
  value[3] = 9u;
  value[4] = value[5] = 0u;
  value[6] = 0u;
  value[7] = 2u;
  value[8] = value[9] = 0u;
  value[10] = 0u;
  value[11] = 1u;
  packet_n = encode_forward(&c, value, sizeof(value), packet, sizeof(packet));
  assert(cnet_sctp_data_input(&d, 1u, packet, packet_n,
      output, sizeof(output), &output_n) == CNET_SCTP_DATA_OK);
  /* RFC 3758 says a stream SHOULD appear once, so tolerate duplicates and
   * retain the serially highest sequence rather than rejecting the peer. */
  assert(d.cumulative_rx == 9u && d.rx_ssn[0] == 3u);
}

int main(void) {
  timed_and_retransmit_abandonment();
  ordered_gap_releases_received_message();
  reliable_predecessor_does_not_resurrect_abandoned_data();
  received_abandoned_sequence_remains_deliverable();
  negotiation_is_mandatory();
  forward_input_validation_is_atomic();
  puts("C11 RFC3758 PR-SCTP FORWARD-TSN bounded Owner path: PASS");
  return 0;
}
