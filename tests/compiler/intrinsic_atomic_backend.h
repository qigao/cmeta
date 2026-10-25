#ifndef SALTS_COMPILER_INTRINSIC_ATOMIC_BACKEND_H
#define SALTS_COMPILER_INTRINSIC_ATOMIC_BACKEND_H
/* TEST-ONLY diagnostic adapter, inspired by c-threads/catomics.h.
 *
 * This uses GNU __atomic intrinsics on separately owned, naturally aligned
 * storage. It does NOT implement C11 _Atomic types or a C11 stdatomic.h shim.
 * Do not copy/cast live C11 atomics into this storage, copy storage while
 * active, install this header, or alter __STDC_NO_ATOMICS__ to admit TCC.
 */
#include <salts/error_codes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/* TinyCC's pinned stdatomic.h supplies ABI declarations for its own
 * atomic_thread_fence/atomic_signal_fence runtime functions and the
 * __atomic_is_lock_free function. TinyCC maps the GNU fence spellings to
 * these runtime symbols. Including it here does NOT change or undefine
 * TinyCC's __STDC_NO_ATOMICS__=1 feature declaration; these storage types
 * remain private and distinct from native C11 _Atomic objects. */
#if defined(__TINYC__)
#include <stdatomic.h>
#endif

#if !defined(__TINYC__) && !defined(__GNUC__) && !defined(__clang__)
#error "The private diagnostic backend requires GNU-style __atomic intrinsics"
#endif

#ifndef __ATOMIC_RELAXED
#define __ATOMIC_RELAXED 0
#define __ATOMIC_CONSUME 1
#define __ATOMIC_ACQUIRE 2
#define __ATOMIC_RELEASE 3
#define __ATOMIC_ACQ_REL 4
#define __ATOMIC_SEQ_CST 5
#endif

typedef enum salts_probe_order {
    SALTS_PROBE_RELAXED = __ATOMIC_RELAXED,
    SALTS_PROBE_CONSUME = __ATOMIC_CONSUME,
    SALTS_PROBE_ACQUIRE = __ATOMIC_ACQUIRE,
    SALTS_PROBE_RELEASE = __ATOMIC_RELEASE,
    SALTS_PROBE_ACQ_REL = __ATOMIC_ACQ_REL,
    SALTS_PROBE_SEQ_CST = __ATOMIC_SEQ_CST
} salts_probe_order;

static inline bool salts_probe_order_valid(salts_probe_order o) {
    return o >= SALTS_PROBE_RELAXED && o <= SALTS_PROBE_SEQ_CST;
}
static inline bool salts_probe_load_order_valid(salts_probe_order o) {
    return o == SALTS_PROBE_RELAXED || o == SALTS_PROBE_CONSUME ||
           o == SALTS_PROBE_ACQUIRE || o == SALTS_PROBE_SEQ_CST;
}
static inline bool salts_probe_store_order_valid(salts_probe_order o) {
    return o == SALTS_PROBE_RELAXED || o == SALTS_PROBE_RELEASE ||
           o == SALTS_PROBE_SEQ_CST;
}
static inline bool salts_probe_cas_orders_valid(salts_probe_order success,
                                                salts_probe_order failure) {
    if (!salts_probe_order_valid(success) ||
        !salts_probe_load_order_valid(failure)) return false;
    switch (failure) {
    case SALTS_PROBE_RELAXED: return true;
    case SALTS_PROBE_CONSUME:
        return success == SALTS_PROBE_CONSUME || success == SALTS_PROBE_ACQUIRE ||
               success == SALTS_PROBE_ACQ_REL || success == SALTS_PROBE_SEQ_CST;
    case SALTS_PROBE_ACQUIRE:
        return success == SALTS_PROBE_ACQUIRE || success == SALTS_PROBE_ACQ_REL ||
               success == SALTS_PROBE_SEQ_CST;
    case SALTS_PROBE_SEQ_CST: return success == SALTS_PROBE_SEQ_CST;
    default: return false;
    }
}

