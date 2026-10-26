/* Targeted memory-order litmus tests for the private external ASM backend.
 *
 * 1. Store Buffering (SB): two SEQ_CST stores/loads MUST NOT both observe 0.
 * 2. Message Passing (MP): release/acquire handoff protects a plain C payload.
 *
 * No <stdatomic.h>, no runtime mutex fallback, no compiler feature override.
 * Linux x86-64 / AArch64 only; not a formal memory-model proof.
 */
#define _POSIX_C_SOURCE 200809L
#include "asm_atomic_private.h"
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

enum { SB_ROUNDS = 12000, MP_ROUNDS = 30000, WAIT_SECONDS = 20 };
#define REQUIRE(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "ASM litmus failed at %d: %s\n", __LINE__, #expr); \
        return 1; \
    } \
} while (0)

static uint64_t monotonic_ns(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) +
           (uint64_t)now.tv_nsec;
}

static int expired(uint64_t began) {
    const uint64_t now = monotonic_ns();
    return now == 0 || now < began ||
           now - began > (uint64_t)WAIT_SECONDS * UINT64_C(1000000000);
}

static int barrier_ok(int outcome) {
    return outcome == 0 || outcome == PTHREAD_BARRIER_SERIAL_THREAD;
}

typedef struct sb_context {
    salts_asm_u64_slot x, y;
    pthread_barrier_t enter;
    pthread_barrier_t finish;
    uint64_t observed[2];
    int failed[2];
} sb_context;

typedef struct sb_lane {
    sb_context *ctx;
    unsigned id;
} sb_lane;

static void *sb_worker(void *argument) {
    sb_lane *lane = (sb_lane *)argument;
    sb_context *ctx = lane->ctx;
    for (unsigned i = 0; i < SB_ROUNDS; ++i) {
        if (!barrier_ok(pthread_barrier_wait(&ctx->enter))) {
            ctx->failed[lane->id] = 1;
            return NULL;
        }
        if (lane->id == 0u) {
            salts_asm_u64_store(&ctx->x, 1u);
            ctx->observed[0] = salts_asm_u64_load(&ctx->y);
        } else {
            salts_asm_u64_store(&ctx->y, 1u);
            ctx->observed[1] = salts_asm_u64_load(&ctx->x);
        }
        if (!barrier_ok(pthread_barrier_wait(&ctx->finish))) {
            ctx->failed[lane->id] = 1;
            return NULL;
        }
    }
    return NULL;
}

static int store_buffering_seq_cst(void) {
    sb_context ctx = {0};
    pthread_t threads[2];
    sb_lane lanes[] = {{&ctx, 0u}, {&ctx, 1u}};
    REQUIRE(pthread_barrier_init(&ctx.enter, NULL, 3u) == 0);
    REQUIRE(pthread_barrier_init(&ctx.finish, NULL, 3u) == 0);
    REQUIRE(pthread_create(&threads[0], NULL, sb_worker, &lanes[0]) == 0);
    REQUIRE(pthread_create(&threads[1], NULL, sb_worker, &lanes[1]) == 0);

    unsigned forbidden = 0;
    const uint64_t start = monotonic_ns();
    REQUIRE(start != 0u);
    for (unsigned i = 0; i < SB_ROUNDS; ++i) {
        salts_asm_u64_store(&ctx.x, 0u);
        salts_asm_u64_store(&ctx.y, 0u);
        REQUIRE(barrier_ok(pthread_barrier_wait(&ctx.enter)));
        REQUIRE(barrier_ok(pthread_barrier_wait(&ctx.finish)));
        if (ctx.observed[0] == 0u && ctx.observed[1] == 0u)
            ++forbidden;
        if ((i & 1023u) == 0u) REQUIRE(!expired(start));
    }
    REQUIRE(pthread_join(threads[0], NULL) == 0);
    REQUIRE(pthread_join(threads[1], NULL) == 0);
    REQUIRE(pthread_barrier_destroy(&ctx.enter) == 0);
    REQUIRE(pthread_barrier_destroy(&ctx.finish) == 0);
    REQUIRE(ctx.failed[0] == 0 && ctx.failed[1] == 0);
    REQUIRE(forbidden == 0);
    printf("SEQ_CST SB litmus: %u rounds, forbidden 0/0 outcome=%u\n",
           (unsigned)SB_ROUNDS, forbidden);
    return 0;
}

typedef struct mp_context {
    salts_asm_u64_slot ready, ack, cancelled;
    uint64_t plain_payload;
    int failed;
} mp_context;

static int spin_until(mp_context *ctx, const salts_asm_u64_slot *slot,
                      uint64_t expected, uint64_t start) {
    while (salts_asm_u64_load_acquire(slot) != expected) {
        if (salts_asm_u64_load_relaxed(&ctx->cancelled) != 0u ||
            expired(start)) return 0;
        (void)sched_yield();
    }
    return 1;
}

static void *mp_publisher(void *argument) {
    mp_context *ctx = (mp_context *)argument;
    const uint64_t start = monotonic_ns();
    if (start == 0u) {
        salts_asm_u64_store_release(&ctx->cancelled, 1u);
        return NULL;
    }
    for (uint64_t seq = 1u; seq <= MP_ROUNDS; ++seq) {
        if (!spin_until(ctx, &ctx->ack, seq - 1u, start)) {
            salts_asm_u64_store_release(&ctx->cancelled, 1u);
            return NULL;
        }
        ctx->plain_payload = seq ^ UINT64_C(0xfeed1223456789ab);
        salts_asm_u64_store_release(&ctx->ready, seq);
    }
    return NULL;
}

static int message_passing_release_acquire(void) {
    mp_context ctx = {0};
    pthread_t publisher;
    salts_asm_u64_store_relaxed(&ctx.ready, 0u);
    salts_asm_u64_store_relaxed(&ctx.ack, 0u);
    salts_asm_u64_store_relaxed(&ctx.cancelled, 0u);
    const uint64_t start = monotonic_ns();
    REQUIRE(start != 0u);
    REQUIRE(pthread_create(&publisher, NULL, mp_publisher, &ctx) == 0);
    for (uint64_t seq = 1u; seq <= MP_ROUNDS; ++seq) {
        if (!spin_until(&ctx, &ctx.ready, seq, start)) {
            ctx.failed = 1;
            salts_asm_u64_store_release(&ctx.cancelled, 1u);
            break;
        }
        if (ctx.plain_payload != (seq ^ UINT64_C(0xfeed1223456789ab))) {
            ctx.failed = 1;
            salts_asm_u64_store_release(&ctx.cancelled, 1u);
            break;
        }
        salts_asm_u64_store_release(&ctx.ack, seq);
    }
    REQUIRE(pthread_join(publisher, NULL) == 0);
    REQUIRE(!ctx.failed);
    REQUIRE(salts_asm_u64_load_acquire(&ctx.cancelled) == 0u);
    REQUIRE(salts_asm_u64_load_acquire(&ctx.ready) == MP_ROUNDS);
    REQUIRE(salts_asm_u64_load_acquire(&ctx.ack) == MP_ROUNDS);
    printf("Release/Acquire MP litmus: %u synchronized payloads PASS\n",
           (unsigned)MP_ROUNDS);
    return 0;
}

int main(void) {
    if (store_buffering_seq_cst() != 0) return 1;
    if (message_passing_release_acquire() != 0) return 1;
    puts("External ASM SB/MP litmus: PASS (observational, not C11 compiler admission)");
    return 0;
}
