/* Salts' typed atomic contract, independently linkable without Salts DSOs. */
#include <salts/atomic.h>

#include <stdbool.h>
#include <stdio.h>

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "%s:%d: failed: %s\n", __FILE__, __LINE__, #expr); \
        return 1; \
    } \
} while (0)

SALTS_ATOMIC_TYPE(conformance_int, int);
SALTS_ATOMIC_TYPE(conformance_ptr, int *);

static int numeric_contract(void) {
    conformance_int value;
    int out = 123;
    int expected = 123;
    bool exchanged = true;
    bool lock_free = false;

    CHECK(conformance_int_init(NULL, 1) == SALTS_EINVAL);
    CHECK(conformance_int_init(&value, 7) == SALTS_OK);
    CHECK(conformance_int_load(&value, memory_order_acquire, &out) == SALTS_OK);
    CHECK(out == 7);
    CHECK(conformance_int_is_lock_free(&value, &lock_free) == SALTS_OK);
    (void)lock_free; /* Correctness must not require lock-free native operations. */
    CHECK(conformance_int_is_lock_free(&value, NULL) == SALTS_EINVAL);

    out = 123;
    CHECK(conformance_int_load(&value, memory_order_release, &out) == SALTS_EINVAL);
    CHECK(out == 123);
    CHECK(conformance_int_load(NULL, memory_order_relaxed, &out) == SALTS_EINVAL);
    CHECK(conformance_int_load(&value, memory_order_relaxed, NULL) == SALTS_EINVAL);
    CHECK(conformance_int_store(&value, 99, memory_order_acquire) == SALTS_EINVAL);
    CHECK(conformance_int_store(&value, 99, (memory_order)99) == SALTS_EINVAL);
    CHECK(conformance_int_load(&value, memory_order_relaxed, &out) == SALTS_OK);
    CHECK(out == 7);

    CHECK(conformance_int_exchange(&value, 9, memory_order_acq_rel, &out) == SALTS_OK);
    CHECK(out == 7);
    CHECK(conformance_int_exchange(&value, 42, memory_order_relaxed, NULL) == SALTS_EINVAL);

    /* A failed CAS updates expected, not the stored value. */
    expected = 8;
    exchanged = true;
    CHECK(conformance_int_compare_exchange(&value, &expected, 10,
        memory_order_acq_rel, memory_order_acquire, &exchanged) == SALTS_OK);
    CHECK(!exchanged && expected == 9);
    CHECK(conformance_int_load(&value, memory_order_relaxed, &out) == SALTS_OK);
    CHECK(out == 9);

    /* Invalid orders are rejected before touching state and caller outputs. */
    expected = 9;
    exchanged = true;
    CHECK(conformance_int_compare_exchange(&value, &expected, 10,
        memory_order_release, memory_order_acquire, &exchanged) == SALTS_EINVAL);
    CHECK(expected == 9 && exchanged);
    CHECK(conformance_int_compare_exchange(&value, &expected, 10,
        memory_order_acq_rel, memory_order_release, &exchanged) == SALTS_EINVAL);
    CHECK(expected == 9 && exchanged);
    CHECK(conformance_int_compare_exchange(&value, &expected, 10,
        memory_order_acq_rel, memory_order_acquire, NULL) == SALTS_EINVAL);
    CHECK(expected == 9 && exchanged);
    CHECK(conformance_int_compare_exchange(&value, &expected, 10,
        memory_order_acq_rel, memory_order_acquire, &exchanged) == SALTS_OK);
    CHECK(exchanged && expected == 9);
    CHECK(conformance_int_store(&value, 13, memory_order_release) == SALTS_OK);
    CHECK(conformance_int_load(&value, memory_order_acquire, &out) == SALTS_OK);
    CHECK(out == 13);
    return 0;
}

static int pointer_contract(void) {
    int items[3] = {0};
    conformance_ptr value;
    int *out = NULL;
    int *expected = &items[1];
    bool exchanged = true;
    bool lock_free = false;

    CHECK(conformance_ptr_init(&value, &items[0]) == SALTS_OK);
    CHECK(conformance_ptr_compare_exchange(&value, &expected, &items[2],
        memory_order_acq_rel, memory_order_acquire, &exchanged) == SALTS_OK);
    CHECK(!exchanged && expected == &items[0]);
    CHECK(conformance_ptr_compare_exchange(&value, &expected, &items[2],
        memory_order_acq_rel, memory_order_acquire, &exchanged) == SALTS_OK);
    CHECK(exchanged && expected == &items[0]);
    CHECK(conformance_ptr_load(&value, memory_order_acquire, &out) == SALTS_OK);
    CHECK(out == &items[2]);
    CHECK(conformance_ptr_is_lock_free(&value, &lock_free) == SALTS_OK);
    (void)lock_free;
    CHECK(conformance_ptr_store(&value, &items[1], memory_order_release) == SALTS_OK);
    CHECK(conformance_ptr_exchange(&value, &items[2], memory_order_acq_rel, &out) == SALTS_OK);
    CHECK(out == &items[1]);
    return 0;
}

int main(void) {
    if (numeric_contract() != 0) return 1;
    if (pointer_contract() != 0) return 1;
    puts("Salts typed atomic contract: PASS");
    return 0;
}
