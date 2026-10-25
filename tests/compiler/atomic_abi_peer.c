/* Compile this translation unit with a *different* C compiler from driver.c. */
#include "atomic_abi.h"
#include <sched.h>

void atomic_abi_peer_layout(atomic_abi_layout *out) {
    if (out != NULL) *out = atomic_abi_layout_local();
}

int atomic_abi_peer_pointer_cas(atomic_abi_shared *shared, void **expected,
                                void *desired) {
    return atomic_compare_exchange_strong_explicit(&shared->pointer, expected,
             desired, memory_order_acq_rel, memory_order_acquire);
}

int atomic_abi_peer_work(atomic_abi_shared *shared, unsigned rounds) {
    while (atomic_load_explicit(&shared->start, memory_order_acquire) == 0u)
        (void)sched_yield();
    for (unsigned i = 0; i < rounds; ++i) {
        uint32_t expected = atomic_load_explicit(&shared->counter32,
                                                 memory_order_relaxed);
        while (!atomic_compare_exchange_weak_explicit(&shared->counter32,
            &expected, expected + 1u, memory_order_relaxed,
            memory_order_relaxed)) {}
        atomic_fetch_add_explicit(&shared->counter64, UINT64_C(0x100000001),
                                  memory_order_relaxed);
    }
    /* Read from the other compiler's TU *before* pthread_join. */
    shared->payload = UINT32_C(0x5a16c0de);
    atomic_store_explicit(&shared->published, 1u, memory_order_release);
    return 0;
}
