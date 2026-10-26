# Standalone TinyCC Disruptor ASM Atomic backend

This directory builds a real static archive, `libsalts_tcc_disruptor.a`,
from the original `concurrency/src/disruptor.c`, original
`platform/src/thread_posix.c`, and the canonical architecture-specific
`concurrency/src/asm-atomic/asm_atomic_{x86_64,aarch64}.S`.

**This is NOT an installed Salts SDK or full Salts::Concurrency.**
Disruptor's public API is opaque and can retain separately owned internal
atomic storage. The public SPSC struct exposes `_Atomic size_t`, and
cannot be silently replaced by this 32/64-bit ASM representation.

Production Disruptor source now accepts explicit
`SALTS_DISRUPTOR_BACKEND_ASM=1` for its own implementation only.
No compiler feature macro is overridden. The private adapter
does not claim to provide C11 `stdatomic.h` conformance, and neither
the public Atomic API nor unrelated Concurrency consumers change.

## Build natively with pinned TinyCC

```sh
cmake -S concurrency/tcc -B build/tcc-disruptor -G Ninja \
  -DCMAKE_C_COMPILER=tcc -DCMAKE_ASM_COMPILER=gcc
cmake --build build/tcc-disruptor --parallel 2
ctest --test-dir build/tcc-disruptor --repeat until-fail:5 --output-on-failure
```

TinyCC compiles every C translation unit; GCC/GAS assembles **only** the
standalone `.S` target object. Three consumer programs link against the
real archive: bounded Broadcast/WorkerPool, reverse-order retirement,
4 publishers + 4 workers with unique message verification, and actual
park/wake/stop. Only Linux x86-64 and Linux AArch64 are admitted for this
bounded build target. Unsupported targets must fail configuration.

`SALTS_THREAD_LOCAL=_Thread_local` is deliberately selected only
inside this target for the actual Platform POSIX source; this is
a tested syntax opt-in, not a declaration that TinyCC fully supports
C11 Threads. The artifact is **not installed** or exported as
`Salts::Concurrency` or a complete SDK.

## Remaining gates

Foreign ASM is not fully visible to source TSan; mixed-object default
GNU unwind metadata, cross-DSO ownership, Apple/MSVC/Windows platforms,
SPSC's public ABI, formal ARMv8 weak-memory behavior, all Salts SDK and
CFlow consumers remain unqualified. The separate strict
`__STDC_NO_ATOMICS__=1` compiler gate remains red.