/* Every live access to storage is an intrinsic; validation failures must
 * preserve both storage and caller-owned outputs. In particular, a failed CAS
 * must write the observed value into *expected exactly as in C11. */
#define SALTS_PROBE_INTRINSIC_TYPE(name_, type_) \
    typedef struct name_ { type_ storage; } name_; \
    static inline int name_##_init(name_ *p, type_ value) { \
        if (!p) return SALTS_EINVAL; \
        __atomic_store(&p->storage, &value, __ATOMIC_RELAXED); \
        return SALTS_OK; \
    } \
    static inline int name_##_load(const name_ *p, salts_probe_order order, type_ *out) { \
        if (!p || !out || !salts_probe_load_order_valid(order)) return SALTS_EINVAL; \
        type_ temporary; \
        __atomic_load(&p->storage, &temporary, (int)order); \
        *out = temporary; \
        return SALTS_OK; \
    } \
    static inline int name_##_store(name_ *p, type_ value, salts_probe_order order) { \
        if (!p || !salts_probe_store_order_valid(order)) return SALTS_EINVAL; \
        __atomic_store(&p->storage, &value, (int)order); \
        return SALTS_OK; \
    } \
    static inline int name_##_exchange(name_ *p, type_ value, salts_probe_order order, type_ *out) { \
        if (!p || !out || !salts_probe_order_valid(order)) return SALTS_EINVAL; \
        type_ temporary; \
        __atomic_exchange(&p->storage, &value, &temporary, (int)order); \
        *out = temporary; \
        return SALTS_OK; \
    } \
    static inline int name_##_cas(name_ *p, type_ *expected, type_ desired, \
            salts_probe_order success, salts_probe_order failure, bool *swapped) { \
        if (!p || !expected || !swapped || !salts_probe_cas_orders_valid(success, failure)) \
            return SALTS_EINVAL; \
        *swapped = __atomic_compare_exchange(&p->storage, expected, &desired, 0, \
                                            (int)success, (int)failure); \
        return SALTS_OK; \
    } \
    static inline int name_##_is_lock_free(name_ *p, bool *out) { \
        if (!p || !out) return SALTS_EINVAL; \
        *out = __atomic_is_lock_free(sizeof(p->storage), &p->storage); \
        return SALTS_OK; \
    } \
    typedef type_ name_##_value_type

#define SALTS_PROBE_INTRINSIC_INTEGER(name_, type_) \
    SALTS_PROBE_INTRINSIC_TYPE(name_, type_); \
    static inline int name_##_fetch_add(name_ *p, type_ operand, \
            salts_probe_order order, type_ *old) { \
        if (!p || !old || !salts_probe_order_valid(order)) return SALTS_EINVAL; \
        *old = __atomic_fetch_add(&p->storage, operand, (int)order); \
        return SALTS_OK; \
    }

SALTS_PROBE_INTRINSIC_INTEGER(salts_probe_u8, uint8_t)
SALTS_PROBE_INTRINSIC_INTEGER(salts_probe_u16, uint16_t)
SALTS_PROBE_INTRINSIC_INTEGER(salts_probe_u32, uint32_t)
SALTS_PROBE_INTRINSIC_INTEGER(salts_probe_u64, uint64_t)
SALTS_PROBE_INTRINSIC_TYPE(salts_probe_ptr, void *);

static inline void salts_probe_thread_fence(salts_probe_order order) {
#if defined(__TINYC__)
    atomic_thread_fence((memory_order)order);
#else
    __atomic_thread_fence((int)order);
#endif
}
static inline void salts_probe_signal_fence(salts_probe_order order) {
#if defined(__TINYC__)
    atomic_signal_fence((memory_order)order);
#else
    __atomic_signal_fence((int)order);
#endif
}
#endif /* SALTS_COMPILER_INTRINSIC_ATOMIC_BACKEND_H */
