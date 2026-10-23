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

static void calculate_mac(const uint8_t key[CNET_SCTP_GMSSL_KEY_BYTES],
                          const uint8_t *message, size_t message_size,
                          uint8_t out[CNET_SCTP_ASSOC_MAC_BYTES]) {
  SM3_HMAC_CTX hmac;
  sm3_hmac_init(&hmac, key, CNET_SCTP_GMSSL_KEY_BYTES);
  sm3_hmac_update(&hmac, cookie_domain, sizeof(cookie_domain) - 1u);
  if (message_size != 0u) sm3_hmac_update(&hmac, message, message_size);
  sm3_hmac_finish(&hmac, out);
  wipe(&hmac, sizeof(hmac));
}

static int ct_equal(const uint8_t *a, const uint8_t *b, size_t n) {
  size_t i;
  uint8_t diff = 0u;
  for (i = 0u; i < n; ++i) diff |= (uint8_t)(a[i] ^ b[i]);
  return diff == 0u;
}

int cnet_sctp_gmssl_mac_sign(void *context,
                            const uint8_t *message, size_t message_size,
                            uint8_t out[CNET_SCTP_ASSOC_MAC_BYTES]) {
  cnet_sctp_gmssl_mac *provider = (cnet_sctp_gmssl_mac *)context;
  if (!provider || provider->active != 1u || !out ||
      (!message && message_size != 0u)) return -1;
  calculate_mac(provider->key, message, message_size, out);
  return 0;
}

int cnet_sctp_gmssl_mac_verify(void *context,
                              const uint8_t *message, size_t message_size,
                              const uint8_t claimed[CNET_SCTP_ASSOC_MAC_BYTES]) {
  cnet_sctp_gmssl_mac *provider = (cnet_sctp_gmssl_mac *)context;
  uint8_t current[CNET_SCTP_ASSOC_MAC_BYTES];
  uint8_t prior[CNET_SCTP_ASSOC_MAC_BYTES];
  int matches;
  if (!provider || provider->active != 1u || !claimed ||
      (!message && message_size != 0u)) return -1;
  calculate_mac(provider->key, message, message_size, current);
  /* Always compute both HMACs so rejection does not short-circuit based on
   * which key matched. When no old key exists, recompute with active key. */
  calculate_mac(provider->previous_active ? provider->previous_key : provider->key,
                message, message_size, prior);
  matches = ct_equal(current, claimed, sizeof(current)) |
            ((provider->previous_active == 1u) & ct_equal(prior, claimed, sizeof(prior)));
  wipe(current, sizeof(current));
  wipe(prior, sizeof(prior));
  return matches ? 0 : -1;
}

int cnet_sctp_gmssl_mac_rotate(cnet_sctp_gmssl_mac *provider,
                              const uint8_t *new_key, size_t key_length) {
  uint8_t replacement[CNET_SCTP_GMSSL_KEY_BYTES];
  size_t i;
  uint8_t nonzero = 0u;
  if (!provider || provider->active != 1u || provider->previous_active != 0u ||
      !new_key || key_length != CNET_SCTP_GMSSL_KEY_BYTES) return -1;
  memcpy(replacement, new_key, sizeof(replacement));
  for (i = 0u; i < sizeof(replacement); ++i) nonzero |= replacement[i];
  if (!nonzero || ct_equal(replacement, provider->key, sizeof(replacement))) {
    wipe(replacement, sizeof(replacement));
    return -1;
  }
  memcpy(provider->previous_key, provider->key, sizeof(provider->key));
  memcpy(provider->key, replacement, sizeof(provider->key));
  provider->previous_active = 1u;
  wipe(replacement, sizeof(replacement));
  return 0;
}

void cnet_sctp_gmssl_mac_retire_previous(cnet_sctp_gmssl_mac *provider) {
  if (!provider) return;
  wipe(provider->previous_key, sizeof(provider->previous_key));
  provider->previous_active = 0u;
}

void cnet_sctp_gmssl_mac_retire(cnet_sctp_gmssl_mac *provider) {
  if (!provider) return;
  wipe(provider, sizeof(*provider));
}
