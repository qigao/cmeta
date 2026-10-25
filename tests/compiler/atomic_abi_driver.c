#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif
/* Mixed-compiler atomic ABI and native concurrent access (diagnostic only). */
#include "atomic_abi.h"
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <time.h>

#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "mixed atomic ABI failed at %d: %s\n", __LINE__, #expr); \
    return 1; \
} } while (0)

enum { ROUNDS = 15000 };

typedef struct worker_arg {
    atomic_abi_shared *shared;
    int status;
} worker_arg;

static void *peer_thread(void *opaque) {
    worker_arg *arg = (worker_arg *)opaque;
    arg->status = atomic_abi_peer_work(arg->shared, ROUNDS);
    return NULL;
}

static int layout_agrees(const atomic_abi_layout *a,
                         const atomic_abi_layout *b) {
    /* Fixed size_t array has no inter-element padding; compare every field. */
    for (size_t i = 0; i < ATOMIC_ABI_LAYOUT_FIELDS; ++i) {
        if (a->fields[i] != b->fields[i]) {
            fprintf(stderr, "Atomic ABI mismatch: field %zu: driver=%zu peer=%zu\n",
                    i, a->fields[i], b->fields[i]);
            return 0;
        }
    }
    return 1;
}

static int wait_for_publication(const atomic_abi_shared *shared) {
    struct timespec until;
    if (clock_gettime(CLOCK_MONOTONIC, &until) != 0) return 0;
    until.tv_sec += 10;
    for (;;) {
        struct timespec now;
        if (atomic_load_explicit(&shared->published, memory_order_acquire))
            return 1;
        if (clock_gettime(CLOCK_MONOTONIC, &now) != 0 ||
            now.tv_sec > until.tv_sec ||
            (now.tv_sec == until.tv_sec && now.tv_nsec >= until.tv_nsec))
            return 0;
        (void)sched_yield();
    }
}

int main(void) {
    atomic_abi_layout local = atomic_abi_layout_local();
    atomic_abi_layout peer = {0};
    atomic_abi_peer_layout(&peer);
    /* Never touch another compiler's _Atomic storage if the layouts differ. */
    CHECK(layout_agrees(&local, &peer));

    atomic_abi_shared shared;
    int items[3] = {0};
    atomic_init(&shared.counter32, 0u);
    atomic_init(&shared.counter64, 0u);
    atomic_init(&shared.pointer, &items[0]);
    atomic_init(&shared.start, 0u);
    atomic_init(&shared.published, 0u);
    shared.payload = 0u;

    void *expected = &items[1];
    CHECK(!atomic_abi_peer_pointer_cas(&shared, &expected, &items[2]));
    CHECK(expected == &items[0]);
    CHECK(atomic_abi_peer_pointer_cas(&shared, &expected, &items[2]));
    CHECK(atomic_load_explicit(&shared.pointer, memory_order_acquire) == &items[2]);

    worker_arg arg = {&shared, -1};
    pthread_t worker;
    CHECK(pthread_create(&worker, NULL, peer_thread, &arg) == 0);
    atomic_store_explicit(&shared.start, 1u, memory_order_release);
    for (unsigned i = 0; i < ROUNDS; ++i) {
        atomic_fetch_add_explicit(&shared.counter32, 1u, memory_order_relaxed);
        atomic_fetch_add_explicit(&shared.counter64, UINT64_C(0x100000001),
                                  memory_order_relaxed);
    }
    /* This is a pre-join acquire: join must not mask incorrect publication. */
    int published = wait_for_publication(&shared);
    int correct_payload = published && shared.payload == UINT32_C(0x5a16c0de);
    CHECK(pthread_join(worker, NULL) == 0);
    CHECK(arg.status == 0);
    CHECK(published && correct_payload);
    CHECK(atomic_load_explicit(&shared.counter32, memory_order_relaxed) ==
          2u * ROUNDS);
    CHECK(atomic_load_explicit(&shared.counter64, memory_order_relaxed) ==
          UINT64_C(0x100000001) * (2u * ROUNDS));
    puts("mixed-compiler atomic ABI and contention: PASS (diagnostic, not admission)");
    return 0;
}
