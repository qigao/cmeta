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

## Concurrent MPMC Disruptor admission experiment

`disruptor_mpmc_real_source.c` is built with the same **unchanged**
`concurrency/src/disruptor.c` and Salts::Platform implementation.
The test starts 4 native pthread publishers and 4 native pthread
workers against a 64-slot `DISRUPTOR_MODE_WORKER_POOL` queue.
Publishers concurrently contend for cursor claims; workers
independently contend for claims and retire their slots, sometimes
out of sequence. All 12,000 published messages are checked by
sequence-derived payload, and a pthread-mutex-protected verdict table
proves each sequence was processed **exactly once in this test**.
The verdict mutex does not guard production publication, claims or
retirement; those still exercise the real Disruptor atomics. Every
worker/publisher polls only with a bounded monotonic deadline and
all threads are joined before destroying the queue.

In addition to ordinary native CTest, the dedicated compiler workflow
repeats each real SPSC/Disruptor test five times under GCC/Clang
in both Debug and Release configurations. The pinned TinyCC
experimental diagnostic independently runs the 4×4 MPMC test five
times. Repeat runs raise the chance of exposing timing-sensitive
faults, but do **not** substitute race instrumentation or
weak-memory/ARM64 conformance.

This is a strictly isolated Linux qualification. TinyCC uses a
private `SALTS_THREAD_LOCAL=_Thread_local` compile definition for
the otherwise-unchanged Platform source, never an ABI or installed
header change. GCC/Clang are the normal CTest baselines. The TCC
diagnostic is deliberately non-blocking and must **not** turn a green
aggregate workflow into a false claim of TCC compiler admission.
More runs, memory-order litmus tests, TSan, cancellation/close
races, parked-worker wakeups and ARM64 are still required.

## Parked worker / wake and cancellation contract

`disruptor_parked_worker_conformance.c` links actual
`concurrency/src/disruptor.c` + actual POSIX Platform. The worker
uses `disruptor_worker_claim_wait`, not a spin-only substitute.
A counted `keep_running` callback publishes its 17th invocation
using native release/acquire atomics. With 16 spin callbacks
before worker-waiter admission in the current implementation,
this handshake proves the worker has incremented
`worker_waiters` and entered the mutex-protected park path
before a producer starts publication or control-plane cancellation.
The caller runs two scenarios: one publication must wake the
parked worker automatically, and one explicit
`keep_running=false` plus `disruptor_worker_wake_all`
must wake it with no claimed record. The one-slot queue
also proves completion retirement before slot reuse.
This tests a *request to stop/wake*, **not** a Disruptor
close API or the complete CFlow/ThreadPool shutdown contract.

The native GCC/Clang CTest jobs and pinned TCC diagnostic
each repeat the new parked-worker test five times, alongside
the existing FIFO/MPMC tests.

**Source sanitizers:** GCC ASan+UBSan is introduced as an
independent required test job for real SPSC and Disruptor
source, while source GCC TSan is initially explicitly
experimental/non-blocking pending runner compatibility
and review of race reports. A successful ordinary
compiler workflow cannot be interpreted as TSan-qualified
if that diagnostic job fails.

## Native Linux ARM64 atomic and queue execution

The compiler qualification workflow runs **GCC and Clang on native
`ubuntu-24.04-arm` runners** (not QEMU, cross-compilation, or an x64
simulator). It verifies `uname -m` is AArch64, compiles the complete
standalone suite with each native compiler, runs all CTest assertions,
then repeats seven native scalar/ABI/real-SPSC/Disruptor test targets
**20 times each**. This specifically exercises release/acquire
publication, competing CAS, 64-bit cursor retirement, wrap-around
payload ownership, worker wake/stop and 4-publisher/4-worker MPMC
under an ARM64 execution model rather than only x86-64.

**Scope remains narrow:** repeated execution is not an exhaustive
ARMv8 litmus/model check, hardware weak-memory proof, source TSan on
ARM64, mixed GCC/TCC shared ABI qualification or an installed SDK
admission. TinyCC remains experimental, separately pinned on Linux
x64, and the strict `__STDC_NO_ATOMICS__` gate stays unchanged.

## Pinned TinyCC native ARM64 diagnostic

