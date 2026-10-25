#ifndef CNET_SCTP_COLLISION_INTERNAL_H
#define CNET_SCTP_COLLISION_INTERNAL_H

#include <stdint.h>

/* RFC 9260 section 5.2.4, Table 12. PRIVATE, decision-only prototype.
 * The classifier NEVER mutates a live Association, signs a Cookie, rotates
 * a key, updates T1 or authorizes a replacement TCB. Operations A/B require
 * a separate Owner/DTLS Generation transaction before they can be applied.
 */
typedef enum cnet_sctp_cookie_decision {
  CNET_SCTP_COOKIE_DISCARD = 0,
  CNET_SCTP_COOKIE_RESTART_REQUIRES_BARRIER = 1, /* Table 12 case A */
  CNET_SCTP_COOKIE_COLLISION_REQUIRES_COMMIT = 2, /* case B */
  CNET_SCTP_COOKIE_STALE_DROP = 3, /* case C */
  CNET_SCTP_COOKIE_DUPLICATE_ACK = 4 /* case D */
} cnet_sctp_cookie_decision;

typedef struct cnet_sctp_cookie_tags {
  uint32_t existing_local;
  uint32_t existing_peer; /* 0 means that peer tag is not yet known */
  uint32_t existing_local_tie;
  uint32_t existing_peer_tie;
  uint32_t cookie_local;
  uint32_t cookie_peer;
  uint32_t cookie_local_tie;
  uint32_t cookie_peer_tie;
} cnet_sctp_cookie_tags;

/* No authentication occurs here. Call only AFTER authenticated Cookie parsing,
 * binding and cookie freshness / existing-association checks. The nonzero tie
 * requirement for case A deliberately refuses an unauthenticated 0/0 match.
 * An unknown or not-yet-representable case always returns DISCARD. */
cnet_sctp_cookie_decision cnet_sctp_cookie_classify(
    const cnet_sctp_cookie_tags *tags);

#endif
