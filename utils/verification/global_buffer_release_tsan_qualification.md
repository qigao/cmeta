# Global buffer cross-owner TSan qualification — Salts 2.3.0-rc.15

**Scope:** `mem_global()`, `mem_get_buffer()`, `mem_buffer_release()`, the
internal pool-owned atomic cache, slab magazine and thread synchronization.
Tracks [Salts #1170](https://github.com/qigao/salts/issues/1170) and the
test-only [Draft PR #1171](https://github.com/qigao/salts/pull/1171).

This record is for **release qualification, not a claim that all concurrent
executions have been proven safe**. It does not change the allocator, SDK ABI,
CNet Owner contract, or the already published Salts release.

## Exact inputs and provenance

- Published tag: `v2.3.0-rc.15`, commit
  `21bdb1145fbb2b39ee3fa644173ec037adba7b87`.
- Published NuGet: `Salts.Native.2.3.0-rc.15.nupkg`, SHA256
  `fa73229c7b2506b393fe779bb56f8d42c387a79d30db54c0cf17b408b7ba7ad1`.
- Host RID: `linux-x64`; the binary consumer checks the package's
  `salts-sdk-manifest.txt` (`version`, `commit`, `profile=release`) and
  resolves `libsalts.so.2.3` from the extracted SDK by ELF linkage, not from
  another installed Salts build.
- The original allocator/thread files are unchanged between the Draft
  test's base `c8a1a3d` and the published rc.15 commit. The exact-tag
  TSan workflow additionally checks out the original release commit and
  rebuilds **that version**, rather than treating a newer `master` as
  representative of the released library.

## Results — different categories, not interchangeable

| Gate | Evidence | Observed outcome | Interpretation |
| --- | --- | --- | --- |
| Source-instrumented allocator | [Salts exact release-source TSan run #38340631080](https://github.com/qigao/salts/actions/runs/38340631080) | SUCCESS; two independent C11 tests, 10 repetitions each | Real rc.15 allocator, platform synchronization and test code are TSan-instrumented; no reported data race in the tested interleavings |
| Original-source allocator | [Salts #38339194853](https://github.com/qigao/salts/actions/runs/38339194853) | SUCCESS, 10/10 for both tests | Confirms the same source conformance on the Draft branch |
| Published binary ownership | [Salts #38339194858](https://github.com/qigao/salts/actions/runs/38339194858) | SUCCESS, 10/10 for both tests | No duplicate live buffer borrowing or corrupted payload was observed |
| Mixed instrumentation diagnostic | [Salts #38339640783](https://github.com/qigao/salts/actions/runs/38339640783), [raw log artifact](https://github.com/qigao/salts/actions/runs/38339640783/artifacts/11785913290) | TSan emitted `WARNING: ThreadSanitizer: data race` and exited 66 in an observation-only step | **NOT TSan-qualified**; released `libsalts.so.2.3` is not instrumented, so internal atomic cache publication/acquisition is invisible to TSan |
| Actual RTSP under source TSan | [TurboMedia #38337337771](https://github.com/qigao/turbomedia/actions/runs/38337337771) | SUCCESS | Tests real RTSP/CNet/Codec/DSO with an actual source-instrumented release-tag Salts |
| Installed SDK Codec/DSO under ASan+UBSan | [TurboMedia #38337337757](https://github.com/qigao/turbomedia/actions/runs/38337337757) | Release and ASan+UBSan both SUCCESS (3/3 each) | Scoped to Linux same-instruction RTSP/Codec/DSO acceptance, not third-party TSan |
| RTSP with mixed published-binary TSan | [TurboMedia #38337337816](https://github.com/qigao/turbomedia/actions/runs/38337337816) | FAILURE in the uninstrumented Salts allocator path | Preserve as diagnostic; **never relabel as PASS**, suppress, or use as sole evidence of a double-owned buffer |

The two C11 allocator conformance tests are:

1. `utils/tests/cmeta_global_buffer_concurrent_tsan.c`: 256 synchronized
   double-live leases, followed by 40,000 un-gated allocations/writes/releases
   per invocation. The barrier establishes ownership checks between epochs,
   but the stress section has no global serializing lock.
2. `utils/tests/cmeta_global_buffer_handoff_tsan.c`: 1,024 explicit alternations
   of the **same recycled buffer shell** between two Owner threads. The test's
   turn marker uses relaxed atomics; it does **not** create a test-level
   acquire/release edge for the buffer payload. The allocator's own atomic
   handoff must establish visibility. A different shell or data pointer
   fails the test.

The source TSan workflows inspect `compile_commands.json` for the actual
`-fsanitize=thread` flags on `utils/src/cmeta_buffer.c`, thread primitives
(`utils/src/cmeta_thread.c`, `platform/src/thread_posix.c` in the
exact-tag check), and the test units.

## Release gate classification

**Qualifying / blocking results for the stated scope:**

- Original released allocator and synchronization code compiled **with**
  TSan, run without any sanitizer suppressions, with ownership and handoff
  checks passing.
- Exact published SDK checked by digest, version, commit and ELF source;
  uninstrumented-binary **runtime ownership** checks passing.
- Actual installed RTSP/Codec/DSO lifecycle acceptance under ASan+UBSan;
  full-source RTSP TSan acceptance separately.

**Diagnostic, not a qualifying TSan pass:**

- A TSan-instrumented TurboMedia or standalone test using an **uninstrumented**
  released Salts DSO. A warning from that configuration is preserved and
  triaged, but the absence of visible synchronization prevents using it alone
  to assert a production data race. A no-warning execution also would not
  prove the precompiled allocator TSan-clean.

The controlled comparison strongly supports missing TSan visibility of
published atomic synchronization as the explanation for this diagnostic.
It does **not** rule out other races in other paths, and it does **not**
authorize TSan suppressions or removal of the diagnostic workflow.

If the allocator, `cmeta_thread` primitives, SDK build flags, release tag
or CNet buffer ownership change, rerun **both** fully instrumented tests
and published-binary comparison with new immutable provenance.

## Integration policy

This qualification does **not** authorize merging long-lived Draft
integration branches. Salts [#1171](https://github.com/qigao/salts/pull/1171),
[#1176](https://github.com/qigao/salts/pull/1176) and TurboMedia
[#187](https://github.com/qigao/turbomedia/pull/187) /
[#193](https://github.com/qigao/turbomedia/pull/193) retain their explicit
**DO NOT MERGE** constraints until separately reversed. No new Salts
release is required solely by the mixed-instrumentation diagnostic.
