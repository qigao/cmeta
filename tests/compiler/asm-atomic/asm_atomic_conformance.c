/* External GAS atomic entrypoint conformance on Linux x86-64 / AArch64.
 * This deliberately uses no <stdatomic.h>, no GNU __atomic builtins,
 * no runtime mutex, and no production Salts atomic storage.
 */
#define _POSIX_C_SOURCE 200809L
#include "asm_atomic_private.h"

#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

#define REQUIRE(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "ASM atomic failed at %d: %s\n", __LINE__, #expr); \
        return 1; \
    } \
} while (0)

enum { WORKERS = 4, ROUNDS = 20000, DEADLINE_SECONDS = 15 };
static salts_asm_u64_slot counter64, gate64, published[WORKERS];
static salts_asm_u32_slot counter32, ready32;
static unsigned payload[WORKERS], indexes[WORKERS];

static uint64_t now_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) +
           (uint64_t)ts.tv_nsec;
}

static int expired(uint64_t begin) {
    uint64_t now = now_ns();
    return now == 0 || now < begin ||
           now - begin >= (uint64_t)DEADLINE_SECONDS * UINT64_C(1000000000);
}

static int basic_operations(void) {
    salts_asm_u64_slot a = {0};
    salts_asm_u32_slot b = {0};

    salts_asm_u64_store(&a, 5);
    REQUIRE(salts_asm_u64_load(&a) == 5);
    REQUIRE(salts_asm_u64_fetch_add(&a, 3) == 5);
    REQUIRE(salts_asm_u64_fetch_sub(&a, 2) == 8);
    REQUIRE(salts_asm_u64_exchange(&a, 11) == 6);

    uint64_t expected64 = 4;
    REQUIRE(!salts_asm_u64_cas(&a, &expected64, 15) && expected64 == 11);
    REQUIRE(salts_asm_u64_cas(&a, &expected64, 15) && expected64 == 11);
    REQUIRE(salts_asm_u64_load(&a) == 15);

    salts_asm_u32_store(&b, 5);
    REQUIRE(salts_asm_u32_load(&b) == 5);
    REQUIRE(salts_asm_u32_fetch_add(&b, 3) == 5);
    REQUIRE(salts_asm_u32_fetch_sub(&b, 2) == 8);
    REQUIRE(salts_asm_u32_exchange(&b, 11) == 6);

    uint32_t expected32 = 4;
    REQUIRE(!salts_asm_u32_cas(&b, &expected32, 15) && expected32 == 11);
    REQUIRE(salts_asm_u32_cas(&b, &expected32, 15) && expected32 == 11);
    REQUIRE(salts_asm_u32_load(&b) == 15);

    salts_asm_fence();
    return 0;
}

static void *increment_worker(void *arg) {
    const unsigned index = *(const unsigned *)arg;
    const uint64_t begin = now_ns();
    (void)salts_asm_u32_fetch_add(&ready32, 1);
    while (!salts_asm_u64_load(&gate64)) {
        if (expired(begin)) return (void *)(uintptr_t)1;
        (void)sched_yield();
    }
    for (unsigned count = 0; count < ROUNDS; ++count) {
        (void)salts_asm_u64_fetch_add(&counter64, UINT64_C(0x100000001));
        uint32_t expected;
        do {
            expected = salts_asm_u32_load(&counter32);
        } while (!salts_asm_u32_cas(&counter32, &expected, expected + 1));
    }
    /* This write must become visible to the consumer before the worker
     * returns; publication is observed BEFORE pthread_join. */
    payload[index] = 919u + index;
    salts_asm_u64_store(&published[index], 1);
    return NULL;
}

static int real_thread_contention(void) {
    pthread_t workers[WORKERS];
    const uint64_t begin = now_ns();
    REQUIRE(begin != 0);
    salts_asm_u64_store(&counter64, 0);
    salts_asm_u64_store(&gate64, 0);
    salts_asm_u32_store(&counter32, 0);
    salts_asm_u32_store(&ready32, 0);
    for (unsigned i = 0; i < WORKERS; ++i) {
        indexes[i] = i;
        salts_asm_u64_store(&published[i], 0);
        REQUIRE(pthread_create(&workers[i], NULL, increment_worker,
                               &indexes[i]) == 0);
    }
    while (salts_asm_u32_load(&ready32) < WORKERS) {
        REQUIRE(!expired(begin));
        (void)sched_yield();
    }
    salts_asm_u64_store(&gate64, 1);
    for (unsigned i = 0; i < WORKERS; ++i) {
        while (!salts_asm_u64_load(&published[i])) {
            REQUIRE(!expired(begin));
            (void)sched_yield();
        }
        REQUIRE(payload[i] == 919u + i);
    }
    for (unsigned i = 0; i < WORKERS; ++i) {
        void *result = (void *)(uintptr_t)1;
        REQUIRE(pthread_join(workers[i], &result) == 0);
        REQUIRE(result == NULL);
    }
    REQUIRE(salts_asm_u64_load(&counter64) ==
            UINT64_C(0x100000001) * WORKERS * ROUNDS);
    REQUIRE(salts_asm_u32_load(&counter32) == WORKERS * ROUNDS);
    return 0;
}

int main(void) {
    if (basic_operations() != 0) return 1;
    if (real_thread_contention() != 0) return 1;
    puts("External GAS 32/64-bit atomics: PASS (private seq_cst, not C11 admission)");
    return 0;
}
