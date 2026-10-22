#include "sctp_assoc_internal.h"
#include "provider_dso_fixture.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { TEST_DSO_GENERATIONS = 24 };
enum { PROVIDER_OK = 0, PROVIDER_BUSY = 1, PROVIDER_STALE = 2 };

typedef struct provider_generation {
  pthread_mutex_t mutex;
  pthread_cond_t changed;
  pthread_t owner_thread;
  uint32_t generation;
  void *handle;
  const cnet_sctp_test_dso_api *api;
  cnet_sctp_test_dso_bridge bridge;
  unsigned active_calls;
  unsigned callback_entered;
  unsigned release_callback;
  unsigned cancel_requested;
  unsigned callback_returned;
  unsigned allow_owner_stop;
  unsigned owner_destroyed;
  unsigned unloaded;
  cnet_sctp_assoc assoc;
} provider_generation;

/* Caller synchronizes this lease with the same mutex used for drain. No
 * unsafe Association API is invoked from a non-Owner thread. */
static int begin_provider_call(provider_generation *g, uint32_t expected,
    const cnet_sctp_test_dso_api **api) {
  int result = PROVIDER_OK;
  assert(pthread_mutex_lock(&g->mutex) == 0);
  if (expected != g->generation)
    result = PROVIDER_STALE;
  else if (!g->handle || g->cancel_requested || g->owner_destroyed)
    result = PROVIDER_BUSY;
  else {
    ++g->active_calls;
    *api = g->api;
  }
  assert(pthread_mutex_unlock(&g->mutex) == 0);
  return result;
}
static void end_provider_call(provider_generation *g) {
  assert(pthread_mutex_lock(&g->mutex) == 0);
  assert(g->active_calls != 0u);
  --g->active_calls;
  assert(pthread_cond_broadcast(&g->changed) == 0);
  assert(pthread_mutex_unlock(&g->mutex) == 0);
}
static int borrowed_entropy(void *context, uint8_t *bytes, size_t size) {
  provider_generation *g = (provider_generation *)context;
  const cnet_sctp_test_dso_api *api = NULL;
  int status, result;
  status = begin_provider_call(g, g->generation, &api);
  if (status != PROVIDER_OK) return -1;
  result = api->entropy(&g->bridge, bytes, size);
  end_provider_call(g);
  return result;
}
static int borrowed_mac(void *context, const uint8_t *message, size_t size,
    uint8_t out[CNET_SCTP_ASSOC_MAC_BYTES]) {
  provider_generation *g = (provider_generation *)context;
  const cnet_sctp_test_dso_api *api = NULL;
  int result;
  if (begin_provider_call(g, g->generation, &api) != PROVIDER_OK) return -1;
  result = api->mac(&g->bridge, message, size, out);
  end_provider_call(g);
  return result;
}
static int borrowed_verify(void *context, const uint8_t *message, size_t size,
    const uint8_t mac[CNET_SCTP_ASSOC_MAC_BYTES]) {
  provider_generation *g = (provider_generation *)context;
  const cnet_sctp_test_dso_api *api = NULL;
  int result;
  if (begin_provider_call(g, g->generation, &api) != PROVIDER_OK) return -1;
  result = api->verify(&g->bridge, message, size, mac);
  end_provider_call(g);
  return result;
}
static int owner_check(void *context) {
  provider_generation *g = (provider_generation *)context;
  return pthread_equal(pthread_self(), g->owner_thread);
}

