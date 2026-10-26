/* Qualify the real Salts Disruptor worker_waiters + condition variable path.
 * No copied Disruptor implementation, test hooks or concurrency backend swaps.
 *
 * By observing the 17th keep_running callback we prove the worker has entered
 * the locked wait path and incremented worker_waiters before publishing or
 * cancelling. A release/acquire handshake prevents "I started a pthread" from
 * being mistaken for "the worker has actually admitted its wait".
 */
#define _POSIX_C_SOURCE 200809L
#include <salts/disruptor.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "Disruptor park/wake failed at %d: %s\n", __LINE__, #x); \
    return 1; \
} } while (0)

enum { WAIT_PATH_CALLBACK = 17, PARK_DEADLINE_SEC = 8 };

typedef struct park_case {
    disruptor_t *ring;
    atomic_uint callback_calls;
    atomic_bool keep_running;
    int got_entry;
    int consumer_ok;
} park_case;

static uint64_t clock_ns(void) {
    struct timespec time_now;
    if (clock_gettime(CLOCK_MONOTONIC, &time_now) != 0)
        return 0u;
    return (uint64_t)time_now.tv_sec * UINT64_C(1000000000) +
           (uint64_t)time_now.tv_nsec;
}

static int should_keep_running(void *opaque) {
    park_case *state = (park_case *)opaque;
    (void)atomic_fetch_add_explicit(&state->callback_calls, 1u,
                                     memory_order_release);
    return atomic_load_explicit(&state->keep_running, memory_order_acquire);
}

static void *waiting_worker(void *opaque) {
    park_case *state = (park_case *)opaque;
    disruptor_cursor_t cursor = {0};
    state->consumer_ok = 0;
    state->got_entry = disruptor_worker_claim_wait(state->ring, &cursor,
                                                    should_keep_running, state);
    if (state->got_entry == 0) {
        state->consumer_ok = 1;
        return NULL;
    }
    const uint64_t *payload = (const uint64_t *)disruptor_show_entry(state->ring, &cursor);
    if (cursor.sequence != 1u || payload == NULL ||
        *payload != UINT64_C(0x7a55000000000001))
        return NULL;
    disruptor_worker_release_entry(state->ring, &cursor);
    state->consumer_ok = 1;
    return NULL;
}

static int run_parked(bool cancel) {
    const disruptor_config_t cfg = {
        sizeof(uint64_t), 1u, 1u, DISRUPTOR_MODE_WORKER_POOL
    };
    park_case state = {0};
    pthread_t worker;
    const uint64_t start_ns = clock_ns();
    CHECK(start_ns != 0u);
    state.ring = disruptor_create(&cfg);
    CHECK(state.ring != NULL);
    atomic_init(&state.callback_calls, 0u);
    atomic_init(&state.keep_running, true);
    CHECK(pthread_create(&worker, NULL, waiting_worker, &state) == 0);

    unsigned admitted_calls = 0u;
    const uint64_t max_elapsed =
        (uint64_t)PARK_DEADLINE_SEC * UINT64_C(1000000000);
    for (;;) {
        admitted_calls = atomic_load_explicit(&state.callback_calls,
                                               memory_order_acquire);
        if (admitted_calls >= WAIT_PATH_CALLBACK)
            break;
        const uint64_t now = clock_ns();
        if (now == 0u || now < start_ns || now - start_ns >= max_elapsed)
            break;
        (void)sched_yield();
    }
    int started_park = admitted_calls >= WAIT_PATH_CALLBACK;
    int published = 0;
    if (started_park && !cancel) {
        disruptor_cursor_t cursor = {0};
        if (disruptor_publisher_try_claim(state.ring, &cursor) &&
            cursor.sequence == 1u) {
            uint64_t *value =
                (uint64_t *)disruptor_acquire_entry(state.ring, &cursor);
            if (value != NULL) {
                *value = UINT64_C(0x7a55000000000001);
                published = disruptor_publisher_publish(state.ring, &cursor);
            }
        }
    }
    if (cancel || !published) {
        /* The cancellation path rechecks keep_running while holding the
         * existing worker wait mutex. Explicit wake avoids lost wakeups. */
        atomic_store_explicit(&state.keep_running, false, memory_order_release);
        disruptor_worker_wake_all(state.ring);
    }
    CHECK(pthread_join(worker, NULL) == 0);
    int ok = started_park && state.consumer_ok &&
             (cancel ? state.got_entry == 0 : (published && state.got_entry == 1));
    if (ok && !cancel) {
        /* Capacity=1: the retiring worker must unlock slot 1 for a new claim. */
        disruptor_cursor_t next = {0};
        ok = disruptor_publisher_try_claim(state.ring, &next) == 1 &&
             next.sequence == 2u;
    }
    disruptor_destroy(state.ring);
    CHECK(ok);
    return 0;
}

int main(void) {
    if (run_parked(false) != 0) return 1; /* real publication wakes parked worker */
    if (run_parked(true) != 0) return 1;  /* stop and wake cleanly with no entry */
    puts("Actual Disruptor parked worker wake and cancellation: PASS (diagnostic)");
    return 0;
}
