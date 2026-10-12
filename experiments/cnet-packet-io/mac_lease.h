#ifndef CNET_RESEARCH_MAC_LEASE_H
#define CNET_RESEARCH_MAC_LEASE_H

#include "reliable.h"

typedef enum rl_mac_status {
  RL_MAC_OK = 0,
  RL_MAC_INVALID,
  RL_MAC_WRONG_OWNER,
  RL_MAC_BUSY,
  RL_MAC_STALE
} rl_mac_status;

/* Owner-local binding shared by the GmSSL reference and installed CMeta SDK
 * adapters. This is a single research lease model, not a crypto registry or a
 * second scheduler. MAC keys remain owned by the R2 session until rl_stop(). */
typedef struct rl_mac_lease {
  rl_session *session;
  pthread_t owner;
  uint64_t generation;
  uint32_t local_id;
  uint32_t peer_id;
  uint32_t epoch;
  rl_mac_fn provider;
  unsigned in_mac;
  bool bound;
  bool sealed;
} rl_mac_lease;

rl_mac_status rl_mac_lease_bind(rl_mac_lease *lease, rl_session *session,
                                rl_ticket ticket);
rl_mac_status rl_mac_lease_seal(rl_mac_lease *lease, rl_ticket ticket);
rl_mac_status rl_mac_lease_release(rl_mac_lease *lease, rl_ticket ticket);

/* A nonreentrant owner-only MAC invocation. A seal does NOT stop admitted
 * R2 ACKs from authenticating; release waits for a stopped R2 session. */
bool rl_mac_lease_enter(rl_mac_lease *lease, rl_mac_fn requested);
void rl_mac_lease_leave(rl_mac_lease *lease);

#endif
