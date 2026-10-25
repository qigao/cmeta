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

TinyCC 0.9.28rc may produce object-file `.eh_frame` information rejected by
GNU `ld` when it constructs `.eh_frame_hdr` (overlapping FDEs). The script
always records the default linker outcome as its own result. It separately
attempts a **diagnostic-only** link using `--no-eh-frame-hdr` with a
non-executable test stack (`-z noexecstack`); these options do **not**
validate TCC's unwind metadata or permit mixing TCC objects in the production
SDK.

The local diagnostic entrypoint is `bash tests/compiler/run_tcc_diagnostics.sh`
(with a pinned TinyCC binary and `gcc` on PATH). Its results are evidence of
a limited implementation, not permission to build Salts with TCC. In particular,
upstream TinyCC `include/tccdefs.h` explicitly defines
`__STDC_NO_ATOMICS__=1`, which Salts must continue to reject.

## Salts::Platform threading smoke (Linux only)

The isolated `tcc_platform_thread_probe.c` links the actual
`platform/src/thread_posix.c` implementation, not a copy or a third-party
replacement. It tests a timed condition wait, thread create/join, thread
identity token, POSIX once, mutex lifecycle and RW locks. GCC and Clang
execute it as a standalone CTest baseline.

TinyCC's own headers advertise `__STDC_NO_THREADS__=1` although the
compiler source documents and recognizes `_Thread_local`. The new
`tcc_tls_probe.c` independently tests real per-pthread TLS isolation without
changing that feature macro. The diagnostic first attempts the unchanged
Salts::Platform source and header; when rejected, it may privately compile
a second test with `-DSALTS_THREAD_LOCAL=_Thread_local` **only after** the
independent native TLS probe passes. That second compile is not an SDK build,
a production fallback, or compiler admission.

The TinyCC diagnostic compiles and links the same production Platform source
directly with TCC's own linker, without the full Salts/vcpkg dependency graph.
The result is separately reported from atomics. TinyCC also explicitly defines
`__STDC_NO_THREADS__=1`; the test does not override this or otherwise
reinterpret thread feature admission.

## Minimal Salts source object compilation

The experimental TinyCC script also compiles isolated, real Salts translation
units: `cmeta/src/abi.c`, `cmeta/src/type_identity.c`, `cstl/src/vec.c`,
and `asn1/src/asn1.c`. It runs each independently to collect all compiler
failures instead of stopping after the first. These are **compile-only** checks
with the actual public includes; they do not qualify inter-module linking,
the CMeta ACE runtime, a CNet stack or an installed SDK. Any failing
translation unit makes the experimental diagnostic step fail without changing
production compiler admission.

## Complete CMeta/CSTL object and linked-consumer qualification

`run_cmeta_cstl_qualification.sh` extracts the **canonical** source sets
from the actual `cmeta/CMakeLists.txt` and `cstl/CMakeLists.txt` library
declarations, fails if source counts change unexpectedly, and builds all
19 CMeta and all 18 CSTL translation units. It records every compile
failure, constructs separate complete static archives only if all 37
objects compile, then links and runs an independent
`cmeta_cstl_consumer.c` exercising CMeta identity, ABI and CSTL vector
ownership/capacity rules.

This is deliberately a standalone, static, **Linux** compiler qualification,
not a substitute for Salts' canonical CMake build, DSO exports, PIC/visibility,
CMeta ACE Plugin/Component linker sections, installed headers or SDK consumers.
GCC and Clang are required baseline jobs; TCC is a separate, non-blocking
experiment pinned to an exact compiler commit. A green main workflow does not
make a red TCC experimental job qualified.

Example:

```sh
CC=gcc BUILD_ROOT=build/compiler-gcc bash tests/compiler/run_cmeta_cstl_qualification.sh
CC=clang BUILD_ROOT=build/compiler-clang bash tests/compiler/run_cmeta_cstl_qualification.sh
CC=tcc BUILD_ROOT=build/compiler-tcc bash tests/compiler/run_cmeta_cstl_qualification.sh
```

Do not override `__STDC_NO_ATOMICS__`, change production C11/TLS
admission logic, or import TurboWasm's Wasm atomic runtime as a workaround.

## CMeta language and Plugin manifest macros

