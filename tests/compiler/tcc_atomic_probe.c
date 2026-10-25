/* Diagnostic only: check individual compiler atomic operations even when the
 * compiler advertises __STDC_NO_ATOMICS__. Never use this as a Salts admission
 * test or to override the compiler's feature declaration. */
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#define REQUIRE(x) do { if (!(x)) { \
    fprintf(stderr, "atomic probe failed at line %d: %s\n", __LINE__, #x); \
    return 1; \
} } while (0)

int main(void) {
#if defined(__STDC_NO_ATOMICS__)
    puts("compiler advertises __STDC_NO_ATOMICS__=1 (NOT QUALIFIED)");
#else
    puts("compiler does not advertise __STDC_NO_ATOMICS__");
#endif
    atomic_uint value;
    atomic_init(&value, 1u);
    REQUIRE(atomic_fetch_add_explicit(&value, 3u, memory_order_relaxed) == 1u);
    unsigned expected = 3u;
    REQUIRE(!atomic_compare_exchange_strong_explicit(&value, &expected, 7u,
        memory_order_acq_rel, memory_order_acquire));
    REQUIRE(expected == 4u);
    REQUIRE(atomic_compare_exchange_strong_explicit(&value, &expected, 7u,
        memory_order_acq_rel, memory_order_acquire));
    REQUIRE(atomic_load_explicit(&value, memory_order_acquire) == 7u);

    _Atomic(uint64_t) wide;
    atomic_init(&wide, 0u);
    REQUIRE(atomic_fetch_add_explicit(&wide, UINT64_C(0x100000001),
        memory_order_seq_cst) == 0u);
    REQUIRE(atomic_load(&wide) == UINT64_C(0x100000001));

    atomic_flag flag = ATOMIC_FLAG_INIT;
    REQUIRE(!atomic_flag_test_and_set(&flag));
    REQUIRE(atomic_flag_test_and_set(&flag));
    atomic_flag_clear(&flag);
    atomic_thread_fence(memory_order_seq_cst);
    atomic_signal_fence(memory_order_seq_cst);
    puts("individual atomic operations: PASS (not Salts qualification)");
    return 0;
}
