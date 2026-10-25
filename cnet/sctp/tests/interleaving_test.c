#include "sctp_data_internal.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <string.h>

enum { I_DATA_B = 2, I_DATA_E = 1, I_DATA_U = 4 };

static int owner(void *context) {
  return *(const int *)context;
}

static cnet_sctp_data_config config(uint16_t local_port,
    uint16_t remote_port, uint32_t local_tag, uint32_t peer_tag,
    uint32_t local_tsn, uint32_t peer_tsn, int *owned) {
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
  c.forward_tsn_supported = 1u;
  c.interleaving_supported = 1u;
  c.is_owner = owner;
  c.owner_context = owned;
  return c;
}

static void write16(uint8_t *out, uint16_t value) {
  out[0] = (uint8_t)(value >> 8);
  out[1] = (uint8_t)value;
}

static void write32(uint8_t *out, uint32_t value) {
  out[0] = (uint8_t)(value >> 24);
  out[1] = (uint8_t)(value >> 16);
  out[2] = (uint8_t)(value >> 8);
  out[3] = (uint8_t)value;
}

static uint32_t read32(const uint8_t *in) {
  return ((uint32_t)in[0] << 24) | ((uint32_t)in[1] << 16) |
      ((uint32_t)in[2] << 8) | in[3];
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

static size_t encode_i_data(const cnet_sctp_data_config *receiver,
    uint32_t tsn, uint16_t stream, uint32_t mid, uint32_t ppid_or_fsn,
    uint8_t flags, const void *payload, size_t payload_size,
    uint8_t *packet, size_t capacity) {
  uint8_t value[16u + CNET_SCTP_DATA_RX_PAYLOAD_MAX] = {0};
  cnet_sctp_wire_chunk chunk = {0};
  size_t written = 0u;
  assert(payload && payload_size != 0u &&
      payload_size <= CNET_SCTP_DATA_RX_PAYLOAD_MAX);
  write32(value, tsn);
  write16(value + 4u, stream);
  write32(value + 8u, mid);
  write32(value + 12u, ppid_or_fsn);
  memcpy(value + 16u, payload, payload_size);
  chunk.type = 64u;
  chunk.flags = flags;
  chunk.value = value;
  chunk.value_size = 16u + payload_size;
  assert(cnet_sctp_wire_encode(receiver->remote_port, receiver->local_port,
      receiver->local_tag, &chunk, 1u, packet, capacity, &written) ==
      CNET_SCTP_WIRE_OK);
  return written;
}

static void sender_uses_mid_fsn_and_i_data_only(void) {
  int owned = 1;
  cnet_sctp_data sender = {0};
  cnet_sctp_data_config cfg = config(5000u, 5001u, 11u, 22u,
      100u, 200u, &owned);
  uint8_t message[CNET_SCTP_DATA_PAYLOAD_MAX + 1u];
  uint8_t packet[1200];
  size_t written = 0u;
  cnet_sctp_wire_chunk chunk;
  memset(message, 0x5Au, sizeof(message));
  assert(cnet_sctp_data_init(&sender, &cfg, 0u) == CNET_SCTP_DATA_OK);
  assert(cnet_sctp_data_queue_message(&sender, 1u, 2u, 53u, 0,
      message, sizeof(message)) == CNET_SCTP_DATA_OK);
  assert(cnet_sctp_data_pump(&sender, 2u, packet, sizeof(packet),
      &written) == CNET_SCTP_DATA_OK);
  chunk = one_chunk(packet, written);
  assert(chunk.type == 64u && chunk.flags == I_DATA_B &&
      chunk.value_size == 16u + CNET_SCTP_DATA_PAYLOAD_MAX &&
      read32(chunk.value + 8u) == 0u &&
      read32(chunk.value + 12u) == 53u);
  assert(cnet_sctp_data_pump(&sender, 3u, packet, sizeof(packet),
      &written) == CNET_SCTP_DATA_OK);
  chunk = one_chunk(packet, written);
  assert(chunk.type == 64u && chunk.flags == I_DATA_E &&
      chunk.value_size == 17u && read32(chunk.value + 8u) == 0u &&
      read32(chunk.value + 12u) == 1u &&
      sender.tx_mid_ordered[2] == 1u);
}

static void receiver_reassembles_by_fsn_across_interleaved_tsns(void) {
  int owned = 1;
  cnet_sctp_data receiver = {0};
  cnet_sctp_data_config cfg = config(6001u, 6000u, 44u, 33u,
      200u, 100u, &owned);
  cnet_sctp_data_message message = {0};
  uint8_t a0[1200], b[1200], a1[1200], sack[1200], payload[16];
  size_t a0n, bn, a1n, sackn = 0u, payloadn = 0u;
  a0n = encode_i_data(&cfg, 100u, 0u, 0u, 53u, I_DATA_B,
      "left", 4u, a0, sizeof(a0));
  bn = encode_i_data(&cfg, 101u, 1u, 0u, 51u,
      I_DATA_B | I_DATA_E, "middle", 6u, b, sizeof(b));
  a1n = encode_i_data(&cfg, 102u, 0u, 0u, 1u, I_DATA_E,
      "right", 5u, a1, sizeof(a1));
  assert(cnet_sctp_data_init(&receiver, &cfg, 0u) == CNET_SCTP_DATA_OK);
  assert(cnet_sctp_data_input(&receiver, 1u, a0, a0n,
      sack, sizeof(sack), &sackn) == CNET_SCTP_DATA_OK);
  assert(cnet_sctp_data_input(&receiver, 2u, b, bn,
      sack, sizeof(sack), &sackn) == CNET_SCTP_DATA_OK);
  assert(cnet_sctp_data_take(&receiver, &message, payload, sizeof(payload),
      &payloadn) == CNET_SCTP_DATA_OK);
  assert(message.interleaved && message.stream_id == 1u &&
      message.message_identifier == 0u && message.ppid == 51u &&
      payloadn == 6u && memcmp(payload, "middle", 6u) == 0);
  assert(cnet_sctp_data_input(&receiver, 3u, a1, a1n,
      sack, sizeof(sack), &sackn) == CNET_SCTP_DATA_OK);
  assert(cnet_sctp_data_take(&receiver, &message, payload, sizeof(payload),
      &payloadn) == CNET_SCTP_DATA_OK);
  assert(message.stream_id == 0u && message.message_identifier == 0u &&
      message.ppid == 53u && payloadn == 9u &&
      memcmp(payload, "leftright", 9u) == 0);
}

static void receiver_accepts_web_rtc_1280_byte_i_data_fragment(void) {
  int owned = 1;
  cnet_sctp_data receiver = {0};
  cnet_sctp_data_config cfg = config(6501u, 6500u, 46u, 45u,
      200u, 100u, &owned);
  cnet_sctp_data_message message = {0};
  uint8_t expected[1248u], packet[CNET_SCTP_DATA_INGRESS_MTU];
  uint8_t sack[CNET_SCTP_DATA_MTU], payload[1248u];
  size_t packetn, sackn = 0u, payloadn = 0u, i;
  for (i = 0u; i < sizeof(expected); ++i)
    expected[i] = (uint8_t)(i * 17u + 3u);
  packetn = encode_i_data(&cfg, 100u, 0u, 0u, 53u,
      I_DATA_B | I_DATA_E, expected, sizeof(expected),
      packet, sizeof(packet));
  assert(packetn == CNET_SCTP_DATA_INGRESS_MTU);
  assert(cnet_sctp_data_init(&receiver, &cfg, 0u) == CNET_SCTP_DATA_OK);
  assert(cnet_sctp_data_input(&receiver, 1u, packet, packetn,
      sack, sizeof(sack), &sackn) == CNET_SCTP_DATA_OK);
  assert(cnet_sctp_data_take(&receiver, &message, payload, sizeof(payload),
      &payloadn) == CNET_SCTP_DATA_OK);
  assert(message.interleaved && message.ppid == 53u &&
      payloadn == sizeof(expected) &&
      memcmp(payload, expected, sizeof(expected)) == 0);
}

static void partial_reliability_uses_i_forward_tsn(void) {
  int owned = 1;
  cnet_sctp_data sender = {0}, receiver = {0};
  cnet_sctp_data_config sc = config(7000u, 7001u, 55u, 66u,
      100u, 200u, &owned);
  cnet_sctp_data_config rc = config(7001u, 7000u, 66u, 55u,
      200u, 100u, &owned);
  cnet_sctp_data_reliability timed = {
    CNET_SCTP_DATA_RELIABILITY_TIMED, 5u
  };
  uint8_t data[1200], forward[1200], sack[1200];
  size_t datan = 0u, forwardn = 0u, sackn = 0u;
  cnet_sctp_wire_chunk chunk;
  assert(cnet_sctp_data_init(&sender, &sc, 0u) == CNET_SCTP_DATA_OK);
  assert(cnet_sctp_data_init(&receiver, &rc, 0u) == CNET_SCTP_DATA_OK);
  assert(cnet_sctp_data_send_pr(&sender, 1u, 3u, 53u, 1, &timed,
      "lost", 4u, data, sizeof(data), &datan) == CNET_SCTP_DATA_OK);
  chunk = one_chunk(data, datan);
  assert(chunk.type == 64u && (chunk.flags & I_DATA_U) != 0u);
  assert(cnet_sctp_data_advance(&sender, 6u, forward, sizeof(forward),
      &forwardn) == CNET_SCTP_DATA_OK);
  chunk = one_chunk(forward, forwardn);
  assert(chunk.type == 194u && chunk.value_size == 12u &&
      read32(chunk.value) == 100u && chunk.value[5] == 3u &&
      chunk.value[7] == 1u && read32(chunk.value + 8u) == 0u);
  assert(cnet_sctp_data_input(&receiver, 7u, forward, forwardn,
      sack, sizeof(sack), &sackn) == CNET_SCTP_DATA_OK);
  assert(receiver.cumulative_rx == 100u &&
      receiver.rx_mid_unordered_floor[3] == 1u);
}

static void bundled_records_remain_i_data(void) {
  int owned = 1;
  cnet_sctp_data sender = {0};
  cnet_sctp_data_config cfg = config(7500u, 7501u, 75u, 76u,
      300u, 400u, &owned);
  const uint8_t a = 'a', b = 'b';
  cnet_sctp_data_bundle_item items[2] = {
    {0u, 51u, 0, &a, 1u},
    {1u, 53u, 1, &b, 1u}
  };
  cnet_sctp_wire_header header = {0};
  cnet_sctp_wire_decoder decoder = {0};
  cnet_sctp_wire_chunk chunk = {0};
  uint8_t packet[1200];
  size_t written = 0u, i;
  assert(cnet_sctp_data_init(&sender, &cfg, 0u) == CNET_SCTP_DATA_OK);
  assert(cnet_sctp_data_send_bundle(&sender, 1u, items, 2u,
      packet, sizeof(packet), &written) == CNET_SCTP_DATA_OK);
  assert(cnet_sctp_wire_decode(packet, written, &header, &decoder) ==
      CNET_SCTP_WIRE_OK && header.chunk_count == 2u);
  for (i = 0u; i < 2u; ++i) {
    assert(cnet_sctp_wire_next_chunk(&decoder, &chunk) ==
        CNET_SCTP_WIRE_OK && chunk.type == 64u &&
        read32(chunk.value + 8u) == 0u);
  }
  assert(sender.tx_mid_ordered[0] == 1u &&
      sender.tx_mid_unordered[1] == 1u);
}

static void ordered_skip_releases_later_mid(void) {
  int owned = 1;
  cnet_sctp_data sender = {0}, receiver = {0}, before = {0};
  cnet_sctp_data_config sc = config(7600u, 7601u, 81u, 82u,
      100u, 200u, &owned);
  cnet_sctp_data_config rc = config(7601u, 7600u, 82u, 81u,
      200u, 100u, &owned);
  cnet_sctp_data_reliability timed = {
    CNET_SCTP_DATA_RELIABILITY_TIMED, 5u
  };
  cnet_sctp_data_message message = {0};
  cnet_sctp_wire_chunk chunk;
  uint8_t lost[1200], received[1200], sack[1200], forward[1200], body[8];
  size_t lostn = 0u, receivedn = 0u, sackn = 0u, forwardn = 0u, bodyn = 0u;
  assert(cnet_sctp_data_init(&sender, &sc, 0u) == CNET_SCTP_DATA_OK);
  assert(cnet_sctp_data_init(&receiver, &rc, 0u) == CNET_SCTP_DATA_OK);
  assert(cnet_sctp_data_send_pr(&sender, 1u, 1u, 53u, 0, &timed,
      "old", 3u, lost, sizeof(lost), &lostn) == CNET_SCTP_DATA_OK);
  assert(cnet_sctp_data_send(&sender, 2u, 1u, 53u, 0,
      "new", 3u, received, sizeof(received), &receivedn) ==
      CNET_SCTP_DATA_OK);
  assert(cnet_sctp_data_input(&receiver, 3u, received, receivedn,
      sack, sizeof(sack), &sackn) == CNET_SCTP_DATA_OK);
  assert(cnet_sctp_data_take(&receiver, &message, body, sizeof(body),
      &bodyn) == CNET_SCTP_DATA_EMPTY);
  assert(cnet_sctp_data_input(&sender, 4u, sack, sackn,
      forward, sizeof(forward), &forwardn) == CNET_SCTP_DATA_OK);
  assert(cnet_sctp_data_advance(&sender, 6u, forward, sizeof(forward),
      &forwardn) == CNET_SCTP_DATA_OK);
  chunk = one_chunk(forward, forwardn);
  assert(chunk.type == 194u && chunk.value_size == 12u &&
      read32(chunk.value) == 101u && chunk.value[5] == 1u &&
      chunk.value[7] == 0u && read32(chunk.value + 8u) == 0u);
  memcpy(&before, &receiver, sizeof(before));
  assert(cnet_sctp_data_input(&receiver, 7u, forward, forwardn,
      sack, 8u, &sackn) == CNET_SCTP_DATA_CAPACITY);
  assert(sackn == 0u && memcmp(&before, &receiver, sizeof(before)) == 0);
  assert(cnet_sctp_data_input(&receiver, 7u, forward, forwardn,
      sack, sizeof(sack), &sackn) == CNET_SCTP_DATA_OK);
  assert(receiver.rx_mid_ordered[1] == 1u &&
      receiver.buffered_rx == 3u);
  assert(cnet_sctp_data_take(&receiver, &message, body, sizeof(body),
      &bodyn) == CNET_SCTP_DATA_OK);
  assert(message.message_identifier == 1u && bodyn == 3u &&
      memcmp(body, "new", 3u) == 0 && receiver.rx_mid_ordered[1] == 2u);
}

static void complete_message_crossed_by_forward_remains_deliverable(void) {
  int owned = 1;
  cnet_sctp_data sender = {0}, receiver = {0};
  cnet_sctp_data_config sc = config(7700u, 7701u, 91u, 92u,
      100u, 200u, &owned);
  cnet_sctp_data_config rc = config(7701u, 7700u, 92u, 91u,
      200u, 100u, &owned);
  cnet_sctp_data_reliability timed = {
    CNET_SCTP_DATA_RELIABILITY_TIMED, 5u
  };
  cnet_sctp_data_message message = {0};
  uint8_t data[1200], first_sack[1200], forward[1200], second_sack[1200];
  uint8_t body[8];
  size_t datan = 0u, first_sackn = 0u, forwardn = 0u;
  size_t second_sackn = 0u, bodyn = 0u;
  assert(cnet_sctp_data_init(&sender, &sc, 0u) == CNET_SCTP_DATA_OK);
  assert(cnet_sctp_data_init(&receiver, &rc, 0u) == CNET_SCTP_DATA_OK);
  assert(cnet_sctp_data_send_pr(&sender, 1u, 2u, 53u, 0, &timed,
      "keep", 4u, data, sizeof(data), &datan) == CNET_SCTP_DATA_OK);
  assert(cnet_sctp_data_input(&receiver, 2u, data, datan,
      first_sack, sizeof(first_sack), &first_sackn) == CNET_SCTP_DATA_OK);
  /* Deliberately lose the SACK so the sender legally abandons its ticket. */
  assert(cnet_sctp_data_advance(&sender, 6u, forward, sizeof(forward),
      &forwardn) == CNET_SCTP_DATA_OK);
  assert(one_chunk(forward, forwardn).type == 194u);
  assert(cnet_sctp_data_input(&receiver, 7u, forward, forwardn,
      second_sack, sizeof(second_sack), &second_sackn) == CNET_SCTP_DATA_OK);
  assert(cnet_sctp_data_take(&receiver, &message, body, sizeof(body),
      &bodyn) == CNET_SCTP_DATA_OK);
  assert(message.message_identifier == 0u && bodyn == 4u &&
      memcmp(body, "keep", 4u) == 0);
}

static void wrong_family_and_invalid_fsn_fail_closed(void) {
  int owned = 1;
  cnet_sctp_data receiver = {0}, legacy = {0}, reliable_only = {0};
  cnet_sctp_data_config ic = config(8001u, 8000u, 88u, 77u,
      200u, 100u, &owned);
  cnet_sctp_data_config lc = ic;
  cnet_sctp_data_config rc = ic;
  cnet_sctp_data_reliability timed = {
    CNET_SCTP_DATA_RELIABILITY_TIMED, 5u
  };
  uint8_t packet[1200], reply[1200];
  size_t packetn, replyn = 0u;
  lc.interleaving_supported = 0u;
  rc.forward_tsn_supported = 0u;
  assert(cnet_sctp_data_init(&receiver, &ic, 0u) == CNET_SCTP_DATA_OK);
  assert(cnet_sctp_data_init(&legacy, &lc, 0u) == CNET_SCTP_DATA_OK);
  assert(cnet_sctp_data_init(&reliable_only, &rc, 0u) == CNET_SCTP_DATA_OK);
  packetn = encode_i_data(&ic, 100u, 0u, 0u, 0u, I_DATA_E,
      "x", 1u, packet, sizeof(packet));
  assert(cnet_sctp_data_input(&receiver, 1u, packet, packetn,
      reply, sizeof(reply), &replyn) == CNET_SCTP_DATA_PROTOCOL);
  packetn = encode_i_data(&lc, 100u, 0u, 0u, 51u,
      I_DATA_B | I_DATA_E, "x", 1u, packet, sizeof(packet));
  assert(cnet_sctp_data_input(&legacy, 1u, packet, packetn,
      reply, sizeof(reply), &replyn) == CNET_SCTP_DATA_PROTOCOL);
  assert(cnet_sctp_data_send(&reliable_only, 1u, 0u, 51u, 0,
      "ok", 2u, packet, sizeof(packet), &packetn) == CNET_SCTP_DATA_OK);
  assert(one_chunk(packet, packetn).type == 64u);
  assert(cnet_sctp_data_send_pr(&reliable_only, 2u, 0u, 51u, 0,
      &timed, "no", 2u, packet, sizeof(packet), &packetn) ==
      CNET_SCTP_DATA_INVALID);
}

int main(void) {
  sender_uses_mid_fsn_and_i_data_only();
  receiver_reassembles_by_fsn_across_interleaved_tsns();
  receiver_accepts_web_rtc_1280_byte_i_data_fragment();
  partial_reliability_uses_i_forward_tsn();
  bundled_records_remain_i_data();
  ordered_skip_releases_later_mid();
  complete_message_crossed_by_forward_remains_deliverable();
  wrong_family_and_invalid_fsn_fail_closed();
  puts("C11 RFC8260 I-DATA/I-FORWARD-TSN bounded Owner path: PASS");
  return 0;
}