`run_cmeta_macro_probes.sh` directly tests the public `cmeta_struct`,
`cmeta_field`, `cmeta_enum`, `cmeta_require_field` declarations
and their reflected metadata. It separately compiles, links and executes an
**explicit empty Plugin manifest** through `CMETA_PLUGIN_DECLARE_EMPTY`.
The Plugin probe reports `CMETA_SECTION_BACKEND` and
`CMETA_PLUGIN_HAS_LINKER_EXPORTS`. That report does **not** qualify
linker-section Plugin fragment discovery, DSO retention, Plugin Registry,
ServiceHost, quiescence or unloading.

These standalone executables deliberately avoid switching production
compiler capabilities; GCC/Clang run as required baseline, and pinned TCC
runs as an experimental-only diagnostic.

## Explicit Plugin shared-object bootstrap (Linux ELF only)

The isolated `run_plugin_dso_smoke.sh` builds a shared provider using
`CMETA_PLUGIN_DECLARE_EMPTY`. A separately compiled host opens it using
`dlopen(RTLD_NOW | RTLD_LOCAL)`, resolves `cmeta_plugin_query`, admits only
the exact manifest size and ABI, checks the three version fields and passive
export contract, rejects the wrong ABI epoch, and closes the module.

GCC↔Clang is a required native baseline; GCC↔TCC is a non-blocking,
diagnostic-only comparison. These are **not** the Salts::Plugin Registry or
ServiceHost: they do not exercise leases, active exports, callback ownership,
linker-section fragment discovery, cancellation/drain or concurrent DSO unload.
A pass cannot authorize use of TCC-built provider DSOs in production.

The CMeta public macro script requires a completed CMeta static archive:
`CMETA_LIBRARY=build/whole-gcc/libcmeta_compiler.a CC=gcc
bash tests/compiler/run_cmeta_macro_probes.sh`.
The reflected struct field declarations reference canonical CMeta built-in
descriptors, so omitting the archive is a test harness error, not compiler
incompatibility.

## Reference review: zelang-dev/c-threads (pinned main `d7cbefa0099044d6d834939290eb62e02ed8bb1b`)

