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
/* Conservative private M2 limit; exceeding it rejects without truncating
 * mandatory RFC error reporting. No dynamic allocation or retained views. */
#define CNET_SCTP_INIT_REPORT_MAX 8u
#define CNET_SCTP_INIT_REPORT_BYTES 512u

typedef enum cnet_sctp_init_param_status {
  CNET_SCTP_INIT_PARAMS_OK = 0,
  CNET_SCTP_INIT_PARAMS_INVALID = -1,
  CNET_SCTP_INIT_PARAMS_MALFORMED = -2,
  CNET_SCTP_INIT_PARAMS_UNSUPPORTED = -3,
  CNET_SCTP_INIT_PARAMS_CAPACITY = -4,
  CNET_SCTP_INIT_PARAMS_ABORT_REQUIRED = -5
} cnet_sctp_init_param_status;

typedef struct cnet_sctp_init_report {
  /* Offset and non-padded length of a complete unrecognized TLV. Both are
   * relative to the same borrowed INIT/INIT ACK chunk value passed to parse. */
  size_t offset;
  size_t size;
} cnet_sctp_init_report;

typedef struct cnet_sctp_init_param_view {
  /* Borrowed. Valid only while the caller retains the full SCTP packet. */
  const uint8_t *cookie;
  size_t cookie_size;
  size_t parsed_count;
  size_t skipped_count;
  size_t report_count;
  size_t report_wire_size; /* sum of padded TLVs within error cause / param */
  cnet_sctp_init_report reports[CNET_SCTP_INIT_REPORT_MAX];
  /* RFC 5061 Supported Extensions (0x8008) and RFC 3758's dedicated
   * Forward-TSN-Supported parameter (0xC000). The parameters are borrowed
   * only during parsing; these negotiated capability bits are owned. */
  int supports_reconfig;
  int supports_forward_tsn;
  int stopped_by_unknown;
} cnet_sctp_init_param_view;

/* value points at the complete INIT/INIT-ACK chunk payload (including its
 * 16 mandatory bytes), never a raw SCTP packet or a C string.
 * init_ack != 0 requires exactly one valid State Cookie (type 7).
 * Accept unknown 01/11 only when all required reports fit within fixed
 * limits. No data is copied or retained, so caller MUST finish encoding
 * before the input chunk's lifetime ends. Known unsupported optional features
 * still fail closed; well-formed deprecated Host Name demands an ABORT. A
 * rejected preflight clears out, never returning an incomplete report set.
 */
cnet_sctp_init_param_status cnet_sctp_init_params_preflight(
    const uint8_t *value, size_t value_size, int init_ack,
    cnet_sctp_init_param_view *out);

#endif
