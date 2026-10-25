#include "sctp_collision_internal.h"

cnet_sctp_cookie_decision cnet_sctp_cookie_classify(
    const cnet_sctp_cookie_tags *t) {
  int local_match;
  int peer_match;
  if (!t || t->existing_local == 0u || t->cookie_local == 0u)
    return CNET_SCTP_COOKIE_DISCARD;
  local_match = t->cookie_local == t->existing_local;
  peer_match = t->existing_peer != 0u &&
               t->cookie_peer == t->existing_peer;

  /* (D) Established association: both current verification tags match.
   * No Tie-Tag is needed, and a retransmitted COOKIE ECHO may be ACKed. */
  if (local_match && peer_match)
    return CNET_SCTP_COOKIE_DUPLICATE_ACK;

  /* (B) Simultaneous open: local tag matches, peer differs/is unknown.
   * The caller must atomically update peer identity and stop T1 before ACK. */
  if (local_match && !peer_match)
    return CNET_SCTP_COOKIE_COLLISION_REQUIRES_COMMIT;

  /* (C) Late cookie with peer tag matching and no Tie-Tags: discard without
   * touching existing TCB, timers or current peer Generation. */
  if (!local_match && peer_match &&
      t->cookie_local_tie == 0u && t->cookie_peer_tie == 0u)
    return CNET_SCTP_COOKIE_STALE_DROP;

  /* (A) Peer restart: both verification tags differ but both existing
   * nonzero Tie-Tags match those bound by the authenticated Cookie.
   * Never permit an all-zero Tie-Tag pair to manufacture a restart. */
  if (!local_match && !peer_match && t->existing_peer != 0u &&
      t->existing_local_tie != 0u && t->existing_peer_tie != 0u &&
      t->cookie_local_tie == t->existing_local_tie &&
      t->cookie_peer_tie == t->existing_peer_tie)
    return CNET_SCTP_COOKIE_RESTART_REQUIRES_BARRIER;

  return CNET_SCTP_COOKIE_DISCARD;
}
