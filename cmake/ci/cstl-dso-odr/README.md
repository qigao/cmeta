# CSTL descriptor ownership across real DSOs — test gate

Tracks [Salts #1167](https://github.com/qigao/salts/issues/1167). **Test/CI only; no production linkage or public ABI change.**

## Why this test exists

A Salts rc.15 ASan+UBSan integration observed
`stl_bplus_tree_container_desc` registered by both instrumented
`libsalts.so.2.3` and a separately linked TurboAgent Provider shared object
when each embedded the static `Salts::CSTL` archive. A duplicate ASan global
registration is a diagnostic, **not yet evidence of production corruption**.

ASan stays active with `detect_odr_violation=2`; we do not suppress errors
or classify unrelated loader failures as ODR rejection.

## Controlled matrix

The workflow restores the exact Salts
`v2.3.0-rc.15` commit `21bdb1145fbb2b39ee3fa644173ec037adba7b87`
into an independent checkout. In every case the complete Salts SDK is built
and installed to a fresh prefix under ASan+UBSan, and a **separate, public
Salts::CSTL SDK consumer** produces a real Provider DSO. A native executable
calls POSIX `dlopen`, invokes the public descriptor pointer query, and
`dlclose`s both images.

| Matrix ID | Only test-mode change | Result classification |
|---|---|---|
| `static-default` | `BUILD_SHARED_LIBS=OFF` | Must produce an exact ASan ODR rejection naming the real descriptor. The job passes **as a negative test**; this mode is not safe. |
| `static-hidden` | Static CSTL, deferred **CSTL target only** `C_VISIBILITY_PRESET=hidden` | Record `HIDDEN_STATIC_ASAN_ACCEPTED` on real clean load, or `HIDDEN_STATIC_REJECTED_BY_ASAN_ODR` on exact rejection. Any unrelated failure fails the job. |
| `shared-single` | `BUILD_SHARED_LIBS=ON` | Both DSOs must carry `libcmeta_cstl.so` DT_NEEDED; the shared CSTL library owns the descriptor. Clean ASan load/invoke/unload is required. |

The hidden-static overlay uses `CMAKE_PROJECT_Salts_INCLUDE` and defers a
CMake property assignment until the end of the *test checkout's* root
configuration. It does not edit the pinned source or alter `Salts::CSTL` in
the user-facing build.

## Evidence / fail-closed conditions

`run_compare.py` refuses a nonmatching source commit, missing CMake SDK
exports, stale build/install inputs, uninstrumented
`cstl/src/associative_meta.c`, absent descriptor definitions, incorrect ELF
link dependencies, missing real ASan symbols, and unexpected runtime failure
types. It preserves the native provider/loader linker maps, compiler command
databases, SHA-256 for actual images and complete ASan output under distinct
per-mode CI artifacts.

**Interpreting green CI:** `static-default` succeeding means the expected
*negative* ASan diagnostic was verified. `static-hidden` may also run green
while its candidate is rejected, because the classification is the result.
Only `shared-single` is required to load successfully. Read the structured
`topology-result.json` before declaring a design winner.

Linux ASan/ELF alone does not establish Windows DLL import, macOS dylib
ownership, installed Native SDK release conformance on other RIDs or a
production static/shared policy. A product change requires a separate
explicit decision, migration and cross-platform consumer tests. Registry
Ref/Lease incarnation fencing is unrelated ([Salts #1156](https://github.com/qigao/salts/issues/1156)).
