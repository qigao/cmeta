#include "sctp_assoc_internal.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* M5 research-only: actual POSIX threads transfer the single Owner under a
 * caller-managed mutex/condition-variable lease. Never call Association APIs
 * concurrently without this lease: such calls are outside its ownership
 * contract and would themselves be undefined C data races.
 *
 * A real TSan run must instrument Association/Data/this test together.
 * This is NOT a Provider DSO unload/cancel race or independent peer test. */
enum { HANDOFF_ROUNDS = 256, OPERATIONS_PER_ROUND = 6 };

typedef struct owner_fixture {
  pthread_mutex_t mutex;
  pthread_cond_t turn_changed;
  pthread_t owner_thread;
  int owner_valid;
  unsigned ready;
  size_t turn;
  cnet_sctp_assoc assoc;
  cnet_sctp_assoc_config config;
  uint8_t response[1200];
} owner_fixture;

typedef struct worker_arg {
  owner_fixture *fixture;
  unsigned id;
} worker_arg;

static int owner_check(void *context) {
  const owner_fixture *f = (const owner_fixture *)context;
  /* All Association APIs AND owner changes are protected by f->mutex. */
  return f->owner_valid && pthread_equal(pthread_self(), f->owner_thread);
}
static int test_entropy(void *context, uint8_t *out, size_t count) {
  static const uint8_t seed[8] = {
    0x11u, 0x22u, 0x33u, 0x44u, 0x09u, 0x08u, 0x07u, 0x06u
  };
  (void)context;
  if (!out || count != sizeof(seed)) return -1;
  memcpy(out, seed, sizeof(seed));
  return 0; /* deterministic test-only fixture, NOT cryptographic entropy */
}
static int test_mac(void *context, const uint8_t *message, size_t count,
    uint8_t out[CNET_SCTP_ASSOC_MAC_BYTES]) {
  (void)context;
  (void)message;
  (void)count;
  memset(out, 0xA5, CNET_SCTP_ASSOC_MAC_BYTES);
  return 0; /* deliberately insecure; no cookie authentication in this test */
}
static int test_verify(void *context, const uint8_t *message, size_t count,
    const uint8_t mac[CNET_SCTP_ASSOC_MAC_BYTES]) {
  size_t i;
  (void)context;
  (void)message;
  (void)count;
  for (i = 0u; i < CNET_SCTP_ASSOC_MAC_BYTES; ++i)
    if (mac[i] != 0xA5u) return -1;
  return 0;
}

static void configure(owner_fixture *f) {
  memset(&f->config, 0, sizeof(f->config));
  f->config.local_port = 5000u;
  f->config.remote_port = 5001u;
  f->config.receive_window = 8192u;
  f->config.outbound_streams = 2u;
  f->config.inbound_streams = 2u;
  f->config.t1_ms = 100u;
  f->config.t1_max_ms = 400u;
  f->config.max_retransmits = 3u;
  f->config.cookie_lifetime_ms = 1000u;
  f->config.peer_binding = UINT64_C(0x1122334455667788);
  f->config.is_owner = owner_check;
  f->config.entropy = test_entropy;
  f->config.mac = test_mac;
  f->config.verify_mac = test_verify;
  f->config.provider_context = f;
}

