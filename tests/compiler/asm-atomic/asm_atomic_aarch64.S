/* Linux AArch64 AAPCS64 GAS; baseline ARMv8 LL/SC, no LSE requirement.
 * All memory operations deliberately impose SEQ_CST or stronger barriers:
 * dmb ish around ldar/stlr/ldaxr/stlxr. Test-only, no W^X/JIT pages.
 */
#if !defined(__linux__) || !defined(__aarch64__)
#error "This source requires Linux AArch64"
#endif

.text
.p2align 2

.globl salts_asm_u64_load
.type salts_asm_u64_load, %function
salts_asm_u64_load:
    dmb ish
    ldar x0, [x0]
    dmb ish
    ret
.size salts_asm_u64_load, .-salts_asm_u64_load

.globl salts_asm_u64_store
.type salts_asm_u64_store, %function
salts_asm_u64_store:
    dmb ish
    stlr x1, [x0]
    dmb ish
    ret
.size salts_asm_u64_store, .-salts_asm_u64_store

.globl salts_asm_u64_exchange
.type salts_asm_u64_exchange, %function
salts_asm_u64_exchange:
    dmb ish
1:
    ldaxr x2, [x0]
    stlxr w3, x1, [x0]
    cbnz w3, 1b
    dmb ish
    mov x0, x2
    ret
.size salts_asm_u64_exchange, .-salts_asm_u64_exchange

.globl salts_asm_u64_fetch_add
.type salts_asm_u64_fetch_add, %function
salts_asm_u64_fetch_add:
    dmb ish
1:
    ldaxr x2, [x0]
    add x3, x2, x1
    stlxr w4, x3, [x0]
    cbnz w4, 1b
    dmb ish
    mov x0, x2
    ret
.size salts_asm_u64_fetch_add, .-salts_asm_u64_fetch_add

.globl salts_asm_u64_fetch_sub
.type salts_asm_u64_fetch_sub, %function
salts_asm_u64_fetch_sub:
    dmb ish
1:
    ldaxr x2, [x0]
    sub x3, x2, x1
    stlxr w4, x3, [x0]
    cbnz w4, 1b
    dmb ish
    mov x0, x2
    ret
.size salts_asm_u64_fetch_sub, .-salts_asm_u64_fetch_sub

.globl salts_asm_u64_cas
.type salts_asm_u64_cas, %function
salts_asm_u64_cas:
    dmb ish
    ldr x3, [x1]
1:
    ldaxr x4, [x0]
    cmp x4, x3
    b.ne 2f
    stlxr w5, x2, [x0]
    cbnz w5, 1b
    mov w0, #1
    b 3f
2:
    clrex
    str x4, [x1]
    mov w0, #0
3:
    dmb ish
    ret
.size salts_asm_u64_cas, .-salts_asm_u64_cas

.globl salts_asm_u32_load
.type salts_asm_u32_load, %function
salts_asm_u32_load:
    dmb ish
    ldar w0, [x0]
    dmb ish
    ret
.size salts_asm_u32_load, .-salts_asm_u32_load

.globl salts_asm_u32_store
.type salts_asm_u32_store, %function
salts_asm_u32_store:
    dmb ish
    stlr w1, [x0]
    dmb ish
    ret
.size salts_asm_u32_store, .-salts_asm_u32_store

.globl salts_asm_u32_exchange
.type salts_asm_u32_exchange, %function
salts_asm_u32_exchange:
    dmb ish
1:
    ldaxr w2, [x0]
    stlxr w3, w1, [x0]
    cbnz w3, 1b
    dmb ish
    mov w0, w2
    ret
.size salts_asm_u32_exchange, .-salts_asm_u32_exchange

.globl salts_asm_u32_fetch_add
.type salts_asm_u32_fetch_add, %function
salts_asm_u32_fetch_add:
    dmb ish
1:
    ldaxr w2, [x0]
    add w3, w2, w1
    stlxr w4, w3, [x0]
    cbnz w4, 1b
    dmb ish
    mov w0, w2
    ret
.size salts_asm_u32_fetch_add, .-salts_asm_u32_fetch_add

.globl salts_asm_u32_fetch_sub
.type salts_asm_u32_fetch_sub, %function
salts_asm_u32_fetch_sub:
    dmb ish
1:
    ldaxr w2, [x0]
    sub w3, w2, w1
    stlxr w4, w3, [x0]
    cbnz w4, 1b
    dmb ish
    mov w0, w2
    ret
.size salts_asm_u32_fetch_sub, .-salts_asm_u32_fetch_sub

.globl salts_asm_u32_cas
.type salts_asm_u32_cas, %function
salts_asm_u32_cas:
    dmb ish
    ldr w3, [x1]
1:
    ldaxr w4, [x0]
    cmp w4, w3
    b.ne 2f
    stlxr w5, w2, [x0]
    cbnz w5, 1b
    mov w0, #1
    b 3f
2:
    clrex
    str w4, [x1]
    mov w0, #0
3:
    dmb ish
    ret
.size salts_asm_u32_cas, .-salts_asm_u32_cas

.globl salts_asm_fence
.type salts_asm_fence, %function
salts_asm_fence:
    dmb ish
    ret
.size salts_asm_fence, .-salts_asm_fence

/* ARMv8 baseline ordering-specific scalar operations.
 * AArch64 aligned ldr/str are indivisible at 32 and 64 bits and do not
 * order unrelated payload operations. LDAR acquires, STLR releases.
 * CAS and RMW continue to call the separate stronger SEQ_CST routines.
 */
.globl salts_asm_u64_load_relaxed
.type salts_asm_u64_load_relaxed, %function
salts_asm_u64_load_relaxed:
    ldr x0, [x0]
    ret
.size salts_asm_u64_load_relaxed, .-salts_asm_u64_load_relaxed

.globl salts_asm_u64_load_acquire
.type salts_asm_u64_load_acquire, %function
salts_asm_u64_load_acquire:
    ldar x0, [x0]
    ret
.size salts_asm_u64_load_acquire, .-salts_asm_u64_load_acquire

.globl salts_asm_u64_store_relaxed
.type salts_asm_u64_store_relaxed, %function
salts_asm_u64_store_relaxed:
    str x1, [x0]
    ret
.size salts_asm_u64_store_relaxed, .-salts_asm_u64_store_relaxed

.globl salts_asm_u64_store_release
.type salts_asm_u64_store_release, %function
salts_asm_u64_store_release:
    stlr x1, [x0]
    ret
.size salts_asm_u64_store_release, .-salts_asm_u64_store_release

.globl salts_asm_u32_load_relaxed
.type salts_asm_u32_load_relaxed, %function
salts_asm_u32_load_relaxed:
    ldr w0, [x0]
    ret
.size salts_asm_u32_load_relaxed, .-salts_asm_u32_load_relaxed

.globl salts_asm_u32_load_acquire
.type salts_asm_u32_load_acquire, %function
salts_asm_u32_load_acquire:
    ldar w0, [x0]
    ret
.size salts_asm_u32_load_acquire, .-salts_asm_u32_load_acquire

.globl salts_asm_u32_store_relaxed
.type salts_asm_u32_store_relaxed, %function
salts_asm_u32_store_relaxed:
    str w1, [x0]
    ret
.size salts_asm_u32_store_relaxed, .-salts_asm_u32_store_relaxed

.globl salts_asm_u32_store_release
.type salts_asm_u32_store_release, %function
salts_asm_u32_store_release:
    stlr w1, [x0]
    ret
.size salts_asm_u32_store_release, .-salts_asm_u32_store_release

.section .note.GNU-stack,"",%progbits
