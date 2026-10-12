#include "cmeta_mac.h"
#include <cmeta_crypto.h>
#include <cmeta_error.h>
#include <string.h>

bool rl_cmeta_mac(void *mac_user, const uint8_t key[RL_KEY_BYTES],
                  const void *bytes, size_t size, uint8_t out[RL_MAC_BYTES]) {
  rl_mac_lease *lease = (rl_mac_lease *)mac_user;
  uint8_t temporary[RL_MAC_BYTES] = {0};
  bool ok = false;
  if (!key || !bytes || !out || size < RL_WIRE_HEADER_BYTES ||
      size > RL_WIRE_HEADER_BYTES + RL_FRAGMENT_BYTES ||
      !rl_mac_lease_enter(lease, rl_cmeta_mac)) return false;
  if (cmeta_hmac_sha256(key, RL_KEY_BYTES, bytes, size, temporary) == SALTS_OK) {
    memcpy(out, temporary, RL_MAC_BYTES);
    ok = true;
  }
  cmeta_crypto_clear(temporary, sizeof(temporary));
  rl_mac_lease_leave(lease);
  return ok;
}
