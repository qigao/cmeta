#ifndef CNET_RESEARCH_GMSSL_MAC_H
#define CNET_RESEARCH_GMSSL_MAC_H

#include "reliable.h"

/* Standalone, opt-in research provider using GmSSL's HMAC-SHA256 primitive.
 * No GmSSL types appear in a CNet public header or installed ABI. The host
 * provides authenticated keys/epochs; this binding DOES NOT establish peers. */
typedef enum rl_gmssl_status {
  RL_GMSSL_OK = 0,
  RL_GMSSL_INVALID,
  RL_GMSSL_WRONG_OWNER,
  RL_GMSSL_BUSY,
  RL_GMSSL_STALE
} rl_gmssl_status;

typedef struct rl_gmssl_lease {
  rl_session *session;
  pthread_t owner;
  uint64_t generation;
  uint32_t local_id;
  uint32_t peer_id;
  uint32_t epoch;
  unsigned in_mac;
  bool bound;
  bool sealed;
} rl_gmssl_lease;

/* Bind AFTER rl_start. Only one generation/peer/epoch at a time; a previous
 * session must have been stopped and its lease released. The lease does not
 * own the configuration's keys: they remain in the R2 session until rl_stop. */
rl_gmssl_status rl_gmssl_lease_bind(rl_gmssl_lease *lease, rl_session *session,
                                   rl_ticket ticket);
rl_gmssl_status rl_gmssl_lease_seal(rl_gmssl_lease *lease, rl_ticket ticket);
/* Exactly after rl_stop and owner-local protocol drain. A successful release
 * is NOT a device/DMA drain; Packet I/O stays an independent owner. */
rl_gmssl_status rl_gmssl_lease_release(rl_gmssl_lease *lease, rl_ticket ticket);

/* Assign to rl_callbacks.mac with rl_callbacks.mac_user = &lease.
 * Returns false on any wrong Owner, generation, peer, epoch or provider error.
 * The full-length HMAC is calculated by the pinned GmSSL implementation. */
bool rl_gmssl_mac(void *mac_user, const uint8_t key[RL_KEY_BYTES],
                  const void *bytes, size_t size,
                  uint8_t out[RL_MAC_BYTES]);

#endif
