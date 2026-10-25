/* Compiler-level C11 atomics conformance. Does not link Salts or TurboWasm.
 * The operation matrix is adapted from TurboWasm's guest/c11_atomics.c;
 * WebAssembly-only wait/notify is replaced with native OS threads.
 */
#if !defined(__STDC_VERSION__) || __STDC_VERSION__ < 201112L
#error "C11 is required"
#endif
#if defined(__STDC_NO_ATOMICS__)
#error "Native C11 atomics are required"
#endif

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
typedef HANDLE native_thread;
static void native_yield(void) { (void)SwitchToThread(); }
#else
#include <pthread.h>
#include <sched.h>
typedef pthread_t native_thread;
static void native_yield(void) { (void)sched_yield(); }
#endif

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "%s:%d: failed: %s\n", __FILE__, __LINE__, #expr); \
        return 1; \
    } \
} while (0)

/* Check scalar widths, RMW return values, CAS expected write-back, and orders.
 * Lock-free availability is a capability, not a requirement for correctness. */
#define CHECK_WIDTH(T) do { \
    volatile _Atomic(T) value; \
    T expected; \
    atomic_init(&value, (T)0); \
    (void)atomic_is_lock_free(&value); \
    atomic_store_explicit(&value, (T)0xf0, memory_order_release); \
    CHECK(atomic_load_explicit(&value, memory_order_acquire) == (T)0xf0); \
    CHECK(atomic_fetch_or(&value, (T)0x0f) == (T)0xf0); \
    CHECK(atomic_fetch_and(&value, (T)0x3c) == (T)0xff); \
    CHECK(atomic_fetch_xor(&value, (T)0x0c) == (T)0x3c); \
    CHECK(atomic_exchange(&value, (T)-1) == (T)0x30); \
    CHECK(atomic_fetch_add(&value, (T)1) == (T)-1); \
    CHECK(atomic_load(&value) == (T)0); \
    CHECK(atomic_fetch_sub(&value, (T)1) == (T)0); \
    expected = 0; \
    CHECK(!atomic_compare_exchange_strong_explicit(&value, &expected, \
        (T)7, memory_order_acq_rel, memory_order_acquire)); \
    CHECK(expected == (T)-1); \
    CHECK(atomic_compare_exchange_strong(&value, &expected, (T)7)); \
    expected = 7; \
    while (!atomic_compare_exchange_weak_explicit(&value, &expected, \
        (T)9, memory_order_relaxed, memory_order_relaxed)) \
        CHECK(expected == (T)7); \
    CHECK(atomic_load(&value) == (T)9); \
} while (0)

static int scalar_and_pointer_tests(void) {
    CHECK_WIDTH(uint8_t);
    CHECK_WIDTH(uint16_t);
    CHECK_WIDTH(uint32_t);
    CHECK_WIDTH(uint64_t);

    /* Byte-pointer arithmetic is unambiguous across native toolchains. */
    unsigned char bytes[8] = {0};
    _Atomic(unsigned char *) pointer;
    atomic_init(&pointer, bytes);
    (void)atomic_is_lock_free(&pointer);
    CHECK(atomic_fetch_add(&pointer, 2) == bytes);
    CHECK(atomic_load(&pointer) == bytes + 2);
    CHECK(atomic_fetch_sub(&pointer, 1) == bytes + 2);
    CHECK(atomic_load(&pointer) == bytes + 1);

    /* Typed pointer load/store/CAS are checked separately from fetch_add:
     * native GCC and Clang currently disagree on fetch_add's scaling. */
    int items[4] = {0};
    _Atomic(int *) typed;
    int *expected_pointer = &items[1];
    atomic_init(&typed, &items[0]);
    CHECK(!atomic_compare_exchange_strong(&typed, &expected_pointer, &items[2]));
    CHECK(expected_pointer == &items[0]);
    CHECK(atomic_compare_exchange_strong(&typed, &expected_pointer, &items[2]));
    CHECK(atomic_load(&typed) == &items[2]);

    /* Detect macro implementations that evaluate an argument more than once. */
    _Atomic(int) slots[2];
    atomic_init(&slots[0], 0);
    atomic_init(&slots[1], 0);
    int index = 0;
    atomic_store_explicit(&slots[index++], 5, memory_order_release);
    CHECK(index == 1);
    CHECK(atomic_load(&slots[0]) == 5 && atomic_load(&slots[1]) == 0);
    index = 0;
    CHECK(atomic_fetch_add(&slots[index++], 1) == 5);
    CHECK(index == 1 && atomic_load(&slots[0]) == 6);
    int expected = 6;
    index = 0;
    CHECK(atomic_compare_exchange_strong(&slots[index++], &expected, 7));
    CHECK(index == 1 && atomic_load(&slots[0]) == 7);

    atomic_flag flag = ATOMIC_FLAG_INIT;
    CHECK(!atomic_flag_test_and_set_explicit(&flag, memory_order_acquire));
    CHECK(atomic_flag_test_and_set_explicit(&flag, memory_order_acquire));
    atomic_flag_clear_explicit(&flag, memory_order_release);
    CHECK(!atomic_flag_test_and_set(&flag));
    atomic_flag_clear(&flag);

    atomic_thread_fence(memory_order_seq_cst);
    atomic_signal_fence(memory_order_seq_cst);
    return 0;
}