static void run_owner_phase(owner_fixture *f, size_t phase) {
  static const uint8_t abort_reason[] = {
    0u, 12u, 0u, 8u, 'p', 'e', 'e', 'r'
  };
  const size_t round = phase / OPERATIONS_PER_ROUND;
  const size_t step = phase % OPERATIONS_PER_ROUND;
  size_t written = 123u, packet_size = 0u, i;
  cnet_sctp_wire_chunk abort_chunk = {6u, 0u,
      abort_reason, sizeof(abort_reason)};
  uint8_t packet[64], reason[8];
  cnet_sctp_assoc_peer_abort_info info = {0};

  switch (step) {
    case 0u:
      assert(f->assoc.magic == 0u); /* previous Owner destroyed it */
      configure(f);
      assert(cnet_sctp_assoc_init(&f->assoc, &f->config, 0u) ==
          CNET_SCTP_ASSOC_OK);
      break;
    case 1u:
      assert(cnet_sctp_assoc_start(&f->assoc, 1u, f->response,
          sizeof(f->response), &written) == CNET_SCTP_ASSOC_OK);
      assert(written == 44u &&
          f->assoc.state == CNET_SCTP_ASSOC_COOKIE_WAIT);
      break;
    case 2u:
      /* Synthetic post-COOKIE state to isolate the Owner lifetime test.
       * The real handshake is covered by the separate Association tests. */
      f->assoc.state = CNET_SCTP_ASSOC_ESTABLISHED;
      f->assoc.peer_tag = UINT32_C(0x87654321);
      assert(cnet_sctp_wire_encode(5001u, 5000u, f->assoc.local_tag,
          &abort_chunk, 1u, packet, sizeof(packet), &packet_size) ==
          CNET_SCTP_WIRE_OK);
      assert(cnet_sctp_assoc_input(&f->assoc, 2u, packet, packet_size,
          f->response, 0u, &written) == CNET_SCTP_ASSOC_OK);
      assert(written == 0u && f->assoc.peer_abort_pending &&
          f->assoc.state == CNET_SCTP_ASSOC_FAILED);
      break;
    case 3u:
      if ((round & 1u) == 0u) {
        memset(reason, 0xCC, sizeof(reason));
        assert(cnet_sctp_assoc_take_peer_abort(&f->assoc, &info,
            reason, 3u, &written) == CNET_SCTP_ASSOC_CAPACITY);
        assert(written == 0u && f->assoc.peer_abort_pending);
        for (i = 0u; i < sizeof(reason); ++i)
          assert(reason[i] == 0xCCu);
        assert(cnet_sctp_assoc_take_peer_abort(&f->assoc, &info,
            reason, 4u, &written) == CNET_SCTP_ASSOC_OK);
        assert(written == 4u && memcmp(reason, "peer", 4u) == 0 &&
            info.has_user_reason == 1u);
      } else {
        /* cancel/stop wins: an unread peer event is explicitly wiped */
        assert(cnet_sctp_assoc_stop(&f->assoc) == CNET_SCTP_ASSOC_OK);
        assert(!f->assoc.peer_abort_pending &&
            f->assoc.peer_abort_reason_size == 0u);
        for (i = 0u; i < sizeof(f->assoc.peer_abort_reason); ++i)
          assert(f->assoc.peer_abort_reason[i] == 0u);
      }
      break;
    case 4u:
      if ((round & 1u) == 0u) {
        assert(cnet_sctp_assoc_take_peer_abort(&f->assoc, &info,
            reason, sizeof(reason), &written) == CNET_SCTP_ASSOC_EMPTY);
        assert(cnet_sctp_assoc_stop(&f->assoc) == CNET_SCTP_ASSOC_OK);
      } else {
        assert(cnet_sctp_assoc_take_peer_abort(&f->assoc, &info,
            reason, sizeof(reason), &written) == CNET_SCTP_ASSOC_EMPTY);
      }
      assert(cnet_sctp_assoc_drained(&f->assoc));
      break;
    case 5u:
      assert(cnet_sctp_assoc_destroy(&f->assoc) == CNET_SCTP_ASSOC_OK);
      assert(f->assoc.magic == 0u &&
          f->assoc.peer_abort_pending == 0u &&
          f->assoc.peer_abort_reason_size == 0u);
      break;
    default:
      assert(0);
  }
}

