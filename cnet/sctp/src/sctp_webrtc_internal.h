#ifndef CNET_SCTP_WEBRTC_INTERNAL_H
#define CNET_SCTP_WEBRTC_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

/* RFC 8831 WebRTC user-message PPID framing: C11, deterministic,
 * stateless, no callback/socket or second ownership. Private adapter.
 * SCTP DATA must contain exactly one user message, with fragmentation
 * performed by the existing bounded CNet Owner session, not by PPID.
 */
#define CNET_SCTP_WEBRTC_TEXT_PPID 51u
#define CNET_SCTP_WEBRTC_BINARY_PPID 53u
#define CNET_SCTP_WEBRTC_EMPTY_TEXT_PPID 56u
#define CNET_SCTP_WEBRTC_EMPTY_BINARY_PPID 57u

typedef enum cnet_sctp_webrtc_status {
  CNET_SCTP_WEBRTC_OK = 0,
  CNET_SCTP_WEBRTC_INVALID = -1,
  CNET_SCTP_WEBRTC_UNSUPPORTED = -2,
  CNET_SCTP_WEBRTC_CAPACITY = -3
} cnet_sctp_webrtc_status;

typedef struct cnet_sctp_webrtc_payload {
  uint32_t ppid;
  const uint8_t *wire_bytes; /* caller-owned or read-only static one-byte zero */
  size_t wire_size;
  size_t logical_size;
  uint8_t binary;
} cnet_sctp_webrtc_payload;

/* Encode one logical UTF-8 text or binary message. RFC 8831 requires
 * a 1-byte zero SCTP DATA payload and PPID 56/57 for empty messages.
 * For nonempty messages, borrowed memory is only valid while caller retains
 * data. No source buffer is retained by this adapter.
 */
cnet_sctp_webrtc_status cnet_sctp_webrtc_prepare(
    int binary, const void *bytes, size_t logical_size,
    cnet_sctp_webrtc_payload *out);

/* Decode a COMPLETE SCTP user message after DATA B/E reassembly. PPID 50
 * DCEP must be handled by DCEP and may not be exposed as user data.
 * Deprecated partial PPIDs 52/54 and unknown PPIDs fail closed.
 * Received PPID 56/57 MUST have exactly one octet of zero. */
cnet_sctp_webrtc_status cnet_sctp_webrtc_decode(
    uint32_t ppid, const void *wire_bytes, size_t wire_size,
    cnet_sctp_webrtc_payload *out);

#endif
