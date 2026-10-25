/* Real Salts::Concurrency SPSC Ring implementation qualification.
 *
 * This test compiles and links the unmodified concurrency/src/spsc_ring.c
 * against the public header. On TCC it is EXPERIMENTAL: successful execution
 * does not override __STDC_NO_ATOMICS__ or qualify the exported _Atomic ABI.
 *
 * The producer and consumer use only the documented owner-affine methods:
 * no reset, shallow copy, or borrowed storage release while active.
 */
#define _POSIX_C_SOURCE 200809L
#include <salts/spsc_ring.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "SPSC real-source test failed at %d: %s\n", __LINE__, #expr); \
        return 1; \
    } \
} while (0)

enum { RING_CAPACITY = 256, FRAME_BYTES = 13, FRAMES = 50000 };
enum { TIMEOUT_SECONDS = 15 };

typedef struct spsc_test_context {
    cmeta_spsc_ring ring;
    uint8_t storage[RING_CAPACITY];
    int consumer_status;
    size_t observed_frames;
} spsc_test_context;

static uint64_t monotonic_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0u;
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) +
           (uint64_t)ts.tv_nsec;
}

static bool past_deadline(uint64_t start_ns) {
    const uint64_t now = monotonic_ns();
    return now == 0u || now < start_ns ||
           now - start_ns >= UINT64_C(TIMEOUT_SECONDS) * UINT64_C(1000000000);
}

static uint32_t frame_checksum(uint64_t sequence) {
    return (uint32_t)((sequence * UINT64_C(2654435761)) ^
                       (sequence >> 29));
}

static void make_frame(uint8_t *bytes, uint64_t sequence) {
    const uint32_t digest = frame_checksum(sequence);
    for (unsigned i = 0u; i < 8u; ++i)
        bytes[i] = (uint8_t)(sequence >> (i * 8u));
    for (unsigned i = 0u; i < 4u; ++i)
        bytes[8u + i] = (uint8_t)(digest >> (i * 8u));
    bytes[12] = (uint8_t)(0xa5u ^ bytes[0]);
}

static int serial_wrap_case(void) {
    uint8_t storage[64] = {0};
    cmeta_spsc_ring ring = {0};
    cmeta_spsc_ring rejected = {0};
    size_t available = 0u;
    uint8_t *where;

    CHECK(!cmeta_spsc_ring_init(&rejected, storage, 63u));
    CHECK(cmeta_spsc_ring_init(&ring, storage, sizeof(storage)));
    CHECK(cmeta_spsc_ring_write_available(&ring) == 63u);
    where = cmeta_spsc_ring_write_acquire(&ring, 45u);
    CHECK(where == storage);
    memset(where, 0x17, 45u);
    cmeta_spsc_ring_write_release(&ring, 45u);
    where = cmeta_spsc_ring_read_acquire(&ring, &available);
    CHECK(where == storage && available == 45u);
    CHECK(where[0] == 0x17u && where[44] == 0x17u);
    cmeta_spsc_ring_read_release(&ring, 45u);

    /* Write position is 45, leaving only 19 contiguous bytes. A 26-byte
     * write must create and publish a skip-to-zero wrap marker. */
    where = cmeta_spsc_ring_write_acquire(&ring, 26u);
    CHECK(where == storage);
    memset(where, 0x83, 26u);
    cmeta_spsc_ring_write_release(&ring, 26u);
    where = cmeta_spsc_ring_read_acquire(&ring, &available);
    CHECK(where == storage && available == 26u);
    CHECK(where[0] == 0x83u && where[25] == 0x83u);
    cmeta_spsc_ring_read_release(&ring, 26u);
    CHECK(cmeta_spsc_ring_read_available(&ring) == 0u);
    CHECK(cmeta_spsc_ring_write_available(&ring) == 63u);
    return 0;
}

static void *consumer(void *arg) {
    spsc_test_context *context = (spsc_test_context *)arg;
    const uint64_t started = monotonic_ns();
    uint8_t expected[FRAME_BYTES];
    context->consumer_status = 1;

    if (started == 0u) return NULL;
    for (uint64_t sequence = 1u; sequence <= FRAMES; ++sequence) {
        size_t available = 0u;
        uint8_t *where;
        do {
            where = cmeta_spsc_ring_read_acquire(&context->ring, &available);
            if (where != NULL) break;
            if (past_deadline(started)) {
                fprintf(stderr, "SPSC read timeout at sequence %llu\n",
                        (unsigned long long)sequence);
                return NULL;
            }
            (void)sched_yield();
        } while (1);
        if (available < FRAME_BYTES) {
            fprintf(stderr, "SPSC read split frame at sequence %llu: %zu\n",
                    (unsigned long long)sequence, available);
            return NULL;
        }
        make_frame(expected, sequence);
        if (memcmp(where, expected, FRAME_BYTES) != 0) {
            fprintf(stderr, "SPSC corrupt/out-of-order frame: %llu\n",
                    (unsigned long long)sequence);
            return NULL;
        }
        cmeta_spsc_ring_read_release(&context->ring, FRAME_BYTES);
        ++context->observed_frames;
    }
    context->consumer_status = 0;
    return NULL;
}

static int concurrent_case(void) {
    spsc_test_context context = {0};
    pthread_t worker;
    size_t writer_wraps = 0u;
    const uint64_t started = monotonic_ns();
    CHECK(started != 0u);
    CHECK(cmeta_spsc_ring_init(&context.ring, context.storage, RING_CAPACITY));
    CHECK(pthread_create(&worker, NULL, consumer, &context) == 0);
    int producer_ok = 1;
    for (uint64_t sequence = 1u; sequence <= FRAMES; ++sequence) {
        uint8_t *where;
        do {
            where = cmeta_spsc_ring_write_acquire(&context.ring, FRAME_BYTES);
            if (where != NULL) break;
            if (past_deadline(started)) {
                fprintf(stderr, "SPSC write timeout at sequence %llu\n",
                        (unsigned long long)sequence);
                producer_ok = 0;
                break;
            }
            (void)sched_yield();
        } while (1);
        if (!producer_ok) break;
        if (where == context.storage) ++writer_wraps;
        make_frame(where, sequence);
        cmeta_spsc_ring_write_release(&context.ring, FRAME_BYTES);
    }
    CHECK(pthread_join(worker, NULL) == 0);
    CHECK(producer_ok);
    CHECK(context.consumer_status == 0);
    CHECK(context.observed_frames == FRAMES);
    CHECK(writer_wraps > 100u);
    CHECK(cmeta_spsc_ring_read_available(&context.ring) == 0u);
    CHECK(cmeta_spsc_ring_write_available(&context.ring) == RING_CAPACITY - 1u);
    return 0;
}

int main(void) {
    if (serial_wrap_case() != 0) return 1;
    if (concurrent_case() != 0) return 1;
    puts("Actual Salts SPSC Ring / wrap / bounded 2-thread FIFO: PASS (not compiler admission)");
    return 0;
}
