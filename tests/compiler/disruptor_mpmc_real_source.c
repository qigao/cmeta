/* Native, unchanged Salts Disruptor MPMC consumer/owner qualification.
 * Four pthread publishers compete for the publication cursor while four
 * worker-pool consumers claim and retire independently. No copied algorithm,
 * no mock atomics and no production ABI/compiler gate changes.
 */
#define _POSIX_C_SOURCE 200809L
#include <salts/disruptor.h>
#include <pthread.h>
#include <sched.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* Both production-source backends use the EXACT SAME workload and verdict.
 * Benchmark mode changes only the number of events and result reporting.
 */
#ifndef SALTS_COMPILER_MPMC_EACH
#define SALTS_COMPILER_MPMC_EACH 3000
#endif
enum {
    PRODUCERS = 4, WORKERS = 4, EACH = SALTS_COMPILER_MPMC_EACH,
    TOTAL = PRODUCERS * EACH, CAPACITY = 64, DEADLINE_SECONDS = 15
};

#ifdef SALTS_COMPILER_MPMC_LATENCY
/* Timestamp is captured just before publish and read immediately after
 * worker claim. This measures publish-to-claim observation age, including
 * scheduling, and is NOT a generic enqueue/dequeue API latency. */
typedef struct mpmc_timed_record {
    uint64_t payload;
    uint64_t before_publish_ns;
} mpmc_timed_record;
static int compare_u64(const void *left, const void *right) {
    const uint64_t a = *(const uint64_t *)left;
    const uint64_t b = *(const uint64_t *)right;
    return (a > b) - (a < b);
}
static size_t percentile_index(unsigned percentile) {
    return ((((size_t)percentile * (size_t)TOTAL) + 99u) / 100u) - 1u;
}
#endif

typedef struct mpmc_context {
    disruptor_t *ring;
    pthread_mutex_t monitor;
    unsigned char observed[TOTAL + 1];
#ifdef SALTS_COMPILER_MPMC_LATENCY
    uint64_t age_ns[TOTAL + 1];
#endif
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
#ifdef SALTS_COMPILER_MPMC_LATENCY
        mpmc_timed_record *slot =
            (mpmc_timed_record *)disruptor_acquire_entry(ctx->ring, &cursor);
        if (slot == NULL) return NULL;
        slot->payload = value_for(cursor.sequence);
        /* The timestamp becomes visible only through Disruptor publication.
         * No concurrent backend is allowed to reuse this slot until release. */
        slot->before_publish_ns = now_ns();
        if (slot->before_publish_ns == 0u) return NULL;
#else
        uint64_t *slot = (uint64_t *)disruptor_acquire_entry(ctx->ring, &cursor);
        if (slot == NULL) return NULL;
        *slot = value_for(cursor.sequence);
#endif
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

#ifdef SALTS_COMPILER_MPMC_LATENCY
        const mpmc_timed_record *slot =
            (const mpmc_timed_record *)disruptor_show_entry(ctx->ring, &cursor);
        /* Capture before lock-protected verdict bookkeeping, to avoid
         * treating the testing mutex delay as queue handoff latency. */
        const uint64_t observed_at_ns = now_ns();
        if (slot == NULL || cursor.sequence == 0u ||
            cursor.sequence > TOTAL || observed_at_ns == 0u ||
            slot->before_publish_ns == 0u ||
            observed_at_ns < slot->before_publish_ns ||
            slot->payload != value_for(cursor.sequence)) {
#else
        const uint64_t *slot =
            (const uint64_t *)disruptor_show_entry(ctx->ring, &cursor);
        if (slot == NULL || cursor.sequence == 0u ||
            cursor.sequence > TOTAL || *slot != value_for(cursor.sequence)) {
#endif
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
#ifdef SALTS_COMPILER_MPMC_LATENCY
            ctx->age_ns[cursor.sequence] = observed_at_ns - slot->before_publish_ns;
#endif
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

int main(int argc, char **argv) {
#if defined(SALTS_COMPILER_MPMC_BENCHMARK) || defined(SALTS_COMPILER_MPMC_LATENCY)
    if (argc != 2 || argv == NULL || argv[1] == NULL) {
        fprintf(stderr, "Benchmark requires one backend name argument\n");
        return 1;
    }
    const uint64_t benchmark_start_ns = now_ns();
    if (benchmark_start_ns == 0u) return 1;
#else
    (void)argc;
    (void)argv;
#endif
    const disruptor_config_t config = {
#ifdef SALTS_COMPILER_MPMC_LATENCY
        sizeof(mpmc_timed_record), CAPACITY, WORKERS, DISRUPTOR_MODE_WORKER_POOL
#else
        sizeof(uint64_t), CAPACITY, WORKERS, DISRUPTOR_MODE_WORKER_POOL
#endif
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
#if defined(SALTS_COMPILER_MPMC_BENCHMARK) || defined(SALTS_COMPILER_MPMC_LATENCY)
    const uint64_t benchmark_end_ns = now_ns();
    if (benchmark_end_ns == 0u || benchmark_end_ns <= benchmark_start_ns)
        return 1;
    const uint64_t elapsed_ns = benchmark_end_ns - benchmark_start_ns;
#ifdef SALTS_COMPILER_MPMC_LATENCY
    /* Sort only after all producers and consumers have joined. No sample
     * is used unless unique delivery and total retirement were verified. */
    qsort(ctx.age_ns + 1u, TOTAL, sizeof(ctx.age_ns[0]), compare_u64);
    printf("LATENCY backend=%s messages=%zu elapsed_ns=%llu "
           "p50_ns=%llu p95_ns=%llu p99_ns=%llu max_ns=%llu\n",
           argv[1], (size_t)TOTAL, (unsigned long long)elapsed_ns,
           (unsigned long long)ctx.age_ns[1u + percentile_index(50u)],
           (unsigned long long)ctx.age_ns[1u + percentile_index(95u)],
           (unsigned long long)ctx.age_ns[1u + percentile_index(99u)],
           (unsigned long long)ctx.age_ns[TOTAL]);
#else
    const double throughput = ((double)TOTAL * 1000000000.0) / (double)elapsed_ns;
    /* This is aggregate amortized cost per event, NOT individual latency. */
    printf("BENCH backend=%s messages=%zu elapsed_ns=%llu throughput_msg_s=%.3f mean_wall_ns_per_msg=%.3f\n",
           argv[1], (size_t)TOTAL, (unsigned long long)elapsed_ns,
           throughput, (double)elapsed_ns / (double)TOTAL);
#endif
#else
    puts("Actual Disruptor 4 publishers + 4 workers / 12000 exact-once: PASS (diagnostic)");
#endif
    return 0;
}
