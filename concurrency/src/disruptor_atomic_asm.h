#ifndef SALTS_DISRUPTOR_PRIVATE_ASM_ATOMIC_ADAPTER_H
#define SALTS_DISRUPTOR_PRIVATE_ASM_ATOMIC_ADAPTER_H
/*
 * Internal alternate Disruptor Atomic backend selected at compile-time.
 * This implementation is permitted only inside concurrency/src/disruptor.c.
 * It cannot be installed, included from a public API or treated as C11 _Atomic.
 *
 * We intentionally map the two internal _Atomic typedefs to separately owned
 * assembly slots. The public opaque disruptor_t ABI remains unchanged.
 * Default ASM uses SEQ_CST for everything. The optional ordered experiment
 * uses corresponding relaxed/acquire/release scalar load/store instructions
 * but retains SEQ_CST for ALL RMW and CAS operations.
 * This is not a C11 shim.
 *
 * Its tiny private order enum intentionally mirrors the C11 numeric
 * spellings WITHOUT pretending TinyCC supports standard <stdatomic.h>.
 * Feature macros remain untouched; unrelated consumers fail closed.
 */
#if !defined(SALTS_DISRUPTOR_BACKEND_ASM) || SALTS_DISRUPTOR_BACKEND_ASM != 1
#error "Private ASM Atomic requires SALTS_DISRUPTOR_BACKEND_ASM=1"
#endif
#if defined(SALTS_DISRUPTOR_ASM_ORDERED) && SALTS_DISRUPTOR_ASM_ORDERED != 1
#error "SALTS_DISRUPTOR_ASM_ORDERED must be 1 when selected"
#endif

/* Declare precisely the memory-order spellings referenced by Disruptor.
 * Do not include <stdatomic.h> or falsify TinyCC feature advertisement.
 * This enum is private to this translation unit, never exposed through API.
 */
typedef enum {
    memory_order_relaxed = 0,
    memory_order_consume = 1,
    memory_order_acquire = 2,
    memory_order_release = 3,
    memory_order_acq_rel = 4,
    memory_order_seq_cst = 5
} memory_order;
#include <stdint.h>
#include <stddef.h>
#include "asm-atomic/asm_atomic_private.h"

_Static_assert(sizeof(uint_fast64_t) == 8 && sizeof(uint64_t) == 8 &&
                   sizeof(unsigned int) == 4,
               "Private Disruptor ASM adapter requires 64-bit cursor and 32-bit waiters");
_Static_assert(sizeof(salts_asm_u64_slot) == sizeof(uint_fast64_t) &&
                   _Alignof(salts_asm_u64_slot) >= _Alignof(uint_fast64_t),
               "Separate ASM cursor storage not admitted on this target");
_Static_assert(sizeof(salts_asm_u32_slot) == sizeof(unsigned int) &&
                   _Alignof(salts_asm_u32_slot) >= _Alignof(unsigned int),
               "Separate ASM waiter storage not admitted on this target");

#undef atomic_uint_fast64_t
#undef atomic_uint
#define atomic_uint_fast64_t salts_asm_u64_slot
#define atomic_uint salts_asm_u32_slot

#undef atomic_load_explicit
#undef atomic_store_explicit
#undef atomic_fetch_add_explicit
#undef atomic_fetch_sub_explicit
#undef atomic_compare_exchange_weak_explicit
#undef atomic_compare_exchange_strong_explicit

/* Only these exact two backend-owned pointer types are admitted.
 * _Generic controlling expressions are not evaluated, so each pointer
 * expression is evaluated once in the selected call.
 */
#define SALTS_DISRUPTOR_ASM_LOAD(p_) \
  _Generic((p_), \
    salts_asm_u64_slot *: salts_asm_u64_load, \
    const salts_asm_u64_slot *: salts_asm_u64_load, \
    salts_asm_u32_slot *: salts_asm_u32_load, \
    const salts_asm_u32_slot *: salts_asm_u32_load)
#define SALTS_DISRUPTOR_ASM_STORE(p_) \
  _Generic((p_), \
    salts_asm_u64_slot *: salts_asm_u64_store, \
    salts_asm_u32_slot *: salts_asm_u32_store)
#define SALTS_DISRUPTOR_ASM_ADD(p_) \
  _Generic((p_), \
    salts_asm_u64_slot *: salts_asm_u64_fetch_add, \
    salts_asm_u32_slot *: salts_asm_u32_fetch_add)
#define SALTS_DISRUPTOR_ASM_SUB(p_) \
  _Generic((p_), \
    salts_asm_u64_slot *: salts_asm_u64_fetch_sub, \
    salts_asm_u32_slot *: salts_asm_u32_fetch_sub)
#define SALTS_DISRUPTOR_ASM_CAS(p_) \
  _Generic((p_), \
    salts_asm_u64_slot *: salts_asm_u64_cas, \
    salts_asm_u32_slot *: salts_asm_u32_cas)

