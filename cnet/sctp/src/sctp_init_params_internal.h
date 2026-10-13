#ifndef CNET_SCTP_INIT_PARAMS_INTERNAL_H
#define CNET_SCTP_INIT_PARAMS_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

/* PRIVATE / RFC 9260 section 3.2.1 and sections 3.3.2-3.3.3.
 * Conservative, single-transport WebRTC research profile. Not installed.
 * No allocation, no callbacks, no retained packet ownership, no reporting.
 * Known parameters requiring address/ECN/cookie-preservative semantics and
 * unknown parameters requiring ERROR/Unrecognized reporting fail closed.
 */
#define CNET_SCTP_INIT_TLV_CEILING 128u

typedef enum cnet_sctp_init_param_status {
  CNET_SCTP_INIT_PARAMS_OK = 0,
  CNET_SCTP_INIT_PARAMS_INVALID = -1,
  CNET_SCTP_INIT_PARAMS_MALFORMED = -2,
  CNET_SCTP_INIT_PARAMS_UNSUPPORTED = -3,
  CNET_SCTP_INIT_PARAMS_REPORT_REQUIRED = -4,
  CNET_SCTP_INIT_PARAMS_CAPACITY = -5
} cnet_sctp_init_param_status;

typedef struct cnet_sctp_init_param_view {
  /* Borrowed. Valid only while the caller retains the full SCTP packet. */
  const uint8_t *cookie;
  size_t cookie_size;
  size_t parsed_count;
  size_t skipped_count;
  int stopped_by_unknown;
} cnet_sctp_init_param_view;

/* value points at the complete INIT/INIT-ACK chunk payload (including its
 * 16 mandatory bytes), never a raw SCTP packet or a C string.
 * init_ack != 0 requires exactly one valid State Cookie (type 7).
 * Reject malformed TLVs, unsupported functionality and reporting requests
 * before publishing any borrowed view or mutating Association state.
 */
cnet_sctp_init_param_status cnet_sctp_init_params_preflight(
    const uint8_t *value, size_t value_size, int init_ack,
    cnet_sctp_init_param_view *out);

#endif