The same exact upstream `TinyCC/tinycc@e23dabed154a8495f3ac1ff3cc9d04fa1c207893`
contains an AArch64 code generator and native `arm64` Makefile target.
The experimental CI builds that revision **on a native
`ubuntu-24.04-arm` runner** and independently attempts its own scalar
atomics, Salts private intrinsic storage contract, the unmodified
SPSC source, and the opaque Disruptor 4×4 MPMC source. Each diagnostic
step retains its distinct result instead of pretending one passing
scalar opcode certifies the whole backend.

The final strict compiler CTest compile remains fail-closed for
`__STDC_NO_ATOMICS__=1`. That admission failure is not overridden
by any of these narrower runtime tests. The entire ARM64 TinyCC job
is marked experimental and non-blocking; real GCC/Clang ARM64 tests
are separate, mandatory baseline jobs. No TCC-built ARM64 artifact
is installed or offered as an SDK.

## MIR/minicoro-inspired external ASM atomic prototype

The design is **not** an imported MIR or minicoro Atomic implementation:
[MIR's C11-to-MIR front-end](https://github.com/vnmakarov/mir/tree/master/c2mir)
explicitly excludes C11 atomics, and
[minicoro](https://github.com/edubart/minicoro/blob/main/minicoro.h)
uses assembly for coroutine register/stack switching, not atomicity.
Their useful pattern is **architecture-specific low-level execution
behind ordinary C callable entrypoints**, with explicit ABI tests
and fail-closed unsupported targets.

New test-only files under `asm-atomic/` implement that architecture split:

- `asm_atomic_x86_64.S`: SysV AMD64 GAS. `lock xadd` and
  `lock cmpxchg` RMW/CAS; `xchg` for stores/exchange; TSO reads;
  `mfence`. Generates an ELF `.note.GNU-stack` marker.
- `asm_atomic_aarch64.S`: AAPCS64 GAS. `ldar/stlr`,
  `ldaxr/stlxr` LL/SC loops (no optional LSE instructions), and
  conservative `dmb ish` fences. Generates a non-executable stack note.
- `asm_atomic_private.h`: separately owned, naturally aligned
  32-bit and 64-bit scalar slots and ordinary external C ABI functions.
  This is *not* the layout or lifetime of C11 `_Atomic` objects.
- `asm_atomic_conformance.c`: strong/failed CAS expected-value
  writeback, load/store/exchange, fetch-add/sub, explicit fence,
  four contending pthread workers (32-bit CAS + 64-bit RMW), and
  release/acquire publication observed **before thread join**.

**All these functions implement a stronger SEQ_CST-only contract**.
There is intentionally no `memory_order` enum yet, no compiler
feature override, no inline ASM dependence in TinyCC, no runtime
code generation/W^X, no locking fallback, and no mixing of backends
within one live object. Linux x86-64 and Linux AArch64 only;
other architectures are rejected at compile time.

The native GCC/Clang CMake+CTest matrix includes the same source and
ASM with the matching assembler, and the pinned TinyCC x86-64/ARM64
diagnostics assemble `.S` via native GNU `gcc -c` and then ask
TinyCC to compile the C consumer and link those ELF objects itself.
This separates **C compiler ASM syntax limitations** from
**assembler/object linking** and **atomic CPU behavior**.

**Limits:** foreign ASM operations are outside ISO C's native atomic
object model. TSan instrumentation cannot see the atomic operations
inside external assembly unless proper instrumentation/annotation is
added and qualified; a TSan pass of surrounding C does *not* certify
this backend. No 8/16-bit primitives, pointer ABI, ARMv8 formal memory
model proof, Windows ABI, cross-DSO lifetime, sanitizer-compatible
mixed ABI, installed SDK or Disruptor replacement is claimed.

## Real Disruptor: original C11 vs private external ASM storage

**The same unmodified production `concurrency/src/disruptor.c` is compiled
twice** in the independent compiler qualification suite. The original
C11 target keeps its own `atomic_uint_fast64_t`, `atomic_uint`,
and relaxed/acquire/release/acq_rel operations. The separate
`asm-atomic/disruptor_asm_inclusion.c` translation unit includes the
*identical* original source after a test-only
`asm-atomic/disruptor_asm_adapter.h` mapping of the **two private,
internal field types** onto independently owned aligned assembly slots.

The ASM mapping selects C-callable 32/64-bit functions by C11
`_Generic`. Every atomic access routes to the independently assembled
Linux x86-64 or AArch64 `.S` file; even weak CAS maps to the
allowed stronger strong CAS implementation. All assembly operations
are **SEQ_CST**, intentionally stronger than the original operation
orders. The compiler feature macros, public opaque
`disruptor_t` signature, and Salts's public C11 admission gate
remain untouched.

Both backends execute the *exact same* three test source programs:
single publisher/consumer Broadcast and Worker Pool, reverse
out-of-order publication/completion, four publishers and four
workers with exact-once-in-test verification, and real
`worker_claim_wait` park/wake/stop semantics. No copied or simplified
Disruptor implementation is used.

The private ASM slots **must never be substituted in the installed
SDK or mixed inside one live C11 atomic object**. They are a
test-specific compile-time internal representation. The native
GCC/Clang CTest binaries run both implementations, and the pinned
TinyCC x86-64/ARM64 diagnostic compiles the ASM-adapted
*actual production Disruptor source* from the inclusion wrapper,
linking the architecture-specific GNU assembler object.

A green ASM run does not remove TinyCC's
`__STDC_NO_ATOMICS__=1` declaration. Nor does a GCC source TSan
pass instrument the unobservable external machine-code memory
accesses. The comparison demonstrates bounded observable
correctness in the tested executions, **not general C11 atomic
semantic, TSan, ABI, or cross-DSO equivalence**.

## Paired Disruptor C11 vs external ASM throughput comparison

The benchmark uses the **same** `disruptor_mpmc_real_source.c` source
and actual unmodified Disruptor implementation, built twice with the
**same GCC or Clang**, same `-O2` flags, 4 publishers, 4 workers, a
bounded 64-slot ring and 80,000 records per invocation. Unlike a
synthetic atomic loop, both versions check payload integrity, duplicate
claims, missed messages and complete retirement before reporting time.

The sole backend distinction is original C11 internal atomics with
their existing relaxed/acquire/release/acq_rel orders versus independent
GAS atomic C ABI calls with conservative `seq_cst` ordering.
This is **not** a like-for-like ordering-strength microbenchmark; it
quantifies the cost of the proposed isolated implementation strategy,
including external function-call overhead and stronger fences.

`asm-atomic/compare_disruptor_benchmark.py` runs two warm-ups and
seven alternating-order paired samples on the same runner, retains
each sample's nanosecond wall time and messages/s, reports the median
throughput and ASM/native ratio, and uploads the raw JSON evidence.
Two native Linux architectures × GCC/Clang are measured independently.
There is **no release performance threshold**; host scheduling and
frequency variation can change ratios, and no cross-runner comparison
should be treated as a hardware speedup measurement.

The printed `mean_wall_ns_per_msg` is aggregate **wall time divided
by total delivered records**, **not** an individual enqueue-to-consume
latency or p99 latency. Such latency measurement requires a separately
instrumented packet timestamped harness. Do not infer p50/p99 latency
from throughput alone. Correctness and native C11 source sanitizer
gates remain separate from the uninstrumented foreign ASM backend.

## True per-message publish-to-claim handoff age

The original throughput benchmark printed `mean_wall_ns_per_msg`,
which is an **aggregate throughput-derived number**, not a
message-specific latency sample. It cannot produce real p50/p95/p99.

A separate `SALTS_COMPILER_MPMC_LATENCY` mode in the **same**
`disruptor_mpmc_real_source.c` now uses a 16-byte payload containing
the integrity-checked sequence value and `CLOCK_MONOTONIC`
timestamp taken **immediately before** `disruptor_publisher_publish`.
A worker records another monotonic timestamp immediately after
successful claim and access, before taking the test-verdict mutex.
It computes the nonnegative age of **each** of 16,000 distinct messages
(4 producers, 4 workers, 64-slot ring), refuses to emit a result
unless every sequence was uniquely and correctly retired, then
sorts the samples after pthread join to report actual per-message
p50, p95, p99 and max for the individual run.

This is specifically a **pre-publish-to-post-claim age**. It
includes publication function execution, consumer claim, scheduler
delays, timestamp overhead and any ring wait. It is not a pure
enqueue API latency, is not an isolated clock-cycle measurement,
and does not represent network or service end-to-end latency.

The `compare_disruptor_latency.py` driver alternates native C11 and
ASM order with two warm-ups and seven measured paired runs, and
retains each run's real quantile and raw message count in JSON.
It reports medians **of the seven per-run quantiles**, not a pooled
p99 or a p99 inferred from aggregate throughput. Both compiled
targets use `-O2`, the same timestamped packet layout, payload
validation and concurrency harness; only the internal atomic
backend changes. This is observational: no speedup threshold and
no promotion of ASM as a TSan-validated production backend.

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
