#include <cnet/sctp_wire.h>

#include <stdint.h>

int main(void) {
  const uint8_t payload[] = {1u, 2u, 3u};
  const cnet_sctp_wire_chunk chunk = {0u, 0u, payload, sizeof(payload)};
  cnet_sctp_wire_chunk received = {0};
  cnet_sctp_wire_header header = {0};
  cnet_sctp_wire_decoder decoder = {0};
  uint8_t packet[64] = {0};
  size_t packet_size = 0u;
  if (CNET_SCTP_WIRE_ABI_VERSION != 1u) return 1;
  if (cnet_sctp_wire_encode(5000u, 5001u, 0x10203040u, &chunk, 1u,
                            packet, sizeof(packet), &packet_size) != CNET_SCTP_WIRE_OK)
    return 2;
  if (cnet_sctp_wire_decode(packet, packet_size, &header, &decoder) != CNET_SCTP_WIRE_OK)
    return 3;
  if (header.chunk_count != 1u || header.verification_tag != 0x10203040u)
    return 4;
  if (cnet_sctp_wire_next_chunk(&decoder, &received) != CNET_SCTP_WIRE_OK)
    return 5;
  if (received.value_size != sizeof(payload) || received.value[2] != 3u)
    return 6;
  return cnet_sctp_wire_next_chunk(&decoder, &received) == CNET_SCTP_WIRE_END ? 0 : 7;
}
