#ifndef CNET_RESEARCH_GMSSL_MAC_H
#define CNET_RESEARCH_GMSSL_MAC_H

#include "mac_lease.h"

/* Explicit, non-installed reference provider pinned to GmSSL source SHA.
 * Owner/Generation semantics are shared with the CMeta SDK adapter. */
bool rl_gmssl_mac(void *mac_user, const uint8_t key[RL_KEY_BYTES],
                  const void *bytes, size_t size, uint8_t out[RL_MAC_BYTES]);

#endif
