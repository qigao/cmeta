#ifndef CNET_SCTP_TEST_PROVIDER_DSO_H
#define CNET_SCTP_TEST_PROVIDER_DSO_H

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

/* Private test-only POSIX DSO ABI; never installed with Wire or Salts. */
#define CNET_SCTP_TEST_DSO_ABI UINT32_C(1)
#define CNET_SCTP_TEST_DSO_MAC_BYTES 32u

typedef struct cnet_sctp_test_dso_bridge {
  pthread_mutex_t *mutex;
  pthread_cond_t *changed;
  unsigned *callback_entered;
  unsigned *release_callback;
  uint32_t generation;
} cnet_sctp_test_dso_bridge;

typedef struct cnet_sctp_test_dso_api {
  uint32_t abi_version;
  int (*entropy)(void *context, uint8_t *bytes, size_t size);
  int (*mac)(void *context, const uint8_t *message, size_t size,
             uint8_t out[CNET_SCTP_TEST_DSO_MAC_BYTES]);
  int (*verify)(void *context, const uint8_t *message, size_t size,
                const uint8_t mac[CNET_SCTP_TEST_DSO_MAC_BYTES]);
} cnet_sctp_test_dso_api;

extern const cnet_sctp_test_dso_api cnet_sctp_test_provider_dso_v1;
#endif