static void *worker(void *arg_) {
  worker_arg *arg = (worker_arg *)arg_;
  owner_fixture *f = arg->fixture;
  const size_t total = (size_t)HANDOFF_ROUNDS * OPERATIONS_PER_ROUND;

  assert(pthread_mutex_lock(&f->mutex) == 0);
  ++f->ready;
  assert(pthread_cond_broadcast(&f->turn_changed) == 0);
  while (f->ready != 2u)
    assert(pthread_cond_wait(&f->turn_changed, &f->mutex) == 0);

  while (f->turn < total) {
    cnet_sctp_assoc before;
    size_t ignored = 123u;
    while (f->turn < total && (f->turn & 1u) != arg->id)
      assert(pthread_cond_wait(&f->turn_changed, &f->mutex) == 0);
    if (f->turn == total) break;
    /* The previous physical thread still owns the prior phase. Before
     * granting a new lease, the incoming non-Owner is refused without
     * changing any Association bytes, even if stop/cancel is requested. */
    if ((f->turn % OPERATIONS_PER_ROUND) != 0u) {
      assert(!owner_check(f));
      memcpy(&before, &f->assoc, sizeof(before));
      assert(cnet_sctp_assoc_stop(&f->assoc) == CNET_SCTP_ASSOC_OWNER);
      assert(cnet_sctp_assoc_take_peer_abort(&f->assoc, NULL, NULL, 0u,
          &ignored) == CNET_SCTP_ASSOC_OWNER);
      assert(ignored == 0u &&
          memcmp(&before, &f->assoc, sizeof(before)) == 0);
    }
    f->owner_thread = pthread_self();
    f->owner_valid = 1;
    assert(owner_check(f));
    run_owner_phase(f, f->turn);
    ++f->turn;
    assert(pthread_cond_broadcast(&f->turn_changed) == 0);
  }
  assert(pthread_mutex_unlock(&f->mutex) == 0);
  return NULL;
}

/* Cancellation requested from another real thread while the Owner is
 * blocked inside its entropy Provider callback. The canceller only posts an
 * externally synchronized intent; it must NEVER call Association APIs or
 * unload a Provider while that callback is still in flight.
 *
 * The Owner observes cancellation after its callback returns, then performs
 * stop/drain/destroy. This tests the required ordering and is not yet a real
 * Provider DSO load/unload race. */
typedef struct inflight_cancel_fixture {
  pthread_mutex_t mutex;
  pthread_cond_t changed;
  pthread_t owner_thread;
  cnet_sctp_assoc association;
  cnet_sctp_assoc_config config;
  unsigned entered;
  unsigned cancel_requested;
  unsigned release_callback;
  unsigned callback_count;
  cnet_sctp_assoc_status reentry_status;
} inflight_cancel_fixture;

static int cancel_owner_check(void *context) {
  const inflight_cancel_fixture *f =
      (const inflight_cancel_fixture *)context;
  return pthread_equal(pthread_self(), f->owner_thread);
}

static int entropy_with_inflight_cancel(void *context, uint8_t *out,
    size_t size) {
  inflight_cancel_fixture *f = (inflight_cancel_fixture *)context;
  /* A callback cannot recursively close its own Association. */
  f->reentry_status = cnet_sctp_assoc_stop(&f->association);
  assert(f->reentry_status == CNET_SCTP_ASSOC_BUSY);
  assert(pthread_mutex_lock(&f->mutex) == 0);
  ++f->callback_count;
  f->entered = 1u;
  assert(pthread_cond_broadcast(&f->changed) == 0);
  while (!f->release_callback)
    assert(pthread_cond_wait(&f->changed, &f->mutex) == 0);
  assert(f->cancel_requested);
  assert(pthread_mutex_unlock(&f->mutex) == 0);
  return test_entropy(NULL, out, size);
}

