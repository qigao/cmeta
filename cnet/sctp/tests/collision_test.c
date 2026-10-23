#include "sctp_collision_internal.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>

static cnet_sctp_cookie_tags baseline(void) {
  cnet_sctp_cookie_tags t = {0};
  t.existing_local = 11u;
  t.existing_peer = 21u;
  t.existing_local_tie = 31u;
  t.existing_peer_tie = 41u;
  t.cookie_local = 11u;
  t.cookie_peer = 21u;
  return t;
}

static void rfc_table_12(void) {
  cnet_sctp_cookie_tags t = baseline();
  assert(cnet_sctp_cookie_classify(NULL) == CNET_SCTP_COOKIE_DISCARD);
  assert(cnet_sctp_cookie_classify(&t) == CNET_SCTP_COOKIE_DUPLICATE_ACK);
  t.cookie_peer = 22u;
  assert(cnet_sctp_cookie_classify(&t) == CNET_SCTP_COOKIE_COLLISION_REQUIRES_COMMIT);
  t.cookie_peer = 0u;
  assert(cnet_sctp_cookie_classify(&t) == CNET_SCTP_COOKIE_COLLISION_REQUIRES_COMMIT);
  t.existing_peer = 0u;
  t.cookie_peer = 21u;
  assert(cnet_sctp_cookie_classify(&t) == CNET_SCTP_COOKIE_COLLISION_REQUIRES_COMMIT);
  t = baseline();
  t.cookie_local = 12u;
  assert(cnet_sctp_cookie_classify(&t) == CNET_SCTP_COOKIE_STALE_DROP);
  t.cookie_local_tie = 31u;
  assert(cnet_sctp_cookie_classify(&t) == CNET_SCTP_COOKIE_DISCARD);
  t.cookie_local_tie = 0u;
  t.cookie_peer_tie = 41u;
  assert(cnet_sctp_cookie_classify(&t) == CNET_SCTP_COOKIE_DISCARD);
  t = baseline();
  t.cookie_local = 12u;
  t.cookie_peer = 22u;
  assert(cnet_sctp_cookie_classify(&t) == CNET_SCTP_COOKIE_DISCARD);
  t.cookie_local_tie = 31u;
  t.cookie_peer_tie = 41u;
  assert(cnet_sctp_cookie_classify(&t) == CNET_SCTP_COOKIE_RESTART_REQUIRES_BARRIER);
  t.cookie_peer_tie ^= 1u;
  assert(cnet_sctp_cookie_classify(&t) == CNET_SCTP_COOKIE_DISCARD);
  t.cookie_peer_tie ^= 1u;
  t.cookie_local_tie = 0u;
  assert(cnet_sctp_cookie_classify(&t) == CNET_SCTP_COOKIE_DISCARD);
  t.cookie_local_tie = 31u;
  t.existing_local_tie = 0u;
  assert(cnet_sctp_cookie_classify(&t) == CNET_SCTP_COOKIE_DISCARD);
  t = baseline();
  t.cookie_local = 0u;
  assert(cnet_sctp_cookie_classify(&t) == CNET_SCTP_COOKIE_DISCARD);
  t = baseline();
  t.existing_local = 0u;
  assert(cnet_sctp_cookie_classify(&t) == CNET_SCTP_COOKIE_DISCARD);
}

static void matrix_negative(void) {
  cnet_sctp_cookie_tags t;
  unsigned int i, j, k, m;
  const uint32_t values[] = {0u, 11u, 12u};
  /* Exhaustively enumerate 3^4 cookie tag/tie combinations against a
   * fixed established TCB: no unknown pattern is allowed to restart. */
  for (i = 0u; i < 3u; ++i)
    for (j = 0u; j < 3u; ++j)
      for (k = 0u; k < 3u; ++k)
        for (m = 0u; m < 3u; ++m) {
          t = baseline();
          t.cookie_local = values[i];
          t.cookie_peer = values[j];
          t.cookie_local_tie = values[k];
          t.cookie_peer_tie = values[m];
          assert(cnet_sctp_cookie_classify(&t) != CNET_SCTP_COOKIE_RESTART_REQUIRES_BARRIER);
        }
}

int main(void) {
  rfc_table_12();
  matrix_negative();
  puts("C11 RFC9260 Table 12 decision-only restart classification PASS (no live restart)");
  return 0;
}
