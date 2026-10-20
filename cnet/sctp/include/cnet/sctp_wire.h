#ifndef CNET_SCTP_WIRE_H
#define CNET_SCTP_WIRE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
#error "The experimental CNet SCTP wire module has a C11-only interface"
#endif

/* RFC 9260 packet framing only. Not an SCTP association or DataChannel API. */
#define CNET_SCTP_WIRE_ABI_VERSION 1u
#define CNET_SCTP_WIRE_MAX_PACKET_BYTES 65535u
#define CNET_SCTP_WIRE_MAX_CHUNKS 128u

typedef enum cnet_sctp_wire_status {
  CNET_SCTP_WIRE_OK = 0,
  CNET_SCTP_WIRE_END = 1,
  CNET_SCTP_WIRE_EINVAL = -1,
  CNET_SCTP_WIRE_MALFORMED = -2,
  CNET_SCTP_WIRE_CHECKSUM = -3,
  CNET_SCTP_WIRE_CAPACITY = -4
} cnet_sctp_wire_status;

/* RFC 9260 §3.2 action bits for an unrecognized Chunk Type. This is a
 * classifier only: Association packet processing must still enforce packet
 * preflight, legal Chunk combinations, report construction and commit rules. */
typedef enum cnet_sctp_wire_unknown_action {
  CNET_SCTP_WIRE_UNKNOWN_STOP_DISCARD = 0,
  CNET_SCTP_WIRE_UNKNOWN_STOP_REPORT = 1,
  CNET_SCTP_WIRE_UNKNOWN_SKIP = 2,
  CNET_SCTP_WIRE_UNKNOWN_SKIP_REPORT = 3
} cnet_sctp_wire_unknown_action;

/* Returns the RFC action encoded in the top two Chunk Type bits. */
cnet_sctp_wire_unknown_action cnet_sctp_wire_unknown_chunk_action(uint8_t type);

typedef struct cnet_sctp_wire_chunk {
  uint8_t type;
  uint8_t flags;
  /* Borrowed memory; for decode valid while original packet remains alive. */
  const uint8_t *value;
  size_t value_size;
} cnet_sctp_wire_chunk;

typedef struct cnet_sctp_wire_header {
  uint16_t source_port;
  uint16_t destination_port;
  uint32_t verification_tag;
  uint32_t checksum;
  size_t chunk_count;
} cnet_sctp_wire_header;

typedef struct cnet_sctp_wire_decoder {
  const uint8_t *packet;
  size_t packet_size;
  size_t next_offset;
  size_t chunks_remaining;
} cnet_sctp_wire_decoder;

/* Castagnoli CRC32C. Input requires a non-null pointer when size > 0. */
uint32_t cnet_sctp_wire_crc32c(const void *data, size_t size);

/* Validates complete framing and checksum before publishing borrowed views.
 * No allocation, mutation, external transport, timers or background threads.
 * SCTP association semantics and unknown-chunk actions are NOT implemented.
 */
cnet_sctp_wire_status cnet_sctp_wire_decode(
    const void *packet, size_t packet_size,
    cnet_sctp_wire_header *header, cnet_sctp_wire_decoder *decoder);

cnet_sctp_wire_status cnet_sctp_wire_next_chunk(
    cnet_sctp_wire_decoder *decoder, cnet_sctp_wire_chunk *chunk);

/* Writes an entire SCTP packet atomically with respect to capacity checks.
 * Chunk payload pointers may not overlap the destination buffer.
 * Checksum bytes use the SCTP-specific CRC32C wire representation.
 */
cnet_sctp_wire_status cnet_sctp_wire_encode(
    uint16_t source_port, uint16_t destination_port, uint32_t verification_tag,
    const cnet_sctp_wire_chunk *chunks, size_t chunk_count,
    void *destination, size_t destination_capacity, size_t *written);

#endif /* CNET_SCTP_WIRE_H */