enum { WORKERS = 4, ROUNDS = 4000 };
typedef struct worker_context { unsigned index; } worker_context;
static atomic_uint ready;
static atomic_uint start_gate;
static atomic_uint increments;
static _Atomic(uint64_t) wide_increments;
static atomic_uint published[WORKERS];
static unsigned payload[WORKERS];
static atomic_flag counter_lock = ATOMIC_FLAG_INIT;
static unsigned guarded_count;

static int worker_body(unsigned index) {
    atomic_fetch_add_explicit(&ready, 1u, memory_order_relaxed);
    while (!atomic_load_explicit(&start_gate, memory_order_acquire))
        native_yield();
    for (unsigned iteration = 0; iteration < ROUNDS; ++iteration) {
        atomic_fetch_add_explicit(&wide_increments,
            UINT64_C(0x100000001), memory_order_relaxed);
        unsigned expected = atomic_load_explicit(&increments, memory_order_relaxed);
        while (!atomic_compare_exchange_weak_explicit(&increments, &expected,
            expected + 1u, memory_order_relaxed, memory_order_relaxed)) {}
        while (atomic_flag_test_and_set_explicit(&counter_lock,
            memory_order_acquire)) native_yield();
        ++guarded_count;
        atomic_flag_clear_explicit(&counter_lock, memory_order_release);
    }
    /* Consumer reads this *before* joining the producer. */
    payload[index] = 73u + index;
    atomic_store_explicit(&published[index], 1u, memory_order_release);
    return 0;
}

#if defined(_WIN32)
static DWORD WINAPI worker_entry(LPVOID arg) {
    return (DWORD)worker_body(((worker_context *)arg)->index);
}
static int native_create(native_thread *thread, worker_context *arg) {
    *thread = CreateThread(NULL, 0, worker_entry, arg, 0, NULL);
    return *thread == NULL ? -1 : 0;
}
static int native_join(native_thread thread) {
    DWORD exit_code = 1u;
    DWORD wait_result = WaitForSingleObject(thread, INFINITE);
    int ok = wait_result == WAIT_OBJECT_0 &&
             GetExitCodeThread(thread, &exit_code) && exit_code == 0u;
    CloseHandle(thread);
    return ok ? 0 : -1;
}
#else
static void *worker_entry(void *arg) {
    return (void *)(intptr_t)worker_body(((worker_context *)arg)->index);
}
static int native_create(native_thread *thread, worker_context *arg) {
    return pthread_create(thread, NULL, worker_entry, arg);
}
static int native_join(native_thread thread) {
    void *result = NULL;
    if (pthread_join(thread, &result) != 0) return -1;
    return (intptr_t)result == 0 ? 0 : -1;
}
#endif

static int threaded_tests(void) {
    native_thread threads[WORKERS];
    worker_context contexts[WORKERS];
    unsigned started = 0;
    int status = 0;

    atomic_init(&ready, 0u);
    atomic_init(&start_gate, 0u);
    atomic_init(&increments, 0u);
    atomic_init(&wide_increments, 0u);
    for (unsigned i = 0; i < WORKERS; ++i)
        atomic_init(&published[i], 0u);

    for (unsigned i = 0; i < WORKERS; ++i) {
        contexts[i].index = i;
        if (native_create(&threads[i], &contexts[i]) != 0) {
            fprintf(stderr, "thread creation failed at worker %u\n", i);
            status = 1;
            break;
        }
        ++started;
    }
    if (status == 0) {
        while (atomic_load_explicit(&ready, memory_order_acquire) < WORKERS)
            native_yield();
    }
    atomic_store_explicit(&start_gate, 1u, memory_order_release);
    if (status == 0) {
        for (unsigned i = 0; i < WORKERS; ++i) {
            while (!atomic_load_explicit(&published[i], memory_order_acquire))
                native_yield();
            if (payload[i] != 73u + i) {
                fprintf(stderr, "release/acquire publication failed: %u\n", i);
                status = 1;
            }
        }
    }
    for (unsigned i = 0; i < started; ++i) {
        if (native_join(threads[i]) != 0) {
            fprintf(stderr, "join failed: %u\n", i);
            status = 1;
        }
    }
    if (status != 0) return 1;
    CHECK(atomic_load(&increments) == WORKERS * ROUNDS);
    CHECK(atomic_load(&wide_increments) ==
          UINT64_C(0x100000001) * WORKERS * ROUNDS);
    CHECK(guarded_count == WORKERS * ROUNDS);
    return 0;
}

int main(void) {
    if (scalar_and_pointer_tests() != 0) return 1;
    if (threaded_tests() != 0) return 1;
    puts("atomic language conformance: PASS");
    return 0;
}