static int try_unload(provider_generation *g) {
  int status;
  assert(pthread_mutex_lock(&g->mutex) == 0);
  /* The precondition is not merely zero calls: the Owner must have
   * stopped, drained and destroyed the Association. No borrowed callback
   * pointer can survive past the generation barrier. */
  status = (!g->handle || g->active_calls || !g->owner_destroyed) ?
      PROVIDER_BUSY : PROVIDER_OK;
  assert(pthread_mutex_unlock(&g->mutex) == 0);
  return status;
}
static void *owner_main(void *context) {
  provider_generation *g = (provider_generation *)context;
  cnet_sctp_assoc_config c = {0};
  g->owner_thread = pthread_self();
  c.local_port = 5000u;
  c.remote_port = 5001u;
  c.receive_window = 8192u;
  c.outbound_streams = 2u;
  c.inbound_streams = 2u;
  c.t1_ms = 100u;
  c.t1_max_ms = 400u;
  c.cookie_lifetime_ms = 1000u;
  c.peer_binding = UINT64_C(0x9988776655443322);
  c.max_retransmits = 3u;
  c.is_owner = owner_check;
  c.entropy = borrowed_entropy;
  c.mac = borrowed_mac;
  c.verify_mac = borrowed_verify;
  c.provider_context = g;

  /* The DSO function blocks in its own code during this initialization. */
  assert(cnet_sctp_assoc_init(&g->assoc, &c, 0u) == CNET_SCTP_ASSOC_OK);
  assert(g->assoc.magic != 0u);
  assert(pthread_mutex_lock(&g->mutex) == 0);
  assert(g->cancel_requested && g->active_calls == 0u);
  /* Hand the canceller an independent observation point: the DSO callback
   * has returned but the live Owner has NOT stopped/destroyed its TCB. */
  g->callback_returned = 1u;
  assert(pthread_cond_broadcast(&g->changed) == 0);
  while (!g->allow_owner_stop)
    assert(pthread_cond_wait(&g->changed, &g->mutex) == 0);
  assert(pthread_mutex_unlock(&g->mutex) == 0);
  assert(cnet_sctp_assoc_stop(&g->assoc) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_drained(&g->assoc));
  assert(cnet_sctp_assoc_destroy(&g->assoc) == CNET_SCTP_ASSOC_OK);
  assert(g->assoc.magic == 0u);
  assert(pthread_mutex_lock(&g->mutex) == 0);
  g->owner_destroyed = 1u;
  assert(pthread_cond_broadcast(&g->changed) == 0);
  assert(pthread_mutex_unlock(&g->mutex) == 0);
  return NULL;
}
static void *canceller_main(void *context) {
  provider_generation *g = (provider_generation *)context;
  assert(pthread_mutex_lock(&g->mutex) == 0);
  while (!g->callback_entered)
    assert(pthread_cond_wait(&g->changed, &g->mutex) == 0);
  /* The Owner's function pointer is currently executing inside the DSO. */
  assert(g->active_calls == 1u && !g->owner_destroyed && !g->unloaded);
  g->cancel_requested = 1u;
  assert(pthread_cond_broadcast(&g->changed) == 0);
  assert(pthread_mutex_unlock(&g->mutex) == 0);
  assert(try_unload(g) == PROVIDER_BUSY);
  /* Cancellation closes admission to all NEW provider calls immediately.
   * Existing callback finishes under its generation lease. */
  {
    const cnet_sctp_test_dso_api *rejected_api = NULL;
    assert(begin_provider_call(g, g->generation,
        &rejected_api) == PROVIDER_BUSY);
    assert(rejected_api == NULL);
  }
  assert(pthread_mutex_lock(&g->mutex) == 0);
  g->release_callback = 1u;
  assert(pthread_cond_broadcast(&g->changed) == 0);
  while (!g->callback_returned || g->active_calls)
    assert(pthread_cond_wait(&g->changed, &g->mutex) == 0);
  assert(!g->owner_destroyed && g->handle && g->api);
  assert(pthread_mutex_unlock(&g->mutex) == 0);
  /* The callback is fully drained, but the TCB still holds Provider function
   * pointers. Generation unload MUST remain BUSY until Owner destruction. */
  assert(try_unload(g) == PROVIDER_BUSY);
  assert(pthread_mutex_lock(&g->mutex) == 0);
  g->allow_owner_stop = 1u;
  assert(pthread_cond_broadcast(&g->changed) == 0);
  while (!g->owner_destroyed || g->active_calls)
    assert(pthread_cond_wait(&g->changed, &g->mutex) == 0);
  assert(g->handle && g->api && !g->unloaded);
  /* Once the Owner has destroyed the TCB and all callbacks have returned,
   * close this DSO reference. Clear every function pointer before dlclose. */
  {
    void *handle = g->handle;
    g->api = NULL;
    g->handle = NULL;
    g->unloaded = 1u;
    assert(pthread_mutex_unlock(&g->mutex) == 0);
    assert(dlclose(handle) == 0);
  }
  return NULL;
}

