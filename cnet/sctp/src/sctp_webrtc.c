#include "sctp_webrtc_internal.h"

#include <string.h>

static const uint8_t empty_payload_byte[1] = {0u};

cnet_sctp_webrtc_status cnet_sctp_webrtc_prepare(
    int binary, const void *bytes, size_t logical_size,
    cnet_sctp_webrtc_payload *out) {
  cnet_sctp_webrtc_payload value = {0};
  if (!out) return CNET_SCTP_WEBRTC_INVALID;
  memset(out, 0, sizeof(*out));
  if ((binary != 0 && binary != 1) ||
      (logical_size != 0u && !bytes))
    return CNET_SCTP_WEBRTC_INVALID;
  if (logical_size == 0u) {
    value.ppid = binary ? CNET_SCTP_WEBRTC_EMPTY_BINARY_PPID :
                          CNET_SCTP_WEBRTC_EMPTY_TEXT_PPID;
    value.wire_bytes = empty_payload_byte;
    value.wire_size = 1u;
  } else {
    value.ppid = binary ? CNET_SCTP_WEBRTC_BINARY_PPID :
                          CNET_SCTP_WEBRTC_TEXT_PPID;
    value.wire_bytes = (const uint8_t *)bytes;
    value.wire_size = logical_size;
  }
  value.logical_size = logical_size;
  value.binary = (uint8_t)binary;
  *out = value;
  return CNET_SCTP_WEBRTC_OK;
}

cnet_sctp_webrtc_status cnet_sctp_webrtc_decode(
    uint32_t ppid, const void *bytes, size_t wire_size,
    cnet_sctp_webrtc_payload *out) {
  cnet_sctp_webrtc_payload value = {0};
  const uint8_t *wire = (const uint8_t *)bytes;
  if (!out) return CNET_SCTP_WEBRTC_INVALID;
  memset(out, 0, sizeof(*out));
  if (!wire) return CNET_SCTP_WEBRTC_INVALID;
  if (ppid == CNET_SCTP_WEBRTC_TEXT_PPID ||
      ppid == CNET_SCTP_WEBRTC_BINARY_PPID) {
    if (wire_size == 0u) return CNET_SCTP_WEBRTC_INVALID;
    value.logical_size = wire_size;
    value.wire_size = wire_size;
    value.binary = (uint8_t)(ppid == CNET_SCTP_WEBRTC_BINARY_PPID);
    value.wire_bytes = wire;
  } else if (ppid == CNET_SCTP_WEBRTC_EMPTY_TEXT_PPID ||
             ppid == CNET_SCTP_WEBRTC_EMPTY_BINARY_PPID) {
    if (wire_size != 1u || wire[0] != 0u)
      return CNET_SCTP_WEBRTC_INVALID;
    value.logical_size = 0u;
    value.wire_size = 1u;
    value.binary = (uint8_t)(ppid == CNET_SCTP_WEBRTC_EMPTY_BINARY_PPID);
    value.wire_bytes = wire;
  } else {
    /* PPID 50 is DCEP; 52/54 are deprecated PPID-level fragmentation.
     * Neither may be promoted to user data. No compatibility fallback. */
    return CNET_SCTP_WEBRTC_UNSUPPORTED;
  }
  value.ppid = ppid;
  *out = value;
  return CNET_SCTP_WEBRTC_OK;
}
