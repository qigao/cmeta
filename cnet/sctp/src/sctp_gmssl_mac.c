#include "sctp_gmssl_mac_internal.h"

#include <gmssl/sm3.h>
#include <string.h>

_Static_assert(SM3_HMAC_SIZE == CNET_SCTP_ASSOC_MAC_BYTES,
               "GmSSL SM3-HMAC output must match the cookie MAC");

/* HMAC-specific domain separation, including the cookie format version.
 * This is intentionally not a general-purpose HMAC provider or MAC key.
 */
static const uint8_t cookie_domain[] = "Salts/CNetSctp/COOKIE/v1";

static void wipe(void *memory, size_t length) {
  volatile uint8_t *bytes = (volatile uint8_t *)memory;
  while (length != 0u) {
    *bytes++ = 0u;
    --length;
  }
}

int cnet_sctp_gmssl_mac_init(cnet_sctp_gmssl_mac *provider,
                            const uint8_t *key, size_t key_length) {
  size_t i;
  uint8_t nonzero = 0u;
  if (!provider || !key || key_length != CNET_SCTP_GMSSL_KEY_BYTES ||
      provider->active != 0u ||
      ((const void *)provider == (const void *)key))
    return -1;
  for (i = 0u; i < key_length; ++i) nonzero |= key[i];
  if (nonzero == 0u) return -1;
  memcpy(provider->key, key, key_length);
  provider->active = 1u;
  return 0;
}

int cnet_sctp_gmssl_mac_sign(void *context,
                            const uint8_t *message, size_t message_size,
                            uint8_t out[CNET_SCTP_ASSOC_MAC_BYTES]) {
  cnet_sctp_gmssl_mac *provider = (cnet_sctp_gmssl_mac *)context;
  SM3_HMAC_CTX hmac;
  uint8_t mac[SM3_HMAC_SIZE];
  if (!provider || provider->active != 1u ||
      !out || (!message && message_size != 0u))
    return -1;
  sm3_hmac_init(&hmac, provider->key, CNET_SCTP_GMSSL_KEY_BYTES);
  sm3_hmac_update(&hmac, cookie_domain, sizeof(cookie_domain) - 1u);
  if (message_size != 0u) sm3_hmac_update(&hmac, message, message_size);
  sm3_hmac_finish(&hmac, mac);
  memcpy(out, mac, sizeof(mac));
  wipe(mac, sizeof(mac));
  wipe(&hmac, sizeof(hmac));
  return 0;
}

void cnet_sctp_gmssl_mac_retire(cnet_sctp_gmssl_mac *provider) {
  if (!provider) return;
  wipe(provider, sizeof(*provider));
}
