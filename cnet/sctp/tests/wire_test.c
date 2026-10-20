#include <cnet/sctp_wire.h>

#ifdef NDEBUG
#undef NDEBUG /* Release CI must actually execute every assertion. */
#endif
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static void put_le32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16);
  p[3] = (uint8_t)(v >> 24);
}

/* Recomputes checksum after intentionally malformed / short test packets. */
static void seal(uint8_t *p, size_t length) {
  put_le32(p + 8u, 0u);
  put_le32(p + 8u, cnet_sctp_wire_crc32c(p, length));
}

static void test_unknown_chunk_action_bits(void) {
  /* RFC 9260 §3.2: 00 stop/discard, 01 stop/report,
   * 10 skip, 11 skip/report. Test each class with distinct type values. */
  assert(cnet_sctp_wire_unknown_chunk_action(0x00u) ==
         CNET_SCTP_WIRE_UNKNOWN_STOP_DISCARD);
  assert(cnet_sctp_wire_unknown_chunk_action(0x41u) ==
         CNET_SCTP_WIRE_UNKNOWN_STOP_REPORT);
  assert(cnet_sctp_wire_unknown_chunk_action(0x82u) ==
         CNET_SCTP_WIRE_UNKNOWN_SKIP);
  assert(cnet_sctp_wire_unknown_chunk_action(0xC3u) ==
         CNET_SCTP_WIRE_UNKNOWN_SKIP_REPORT);
  assert(cnet_sctp_wire_unknown_chunk_action(0xFFu) ==
         CNET_SCTP_WIRE_UNKNOWN_SKIP_REPORT);
}

static void test_crc_and_init(void) {
  static const uint8_t init[] = {
    0x11u, 0x22u, 0x33u, 0x44u, /* initiate tag */
    0x00u, 0x00u, 0x10u, 0x00u, /* a_rwnd */
    0x00u, 0x01u, 0x00u, 0x01u, /* stream counts */
    0x55u, 0x66u, 0x77u, 0x88u  /* initial TSN */
  };
  const cnet_sctp_wire_chunk chunk = {1u, 0u, init, sizeof(init)};
  cnet_sctp_wire_chunk output = {0};
  cnet_sctp_wire_header header = {0};
  cnet_sctp_wire_decoder decoder = {0};
  uint8_t packet[64] = {0};
  size_t written = 0u;
  assert(cnet_sctp_wire_crc32c("123456789", 9u) == UINT32_C(0xe3069283));
  assert(cnet_sctp_wire_encode(5000u, 5001u, 0u, &chunk, 1u,
                               packet, sizeof(packet), &written) == CNET_SCTP_WIRE_OK);
  assert(written == 32u);
  assert(packet[0] == 0x13u && packet[1] == 0x88u);
  assert(packet[2] == 0x13u && packet[3] == 0x89u);
  assert(packet[12] == 1u && packet[14] == 0u && packet[15] == 20u);
  assert(cnet_sctp_wire_decode(packet, written, &header, &decoder) == CNET_SCTP_WIRE_OK);
  assert(header.source_port == 5000u && header.destination_port == 5001u);
  assert(header.verification_tag == 0u && header.chunk_count == 1u);
  assert(cnet_sctp_wire_next_chunk(&decoder, &output) == CNET_SCTP_WIRE_OK);
  assert(output.type == 1u && output.value_size == sizeof(init));
  assert(memcmp(output.value, init, sizeof(init)) == 0);
  assert(cnet_sctp_wire_next_chunk(&decoder, &output) == CNET_SCTP_WIRE_END);
  packet[17] ^= 0x01u;
  assert(cnet_sctp_wire_decode(packet, written, &header, &decoder) == CNET_SCTP_WIRE_CHECKSUM);
}

static void test_multiple_chunks_and_padding(void) {
  static const uint8_t first[] = {0x10u};
  static const uint8_t second[] = {0x20u, 0x30u, 0x40u};
  const cnet_sctp_wire_chunk chunks[] = {
    {0u, 3u, first, sizeof(first)},
    {3u, 0u, second, sizeof(second)}
  };
  cnet_sctp_wire_chunk output = {0};
  cnet_sctp_wire_header header = {0};
  cnet_sctp_wire_decoder decoder = {0};
  uint8_t packet[64] = {0};
  size_t written = 0u;
  assert(cnet_sctp_wire_encode(1000u, 1001u, 0x10203040u, chunks, 2u,
                               packet, sizeof(packet), &written) == CNET_SCTP_WIRE_OK);
  assert(written == 28u); /* header 12 + 8 + 8 */
  assert(packet[17] == 0u && packet[18] == 0u && packet[19] == 0u);
  assert(cnet_sctp_wire_decode(packet, written, &header, &decoder) == CNET_SCTP_WIRE_OK);
  assert(header.verification_tag == 0x10203040u && header.chunk_count == 2u);
  assert(cnet_sctp_wire_next_chunk(&decoder, &output) == CNET_SCTP_WIRE_OK);
  assert(output.type == 0u && output.flags == 3u && output.value_size == 1u);
  assert(output.value[0] == 0x10u);
  assert(cnet_sctp_wire_next_chunk(&decoder, &output) == CNET_SCTP_WIRE_OK);
  assert(output.type == 3u && output.value_size == 3u);
  assert(output.value[0] == 0x20u && output.value[2] == 0x40u);
  assert(cnet_sctp_wire_next_chunk(&decoder, &output) == CNET_SCTP_WIRE_END);
  /* RFC 9260 robust receiver tolerates omitted final padding. */
  seal(packet, written - 1u);
  assert(cnet_sctp_wire_decode(packet, written - 1u, &header, &decoder) ==
         CNET_SCTP_WIRE_OK);
  assert(header.chunk_count == 2u);
  assert(cnet_sctp_wire_next_chunk(&decoder, &output) == CNET_SCTP_WIRE_OK);
  assert(cnet_sctp_wire_next_chunk(&decoder, &output) == CNET_SCTP_WIRE_OK);
  assert(output.value_size == 3u);
}

