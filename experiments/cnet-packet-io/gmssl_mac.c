#include "gmssl_mac.h"

#include <gmssl/digest.h>
#include <gmssl/hmac.h>
#include <gmssl/mem.h>

#include <string.h>

bool rl_gmssl_mac(void *mac_user, const uint8_t key[RL_KEY_BYTES],
                  const void *bytes, size_t size, uint8_t out[RL_MAC_BYTES]) {
  rl_mac_lease *lease = (rl_mac_lease *)mac_user;
  uint8_t temporary[RL_MAC_BYTES] = {0};
  size_t mac_length = 0;
  bool ok = false;
  if (!key || !bytes || !out || size < RL_WIRE_HEADER_BYTES ||
      size > RL_WIRE_HEADER_BYTES + RL_FRAGMENT_BYTES ||
      !rl_mac_lease_enter(lease, rl_gmssl_mac)) return false;
  if (hmac(DIGEST_sha256(), key, RL_KEY_BYTES,
           (const uint8_t *)bytes, size, temporary, &mac_length) == 1 &&
      mac_length == RL_MAC_BYTES) {
    memcpy(out, temporary, RL_MAC_BYTES);
    ok = true;
  }
  gmssl_secure_clear(temporary, sizeof(temporary));
  rl_mac_lease_leave(lease);
  return ok;
}
