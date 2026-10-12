#ifndef CNET_RESEARCH_TEST_MAC_H
#define CNET_RESEARCH_TEST_MAC_H
#include "../reliable.h"

/* Test-only real HMAC-SHA256 fixture; it is NOT a production key authority. */
bool fixture_hmac_sha256(void *user, const uint8_t key[RL_KEY_BYTES],
                         const void *bytes, size_t size, uint8_t out[RL_MAC_BYTES]);
void fixture_set_directional_keys(rl_config *config);
#endif
