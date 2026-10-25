#ifndef SALTS_COMPILER_ATOMIC_ABI_H
#define SALTS_COMPILER_ATOMIC_ABI_H

/* Diagnostic-only cross-compiler carrier; NOT a Salts production public ABI.
 * Both translation units must agree on all atomic storage layout before access.
 */
#include <stdatomic.h>
#include <stdint.h>
#include <stddef.h>

typedef struct atomic_abi_shared {
    _Atomic(uint32_t) counter32;
    _Atomic(uint64_t) counter64;
    _Atomic(void *) pointer;
    _Atomic(uint32_t) start;
    _Atomic(uint32_t) published;
    uint32_t payload;
} atomic_abi_shared;

enum { ATOMIC_ABI_LAYOUT_FIELDS = 14 };
typedef struct atomic_abi_layout {
    size_t fields[ATOMIC_ABI_LAYOUT_FIELDS];
} atomic_abi_layout;

static inline atomic_abi_layout atomic_abi_layout_local(void) {
    atomic_abi_layout layout = {{
        sizeof(atomic_abi_shared), _Alignof(atomic_abi_shared),
        offsetof(atomic_abi_shared, counter32),
        sizeof(((_Atomic(uint32_t) *)0)[0]), _Alignof(_Atomic(uint32_t)),
        offsetof(atomic_abi_shared, counter64),
        sizeof(((_Atomic(uint64_t) *)0)[0]), _Alignof(_Atomic(uint64_t)),
        offsetof(atomic_abi_shared, pointer),
        sizeof(((_Atomic(void *) *)0)[0]), _Alignof(_Atomic(void *)),
        offsetof(atomic_abi_shared, start),
        offsetof(atomic_abi_shared, published),
        offsetof(atomic_abi_shared, payload)
    }};
    return layout;
}

void atomic_abi_peer_layout(atomic_abi_layout *out);
int atomic_abi_peer_pointer_cas(atomic_abi_shared *shared, void **expected,
                                void *desired);
int atomic_abi_peer_work(atomic_abi_shared *shared, unsigned rounds);

#endif /* SALTS_COMPILER_ATOMIC_ABI_H */
