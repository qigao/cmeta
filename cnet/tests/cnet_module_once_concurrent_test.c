/* Qualify the CNet first-use mutex publication with source-instrumented
 * Platform + CNet. No prebuilt Salts DSO and no TSan suppressions. */
#include "cnet_module.h"
#include <salts/clock.h>
#include <salts/thread.h>
#include <salts/error_codes.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>

enum {
  THREAD_COUNT = 12,
  MODULE_ROUNDS = 8,
  WAIT_LIMIT_MS = 5000
};

typedef struct phase {
  atomic_int ready;
  atomic_int go;
  atomic_int entered;
  atomic_int release;
  atomic_int failures;
} phase;

static int wait_until(atomic_int *value, int threshold) {
  const uint64_t end = cmeta_monotonic_ms() + WAIT_LIMIT_MS;
  while (atomic_load_explicit(value, memory_order_acquire) < threshold) {
    if (cmeta_monotonic_ms() >= end) return 0;
    cmeta_thread_yield();
  }
  return 1;
}

typedef struct once_probe {
  phase gate;
  cmeta_once_t once;
  cmeta_mutex_t mutex;
  atomic_int initialized;
  atomic_int callback_entered;
  atomic_int observed;
} once_probe;

/* cmeta_once's callback is context-free; this stable storage belongs to the
 * test executable, not to a dynamically loaded or per-thread owner. */
static once_probe once_state = { .once = SALTS_ONCE_INIT };

static void initialize_mutex_once(void) {
  cmeta_mutex_init(&once_state.mutex);
  atomic_fetch_add_explicit(&once_state.initialized, 1, memory_order_relaxed);
  atomic_store_explicit(&once_state.callback_entered, 1, memory_order_release);
  while (!atomic_load_explicit(&once_state.gate.release, memory_order_acquire))
    cmeta_thread_yield();
}

static void observe_published_mutex(void *unused) {
  (void)unused;
  atomic_fetch_add_explicit(&once_state.gate.ready, 1, memory_order_release);
  if (!wait_until(&once_state.gate.go, 1)) {
    atomic_fetch_add(&once_state.gate.failures, 1);
    return;
  }
  atomic_fetch_add_explicit(&once_state.gate.entered, 1, memory_order_release);
  cmeta_once(&once_state.once, initialize_mutex_once);
  if (once_state.mutex == NULL) {
    atomic_fetch_add(&once_state.gate.failures, 1);
    return;
  }
  cmeta_mutex_lock(&once_state.mutex);
  atomic_fetch_add_explicit(&once_state.observed, 1, memory_order_relaxed);
  cmeta_mutex_unlock(&once_state.mutex);
}

typedef struct module_probe {
  phase gate;
  atomic_int completed;
} module_probe;

static void concurrent_module_init(void *user) {
  module_probe *probe = (module_probe *)user;
  int status;
  atomic_fetch_add_explicit(&probe->gate.ready, 1, memory_order_release);
  if (!wait_until(&probe->gate.go, 1)) {
    atomic_fetch_add(&probe->gate.failures, 1);
    atomic_fetch_add_explicit(&probe->completed, 1, memory_order_release);
    return;
  }
  status = cnet_module_init();
  if (status != SALTS_OK)
    atomic_fetch_add(&probe->gate.failures, 1);
  atomic_fetch_add_explicit(&probe->completed, 1, memory_order_release);
  /* Prevent premature global resolver shutdown during concurrent first init.
   * Shutdown may begin only after every worker has completed admission. */
  if (!wait_until(&probe->gate.release, 1)) {
    atomic_fetch_add(&probe->gate.failures, 1);
  }
  if (status == SALTS_OK && cnet_module_shutdown() != SALTS_OK)
    atomic_fetch_add(&probe->gate.failures, 1);
}

static int run_phase(phase *gate, atomic_int *done, int done_target,
                     cmeta_thread_cb entry, void *user,
                     const char *label) {
  cmeta_thread_t threads[THREAD_COUNT] = {0};
  int created = 0, joined = 0;
  int okay;
  for (int i = 0; i < THREAD_COUNT; ++i) {
    if (cmeta_thread_create(&threads[i], entry, user) != SALTS_OK) break;
    ++created;
  }
  okay = created == THREAD_COUNT && wait_until(&gate->ready, THREAD_COUNT);
  atomic_store_explicit(&gate->go, 1, memory_order_release);
  if (okay && done != NULL)
    okay = wait_until(done, done_target);
  if (okay && done == NULL)
    okay = wait_until(&gate->entered, THREAD_COUNT) &&
           wait_until(&once_state.callback_entered, 1);
  atomic_store_explicit(&gate->release, 1, memory_order_release);
  for (int i = 0; i < created; ++i) {
    if (cmeta_thread_join(&threads[i]) == SALTS_OK) ++joined;
    else cmeta_thread_destroy(&threads[i]);
  }
  okay = okay && joined == created &&
         atomic_load_explicit(&gate->failures, memory_order_acquire) == 0;
  if (!okay)
    fprintf(stderr, "%s: created=%d joined=%d ready=%d entered=%d failures=%d\n",
            label, created, joined, atomic_load(&gate->ready),
            atomic_load(&gate->entered), atomic_load(&gate->failures));
  return okay;
}

int main(void) {
  if (!run_phase(&once_state.gate, NULL, 0, observe_published_mutex, NULL,
                 "Platform once publication"))
    return 1;
  if (atomic_load(&once_state.initialized) != 1 ||
      atomic_load(&once_state.observed) != THREAD_COUNT) {
    fprintf(stderr, "once: initialized=%d observed=%d\n",
            atomic_load(&once_state.initialized),
            atomic_load(&once_state.observed));
    return 2;
  }
  cmeta_mutex_destroy(&once_state.mutex);

  for (int round = 0; round < MODULE_ROUNDS; ++round) {
    module_probe probe = {0};
    if (!run_phase(&probe.gate, &probe.completed, THREAD_COUNT,
                   concurrent_module_init, &probe, "CNet first/refcount init"))
      return 3;
  }
  puts("CNet/Platform once: published mutex, 12 concurrent clients, 8 balanced init/shutdown rounds");
  return 0;
}
