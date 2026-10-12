#ifndef CNET_RESEARCH_CMETA_MAC_H
#define CNET_RESEARCH_CMETA_MAC_H

#include "mac_lease.h"

/* Production ABI consumer experiment: no direct GmSSL or provider headers. */
bool rl_cmeta_mac(void *mac_user, const uint8_t key[RL_KEY_BYTES],
                  const void *bytes, size_t size, uint8_t out[RL_MAC_BYTES]);

#endif