static void test_reject_and_no_partial_write(void) {
  static const uint8_t init[] = {
    0x11u, 0x22u, 0x33u, 0x44u, 0, 0, 0, 1,
    0, 1, 0, 1, 0, 0, 0, 5
  };
  const cnet_sctp_wire_chunk chunks[] = {
    {1u, 0u, init, sizeof(init)},
    {3u, 0u, init, sizeof(init)}
  };
  cnet_sctp_wire_header header = {0};
  cnet_sctp_wire_decoder decoder = {0};
  uint8_t packet[64];
  size_t written = 44u;
  memset(packet, 0x5au, sizeof(packet));
  assert(cnet_sctp_wire_encode(1u, 2u, 0u, chunks, 2u,
                               packet, sizeof(packet), &written) == CNET_SCTP_WIRE_MALFORMED);
  assert(packet[0] == 0x5au && written == 0u);
  assert(cnet_sctp_wire_encode(1u, 2u, 3u, chunks, 1u,
                               packet, sizeof(packet), &written) == CNET_SCTP_WIRE_MALFORMED);
  assert(cnet_sctp_wire_encode(0u, 2u, 0u, chunks, 1u,
                               packet, sizeof(packet), &written) == CNET_SCTP_WIRE_EINVAL);
  assert(cnet_sctp_wire_encode(1u, 2u, 0u, chunks, 1u,
                               packet, 31u, &written) == CNET_SCTP_WIRE_CAPACITY);
  assert(packet[0] == 0x5au);
  assert(cnet_sctp_wire_encode(1u, 2u, 0u, chunks, 1u,
                               packet, sizeof(packet), &written) == CNET_SCTP_WIRE_OK);
  assert(written == 32u);
  packet[12 + 2] = 0;
  packet[12 + 3] = 0;
  seal(packet, written);
  assert(cnet_sctp_wire_decode(packet, written, &header, &decoder) ==
         CNET_SCTP_WIRE_MALFORMED);
  assert(cnet_sctp_wire_decode(NULL, 0u, &header, &decoder) ==
         CNET_SCTP_WIRE_EINVAL);
  assert(cnet_sctp_wire_decode(packet, CNET_SCTP_WIRE_MAX_PACKET_BYTES + 1u,
                               &header, &decoder) == CNET_SCTP_WIRE_CAPACITY);
}

static void test_truncation_and_mutations(void) {
  const uint8_t payload[] = {0x11u, 0x22u, 0x33u, 0x44u, 0x55u};
  const cnet_sctp_wire_chunk chunk = {0u, 0u, payload, sizeof(payload)};
  cnet_sctp_wire_header header = {0};
  cnet_sctp_wire_decoder decoder = {0};
  uint8_t packet[64];
  uint8_t scratch[64];
  size_t written = 0u;
  size_t i;
  uint32_t seed = UINT32_C(0x31415926);
  assert(cnet_sctp_wire_encode(123u, 456u, 789u, &chunk, 1u,
                               packet, sizeof(packet), &written) == CNET_SCTP_WIRE_OK);
  for (i = 0u; i < written; ++i) {
    assert(cnet_sctp_wire_decode(packet, i, &header, &decoder) != CNET_SCTP_WIRE_OK);
  }
  for (i = 0u; i < 5000u; ++i) {
    size_t at;
    seed = seed * UINT32_C(1664525) + UINT32_C(1013904223);
    at = (size_t)(seed % (uint32_t)written);
    memcpy(scratch, packet, written);
    scratch[at] ^= (uint8_t)((seed >> 8) | 1u);
    /* Any structured invalid-input rejection is valid; no crash or OOB. */
    {
      cnet_sctp_wire_status status = cnet_sctp_wire_decode(scratch, written,
                                                            &header, &decoder);
      assert(status == CNET_SCTP_WIRE_OK || status == CNET_SCTP_WIRE_CHECKSUM ||
             status == CNET_SCTP_WIRE_MALFORMED);
    }
  }
}

int main(void) {
  test_unknown_chunk_action_bits();
  test_crc_and_init();
  test_multiple_chunks_and_padding();
  test_reject_and_no_partial_write();
  test_truncation_and_mutations();
  puts("cnet SCTP RFC 9260 wire M1: PASS (not protocol interoperability)");
  return 0;
}
