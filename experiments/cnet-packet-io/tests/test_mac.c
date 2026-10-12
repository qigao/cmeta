#include "test_mac.h"
#include <openssl/evp.h>
#include <openssl/hmac.h>

bool fixture_hmac_sha256(void *user, const uint8_t key[RL_KEY_BYTES],
                         const void *bytes, size_t size, uint8_t out[RL_MAC_BYTES]) {
  unsigned int written = 0;
  (void)user;
  return HMAC(EVP_sha256(), key, RL_KEY_BYTES, (const unsigned char *)bytes,
              size, out, &written) != NULL && written == RL_MAC_BYTES;
}

void fixture_set_directional_keys(rl_config *config) {
  for (unsigned i = 0; i < RL_KEY_BYTES; ++i) {
    /* Fixed vectors for reproducible tests only. Never deploy as keys. */
    config->tx_auth_key[i] = (uint8_t)((config->local_id < config->peer_id ? 0x31u : 0x71u) + i);
    config->rx_auth_key[i] = (uint8_t)((config->local_id < config->peer_id ? 0x71u : 0x31u) + i);
  }
}
