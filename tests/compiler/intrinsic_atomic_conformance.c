/* Private intrinsic atomic conformance based on the same primitive families
 * as zelang-dev/c-threads/catomics.h. NOT C11 _Atomic or a public backend.
 * This compiles without overriding TinyCC's __STDC_NO_ATOMICS__ declaration.
 */
#define _POSIX_C_SOURCE 200809L
#include "intrinsic_atomic_backend.h"
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdint.h>

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "intrinsic atomic failed at line %d: %s\n", __LINE__, #expr); \
        return 1; \
    } \
} while (0)

#ifdef SALTS_PROBE_DIAG
#define MARK(stage) do { fprintf(stderr, "intrinsic stage: %s\n", stage); fflush(stderr); } while (0)
#else
#define MARK(stage) ((void)0)
#endif

enum { WORKERS = 6, ROUNDS = 7000 };
static salts_probe_u32 gate, ready, published[WORKERS], count;
static salts_probe_u64 sum;
static unsigned payload[WORKERS], indexes[WORKERS];

static int scalar_tests(void) {
    salts_probe_u8 b8;
    salts_probe_u16 b16;
    salts_probe_u32 b32;
    salts_probe_u64 b64;
    salts_probe_ptr ptr;
    int items[3] = {0};
    uint8_t u8out = 99;
    uint16_t u16out = 99;
    uint32_t u32 = 323u, u32out = 99;
    uint64_t u64out = 99;
    void *pout = &items[1], *pexpected = &items[2];
    bool swapped = true, lock_free = false;

    MARK("scalar init 8/16/32/64/pointer");
    CHECK(salts_probe_u8_init(&b8, 11u) == SALTS_OK);
    CHECK(salts_probe_u16_init(&b16, 77u) == SALTS_OK);
    CHECK(salts_probe_u32_init(&b32, 321u) == SALTS_OK);
    CHECK(salts_probe_u64_init(&b64, UINT64_C(0x100000001)) == SALTS_OK);
    CHECK(salts_probe_ptr_init(&ptr, &items[0]) == SALTS_OK);

    MARK("scalar fetch-add all widths");
    CHECK(salts_probe_u8_fetch_add(&b8, 2u, SALTS_PROBE_RELAXED, &u8out) == SALTS_OK);
    CHECK(u8out == 11u);
    CHECK(salts_probe_u16_fetch_add(&b16, 2u, SALTS_PROBE_RELAXED, &u16out) == SALTS_OK);
    CHECK(u16out == 77u);
    CHECK(salts_probe_u32_fetch_add(&b32, 2u, SALTS_PROBE_RELAXED, &u32out) == SALTS_OK);
    CHECK(u32out == 321u);
    CHECK(salts_probe_u64_fetch_add(&b64, UINT64_C(0x100000001),
        SALTS_PROBE_RELAXED, &u64out) == SALTS_OK);
    CHECK(u64out == UINT64_C(0x100000001));
    MARK("scalar load all widths");
    MARK("load8");
    CHECK(salts_probe_u8_load(&b8, SALTS_PROBE_ACQUIRE, &u8out) == SALTS_OK && u8out == 13u);
    MARK("load16");
    CHECK(salts_probe_u16_load(&b16, SALTS_PROBE_ACQUIRE, &u16out) == SALTS_OK && u16out == 79u);
    MARK("load32");
    CHECK(salts_probe_u32_load(&b32, SALTS_PROBE_ACQUIRE, &u32out) == SALTS_OK && u32out == 323u);
    MARK("load64");
    CHECK(salts_probe_u64_load(&b64, SALTS_PROBE_ACQUIRE, &u64out) == SALTS_OK &&
        u64out == UINT64_C(0x200000002));

    MARK("pointer CAS");
    CHECK(salts_probe_ptr_cas(&ptr, &pexpected, &items[1],
        SALTS_PROBE_ACQ_REL, SALTS_PROBE_ACQUIRE, &swapped) == SALTS_OK);
    CHECK(!swapped && pexpected == &items[0]);
    CHECK(salts_probe_ptr_cas(&ptr, &pexpected, &items[1],
        SALTS_PROBE_ACQ_REL, SALTS_PROBE_ACQUIRE, &swapped) == SALTS_OK);
    CHECK(swapped);
    CHECK(salts_probe_ptr_load(&ptr, SALTS_PROBE_ACQUIRE, &pout) == SALTS_OK &&
        pout == &items[1]);
    MARK("is_lock_free");
    CHECK(salts_probe_u64_is_lock_free(&b64, &lock_free) == SALTS_OK);
    (void)lock_free; /* Correct atomics need not be lock-free. */

    /* Validation failures must not touch caller outputs or live storage. */
    MARK("invalid-order validation");
    u32out = 211u;
    CHECK(salts_probe_u32_load(&b32, SALTS_PROBE_RELEASE, &u32out) == SALTS_EINVAL);
    CHECK(u32out == 211u);
    CHECK(salts_probe_u32_store(&b32, 17u, SALTS_PROBE_ACQUIRE) == SALTS_EINVAL);
    CHECK(salts_probe_u32_load(&b32, SALTS_PROBE_RELAXED, &u32out) == SALTS_OK &&
        u32out == 323u);
    swapped = true;
    CHECK(salts_probe_u32_cas(&b32, &u32, 1u,
        SALTS_PROBE_RELEASE, SALTS_PROBE_ACQUIRE, &swapped) == SALTS_EINVAL);
    CHECK(swapped && u32 == 323u);
    CHECK(salts_probe_u32_cas(&b32, &u32, 99u,
        SALTS_PROBE_ACQ_REL, SALTS_PROBE_ACQUIRE, &swapped) == SALTS_OK);
    CHECK(swapped && u32 == 323u);
    CHECK(salts_probe_u32_load(&b32, SALTS_PROBE_ACQUIRE, &u32out) == SALTS_OK &&
        u32out == 99u);
    CHECK(salts_probe_u32_exchange(&b32, 33u, SALTS_PROBE_SEQ_CST, &u32out) == SALTS_OK);
    CHECK(u32out == 99u);
    CHECK(salts_probe_u64_store(&b64, 7u, SALTS_PROBE_RELEASE) == SALTS_OK);
    CHECK(salts_probe_u64_load(&b64, SALTS_PROBE_ACQUIRE, &u64out) == SALTS_OK && u64out == 7u);
    MARK("thread fence");
    salts_probe_thread_fence(SALTS_PROBE_SEQ_CST);
    MARK("signal fence");
    salts_probe_signal_fence(SALTS_PROBE_SEQ_CST);
    MARK("scalar complete");
    return 0;
}

