#include <cnet/sctp_session.h>
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

/* Research-only public opaque Session + real POSIX DSO lease gate.
 * A test entropy callback executes inside a dlopen'd module while another
 * thread posts cancel intent. The canceller never calls unsynchronized
 * Session APIs. This is NOT production CNet/GmSSL/DTLS integration. */
enum { SESSION_DSO_ROUNDS = 24 };
enum { PROVIDER_OK = 0, PROVIDER_BUSY = 1, PROVIDER_STALE = 2 };

typedef struct session_provider_fixture {
  pthread_mutex_t mutex;
  pthread_cond_t changed;
  pthread_t owner_thread;
  uint32_t generation;
  void *handle;
  const cnet_sctp_test_dso_api *api;
  cnet_sctp_test_dso_bridge bridge;
  cnet_sctp_session *session;
  unsigned active_calls;
  unsigned callback_entered;
  unsigned release_callback;
  unsigned cancel_requested;
  unsigned created;
  unsigned revoked;
  unsigned revoked_checked;
  unsigned allow_cleanup;
  unsigned owner_destroyed;
  unsigned unloaded;
} session_provider_fixture;

static int begin_provider_call(session_provider_fixture *f,
    uint32_t expected, const cnet_sctp_test_dso_api **api) {
  int result = PROVIDER_OK;
  assert(pthread_mutex_lock(&f->mutex) == 0);
  if (expected != f->generation)
    result = PROVIDER_STALE;
  else if (!f->handle || f->cancel_requested || f->owner_destroyed)
    result = PROVIDER_BUSY;
  else {
    ++f->active_calls;
    *api = f->api;
  }
  assert(pthread_mutex_unlock(&f->mutex) == 0);
  return result;
}
static void end_provider_call(session_provider_fixture *f) {
  assert(pthread_mutex_lock(&f->mutex) == 0);
  assert(f->active_calls != 0u);
  --f->active_calls;
  assert(pthread_cond_broadcast(&f->changed) == 0);
  assert(pthread_mutex_unlock(&f->mutex) == 0);
}
static int session_owner(void *context) {
  session_provider_fixture *f = (session_provider_fixture *)context;
  int admitted;
  if (!pthread_equal(pthread_self(), f->owner_thread)) return 0;
  assert(pthread_mutex_lock(&f->mutex) == 0);
  admitted = !f->revoked;
  assert(pthread_mutex_unlock(&f->mutex) == 0);
  return admitted;
}
static int borrowed_entropy(void *context, uint8_t *bytes, size_t size) {
  session_provider_fixture *f = (session_provider_fixture *)context;
  const cnet_sctp_test_dso_api *api = NULL;
  int result;
  if (begin_provider_call(f, f->generation, &api) != PROVIDER_OK)
    return -1;
  result = api->entropy(&f->bridge, bytes, size);
  end_provider_call(f);
  return result;
}
static int borrowed_mac(void *context, const uint8_t *message, size_t size,
    uint8_t output[CNET_SCTP_SESSION_MAC_BYTES]) {
  session_provider_fixture *f = (session_provider_fixture *)context;
  const cnet_sctp_test_dso_api *api = NULL;
  int result;
  if (begin_provider_call(f, f->generation, &api) != PROVIDER_OK)
    return -1;
  result = api->mac(&f->bridge, message, size, output);
  end_provider_call(f);
  return result;
}
static int borrowed_verify(void *context, const uint8_t *message,
    size_t size, const uint8_t mac[CNET_SCTP_SESSION_MAC_BYTES]) {
  session_provider_fixture *f = (session_provider_fixture *)context;
  const cnet_sctp_test_dso_api *api = NULL;
  int result;
  if (begin_provider_call(f, f->generation, &api) != PROVIDER_OK)
    return -1;
  result = api->verify(&f->bridge, message, size, mac);
  end_provider_call(f);
  return result;
}
static int unload_admission(session_provider_fixture *f) {
  int result;
  assert(pthread_mutex_lock(&f->mutex) == 0);
  result = (!f->handle || f->active_calls || !f->owner_destroyed) ?
      PROVIDER_BUSY : PROVIDER_OK;
  assert(pthread_mutex_unlock(&f->mutex) == 0);
  return result;
}
static void *owner_main(void *context) {
  session_provider_fixture *f = (session_provider_fixture *)context;
  cnet_sctp_session_create_config config = {0};
  cnet_sctp_session_event event = {0};
  cnet_sctp_session_terminal_failure failure = {0};
  cnet_sctp_session_peer_abort abort_info = {0};
  uint8_t packet[CNET_SCTP_SESSION_MAX_PACKET_BYTES];
  uint8_t message[16], reason[16];
  size_t written;
  f->owner_thread = pthread_self();
  config.abi_version = CNET_SCTP_SESSION_ABI_VERSION;
  config.struct_size = sizeof(config);
  config.local_port = 5000u;
  config.remote_port = 5001u;
  config.receive_window = 8192u;
  config.outbound_streams = 2u;
  config.inbound_streams = 2u;
  config.t1_ms = 100u;
  config.t1_max_ms = 400u;
  config.cookie_lifetime_ms = 1000u;
  config.peer_binding = UINT64_C(0x9988776655443322);
  config.max_retransmits = 3u;
  config.is_dtls_client = 1u;
  config.is_owner = session_owner;
  config.entropy = borrowed_entropy;
  config.mac = borrowed_mac;
  config.verify_mac = borrowed_verify;
  config.provider_context = f;

  /* The DSO entropy call blocks. Cancel intent is posted during this
   * callback; the Owner is still admitted until create has committed. */
  assert(cnet_sctp_session_create(&config, 0u, &f->session) ==
      CNET_SCTP_SESSION_OK);
  assert(f->session != NULL);
  assert(pthread_mutex_lock(&f->mutex) == 0);
  assert(f->cancel_requested && f->active_calls == 0u);
  f->created = 1u;
  assert(pthread_cond_broadcast(&f->changed) == 0);
  while (!f->revoked)
    assert(pthread_cond_wait(&f->changed, &f->mutex) == 0);
  assert(pthread_mutex_unlock(&f->mutex) == 0);

  /* The external Owner generation is now revoked. This same physical
   * thread must still be rejected by the public opaque Session, including
   * stop/destroy; a rejected destroy MUST leave the handle allocated. */
  written = 123u;
  assert(cnet_sctp_session_poll(f->session, 1u, &event, message,
      sizeof(message), packet, sizeof(packet), &written) ==
      CNET_SCTP_SESSION_OWNER && written == 0u &&
      event.kind == CNET_SCTP_SESSION_EVENT_NONE);
  assert(cnet_sctp_session_take_terminal_failure(f->session, &failure) ==
      CNET_SCTP_SESSION_OWNER && failure.generation == 0u);
  written = 123u;
  assert(cnet_sctp_session_take_peer_abort(f->session, &abort_info,
      reason, sizeof(reason), &written) == CNET_SCTP_SESSION_OWNER &&
      written == 0u && abort_info.has_user_reason == 0u);
  assert(cnet_sctp_session_start(f->session, 1u, packet,
      sizeof(packet), &written) == CNET_SCTP_SESSION_OWNER);
  assert(cnet_sctp_session_stop(f->session) == CNET_SCTP_SESSION_OWNER);
  assert(cnet_sctp_session_destroy(f->session) == CNET_SCTP_SESSION_OWNER);
  assert(!cnet_sctp_session_drained(f->session));

  assert(pthread_mutex_lock(&f->mutex) == 0);
  f->revoked_checked = 1u;
  assert(pthread_cond_broadcast(&f->changed) == 0);
  while (!f->allow_cleanup)
    assert(pthread_cond_wait(&f->changed, &f->mutex) == 0);
  assert(!f->revoked);
  assert(pthread_mutex_unlock(&f->mutex) == 0);
  /* Only an explicit synchronized Owner hand-back permits stop/drain and
   * destroy. No Provider unload can precede this barrier. */
  assert(cnet_sctp_session_stop(f->session) == CNET_SCTP_SESSION_OK);
  assert(cnet_sctp_session_drained(f->session));
  assert(cnet_sctp_session_destroy(f->session) == CNET_SCTP_SESSION_OK);
  f->session = NULL;
  assert(pthread_mutex_lock(&f->mutex) == 0);
  f->owner_destroyed = 1u;
  assert(pthread_cond_broadcast(&f->changed) == 0);
  assert(pthread_mutex_unlock(&f->mutex) == 0);
  return NULL;
}
static void *canceller_main(void *context) {
  session_provider_fixture *f = (session_provider_fixture *)context;
  const cnet_sctp_test_dso_api *denied_api = NULL;
  void *old_handle;
  assert(pthread_mutex_lock(&f->mutex) == 0);
  while (!f->callback_entered)
    assert(pthread_cond_wait(&f->changed, &f->mutex) == 0);
  assert(f->active_calls == 1u && !f->created && !f->owner_destroyed);
  f->cancel_requested = 1u;
  assert(pthread_cond_broadcast(&f->changed) == 0);
  assert(pthread_mutex_unlock(&f->mutex) == 0);

  /* In-flight callback still executes DSO code. Cancel rejects NEW
   * borrowed callbacks, but cannot unload the executable code. */
  assert(unload_admission(f) == PROVIDER_BUSY);
  assert(begin_provider_call(f, f->generation, &denied_api) ==
      PROVIDER_BUSY && denied_api == NULL);
  assert(pthread_mutex_lock(&f->mutex) == 0);
  f->release_callback = 1u;
  assert(pthread_cond_broadcast(&f->changed) == 0);
  while (!f->created || f->active_calls != 0u)
    assert(pthread_cond_wait(&f->changed, &f->mutex) == 0);
  assert(!f->owner_destroyed && f->handle && f->api);
  f->revoked = 1u;
  assert(pthread_cond_broadcast(&f->changed) == 0);
  while (!f->revoked_checked)
    assert(pthread_cond_wait(&f->changed, &f->mutex) == 0);
  assert(pthread_mutex_unlock(&f->mutex) == 0);

  /* Even after callback drain, Session retains borrowed provider function
   * pointers. A revoked Owner cannot stop/destroy it: unload stays BUSY. */
  assert(unload_admission(f) == PROVIDER_BUSY);
  assert(pthread_mutex_lock(&f->mutex) == 0);
  f->revoked = 0u;
  f->allow_cleanup = 1u;
  assert(pthread_cond_broadcast(&f->changed) == 0);
  while (!f->owner_destroyed || f->active_calls != 0u)
    assert(pthread_cond_wait(&f->changed, &f->mutex) == 0);
  assert(pthread_mutex_unlock(&f->mutex) == 0);
  assert(unload_admission(f) == PROVIDER_OK);

  assert(pthread_mutex_lock(&f->mutex) == 0);
  old_handle = f->handle;
  f->api = NULL;
  f->handle = NULL;
  f->unloaded = 1u;
  assert(pthread_mutex_unlock(&f->mutex) == 0);
  assert(dlclose(old_handle) == 0);
  return NULL;
}
static void run_generation(const char *path, uint32_t generation) {
  session_provider_fixture f = {0};
  pthread_t owner, canceller;
  const cnet_sctp_test_dso_api *stale_api = NULL;
  void *symbol;

  assert(pthread_mutex_init(&f.mutex, NULL) == 0);
  assert(pthread_cond_init(&f.changed, NULL) == 0);
  f.generation = generation;
  f.bridge.mutex = &f.mutex;
  f.bridge.changed = &f.changed;
  f.bridge.callback_entered = &f.callback_entered;
  f.bridge.release_callback = &f.release_callback;
  f.bridge.generation = generation;
  f.handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
  assert(f.handle != NULL);
  symbol = dlsym(f.handle, "cnet_sctp_test_provider_dso_v1");
  assert(symbol != NULL);
  f.api = (const cnet_sctp_test_dso_api *)symbol;
  assert(f.api->abi_version == CNET_SCTP_TEST_DSO_ABI &&
      f.api->entropy && f.api->mac && f.api->verify);
  assert(begin_provider_call(&f, generation - 1u, &stale_api) ==
      PROVIDER_STALE && stale_api == NULL);

  assert(pthread_create(&canceller, NULL, canceller_main, &f) == 0);
  assert(pthread_create(&owner, NULL, owner_main, &f) == 0);
  assert(pthread_join(owner, NULL) == 0);
  assert(pthread_join(canceller, NULL) == 0);
  assert(f.owner_destroyed && f.unloaded && f.session == NULL);
  assert(f.active_calls == 0u && f.handle == NULL && f.api == NULL);
  assert(unload_admission(&f) == PROVIDER_BUSY);
  assert(pthread_cond_destroy(&f.changed) == 0);
  assert(pthread_mutex_destroy(&f.mutex) == 0);
}
int main(int argc, char **argv) {
  uint32_t generation;
  assert(argc == 2 && argv[1] != NULL);
  for (generation = 1u; generation <= SESSION_DSO_ROUNDS; ++generation)
    run_generation(argv[1], generation);
  puts("CNet SCTP opaque Session real DSO cancel/Owner drain/dlclose: PASS");
  return 0;
}
