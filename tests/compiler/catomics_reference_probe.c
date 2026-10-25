/* Compile against upstream, pinned zelang-dev/c-threads/include/catomics.h.
 * Narrow A/B diagnosis only: NEVER install or include it in Salts products.
 * This does not override __STDC_NO_ATOMICS__ and is not C11 admission.
 */
#include <catomics.h>
#include <stdint.h>
#include <stdio.h>

int main(void) {
    atomic_uint counter;
    atomic_init(&counter, 1u);
    unsigned old = atomic_fetch_add_explicit(&counter, 3u, memory_order_relaxed);
    if (old != 1u || atomic_load_explicit(&counter, memory_order_acquire) != 4u) {
        fprintf(stderr, "catomics fetch-add / acquire-load mismatch\n");
        return 1;
    }
    unsigned expected = 5u;
    if (atomic_compare_exchange_strong_explicit(
        &counter, &expected, 7u, memory_order_acq_rel, memory_order_acquire)) {
        fprintf(stderr, "catomics failed CAS incorrectly succeeded\n");
        return 1;
    }
    if (expected != 4u) {
        fprintf(stderr, "catomics failed CAS expected writeback mismatch\n");
        return 1;
    }
    printf("c-threads catomics direct header probe: PASS (limited only)\n");
    return 0;
}