static void exercise_generation(const char *dso_path, uint32_t generation) {
  provider_generation g = {0};
  pthread_t owner, canceller;
  void *symbol;
  const cnet_sctp_test_dso_api *api;
  uint8_t mac[CNET_SCTP_ASSOC_MAC_BYTES];
  static const uint8_t message[4] = {0x11u, 0x22u, 0x33u, 0x44u};

  assert(pthread_mutex_init(&g.mutex, NULL) == 0);
  assert(pthread_cond_init(&g.changed, NULL) == 0);
  g.generation = generation;
  g.bridge.mutex = &g.mutex;
  g.bridge.changed = &g.changed;
  g.bridge.callback_entered = &g.callback_entered;
  g.bridge.release_callback = &g.release_callback;
  g.bridge.generation = generation;

  g.handle = dlopen(dso_path, RTLD_NOW | RTLD_LOCAL);
  assert(g.handle != NULL);
  symbol = dlsym(g.handle, "cnet_sctp_test_provider_dso_v1");
  assert(symbol != NULL);
  api = (const cnet_sctp_test_dso_api *)symbol;
  assert(api->abi_version == CNET_SCTP_TEST_DSO_ABI);
  assert(api->entropy && api->mac && api->verify);
  g.api = api;
  /* A stale caller token cannot acquire the newer loaded generation even
   * while its DSO handle and function pointers are still valid. */
  {
    const cnet_sctp_test_dso_api *stale_api = NULL;
    assert(begin_provider_call(&g, generation - 1u,
        &stale_api) == PROVIDER_STALE);
    assert(stale_api == NULL);
  }
  /* Verify the real DSO MAC callbacks (test-only insecure algorithm), not
   * just symbol presence. No callback is retained across the close barrier. */
  assert(borrowed_mac(&g, message, sizeof(message), mac) == 0);
  assert(borrowed_verify(&g, message, sizeof(message), mac) == 0);
  mac[0] ^= 1u;
  assert(borrowed_verify(&g, message, sizeof(message), mac) != 0);

  assert(pthread_create(&canceller, NULL, canceller_main, &g) == 0);
  assert(pthread_create(&owner, NULL, owner_main, &g) == 0);
  assert(pthread_join(canceller, NULL) == 0);
  assert(pthread_join(owner, NULL) == 0);
  assert(g.cancel_requested && g.owner_destroyed && g.unloaded);
  assert(g.active_calls == 0u && g.handle == NULL && g.api == NULL);
  assert(g.assoc.magic == 0u);
  assert(try_unload(&g) == PROVIDER_BUSY); /* cannot double-dlclose */
  {
    const cnet_sctp_test_dso_api *stale = NULL;
    assert(begin_provider_call(&g, generation - 1u, &stale) ==
        PROVIDER_STALE);
    assert(stale == NULL);
    assert(begin_provider_call(&g, generation, &stale) ==
        PROVIDER_BUSY);
    assert(stale == NULL);
  }
  assert(pthread_cond_destroy(&g.changed) == 0);
  assert(pthread_mutex_destroy(&g.mutex) == 0);
}
int main(int argc, char **argv) {
  uint32_t generation;
  assert(argc == 2 && argv[1] != NULL);
  for (generation = 1u; generation <= TEST_DSO_GENERATIONS; ++generation)
    exercise_generation(argv[1], generation);
  puts("CNet SCTP real DSO callback/Generation drain/dlclose: PASS");
  return 0;
}