The external [c-threads](https://github.com/zelang-dev/c-threads)
library is **reference material**, not a dependency, ABI fallback, or vendor
import. It wraps pthreads/Pthreads4w and provides an accessor-based
`pthread_key_t` TSS emulation and a separate `catomics.h` compatibility layer.

The emulated `thrd_local(type, var, initial)` is **not interchangeable** with
Salts' `static SALTS_THREAD_LOCAL type var` storage-class declaration. Its
accessors return allocated objects and require key/owner lifecycle management.
Salts' current thread-affine objects cannot silently change their address,
storage, error, or destruction semantics. Moreover, the reviewed
`include/cthreads.h` lazily checks/writes a shared `thrd_##var##_tls`
value around `tss_create` without `pthread_once` or an equivalent
initialization lock; simultaneous first-use requires its own qualification.
The emulation allocates the per-thread object but does not initialize it with
the macro's requested `initial` value on first allocation. The
`src/cthreads.c` `thrd_sleep` wrapper also ignores interrupted sleep and
the remaining duration, which differs from Salts' contract expectations.

`include/catomics.h` mixes native builtins/inline assembly with older
compatibility macros. Its lock-free macro uses `sizeof(obj)` rather than
`sizeof(*obj)` in the relevant fallback and must not be treated as a
complete substitute for Salts' `_Atomic` object and memory-order contract.
TinyCC still advertises `__STDC_NO_ATOMICS__=1`. Do not include that header
instead of `<stdatomic.h>`, redefine compiler feature macros, or import a
global allocator to turn a failing Salts admission test green.

Borrowed **design principle only**: make key-backed TSS initialization exactly
once and test first use under real concurrent pthreads. The independent
`posix_tss_semantics_probe.c` exercises 16 racing first-use threads,
repeated per-thread access, address isolation **while all threads are live**,
one key creation, exactly one destructor per exiting worker, and explicit
main-thread teardown before key deletion. It uses only POSIX pthreads (no
`cthreads.h`, rpmalloc, Salts runtime, or C11 atomic override). Native
GCC/Clang CTest and pinned-TCC diagnostics run the same test. A pass is **not**
evidence that Salts supports emulated TLS or a new compiler in production.

## Can we reuse c-threads atomic implementation?

**Yes, as a reference and as an explicitly isolated experiment.**
`zelang-dev/c-threads/include/catomics.h` mixes GNU `__atomic`
intrinsics, legacy `__sync` fallbacks, Win32 intrinsics and architecture
assembly. Those techniques are useful for a future finite, qualified
native-atomic adapter, but the header cannot replace `<stdatomic.h>`
throughout Salts. In particular, its storage declaration and some
fallback `atomic_is_lock_free` paths differ from C11 object contracts.
It cannot make the compiler's `__STDC_NO_ATOMICS__=1` capability declaration
disappear, and moving a live native C11 atomic object into such storage by
cast or memcpy is prohibited.

Two independent experiments now test this distinction:

1. `intrinsic_atomic_backend.h` is **test-only**: it uses typed,
   naturally aligned 8/16/32/64-bit and pointer storage, checked memory orders,
   native GNU `__atomic_load/store/exchange/compare_exchange/fetch_add`
   operations, CAS expected-value writeback, lock-free observation and fences.
   `intrinsic_atomic_conformance.c` checks scalar/invalid-order semantics,
   multi-worker CAS contention, 64-bit RMW, and pre-join release/acquire
   publication. No `_Atomic` ABI is shared, no public header changes, and
   the code lives only under `tests/compiler`. GCC/Clang run it through
   CTest; the pinned TCC diagnostic compiles and runs the same source directly.
2. `catomics_reference_probe.c` is compiled in the experimental TCC
   workflow against the **unchanged, externally fetched**
   `zelang-dev/c-threads@d7cbefa0099044d6d834939290eb62e02ed8bb1b`
   `include/catomics.h` with GCC, Clang and TCC. It checks a deliberately
   narrow fetch-add/acquire-load/failed-CAS case, reporting failures
   independently. This file does not ship a copy of that upstream header
   or add `c-threads` to any target.

### Verified direct comparison (Linux x64, 2026-10-26)

The pinned upstream `catomics.h` **did not compile unchanged** under
either Clang (incompatible atomic-bool/spinlock intrinsic operands) or
TinyCC (`#error "stdatomic.h does not support your compiler"`). The narrow
GCC direct-header probe passed. See
[initial A/B evidence](https://github.com/qigao/salts/actions/runs/40228955216).

The **separately implemented, private** GNU-intrinsic backend passed GCC
and Clang CTest, plus pinned TinyCC run in
[exact-head #40229725519](https://github.com/qigao/salts/actions/runs/40229725519).
TinyCC required two implementation details:

- Its own installed `<stdatomic.h>` declares
  `atomic_thread_fence`, `atomic_signal_fence` and
  `__atomic_is_lock_free` runtime entrypoints. Calling the generic GNU
  fence spellings as external functions without those TinyCC mappings
  failed to link; the diagnostic uses the exact TinyCC declarations
  without changing `__STDC_NO_ATOMICS__`.
- The TinyCC generic `__atomic_load` diagnostic crashed when writing
  directly to an arbitrary caller output pointer. Loading into a
  correctly typed *local temporary* and then copying to the caller
  passed the scalar widths, pointer operations, concurrent CAS and
  publication tests on the pinned Linux x64 runner. This is a narrow,
  compiler-revision-specific observation, not a guarantee against
  different optimization, architectures or lifetime conditions.

The exact-head GCC/Clang suite remained green, as did the broader
[Salts CI](https://github.com/qigao/salts/actions/runs/40229725509).
The TCC job itself **remains failed** because Salts' normal
`<salts/atomic.h>` gate correctly rejects the declared
`__STDC_NO_ATOMICS__=1` compiler capability. Do not interpret the
aggregate GitHub workflow success as admission.

These are **capability diagnostics, not a supported Salts backend**. A future
real backend must establish object layout, alignment, lifetime, memory-order
and ABI equivalence or expose a distinct opaque API; it must additionally
pass ARM64 weak-memory, TSan, cross-toolchain and installed-SDK gates.
Do not redefine `__STDC_NO_ATOMICS__` or enable this experimental backend in
production to bypass a failing admission test.

## Real Concurrency consumer: SPSC Ring

`spsc_real_source_conformance.c` compiles **the unmodified**
`concurrency/src/spsc_ring.c` with its original
`concurrency/include/salts/spsc_ring.h`, not a copied algorithm.
It proves a deterministic wrap-marker path (64-byte capacity,
45-byte prior write/read, then a 26-byte wrap), invalid
non-power-of-two capacity rejection, and a borrowed-storage
50,000-frame FIFO handoff between exactly one producer and
one consumer. Frames are 13 bytes to force repeated nontrivial
wraps; the test checks payload/order/hash, capacity boundedness
and a quiescent empty state. Spin polling is bounded by monotonic
timeouts and both threads are joined before ring storage is freed.

This test reveals a critical adoption boundary: `cmeta_spsc_ring`
exposes aligned `_Atomic size_t` members in its **public struct**.
A private intrinsic backend using ordinary `size_t` storage is
*not* ABI interchangeable simply because sizes happen to agree.
**Do not cast, memcpy, alias, or silently swap that storage.**
Any eventual native-atomic facade for SPSC needs a separately
versioned opaque handle or demonstrated compile/ABI equivalence
across every supported consumer. Here we instead compile the actual
C11 consumer with the selected compiler as an isolated experiment.
TinyCC still declares `__STDC_NO_ATOMICS__=1`, therefore even a
passing SPSC test does not qualify TCC for the installed Salts SDK.

`DeadlineQueue` and `RCU` are not good atomic-backend
migration targets: they already use bounded plain data or
mutex-protected ownership. `Disruptor` is the more relevant next
opaque, synchronization-heavy runtime to qualify after SPSC.

## Real opaque Concurrency consumer: Disruptor

After SPSC, `disruptor_real_source_conformance.c` links the
**unmodified** `concurrency/src/disruptor.c` and
`platform/src/thread_posix.c` in one isolated Linux target.
It tests both BROADCAST (one registered consumer) and WORKER_POOL
(one worker) with exactly 20,000 ordered messages per mode,
power-of-two capacity rejection, real concurrent publisher/consumer
hand-off, timeouts, and final join-before-destroy.

Unlike SPSC, the public `disruptor_t` type is **opaque**: atomic
fields such as publication cursors, CAS loops and worker-completion
sequences are private. This makes Disruptor a better *candidate* for
future explicit native-atomic storage experiments, but this CI
**does not substitute** the private intrinsic backend for production
`_Atomic`: it compiles the original C11 atomics. TinyCC's
`-DSALTS_THREAD_LOCAL=_Thread_local` is limited to its isolated
diagnostic build, already independently tested for native TLS
semantics; the normal Salts header still rejects TCC.

An actual Disruptor production-compiler admission also requires
multi-producer or multi-consumer contention, out-of-order publication
and completion, parked-worker wake-up, cancellation, sanitizer
instrumentation and cross-platform/wide-memory-order testing.
One publisher and one consumer are only the first execution gate.

## Finite Native Atomic API: actual Disruptor needs

A source inventory of the unchanged `concurrency/src/disruptor.c`
(current branch) found 48 native atomic-operation call sites:

| Operation | Calls | Primitive exposed by private adapter |
| --- | ---: | --- |
| `atomic_load_explicit` | 20 | typed load with order validation |
| `atomic_store_explicit` | 16 | typed store with order validation |
| `atomic_fetch_add_explicit` | 4 | typed add, including acq_rel zero-add |
| `atomic_fetch_sub_explicit` | 2 | typed subtract |
| `atomic_compare_exchange_weak_explicit` | 4 | typed weak CAS, expected writeback |
| `atomic_compare_exchange_strong_explicit` | 2 | typed strong CAS, expected writeback |

The private `intrinsic_atomic_backend.h` now supplies the exact
finite operation families needed by this opaque component, without
providing a catch-all `stdatomic.h` replacement. The diagnostic
`intrinsic_atomic_conformance.c` exercises all 8/16/32/64-bit
add/sub, acq_rel zero-add RMW, failed/weak-CAS semantics, bounded
spurious retries, and six concurrently contending weak-CAS workers
with 64-bit RMW and release/acquire publication.

**Storage must remain independently owned by one backend.** Even if
pointer size and alignment happen to match, the private intrinsic
storage must not be reinterpreted as `_Atomic` through pointer casts,
memcpy or a mixed TCC/GCC object alias. Disruptor's public opaque
handle is a possible *future* migration boundary; SPSC's public
`_Atomic size_t` fields are not. No Salts production code or installed
SDK uses this experimental adapter.

In the unchanged Disruptor, `reverse_gap_case()` additionally
forces a 4→3→2→1 publication order (BROADCAST) and a 4→3→2→1
worker completion order (WORKER_POOL). It verifies that the
respective cursors cover the full contiguous range without needing
an extra publisher to rescue publication or retirement, and that
the next bounded slot becomes available.

These tests prove only the specified x64/Linux executions. They do
not substitute weak-memory litmus testing on ARM64, TSan, exact
compiler/DSO unwind ABI, or complete installed SDK admission.

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
