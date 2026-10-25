/* Native, unchanged Salts Disruptor MPMC consumer/owner qualification.
 * Four pthread publishers compete for the publication cursor while four
 * worker-pool consumers claim and retire independently. No copied algorithm,
 * no mock atomics and no production ABI/compiler gate changes.
 */
#define _POSIX_C_SOURCE 200809L
#include <salts/disruptor.h>
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

enum {
    PRODUCERS = 4, WORKERS = 4, EACH = 3000,
    TOTAL = PRODUCERS * EACH, CAPACITY = 64, DEADLINE_SECONDS = 15
};

typedef struct mpmc_context {
    disruptor_t *ring;
    pthread_mutex_t monitor;
    unsigned char observed[TOTAL + 1];
    size_t consumed;
    int failed;
} mpmc_context;

typedef struct mpmc_lane {
    mpmc_context *ctx;
    unsigned id;
    size_t completed;
    int status;
} mpmc_lane;

static uint64_t now_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0u;
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) +
           (uint64_t)ts.tv_nsec;
}
static bool expired(uint64_t start) {
    const uint64_t current = now_ns();
    return current == 0u || current < start ||
           current - start >= (uint64_t)DEADLINE_SECONDS * UINT64_C(1000000000);
}
static uint64_t value_for(uint64_t sequence) {
    return (sequence << 8u) ^ UINT64_C(0x89abcdef13245670);
}

static void *publisher(void *arg) {
    mpmc_lane *lane = (mpmc_lane *)arg;
    mpmc_context *ctx = lane->ctx;
    const uint64_t start = now_ns();
    lane->status = 1;
    if (start == 0u) return NULL;
    for (unsigned n = 0; n < EACH; ++n) {
        disruptor_cursor_t cursor = {0};
        for (;;) {
            if (disruptor_publisher_try_claim(ctx->ring, &cursor)) break;
            if (expired(start)) {
                fprintf(stderr, "MPMC producer %u claim timeout at %u\n", lane->id, n);
                return NULL;
            }
            (void)sched_yield();
        }
        if (cursor.sequence == 0u || cursor.sequence > TOTAL) {
            fprintf(stderr, "MPMC producer %u invalid cursor %llu\n",
                    lane->id, (unsigned long long)cursor.sequence);
            return NULL;
        }
        uint64_t *slot = (uint64_t *)disruptor_acquire_entry(ctx->ring, &cursor);
        if (slot == NULL) return NULL;
        *slot = value_for(cursor.sequence);
        if (!disruptor_publisher_publish(ctx->ring, &cursor)) {
            fprintf(stderr, "MPMC producer %u publish failed\n", lane->id);
            return NULL;
        }
        ++lane->completed;
    }
    lane->status = 0;
    return NULL;
}

static void *worker(void *arg) {
    mpmc_lane *lane = (mpmc_lane *)arg;
    mpmc_context *ctx = lane->ctx;
    const uint64_t start = now_ns();
    lane->status = 1;
    if (start == 0u) return NULL;
    for (;;) {
        (void)pthread_mutex_lock(&ctx->monitor);
        const bool complete = ctx->consumed == TOTAL;
        const bool failed = ctx->failed != 0;
        (void)pthread_mutex_unlock(&ctx->monitor);
        if (complete) {
            lane->status = 0;
            return NULL;
        }
        if (failed) return NULL;

        disruptor_cursor_t cursor = {0};
        if (!disruptor_worker_try_claim(ctx->ring, &cursor)) {
            if (expired(start)) {
                fprintf(stderr, "MPMC worker %u claim timeout\n", lane->id);
                return NULL;
            }
            (void)sched_yield();
            continue;
        }

        const uint64_t *slot =
            (const uint64_t *)disruptor_show_entry(ctx->ring, &cursor);
        if (slot == NULL || cursor.sequence == 0u ||
            cursor.sequence > TOTAL || *slot != value_for(cursor.sequence)) {
            fprintf(stderr, "MPMC worker %u invalid entry seq=%llu\n",
                    lane->id, (unsigned long long)cursor.sequence);
            (void)pthread_mutex_lock(&ctx->monitor);
            ctx->failed = 1;
            (void)pthread_mutex_unlock(&ctx->monitor);
            return NULL;
        }
        (void)pthread_mutex_lock(&ctx->monitor);
        if (ctx->observed[cursor.sequence] != 0u || ctx->consumed >= TOTAL) {
            ctx->failed = 1;
        } else {
            ctx->observed[cursor.sequence] = 1u;
            ++ctx->consumed;
        }
        const bool duplicate = ctx->failed != 0;
        (void)pthread_mutex_unlock(&ctx->monitor);
        if (duplicate) {
            fprintf(stderr, "MPMC duplicate/overflow at seq=%llu\n",
                    (unsigned long long)cursor.sequence);
            return NULL;
        }
        disruptor_worker_release_entry(ctx->ring, &cursor);
        ++lane->completed;
    }
}

