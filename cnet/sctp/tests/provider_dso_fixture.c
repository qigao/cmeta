#include "provider_dso_fixture.h"
#include <string.h>

/* Real dynamically loaded test provider, not GmSSL and not cryptographic.
 * The entropy callback deliberately blocks inside this DSO, proving that
 * the host's drain barrier prevents dlclose while executable code is live. */
static int provider_entropy(void *context, uint8_t *bytes, size_t count) {
  cnet_sctp_test_dso_bridge *bridge = (cnet_sctp_test_dso_bridge *)context;
  uint32_t generation;
  if (!bridge || !bytes || count != 8u) return -1;
  if (pthread_mutex_lock(bridge->mutex) != 0) return -1;
  *bridge->callback_entered = 1u;
  if (pthread_cond_broadcast(bridge->changed) != 0) {
    (void)pthread_mutex_unlock(bridge->mutex);
    return -1;
  }
  while (!*bridge->release_callback)
    if (pthread_cond_wait(bridge->changed, bridge->mutex) != 0) {
      (void)pthread_mutex_unlock(bridge->mutex);
      return -1;
    }
  generation = bridge->generation;
  if (pthread_mutex_unlock(bridge->mutex) != 0) return -1;
  /* Stable, non-zero Tag/TSN, uniquely tagged per fixture generation. */
  bytes[0] = 0x11u;
  bytes[1] = 0x22u;
  bytes[2] = 0x33u;
  bytes[3] = (uint8_t)generation;
  bytes[4] = 0x44u;
  bytes[5] = 0x55u;
  bytes[6] = 0x66u;
  bytes[7] = (uint8_t)(generation ^ 0xA5u);
  return 0;
}
static int provider_mac(void *context, const uint8_t *message, size_t size,
    uint8_t out[CNET_SCTP_TEST_DSO_MAC_BYTES]) {
  uint32_t hash = UINT32_C(2166136261);
  size_t i;
  (void)context;
  if (!out || (!message && size)) return -1;
  for (i = 0u; i < size; ++i) {
    hash ^= message[i];
    hash *= UINT32_C(16777619);
  }
  for (i = 0u; i < CNET_SCTP_TEST_DSO_MAC_BYTES; ++i) {
    hash ^= (uint32_t)i;
    hash *= UINT32_C(16777619);
    out[i] = (uint8_t)(hash >> 24);
  }
  return 0; /* intentionally non-cryptographic, research fixture only */
}
static int provider_verify(void *context, const uint8_t *message, size_t size,
    const uint8_t mac[CNET_SCTP_TEST_DSO_MAC_BYTES]) {
  uint8_t computed[CNET_SCTP_TEST_DSO_MAC_BYTES];
  uint8_t difference = 0u;
  size_t i;
  if (!mac || provider_mac(context, message, size, computed) != 0) return -1;
  for (i = 0u; i < sizeof(computed); ++i)
    difference |= (uint8_t)(computed[i] ^ mac[i]);
  memset(computed, 0, sizeof(computed));
  return difference == 0u ? 0 : -1;
}

const cnet_sctp_test_dso_api cnet_sctp_test_provider_dso_v1 = {
  CNET_SCTP_TEST_DSO_ABI, provider_entropy, provider_mac, provider_verify
};
