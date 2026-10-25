#ifndef CNET_SCTP_RELIABILITY_INTERNAL_H
#define CNET_SCTP_RELIABILITY_INTERNAL_H

#include <stdint.h>

/* Shared private value contract between RFC 8832 DCEP negotiation and the
 * RFC 3758 DATA sender. Storage, timers and abandonment remain DATA-owned. */
typedef enum cnet_sctp_data_reliability_mode {
  CNET_SCTP_DATA_RELIABLE = 0,
  CNET_SCTP_DATA_RELIABILITY_RETRANSMITS = 1,
  CNET_SCTP_DATA_RELIABILITY_TIMED = 2
} cnet_sctp_data_reliability_mode;

typedef struct cnet_sctp_data_reliability {
  cnet_sctp_data_reliability_mode mode;
  uint32_t value; /* retransmission count or lifetime in milliseconds */
} cnet_sctp_data_reliability;

#endif
