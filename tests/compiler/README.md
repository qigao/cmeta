# C11 atomic compiler conformance (experimental TCC qualification)

This is a standalone, dependency-free compiler qualification suite for Salts.
It does not implement atomics, replace the Salts::Concurrency contract,
install a runtime, or enable an unverified compiler backend.

The operation matrix was adapted from TurboWasm's
[guest atomic tests](https://github.com/qigao/turbowasm/blob/master/tests/guest/c11_atomics.c).
WebAssembly-only wait/notify operations have been replaced with native
POSIX/Windows threads. The suite uses the actual Salts headers for the typed
atomic API, but does not link any Salts shared library or require vcpkg.

## Run standalone

    cmake -S tests/compiler -B build/atomic-gcc -G Ninja -DCMAKE_C_COMPILER=gcc
    cmake --build build/atomic-gcc
    ctest --test-dir build/atomic-gcc --output-on-failure

    cmake -S tests/compiler -B build/atomic-clang -G Ninja -DCMAKE_C_COMPILER=clang
    cmake --build build/atomic-clang
    ctest --test-dir build/atomic-clang --output-on-failure

    # Experimental. Pin a TinyCC revision with native C11 atomic support.
    cmake -S tests/compiler -B build/atomic-tcc -G Ninja -DCMAKE_C_COMPILER=tcc
    cmake --build build/atomic-tcc
    ctest --test-dir build/atomic-tcc --output-on-failure

Use distinct build directories for each compiler. A C11-capable standard
library and native OS threading support are required.

The dedicated workflow runs GCC/Clang baselines and a diagnostic TinyCC build
pinned to TinyCC/tinycc commit e23dabed154a8495f3ac1ff3cc9d04fa1c207893.
The TinyCC job is non-blocking because TinyCC is not admitted as a production
Salts SDK compiler. A failed TinyCC experiment is **NOT QUALIFIED**, even when
the aggregate GitHub Actions workflow result is green.

## What is checked

- Native C11 atomic 8/16/32/64-bit scalar operations, byte-pointer arithmetic,
  typed-pointer load/store/CAS, exchange, RMW operations, weak/strong CAS,
  failure write-back, atomic flag, fences and single evaluation of macro args.
- Real worker-thread CAS contention, 64-bit arithmetic, atomic-flag protected
  increments and release/acquire publication observed before joining.
- Salts' typed atomic status API, invalid memory order rejection, unchanged
  storage and outputs after validation failures, and pointer CAS.
- Whether lock-free operation is offered is observed but **not required**.
  Compiler correctness is required even with lock-based native atomics.

C11 typed-pointer atomic_fetch_add scaling is intentionally not a portability
pass/fail assertion: GCC and Clang have produced different byte-vs-element
increments. Only unambiguous byte-pointer arithmetic and typed-pointer CAS are
asserted.

## Cross-translation-unit atomic ABI diagnostics

`atomic_abi.h`, `atomic_abi_driver.c` and `atomic_abi_peer.c` establish a
**diagnostic-only**, fixed shared layout for 32-bit, 64-bit and pointer atomic
fields. Before touching the shared object, both translation units independently
report struct size, alignment, field offsets, atomic sizes and alignments. The
driver rejects mismatches before atomic access.

When compatible, the probe checks cross-compiler pointer CAS, competing 32-bit
CAS increments and 64-bit fetch-add, plus release/acquire publication **before**
`pthread_join`. The pthread test is Linux/POSIX only; a bounded timeout
prevents an unknown compiler bug from hanging CI.

The standalone CMake suite runs the **same-compiler** GCC/Clang baseline. The
experimental TCC workflow runs this separately in three directions:

- GCC driver + TCC peer;
- TCC driver + GCC peer;
- TCC driver + TCC peer.

The local diagnostic entrypoint is `bash tests/compiler/run_tcc_diagnostics.sh`
(with a pinned TinyCC binary and `gcc` on PATH). Its results are evidence of
a limited implementation, not permission to build Salts with TCC. In particular,
upstream TinyCC `include/tccdefs.h` explicitly defines
`__STDC_NO_ATOMICS__=1`, which Salts must continue to reject.

## Limits

Passing is **necessary but not sufficient** for production qualification:
arbitrary aggregate atomics, wider-than-64-bit atomics, TSan, real ABI
interoperability between separately compiled GCC/Clang/TCC translation units,
weak-memory litmus tests on ARM64, and complete Salts Concurrency/CFlow/Plugin
integration remain independent admission gates.

Do not copy TurboWasm's interpreter src/atomic.c into Salts. That file maps
WebAssembly opcodes, and its runtime-wide serialized guest-memory atomics
depend on Salts::Platform. Using it as Salts' native atomic backend would
both reverse dependency direction and change the public atomic contract.
