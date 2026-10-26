#ifndef SALTS_COMPILER_DISRUPTOR_ASM_ADAPTER_H
#define SALTS_COMPILER_DISRUPTOR_ASM_ADAPTER_H
/*
 * PRIVATE build adapter: effective ONLY when included immediately before
 * original concurrency/src/disruptor.c in the isolated experimental TU.
 * Do not -include this header in an SDK, another public header, or callers.
 *
 * We intentionally map the two internal _Atomic typedefs to separately owned
 * assembly slots. The public opaque disruptor_t ABI remains unchanged.
 * All assembly operations use stronger SEQ_CST ordering than the original
 * relaxed/acquire/release/acq_rel operation sites. This is not a C11 shim.
 *
 * The included source may still include <stdatomic.h>. We load that first
 * for memory_order enumeration constants, then rebind only the operations
 * and types used by the private implementation. The compiler's feature
 * macros remain UNTOUCHED and must still fail strict Salts admission.
 */
#ifndef SALTS_TEST_ONLY_ASM_DISRUPTOR
#error "ASM Disruptor adapter must never be used outside an isolated test TU"
#endif

#include <stdatomic.h>
#include <stdint.h>
#include <stddef.h>
#include "asm_atomic_private.h"

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
#define SALTS_TEST_ASM_LOAD(p_) \
  _Generic((p_), \
    salts_asm_u64_slot *: salts_asm_u64_load, \
    const salts_asm_u64_slot *: salts_asm_u64_load, \
    salts_asm_u32_slot *: salts_asm_u32_load, \
    const salts_asm_u32_slot *: salts_asm_u32_load)
#define SALTS_TEST_ASM_STORE(p_) \
  _Generic((p_), \
    salts_asm_u64_slot *: salts_asm_u64_store, \
    salts_asm_u32_slot *: salts_asm_u32_store)
#define SALTS_TEST_ASM_ADD(p_) \
  _Generic((p_), \
    salts_asm_u64_slot *: salts_asm_u64_fetch_add, \
    salts_asm_u32_slot *: salts_asm_u32_fetch_add)
#define SALTS_TEST_ASM_SUB(p_) \
  _Generic((p_), \
    salts_asm_u64_slot *: salts_asm_u64_fetch_sub, \
    salts_asm_u32_slot *: salts_asm_u32_fetch_sub)
#define SALTS_TEST_ASM_CAS(p_) \
  _Generic((p_), \
    salts_asm_u64_slot *: salts_asm_u64_cas, \
    salts_asm_u32_slot *: salts_asm_u32_cas)

/* Evaluating the original order expression preserves caller diagnostics,
 * but backend deliberately selects stronger SEQ_CST for all accesses.
 */
#define atomic_load_explicit(p_, order_) \
  ((void)(order_), SALTS_TEST_ASM_LOAD(p_)((p_)))
#define atomic_store_explicit(p_, value_, order_) \
  ((void)(order_), SALTS_TEST_ASM_STORE(p_)((p_), (value_)))
#define atomic_fetch_add_explicit(p_, value_, order_) \
  ((void)(order_), SALTS_TEST_ASM_ADD(p_)((p_), (value_)))
#define atomic_fetch_sub_explicit(p_, value_, order_) \
  ((void)(order_), SALTS_TEST_ASM_SUB(p_)((p_), (value_)))
#define atomic_compare_exchange_strong_explicit(p_, expected_, desired_, success_, failure_) \
  ((void)(success_), (void)(failure_), SALTS_TEST_ASM_CAS(p_)((p_), (expected_), (desired_)))
/* An always-strong CAS is an admissible implementation of weak CAS.
 * The producer's existing retry loops still handle real contention.
 */
#define atomic_compare_exchange_weak_explicit(p_, expected_, desired_, success_, failure_) \
  atomic_compare_exchange_strong_explicit((p_), (expected_), (desired_), (success_), (failure_))

#endif /* SALTS_COMPILER_DISRUPTOR_ASM_ADAPTER_H */
