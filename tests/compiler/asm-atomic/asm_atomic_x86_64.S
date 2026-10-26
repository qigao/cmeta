/* Linux System V AMD64 GAS. Private diagnostic only.
 * All memory operations are SEQ_CST (lock RMW / xchg store; x86 TSO load).
 * External functions preserve SysV nonvolatile registers, never JIT pages.
 */
#if !defined(__linux__) || !defined(__x86_64__)
#error "This source requires Linux x86-64"
#endif

.text
.globl salts_asm_u64_load
.type salts_asm_u64_load, @function
salts_asm_u64_load:
    movq (%rdi), %rax
    ret
.size salts_asm_u64_load, .-salts_asm_u64_load

.globl salts_asm_u64_store
.type salts_asm_u64_store, @function
salts_asm_u64_store:
    xchgq %rsi, (%rdi)
    ret
.size salts_asm_u64_store, .-salts_asm_u64_store

.globl salts_asm_u64_exchange
.type salts_asm_u64_exchange, @function
salts_asm_u64_exchange:
    movq %rsi, %rax
    xchgq %rax, (%rdi)
    ret
.size salts_asm_u64_exchange, .-salts_asm_u64_exchange

.globl salts_asm_u64_fetch_add
.type salts_asm_u64_fetch_add, @function
salts_asm_u64_fetch_add:
    movq %rsi, %rax
    lock xaddq %rax, (%rdi)
    ret
.size salts_asm_u64_fetch_add, .-salts_asm_u64_fetch_add

.globl salts_asm_u64_fetch_sub
.type salts_asm_u64_fetch_sub, @function
salts_asm_u64_fetch_sub:
    negq %rsi
    movq %rsi, %rax
    lock xaddq %rax, (%rdi)
    ret
.size salts_asm_u64_fetch_sub, .-salts_asm_u64_fetch_sub

.globl salts_asm_u64_cas
.type salts_asm_u64_cas, @function
salts_asm_u64_cas:
    movq (%rsi), %rax
    lock cmpxchgq %rdx, (%rdi)
    movq %rax, (%rsi)
    sete %al
    movzbl %al, %eax
    ret
.size salts_asm_u64_cas, .-salts_asm_u64_cas

.globl salts_asm_u32_load
.type salts_asm_u32_load, @function
salts_asm_u32_load:
    movl (%rdi), %eax
    ret
.size salts_asm_u32_load, .-salts_asm_u32_load

.globl salts_asm_u32_store
.type salts_asm_u32_store, @function
salts_asm_u32_store:
    xchgl %esi, (%rdi)
    ret
.size salts_asm_u32_store, .-salts_asm_u32_store

.globl salts_asm_u32_exchange
.type salts_asm_u32_exchange, @function
salts_asm_u32_exchange:
    movl %esi, %eax
    xchgl %eax, (%rdi)
    ret
.size salts_asm_u32_exchange, .-salts_asm_u32_exchange

.globl salts_asm_u32_fetch_add
.type salts_asm_u32_fetch_add, @function
salts_asm_u32_fetch_add:
    movl %esi, %eax
    lock xaddl %eax, (%rdi)
    ret
.size salts_asm_u32_fetch_add, .-salts_asm_u32_fetch_add

.globl salts_asm_u32_fetch_sub
.type salts_asm_u32_fetch_sub, @function
salts_asm_u32_fetch_sub:
    negl %esi
    movl %esi, %eax
    lock xaddl %eax, (%rdi)
    ret
.size salts_asm_u32_fetch_sub, .-salts_asm_u32_fetch_sub

.globl salts_asm_u32_cas
.type salts_asm_u32_cas, @function
salts_asm_u32_cas:
    movl (%rsi), %eax
    lock cmpxchgl %edx, (%rdi)
    movl %eax, (%rsi)
    sete %al
    movzbl %al, %eax
    ret
.size salts_asm_u32_cas, .-salts_asm_u32_cas

.globl salts_asm_fence
.type salts_asm_fence, @function
salts_asm_fence:
    mfence
    ret
.size salts_asm_fence, .-salts_asm_fence

/* GAS-produced objects must not silently request an executable stack. */
.section .note.GNU-stack,"",@progbits
