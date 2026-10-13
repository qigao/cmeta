#include <cnet/sctp_wire.h>

#include <limits.h>
#include <string.h>

enum { SCTP_HEADER_BYTES = 12, SCTP_CHUNK_HEADER_BYTES = 4 };

enum {
  SCTP_CHUNK_INIT = 1,
  SCTP_CHUNK_INIT_ACK = 2,
  SCTP_CHUNK_SHUTDOWN_COMPLETE = 14
};

static uint16_t read_be16(const uint8_t *p) {
  return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static uint32_t read_be32(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
         ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint32_t read_le32(const uint8_t *p) {
  return ((uint32_t)p[3] << 24) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[1] << 8) | (uint32_t)p[0];
}

static void write_be16(uint8_t *p, uint16_t value) {
  p[0] = (uint8_t)(value >> 8);
  p[1] = (uint8_t)value;
}

static void write_be32(uint8_t *p, uint32_t value) {
  p[0] = (uint8_t)(value >> 24);
  p[1] = (uint8_t)(value >> 16);
  p[2] = (uint8_t)(value >> 8);
  p[3] = (uint8_t)value;
}

static void write_le32(uint8_t *p, uint32_t value) {
  p[0] = (uint8_t)value;
  p[1] = (uint8_t)(value >> 8);
  p[2] = (uint8_t)(value >> 16);
  p[3] = (uint8_t)(value >> 24);
}

/* Reflected Castagnoli polynomial. The complement is part of RFC 9260's
 * checksum calculation; unlike its other integer fields, SCTP emits these
 * four checksum bytes in little-endian order. */
static uint32_t crc_step(uint32_t crc, uint8_t octet) {
  unsigned int i;
  crc ^= octet;
  for (i = 0; i != 8u; ++i) {
    crc = (crc >> 1) ^ ((crc & 1u) ? UINT32_C(0x82f63b78) : 0u);
  }
  return crc;
}

uint32_t cnet_sctp_wire_crc32c(const void *data, size_t size) {
  const uint8_t *bytes = (const uint8_t *)data;
  uint32_t crc = UINT32_MAX;
  size_t i;
  if (size != 0u && bytes == NULL) return 0u;
  for (i = 0u; i < size; ++i) crc = crc_step(crc, bytes[i]);
  return ~crc;
}

static uint32_t packet_checksum(const uint8_t *data, size_t size) {
  uint32_t crc = UINT32_MAX;
  size_t i;
  for (i = 0u; i < size; ++i) {
    crc = crc_step(crc, (i >= 8u && i < 12u) ? 0u : data[i]);
  }
  return ~crc;
}

static int standalone_chunk(uint8_t type) {
  return type == SCTP_CHUNK_INIT || type == SCTP_CHUNK_INIT_ACK ||
         type == SCTP_CHUNK_SHUTDOWN_COMPLETE;
}

/* Decode permissively ignores final chunk padding, as required by RFC 9260.
 * Reject internal truncation, zero-length chunks and arithmetic overflow.
 */
static cnet_sctp_wire_status next_extent(
    const uint8_t *packet, size_t size, size_t offset,
    size_t *payload_length, size_t *next_offset) {
  size_t length;
  size_t padded;
  size_t remaining;
  if (offset > size || size - offset < SCTP_CHUNK_HEADER_BYTES)
    return CNET_SCTP_WIRE_MALFORMED;
  length = (size_t)read_be16(packet + offset + 2u);
  remaining = size - offset;
  if (length < SCTP_CHUNK_HEADER_BYTES || length > remaining)
    return CNET_SCTP_WIRE_MALFORMED;
  padded = (length + 3u) & ~(size_t)3u;
  if (padded > remaining) {
    if (length != remaining) return CNET_SCTP_WIRE_MALFORMED;
    padded = length; /* tolerate omitted trailing padding on last chunk */
  }
  *payload_length = length - SCTP_CHUNK_HEADER_BYTES;
  *next_offset = offset + padded;
  return CNET_SCTP_WIRE_OK;
}

cnet_sctp_wire_status cnet_sctp_wire_decode(
    const void *packet, size_t packet_size,
    cnet_sctp_wire_header *header, cnet_sctp_wire_decoder *decoder) {
  const uint8_t *bytes = (const uint8_t *)packet;
  cnet_sctp_wire_header local;
  cnet_sctp_wire_decoder cursor;
  size_t position;
  size_t value_size;
  size_t next;
  size_t count = 0u;
  uint8_t first_type = 0;
  if (!bytes || !header || !decoder) return CNET_SCTP_WIRE_EINVAL;
  if (packet_size > CNET_SCTP_WIRE_MAX_PACKET_BYTES)
    return CNET_SCTP_WIRE_CAPACITY;
  if (packet_size < SCTP_HEADER_BYTES + SCTP_CHUNK_HEADER_BYTES)
    return CNET_SCTP_WIRE_MALFORMED;
  local.source_port = read_be16(bytes);
  local.destination_port = read_be16(bytes + 2u);
  local.verification_tag = read_be32(bytes + 4u);
  local.checksum = read_le32(bytes + 8u);
  if (local.source_port == 0u || local.destination_port == 0u)
    return CNET_SCTP_WIRE_MALFORMED;
  if (packet_checksum(bytes, packet_size) != local.checksum)
    return CNET_SCTP_WIRE_CHECKSUM;
  position = SCTP_HEADER_BYTES;
  while (position < packet_size) {
    uint8_t type = bytes[position];
    if (++count > CNET_SCTP_WIRE_MAX_CHUNKS)
      return CNET_SCTP_WIRE_CAPACITY;
    if (next_extent(bytes, packet_size, position, &value_size, &next) !=
        CNET_SCTP_WIRE_OK)
      return CNET_SCTP_WIRE_MALFORMED;
    if (count == 1u) first_type = type;
    if ((count > 1u && (standalone_chunk(first_type) || standalone_chunk(type))) ||
        (type == SCTP_CHUNK_INIT &&
         (local.verification_tag != 0u || value_size < 16u ||
          read_be32(bytes + position + 4u) == 0u)))
      return CNET_SCTP_WIRE_MALFORMED;
    position = next;
  }
  if (position != packet_size || count == 0u)
    return CNET_SCTP_WIRE_MALFORMED;
  local.chunk_count = count;
  cursor.packet = bytes;
  cursor.packet_size = packet_size;
  cursor.next_offset = SCTP_HEADER_BYTES;
  cursor.chunks_remaining = count;
  *header = local;
  *decoder = cursor;
  return CNET_SCTP_WIRE_OK;
}

cnet_sctp_wire_status cnet_sctp_wire_next_chunk(
    cnet_sctp_wire_decoder *decoder, cnet_sctp_wire_chunk *chunk) {
  size_t value_size;
  size_t next;
  size_t position;
  if (!decoder || !chunk) return CNET_SCTP_WIRE_EINVAL;
  if (decoder->chunks_remaining == 0u) return CNET_SCTP_WIRE_END;
  if (!decoder->packet ||
      next_extent(decoder->packet, decoder->packet_size,
                  decoder->next_offset, &value_size, &next) != CNET_SCTP_WIRE_OK)
    return CNET_SCTP_WIRE_MALFORMED;
  position = decoder->next_offset;
  chunk->type = decoder->packet[position];
  chunk->flags = decoder->packet[position + 1u];
  chunk->value = decoder->packet + position + SCTP_CHUNK_HEADER_BYTES;
  chunk->value_size = value_size;
  decoder->next_offset = next;
  --decoder->chunks_remaining;
  return CNET_SCTP_WIRE_OK;
}

cnet_sctp_wire_status cnet_sctp_wire_encode(
    uint16_t source_port, uint16_t destination_port, uint32_t verification_tag,
    const cnet_sctp_wire_chunk *chunks, size_t chunk_count,
    void *destination, size_t destination_capacity, size_t *written) {
  uint8_t *packet = (uint8_t *)destination;
  size_t position = SCTP_HEADER_BYTES;
  size_t i;
  if (!packet || !written || !chunks || chunk_count == 0u ||
      source_port == 0u || destination_port == 0u)
    return CNET_SCTP_WIRE_EINVAL;
  *written = 0u;
  if (chunk_count > CNET_SCTP_WIRE_MAX_CHUNKS)
    return CNET_SCTP_WIRE_CAPACITY;
  for (i = 0u; i < chunk_count; ++i) {
    size_t length;
    size_t padded;
    if (chunks[i].value_size != 0u && !chunks[i].value)
      return CNET_SCTP_WIRE_EINVAL;
    if (chunks[i].value_size > UINT16_MAX - SCTP_CHUNK_HEADER_BYTES)
      return CNET_SCTP_WIRE_CAPACITY;
    if ((chunk_count != 1u && standalone_chunk(chunks[i].type)) ||
        (chunks[i].type == SCTP_CHUNK_INIT &&
         (verification_tag != 0u || chunks[i].value_size < 16u ||
          read_be32(chunks[i].value) == 0u)))
      return CNET_SCTP_WIRE_MALFORMED;
    length = chunks[i].value_size + SCTP_CHUNK_HEADER_BYTES;
    padded = (length + 3u) & ~(size_t)3u;
    if (padded > CNET_SCTP_WIRE_MAX_PACKET_BYTES - position)
      return CNET_SCTP_WIRE_CAPACITY;
    position += padded;
  }
  if (position > destination_capacity) return CNET_SCTP_WIRE_CAPACITY;
  memset(packet, 0, position);
  write_be16(packet, source_port);
  write_be16(packet + 2u, destination_port);
  write_be32(packet + 4u, verification_tag);
  position = SCTP_HEADER_BYTES;
  for (i = 0u; i < chunk_count; ++i) {
    size_t length = chunks[i].value_size + SCTP_CHUNK_HEADER_BYTES;
    packet[position] = chunks[i].type;
    packet[position + 1u] = chunks[i].flags;
    write_be16(packet + position + 2u, (uint16_t)length);
    if (chunks[i].value_size != 0u)
      memcpy(packet + position + SCTP_CHUNK_HEADER_BYTES,
             chunks[i].value, chunks[i].value_size);
    position += (length + 3u) & ~(size_t)3u;
  }
  write_le32(packet + 8u, packet_checksum(packet, position));
  *written = position;
  return CNET_SCTP_WIRE_OK;
}
