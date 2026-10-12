/* #1143: cross-DSO first CNet module initialization publication.
 * Run as a standalone CTest process, so the module's once-guard starts cold.
 * Never warm up in the control thread: TSan must observe first admission.
 */
#include "cnet_module.h"
#include "tinytest.h"
#include <salts/clock.h>
#include <salts/thread.h>
#include <stdatomic.h>
#include <stddef.h>

enum { CNET_ONCE_WORKERS = 8, CNET_ONCE_TIMEOUT_MS = 5000 };

typedef struct cnet_once_gate {
  atomic_int ready;
  atomic_int go;
} cnet_once_gate;

typedef struct cnet_once_worker {
  cnet_once_gate *gate;
  int init_status;
} cnet_once_worker;

static void cnet_once_worker_run(void *opaque) {
  cnet_once_worker *worker = (cnet_once_worker *)opaque;
  const uint64_t deadline = cmeta_monotonic_ms() + CNET_ONCE_TIMEOUT_MS;
  atomic_fetch_add_explicit(&worker->gate->ready, 1, memory_order_release);
  while (atomic_load_explicit(&worker->gate->go, memory_order_acquire) == 0) {
    if (cmeta_monotonic_ms() >= deadline) {
      worker->init_status = SALTS_ETIMEDOUT;
      return;
    }
    cmeta_thread_yield();
  }
  /* Concurrent first entry must publish the mutex before any thread locks. */
  worker->init_status = cnet_module_init();
}

spec("CNet module-once synchronization, issue 1143") {
  it("publishes first initialized mutex to eight concurrent callers") {
    cnet_once_gate gate = {0};
    cnet_once_worker workers[CNET_ONCE_WORKERS] = {0};
    cmeta_thread_t threads[CNET_ONCE_WORKERS] = {0};
    const uint64_t deadline = cmeta_monotonic_ms() + CNET_ONCE_TIMEOUT_MS;
    size_t created = 0u;
    size_t initialized = 0u;
    atomic_init(&gate.ready, 0);
    atomic_init(&gate.go, 0);
    for (size_t i = 0u; i < CNET_ONCE_WORKERS; ++i) {
      workers[i].gate = &gate;
      workers[i].init_status = SALTS_EBUSY;
      if (cmeta_thread_create(&threads[i], cnet_once_worker_run, &workers[i]) != SALTS_OK)
        break;
      ++created;
    }
    while (atomic_load_explicit(&gate.ready, memory_order_acquire) < (int)created
           && cmeta_monotonic_ms() < deadline)
      cmeta_thread_yield();
    atomic_store_explicit(&gate.go, 1, memory_order_release);
    for (size_t i = 0u; i < created; ++i) {
      check_equal(cmeta_thread_join(&threads[i]), SALTS_OK);
      cmeta_thread_destroy(&threads[i]);
      if (workers[i].init_status == SALTS_OK) ++initialized;
    }
    /* Refcount must account for every successful worker admission. */
    for (size_t i = 0u; i < initialized; ++i)
      check_equal(cnet_module_shutdown(), SALTS_OK);
    check_equal(created, (size_t)CNET_ONCE_WORKERS);
    check_equal(initialized, (size_t)CNET_ONCE_WORKERS);
    /* Confirm the balanced final drain rejects underflow. */
    check_equal(cnet_module_shutdown(), SALTS_EINVAL);
  }
}
