# TinyCC Disruptor with explicit ASM Atomic backend

The standalone `concurrency/tcc` CMake project builds a real
`libsalts_tcc_disruptor.a` static archive from the original
`concurrency/src/disruptor.c`, `platform/src/thread_posix.c`,
and the x86-64 or AArch64 GAS Atomic assembly source.

**Why this is now usable:** the actual Disruptor source has a
compile-time `SALTS_DISRUPTOR_BACKEND_ASM=1` selector. The private
C ABI implements 32/64-bit Load/Store, RMW and CAS using external
GAS objects with conservative seq_cst semantics. The wrapper changes
*only* its opaque internal atomic fields, never public Disruptor API
types or any native C11 compiler feature declaration.

This dedicated target always selects ASM on TinyCC; it does not
silently fall back to compiler-provided Atomics, and CMake
fails for other compilers, non-Linux OSes, unadmitted CPU targets
and unsupported assembler drivers.

## Build and verify (native Linux x86-64 / AArch64)

Pin TinyCC to
`TinyCC/tinycc@e23dabed154a8495f3ac1ff3cc9d04fa1c207893`.

```sh
cmake -S concurrency/tcc -B build/tcc-disruptor -G Ninja \
  -DCMAKE_C_COMPILER=tcc -DCMAKE_ASM_COMPILER=gcc
cmake --build build/tcc-disruptor --parallel 2
ctest --test-dir build/tcc-disruptor --repeat until-fail:5 --output-on-failure
```

The generated archive lives in the build directory. It is **not
installed or exported as Salts::Concurrency**. Link a compatible
Linux TinyCC consumer against the archive with the public
`<salts/disruptor.h>` header and pthread support.

Three real public API client suites run against this archive:
two canonical Disruptor retirement tests already in
`concurrency/tests/retirement/` (Broadcast, reverse retirement,
capacity reuse and 4×4 MPMC), plus an independent parked-worker
notify/stop test. TinyCC builds the actual C translation units;
GCC/GAS only assembles the standalone `.S` target.

The bundled POSIX Platform source uses
`SALTS_THREAD_LOCAL=_Thread_local` in this limited target because
TinyCC currently advertises `__STDC_NO_THREADS__=1`. This is an
isolated, tested syntax selection, *not* full C11 Threads admission.

## Compatibility and safety

The public SPSC ring exposes `_Atomic size_t` and is **not** migrated.
The `salts/atomic.h` production C11 gate remains unchanged, as do
all native GCC/Clang compiler paths. An external GAS object cannot
be assumed to be fully visible to TSan. No complete Salts SDK, CFlow,
Windows/iOS/macOS runtime, or cross-DSO Atomic ABI support is claimed.
Full TinyCC C11 compiler qualification remains in
[Draft #1316](https://github.com/qigao/salts/pull/1316).

The independent CI pins a specific TinyCC commit and executes the
three real linked-client tests **five times on both native x86-64
and AArch64**, as a fail-closed required component gate.
