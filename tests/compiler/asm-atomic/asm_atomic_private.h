#ifndef SALTS_COMPILER_ASM_ATOMIC_PRIVATE_H
#define SALTS_COMPILER_ASM_ATOMIC_PRIVATE_H
/*
 * EXPERIMENTAL, TEST-ONLY: independently owned Linux x86_64/AArch64
 * GAS assembly atomic storage. Inspired by MIR's arch-specific codegen
 * boundaries and minicoro's separate assembly entrypoints (NOT copied).
 *
 * NOT a <stdatomic.h> replacement. The operations are deliberately all
 * sequentially consistent (stronger than acquire/release/relaxed).
 * Never cast/memcpy an existing _Atomic object or expose these structs
 * through Salts' production or installed ABI.
 *
 * The caller owns *expected and must not alias it to the live atomic slot.
 * A shared slot must be accessed ONLY through the functions declared here
 * for its entire concurrent lifetime; do not mix backend implementations.
 */
#include <stddef.h>
#include <stdint.h>

#if !defined(__linux__) || (!defined(__x86_64__) && !defined(__aarch64__))
#error "Private assembly atomic diagnostics support only Linux x86-64/AArch64"
#endif

typedef struct salts_asm_u64_slot {
  _Alignas(8) uint64_t bits;
} salts_asm_u64_slot;

typedef struct salts_asm_u32_slot {
  _Alignas(4) uint32_t bits;
} salts_asm_u32_slot;

_Static_assert(sizeof(salts_asm_u64_slot) == 8 &&
                   _Alignof(salts_asm_u64_slot) == 8 &&
                   offsetof(salts_asm_u64_slot, bits) == 0,
               "u64 assembly ABI requires aligned storage at offset zero");
_Static_assert(sizeof(salts_asm_u32_slot) == 4 &&
                   _Alignof(salts_asm_u32_slot) == 4 &&
                   offsetof(salts_asm_u32_slot, bits) == 0,
               "u32 assembly ABI requires aligned storage at offset zero");

#ifdef __cplusplus
extern "C" {
#endif

uint64_t salts_asm_u64_load(const salts_asm_u64_slot *slot);
void salts_asm_u64_store(salts_asm_u64_slot *slot, uint64_t value);
uint64_t salts_asm_u64_exchange(salts_asm_u64_slot *slot, uint64_t value);
uint64_t salts_asm_u64_fetch_add(salts_asm_u64_slot *slot, uint64_t delta);
uint64_t salts_asm_u64_fetch_sub(salts_asm_u64_slot *slot, uint64_t delta);
/* Return 1 on success, 0 on failure; update *expected with observed value
 * on failure and preserve its original value on success. */
int salts_asm_u64_cas(salts_asm_u64_slot *slot,
                      uint64_t *expected, uint64_t desired);

uint32_t salts_asm_u32_load(const salts_asm_u32_slot *slot);
void salts_asm_u32_store(salts_asm_u32_slot *slot, uint32_t value);
uint32_t salts_asm_u32_exchange(salts_asm_u32_slot *slot, uint32_t value);
uint32_t salts_asm_u32_fetch_add(salts_asm_u32_slot *slot, uint32_t delta);
uint32_t salts_asm_u32_fetch_sub(salts_asm_u32_slot *slot, uint32_t delta);
int salts_asm_u32_cas(salts_asm_u32_slot *slot,
                      uint32_t *expected, uint32_t desired);

void salts_asm_fence(void);

/* Private load/store-only ordering specialization.
 * Relaxed: atomic, without a cross-core ordering edge.
 * Acquire: read orders later payload accesses.
 * Release: prior payload accesses happen before the store.
 *
 * CAS / fetch-add / fetch-sub are STILL the stronger seq_cst routines.
 * These separate ABI entrypoints are NOT installed or allowed on native
 * _Atomic storage; caller must own the slot entirely under one backend.
 */
uint64_t salts_asm_u64_load_relaxed(const salts_asm_u64_slot *slot);
uint64_t salts_asm_u64_load_acquire(const salts_asm_u64_slot *slot);
void salts_asm_u64_store_relaxed(salts_asm_u64_slot *slot, uint64_t value);
void salts_asm_u64_store_release(salts_asm_u64_slot *slot, uint64_t value);
uint32_t salts_asm_u32_load_relaxed(const salts_asm_u32_slot *slot);
uint32_t salts_asm_u32_load_acquire(const salts_asm_u32_slot *slot);
void salts_asm_u32_store_relaxed(salts_asm_u32_slot *slot, uint32_t value);
void salts_asm_u32_store_release(salts_asm_u32_slot *slot, uint32_t value);

#ifdef __cplusplus
}
#endif
#endif /* SALTS_COMPILER_ASM_ATOMIC_PRIVATE_H */