/* Optional optimized experiment: match the original Disruptor load/store
 * memory orders exactly, while retaining the stronger seq_cst RMW/CAS.
 * C functions serve as the narrowly typed admission boundary. Unsupported
 * orders fail at runtime instead of silently weakening a future call site.
 */
#ifdef SALTS_DISRUPTOR_ASM_ORDERED
#include <stdlib.h>
static inline uint64_t salts_disruptor_u64_load_ordered(
    const salts_asm_u64_slot *slot, memory_order order) {
    switch (order) {
    case memory_order_relaxed: return salts_asm_u64_load_relaxed(slot);
    case memory_order_consume:
    case memory_order_acquire: return salts_asm_u64_load_acquire(slot);
    case memory_order_seq_cst: return salts_asm_u64_load(slot);
    default: abort();
    }
}
static inline uint32_t salts_disruptor_u32_load_ordered(
    const salts_asm_u32_slot *slot, memory_order order) {
    switch (order) {
    case memory_order_relaxed: return salts_asm_u32_load_relaxed(slot);
    case memory_order_consume:
    case memory_order_acquire: return salts_asm_u32_load_acquire(slot);
    case memory_order_seq_cst: return salts_asm_u32_load(slot);
    default: abort();
    }
}
static inline void salts_disruptor_u64_store_ordered(
    salts_asm_u64_slot *slot, uint64_t value, memory_order order) {
    switch (order) {
    case memory_order_relaxed: salts_asm_u64_store_relaxed(slot, value); break;
    case memory_order_release: salts_asm_u64_store_release(slot, value); break;
    case memory_order_seq_cst: salts_asm_u64_store(slot, value); break;
    default: abort();
    }
}
static inline void salts_disruptor_u32_store_ordered(
    salts_asm_u32_slot *slot, uint32_t value, memory_order order) {
    switch (order) {
    case memory_order_relaxed: salts_asm_u32_store_relaxed(slot, value); break;
    case memory_order_release: salts_asm_u32_store_release(slot, value); break;
    case memory_order_seq_cst: salts_asm_u32_store(slot, value); break;
    default: abort();
    }
}
#define SALTS_DISRUPTOR_ASM_LOAD_ORDERED(p_) \
    _Generic((p_), \
      salts_asm_u64_slot *: salts_disruptor_u64_load_ordered, \
      const salts_asm_u64_slot *: salts_disruptor_u64_load_ordered, \
      salts_asm_u32_slot *: salts_disruptor_u32_load_ordered, \
      const salts_asm_u32_slot *: salts_disruptor_u32_load_ordered)
#define SALTS_DISRUPTOR_ASM_STORE_ORDERED(p_) \
    _Generic((p_), \
      salts_asm_u64_slot *: salts_disruptor_u64_store_ordered, \
      salts_asm_u32_slot *: salts_disruptor_u32_store_ordered)
#define atomic_load_explicit(p_, order_) \
    SALTS_DISRUPTOR_ASM_LOAD_ORDERED(p_)((p_), (order_))
#define atomic_store_explicit(p_, value_, order_) \
    SALTS_DISRUPTOR_ASM_STORE_ORDERED(p_)((p_), (value_), (order_))
#else
/* Conservative reference variant: all loads and stores use seq_cst. */
#define atomic_load_explicit(p_, order_) \
    ((void)(order_), SALTS_DISRUPTOR_ASM_LOAD(p_)((p_)))
#define atomic_store_explicit(p_, value_, order_) \
    ((void)(order_), SALTS_DISRUPTOR_ASM_STORE(p_)((p_), (value_)))
#endif

/* The RMW/CAS operations remain SEQ_CST for BOTH backends, and
 * evaluate the existing order parameters exactly once for diagnostics.
 * Thus no original order is weakened by this experiment.
 */

#define atomic_fetch_add_explicit(p_, value_, order_) \
  ((void)(order_), SALTS_DISRUPTOR_ASM_ADD(p_)((p_), (value_)))
#define atomic_fetch_sub_explicit(p_, value_, order_) \
  ((void)(order_), SALTS_DISRUPTOR_ASM_SUB(p_)((p_), (value_)))
#define atomic_compare_exchange_strong_explicit(p_, expected_, desired_, success_, failure_) \
  ((void)(success_), (void)(failure_), SALTS_DISRUPTOR_ASM_CAS(p_)((p_), (expected_), (desired_)))
/* An always-strong CAS is an admissible implementation of weak CAS.
 * The producer's existing retry loops still handle real contention.
 */
#define atomic_compare_exchange_weak_explicit(p_, expected_, desired_, success_, failure_) \
  atomic_compare_exchange_strong_explicit((p_), (expected_), (desired_), (success_), (failure_))

#endif /* SALTS_DISRUPTOR_PRIVATE_ASM_ATOMIC_ADAPTER_H */
