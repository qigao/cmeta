/* #1143: instrumented *consumer*, deliberately non-TSan native CNet SDK.
 *
 * A fully instrumented source graph already passed its cold init qualification.
 * This test adds the missing compilation-asymmetry boundary from CHttp:
 * pthread_once and libcnet live in an uninstrumented shared object while 12
 * real pthread clients are instrumented by TSan. Do not link a second CNet or
 * Platform implementation into this executable. Run in a fresh process.
 */
#define _POSIX_C_SOURCE 200809L
#include <dlfcn.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

enum { MIXED_WORKERS = 12, MIXED_TIMEOUT_SECONDS = 12 };
typedef int (*cnet_module_callback)(void);
typedef struct mixed_gate {
  atomic_int ready;
  atomic_int go;
  cnet_module_callback init;
} mixed_gate;
typedef struct mixed_worker {
  mixed_gate *gate;
  int result;
} mixed_worker;

static int mixed_deadline_expired(const struct timespec *started) {
  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 1;
  return now.tv_sec - started->tv_sec >= MIXED_TIMEOUT_SECONDS;
}

static void *mixed_cold_entry(void *opaque) {
  mixed_worker *worker = (mixed_worker *)opaque;
  atomic_fetch_add_explicit(&worker->gate->ready, 1, memory_order_release);
  while (!atomic_load_explicit(&worker->gate->go, memory_order_acquire))
    sched_yield();
  worker->result = worker->gate->init();
  return NULL;
}

/* Assign dlsym through memcpy; POSIX defines a function symbol representation
 * suitable for this use, but ISO C does not allow a direct object/function
 * pointer cast. */
static cnet_module_callback mixed_symbol(void *dso, const char *name) {
  void *symbol = dlsym(dso, name);
  cnet_module_callback target = NULL;
  if (symbol != NULL) memcpy(&target, &symbol, sizeof(target));
  return target;
}

int main(int argc, char **argv) {
  void *dso;
  cnet_module_callback init;
  cnet_module_callback shutdown;
  pthread_t threads[MIXED_WORKERS];
  mixed_worker workers[MIXED_WORKERS] = {{0}};
  mixed_gate gate = {0};
  struct timespec started;
  size_t created = 0u, passed = 0u, i;
  int failed = 0;

  if (argc != 2 || argv[1] == NULL) {
    fprintf(stderr, "usage: mixed_tsan_driver /exact/uninstrumented/libcnet.so\n");
    return 2;
  }
  dso = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
  if (dso == NULL) {
    fprintf(stderr, "unable to load exact CNet SDK: %s\n", dlerror());
    return 3;
  }
  init = mixed_symbol(dso, "cnet_module_init");
  shutdown = mixed_symbol(dso, "cnet_module_shutdown");
  if (init == NULL || shutdown == NULL) {
    fprintf(stderr, "CNet SDK does not export module-init/refcount entrypoints\n");
    (void)dlclose(dso);
    return 4;
  }

  if (clock_gettime(CLOCK_MONOTONIC, &started) != 0) {
    (void)dlclose(dso);
    return 5;
  }
  atomic_init(&gate.ready, 0);
  atomic_init(&gate.go, 0);
  gate.init = init;
  for (i = 0u; i < MIXED_WORKERS; ++i) {
    workers[i].gate = &gate;
    workers[i].result = -1;
    if (pthread_create(&threads[i], NULL, mixed_cold_entry, &workers[i]) != 0)
      break;
    ++created;
  }
  while (atomic_load_explicit(&gate.ready, memory_order_acquire) <
             (int)created && !mixed_deadline_expired(&started))
    sched_yield();
  atomic_store_explicit(&gate.go, 1, memory_order_release);

  for (i = 0u; i < created; ++i) {
    if (pthread_join(threads[i], NULL) != 0) failed = 1;
    if (workers[i].result == 0) ++passed;
    else failed = 1;
  }
  if (created != MIXED_WORKERS || passed != MIXED_WORKERS) {
    fprintf(stderr, "cold init admission: created=%zu passed=%zu\n",
            created, passed);
    failed = 1;
  }
  for (i = 0u; i < passed; ++i) {
    if (shutdown() != 0) failed = 1;
  }
  /* No silent rollover or automatic new initialization on final release. */
  if (shutdown() == 0) failed = 1;
  if (dlclose(dso) != 0) failed = 1;
  if (!failed) fprintf(stdout,
      "mixed-sdk-cnet: 12/12 cold admissions, 12/12 releases, underflow rejected\n");
  return failed ? 6 : 0;
}