static void *worker(void *ctx) {
    const unsigned index = *(const unsigned *)ctx;
    uint32_t observed = 0, expected = 0, old = 0;
    uint64_t old64 = 0;
    (void)salts_probe_u32_fetch_add(&ready, 1u, SALTS_PROBE_RELAXED, &old);
    do {
        (void)salts_probe_u32_load(&gate, SALTS_PROBE_ACQUIRE, &observed);
        if (!observed) (void)sched_yield();
    } while (!observed);
    for (unsigned i = 0; i < ROUNDS; ++i) {
        (void)salts_probe_u64_fetch_add(&sum, UINT64_C(0x100000001),
                                        SALTS_PROBE_RELAXED, &old64);
        bool swapped = false;
        do {
            (void)salts_probe_u32_load(&count, SALTS_PROBE_RELAXED, &expected);
            (void)salts_probe_u32_cas(&count, &expected, expected + 1u,
                  SALTS_PROBE_RELAXED, SALTS_PROBE_RELAXED, &swapped);
        } while (!swapped);
    }
    payload[index] = 0x14000u + index;
    (void)salts_probe_u32_store(&published[index], 1u, SALTS_PROBE_RELEASE);
    return NULL;
}

static int threaded_tests(void) {
    MARK("threaded start");
    pthread_t threads[WORKERS];
    uint32_t observed = 0, total = 0;
    uint64_t sum_observed = 0;
    CHECK(salts_probe_u32_init(&gate, 0u) == SALTS_OK);
    CHECK(salts_probe_u32_init(&ready, 0u) == SALTS_OK);
    CHECK(salts_probe_u32_init(&count, 0u) == SALTS_OK);
    CHECK(salts_probe_u64_init(&sum, 0u) == SALTS_OK);
    for (unsigned i = 0; i < WORKERS; ++i) {
        CHECK(salts_probe_u32_init(&published[i], 0u) == SALTS_OK);
        indexes[i] = i;
        CHECK(pthread_create(&threads[i], NULL, worker, &indexes[i]) == 0);
    }
    do {
        CHECK(salts_probe_u32_load(&ready, SALTS_PROBE_ACQUIRE, &observed) == SALTS_OK);
        if (observed < WORKERS) (void)sched_yield();
    } while (observed < WORKERS);
    CHECK(salts_probe_u32_store(&gate, 1u, SALTS_PROBE_RELEASE) == SALTS_OK);
    for (unsigned i = 0; i < WORKERS; ++i) {
        do {
            CHECK(salts_probe_u32_load(&published[i], SALTS_PROBE_ACQUIRE,
                                       &observed) == SALTS_OK);
            if (!observed) (void)sched_yield();
        } while (!observed);
        CHECK(payload[i] == 0x14000u + i); /* Before joining the producer. */
    }
    for (unsigned i = 0; i < WORKERS; ++i)
        CHECK(pthread_join(threads[i], NULL) == 0);
    CHECK(salts_probe_u32_load(&count, SALTS_PROBE_RELAXED, &total) == SALTS_OK);
    MARK("threaded CAS result");
    CHECK(total == WORKERS * ROUNDS);
    CHECK(salts_probe_u64_load(&sum, SALTS_PROBE_RELAXED, &sum_observed) == SALTS_OK);
    CHECK(sum_observed == UINT64_C(0x100000001) * WORKERS * ROUNDS);
    MARK("threaded complete");
    return 0;
}

int main(void) {
    if (scalar_tests() != 0) return 1;
    if (threaded_tests() != 0) return 1;
    puts("GNU intrinsic atomic backend: PASS (private diagnostic, not C11 admission)");
    return 0;
}
