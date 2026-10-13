#include "sctp_init_params_internal.h"

#include <string.h>

enum {
  PARAM_IPV4 = 5,
  PARAM_IPV6 = 6,
  PARAM_COOKIE = 7,
  PARAM_UNRECOGNIZED = 8,
  PARAM_COOKIE_PRESERVATIVE = 9,
  PARAM_HOSTNAME = 11,
  PARAM_SUPPORTED_ADDRESSES = 12,
  PARAM_ECN_CAPABLE = 0x8000
};

static uint16_t read16(const uint8_t *bytes) {
  return (uint16_t)((uint16_t)((uint16_t)bytes[0] << 8) | bytes[1]);
}

cnet_sctp_init_param_status cnet_sctp_init_params_preflight(
    const uint8_t *value, size_t value_size, int init_ack,
    cnet_sctp_init_param_view *out) {
  cnet_sctp_init_param_view parsed = {0};
  size_t position = 16u;
  if (!out) return CNET_SCTP_INIT_PARAMS_INVALID;
  /* A failed preflight must also revoke any previously returned borrowed view. */
  memset(out, 0, sizeof(*out));
  if (!value || value_size < 16u)
    return CNET_SCTP_INIT_PARAMS_INVALID;
  while (position < value_size) {
    const size_t remaining = value_size - position;
    size_t length;
    size_t padded;
    uint16_t type;
    if (remaining < 4u) return CNET_SCTP_INIT_PARAMS_MALFORMED;
    if (++parsed.parsed_count > CNET_SCTP_INIT_TLV_CEILING)
      return CNET_SCTP_INIT_PARAMS_CAPACITY;
    type = read16(value + position);
    length = (size_t)read16(value + position + 2u);
    if (length < 4u || length > remaining)
      return CNET_SCTP_INIT_PARAMS_MALFORMED;
    padded = (length + 3u) & ~(size_t)3u;
    /* Final parameter padding may be omitted. Non-final padding must not. */
    if (padded > remaining) {
      if (length != remaining) return CNET_SCTP_INIT_PARAMS_MALFORMED;
      padded = length;
    }
    switch (type) {
      case PARAM_COOKIE:
        if (!init_ack || parsed.cookie || length <= 4u)
          return CNET_SCTP_INIT_PARAMS_MALFORMED;
        parsed.cookie = value + position + 4u;
        parsed.cookie_size = length - 4u;
        break;
      case PARAM_UNRECOGNIZED:
        if (!init_ack || length < 8u)
          return CNET_SCTP_INIT_PARAMS_MALFORMED;
        /* Full reporting semantics cannot be implemented in this M2 profile. */
        return CNET_SCTP_INIT_PARAMS_UNSUPPORTED;
      case PARAM_IPV4:
        if (length != 8u) return CNET_SCTP_INIT_PARAMS_MALFORMED;
        return CNET_SCTP_INIT_PARAMS_UNSUPPORTED; /* no multihoming */
      case PARAM_IPV6:
        if (length != 20u) return CNET_SCTP_INIT_PARAMS_MALFORMED;
        return CNET_SCTP_INIT_PARAMS_UNSUPPORTED;
      case PARAM_COOKIE_PRESERVATIVE:
        if (init_ack || length != 8u)
          return CNET_SCTP_INIT_PARAMS_MALFORMED;
        return CNET_SCTP_INIT_PARAMS_UNSUPPORTED; /* no TTL negotiation */
      case PARAM_HOSTNAME:
        /* RFC 9260 section 5.1.2(B): well-formed Host Name Address
         * requires an ABORT rather than ordinary parameter rejection. */
        return CNET_SCTP_INIT_PARAMS_ABORT_REQUIRED;
      case PARAM_SUPPORTED_ADDRESSES:
        if (init_ack || length < 6u || ((length - 4u) & 1u) != 0u)
          return CNET_SCTP_INIT_PARAMS_MALFORMED;
        return CNET_SCTP_INIT_PARAMS_UNSUPPORTED;
      case PARAM_ECN_CAPABLE:
        if (length != 4u) return CNET_SCTP_INIT_PARAMS_MALFORMED;
        return CNET_SCTP_INIT_PARAMS_UNSUPPORTED; /* ECN unimplemented */
      default: {
        const uint16_t action = (uint16_t)(type >> 14);
        if (action == 1u || action == 3u) {
          /* 01: stop+report; 11: skip+report. RFC 9260 requires each
           * unrecognized parameter to be copied as a complete TLV.
           * Never silently drop excess reports, and do not store pointers. */
          if (parsed.report_count >= CNET_SCTP_INIT_REPORT_MAX ||
              padded > CNET_SCTP_INIT_REPORT_BYTES - parsed.report_wire_size)
            return CNET_SCTP_INIT_PARAMS_CAPACITY;
          parsed.reports[parsed.report_count].offset = position;
          parsed.reports[parsed.report_count].size = length;
          ++parsed.report_count;
          parsed.report_wire_size += padded;
        }
        if (action == 0u || action == 1u) {
          /* Stop scanning after this parameter. Parameters previously
           * processed are not rolled back; later bytes are untouched. */
          parsed.stopped_by_unknown = 1;
          position = value_size;
        } else {
          ++parsed.skipped_count; /* 10/11: skip, optionally report */
          position += padded;
        }
        continue;
      }
    }
    position += padded;
  }
  if (init_ack && !parsed.cookie)
    return CNET_SCTP_INIT_PARAMS_MALFORMED;
  *out = parsed;
  return CNET_SCTP_INIT_PARAMS_OK;
}
