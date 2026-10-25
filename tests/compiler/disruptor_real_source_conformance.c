/* Real Salts::Concurrency Disruptor runtime qualification.
 * Compile original concurrency/src/disruptor.c and Platform thread_posix.c;
 * no reimplementation and no external full-SDK or CFlow dependency.
 *
 * In TinyCC only, the build is an explicitly diagnostic native-TLS probe,
 * not compiler admission or a production ABI change. Tests run both
 * BROADCAST and WORKER_POOL modes with bounded spin timeouts.
 */
#define _POSIX_C_SOURCE 200809L
#include <salts/disruptor.h>
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "Disruptor real-source test failed at %d: %s\n", __LINE__, #expr); \
        return 1; \
    } \
} while (0)

enum { RING_CAPACITY = 64, MESSAGE_COUNT = 20000, DEADLINE_SEC = 15 };
typedef struct disruptor_probe {
    disruptor_t *ring;
    disruptor_consumer_t consumer;
    disruptor_mode_t mode;
    int worker_status;
    size_t completed;
} disruptor_probe;

static uint64_t clock_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0u;
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) +
           (uint64_t)ts.tv_nsec;
}
static bool timed_out(uint64_t start_ns) {
    const uint64_t now = clock_ns();
    return now == 0u || now < start_ns ||
           now - start_ns >= (uint64_t)DEADLINE_SEC * UINT64_C(1000000000);
}

static uint64_t content_of(uint64_t sequence) {
    return sequence ^ UINT64_C(0xdecafbad10203040);
}

static void *read_worker(void *opaque) {
    disruptor_probe *probe = (disruptor_probe *)opaque;
    const uint64_t start_ns = clock_ns();
    uint64_t next = 1u;
    probe->worker_status = 1;
    if (start_ns == 0u) return NULL;

    while (next <= MESSAGE_COUNT) {
        disruptor_cursor_t cursor = {0};
        if (probe->mode == DISRUPTOR_MODE_BROADCAST) {
            cursor.sequence = next;
            if (!disruptor_consumer_wait_for_nonblocking_for(
                    probe->ring, &probe->consumer, &cursor)) {
                if (timed_out(start_ns)) {
                    fprintf(stderr, "Disruptor broadcast read timeout at %llu\n",
                            (unsigned long long)next);
                    return NULL;
                }
                (void)sched_yield();
                continue;
            }
            if (cursor.sequence < next || cursor.sequence > MESSAGE_COUNT) {
                fprintf(stderr, "Disruptor broadcast cursor jump %llu\n",
                        (unsigned long long)cursor.sequence);
                return NULL;
            }
            for (uint64_t sequence = next; sequence <= cursor.sequence; ++sequence) {
                disruptor_cursor_t read_cursor = {sequence};
                const uint64_t *item = (const uint64_t *)disruptor_show_entry(
                    probe->ring, &read_cursor);
                if (item == NULL || *item != content_of(sequence)) {
                    fprintf(stderr, "Disruptor broadcast corrupted seq %llu\n",
                            (unsigned long long)sequence);
                    return NULL;
                }
                ++probe->completed;
            }
            disruptor_consumer_release_entry(probe->ring, &probe->consumer, &cursor);
            next = cursor.sequence + 1u;
        } else {
            if (!disruptor_worker_try_claim(probe->ring, &cursor)) {
                if (timed_out(start_ns)) {
                    fprintf(stderr, "Disruptor worker read timeout at %llu\n",
                            (unsigned long long)next);
                    return NULL;
                }
                (void)sched_yield();
                continue;
            }
            const uint64_t *item = (const uint64_t *)disruptor_show_entry(
                probe->ring, &cursor);
            if (cursor.sequence != next || item == NULL ||
                *item != content_of(next)) {
                fprintf(stderr, "Disruptor worker corrupted/out-of-order seq %llu\n",
                        (unsigned long long)cursor.sequence);
                return NULL;
            }
            disruptor_worker_release_entry(probe->ring, &cursor);
            ++probe->completed;
            ++next;
        }
    }
    probe->worker_status = 0;
    return NULL;
}

static int run_mode(disruptor_mode_t mode) {
    const disruptor_config_t config = {
        sizeof(uint64_t), RING_CAPACITY, 1u, mode
    };
    disruptor_probe probe = {0};
    pthread_t worker;
    int published = 1;
    const uint64_t start_ns = clock_ns();
    CHECK(start_ns != 0u);
    probe.mode = mode;
    probe.ring = disruptor_create(&config);
    CHECK(probe.ring != NULL);
    CHECK(disruptor_capacity(probe.ring) == RING_CAPACITY);
    CHECK(disruptor_entry_size(probe.ring) == sizeof(uint64_t));
    if (mode == DISRUPTOR_MODE_BROADCAST) {
        CHECK(disruptor_consumer_register(probe.ring, &probe.consumer) == 1u);
    }
    CHECK(pthread_create(&worker, NULL, read_worker, &probe) == 0);
    for (uint64_t sequence = 1u; sequence <= MESSAGE_COUNT; ++sequence) {
        disruptor_cursor_t cursor = {0};
        do {
            if (disruptor_publisher_try_claim(probe.ring, &cursor)) break;
            if (timed_out(start_ns)) {
                fprintf(stderr, "Disruptor publisher timeout at %llu mode=%d\n",
                        (unsigned long long)sequence, (int)mode);
                published = 0;
                break;
            }
            (void)sched_yield();
        } while (1);
        if (!published) break;
        if (cursor.sequence != sequence) {
            published = 0;
            fprintf(stderr, "Disruptor publisher cursor gap at %llu\n",
                    (unsigned long long)sequence);
            break;
        }
        uint64_t *item = (uint64_t *)disruptor_acquire_entry(probe.ring, &cursor);
        if (item == NULL) { published = 0; break; }
        *item = content_of(sequence);
        if (!disruptor_publisher_publish(probe.ring, &cursor)) {
            published = 0;
            break;
        }
    }
    CHECK(pthread_join(worker, NULL) == 0);
    int ok = published && probe.worker_status == 0 &&
             probe.completed == MESSAGE_COUNT;
    if (mode == DISRUPTOR_MODE_BROADCAST)
        disruptor_consumer_unregister(probe.ring, &probe.consumer);
    disruptor_destroy(probe.ring);
    CHECK(ok);
    return 0;
}

int main(void) {
    const disruptor_config_t invalid = {sizeof(uint64_t), 63u, 1u,
                                        DISRUPTOR_MODE_WORKER_POOL};
    CHECK(disruptor_create(&invalid) == NULL);
    if (run_mode(DISRUPTOR_MODE_BROADCAST) != 0) return 1;
    if (run_mode(DISRUPTOR_MODE_WORKER_POOL) != 0) return 1;
    puts("Actual Salts Disruptor Broadcast/WorkerPool 20k FIFO: PASS (diagnostic)");
    return 0;
}