static void *owner_with_inflight_cancel(void *context) {
  inflight_cancel_fixture *f = (inflight_cancel_fixture *)context;
  f->owner_thread = pthread_self();
  memset(&f->config, 0, sizeof(f->config));
  f->config.local_port = 5000u;
  f->config.remote_port = 5001u;
  f->config.receive_window = 8192u;
  f->config.outbound_streams = 2u;
  f->config.inbound_streams = 2u;
  f->config.t1_ms = 100u;
  f->config.t1_max_ms = 400u;
  f->config.cookie_lifetime_ms = 1000u;
  f->config.peer_binding = UINT64_C(0x1029384756abcdef);
  f->config.max_retransmits = 3u;
  f->config.is_owner = cancel_owner_check;
  f->config.entropy = entropy_with_inflight_cancel;
  f->config.mac = test_mac;
  f->config.verify_mac = test_verify;
  f->config.provider_context = f;

  assert(cnet_sctp_assoc_init(&f->association, &f->config, 0u) ==
      CNET_SCTP_ASSOC_OK);
  /* Observe the cancellation request through its synchronization barrier. */
  assert(pthread_mutex_lock(&f->mutex) == 0);
  assert(f->cancel_requested && f->callback_count == 1u);
  assert(pthread_mutex_unlock(&f->mutex) == 0);
  assert(cnet_sctp_assoc_stop(&f->association) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_drained(&f->association));
  assert(cnet_sctp_assoc_destroy(&f->association) == CNET_SCTP_ASSOC_OK);
  assert(f->association.magic == 0u);
  return NULL;
}

static void *post_cancel_during_callback(void *context) {
  inflight_cancel_fixture *f = (inflight_cancel_fixture *)context;
  assert(pthread_mutex_lock(&f->mutex) == 0);
  while (!f->entered)
    assert(pthread_cond_wait(&f->changed, &f->mutex) == 0);
  /* No touching the unleased Association from the other thread. */
  f->cancel_requested = 1u;
  f->release_callback = 1u;
  assert(pthread_cond_broadcast(&f->changed) == 0);
  assert(pthread_mutex_unlock(&f->mutex) == 0);
  return NULL;
}

static void test_cancel_while_provider_callback_in_flight(void) {
  unsigned round;
  for (round = 0u; round < 64u; ++round) {
    inflight_cancel_fixture f = {0};
    pthread_t owner, canceller;
    assert(pthread_mutex_init(&f.mutex, NULL) == 0);
    assert(pthread_cond_init(&f.changed, NULL) == 0);
    assert(pthread_create(&canceller, NULL,
        post_cancel_during_callback, &f) == 0);
    assert(pthread_create(&owner, NULL,
        owner_with_inflight_cancel, &f) == 0);
    assert(pthread_join(owner, NULL) == 0);
    assert(pthread_join(canceller, NULL) == 0);
    assert(f.callback_count == 1u && f.reentry_status ==
        CNET_SCTP_ASSOC_BUSY && f.association.magic == 0u);
    assert(pthread_cond_destroy(&f.changed) == 0);
    assert(pthread_mutex_destroy(&f.mutex) == 0);
  }
}

int main(void) {
  owner_fixture f = {0};
  worker_arg a = {&f, 0u}, b = {&f, 1u};
  pthread_t worker_a, worker_b;
  assert(pthread_mutex_init(&f.mutex, NULL) == 0);
  assert(pthread_cond_init(&f.turn_changed, NULL) == 0);
  assert(pthread_create(&worker_a, NULL, worker, &a) == 0);
  assert(pthread_create(&worker_b, NULL, worker, &b) == 0);
  assert(pthread_join(worker_a, NULL) == 0);
  assert(pthread_join(worker_b, NULL) == 0);
  assert(f.turn == (size_t)HANDOFF_ROUNDS * OPERATIONS_PER_ROUND);
  assert(f.assoc.magic == 0u);
  assert(pthread_cond_destroy(&f.turn_changed) == 0);
  assert(pthread_mutex_destroy(&f.mutex) == 0);
  test_cancel_while_provider_callback_in_flight();
  puts("CNet SCTP Owner-thread handoff/stop/event/drain/destroy: PASS");
  return 0;
}