int main(void) {
    const disruptor_config_t config = {
        sizeof(uint64_t), CAPACITY, WORKERS, DISRUPTOR_MODE_WORKER_POOL
    };
    mpmc_context ctx = {0};
    mpmc_lane producers[PRODUCERS] = {{0}}, workers[WORKERS] = {{0}};
    pthread_t publisher_threads[PRODUCERS], worker_threads[WORKERS];
    unsigned started_publishers = 0, started_workers = 0;
    int creation_ok = 1;

    ctx.ring = disruptor_create(&config);
    if (ctx.ring == NULL || pthread_mutex_init(&ctx.monitor, NULL) != 0) {
        fprintf(stderr, "MPMC initialization failed\n");
        if (ctx.ring) disruptor_destroy(ctx.ring);
        return 1;
    }
    for (unsigned i = 0; i < WORKERS; ++i) {
        workers[i].ctx = &ctx;
        workers[i].id = i;
        if (pthread_create(&worker_threads[i], NULL, worker, &workers[i]) != 0) {
            creation_ok = 0;
            break;
        }
        ++started_workers;
    }
    if (creation_ok) {
        for (unsigned i = 0; i < PRODUCERS; ++i) {
            producers[i].ctx = &ctx;
            producers[i].id = i;
            if (pthread_create(&publisher_threads[i], NULL, publisher, &producers[i]) != 0) {
                creation_ok = 0;
                break;
            }
            ++started_publishers;
        }
    }
    for (unsigned i = 0; i < started_publishers; ++i)
        if (pthread_join(publisher_threads[i], NULL) != 0) creation_ok = 0;
    for (unsigned i = 0; i < started_workers; ++i)
        if (pthread_join(worker_threads[i], NULL) != 0) creation_ok = 0;

    size_t published = 0, consumed_by_lanes = 0, missing = 0;
    for (unsigned i = 0; i < started_publishers; ++i) {
        published += producers[i].completed;
        if (producers[i].status != 0) creation_ok = 0;
    }
    for (unsigned i = 0; i < started_workers; ++i) {
        consumed_by_lanes += workers[i].completed;
        if (workers[i].status != 0) creation_ok = 0;
    }
    for (size_t i = 1; i <= TOTAL; ++i)
        if (ctx.observed[i] != 1u) ++missing;

    const int ok = creation_ok && !ctx.failed &&
                   started_publishers == PRODUCERS &&
                   started_workers == WORKERS &&
                   published == TOTAL && consumed_by_lanes == TOTAL &&
                   ctx.consumed == TOTAL && missing == 0u;
    (void)pthread_mutex_destroy(&ctx.monitor);
    disruptor_destroy(ctx.ring);

    if (!ok) {
        fprintf(stderr, "MPMC mismatch: published=%zu consumed=%zu retired=%zu missing=%zu status=%d\n",
                published, consumed_by_lanes, ctx.consumed, missing, creation_ok);
        return 1;
    }
    puts("Actual Disruptor 4 publishers + 4 workers / 12000 exact-once: PASS (diagnostic)");
    return 0;
}
