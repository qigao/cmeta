# Release readiness — RFC-first C11 SCTP (#1281)

**Initial preflight: 2026-10-13. Feature-checkpoint update: 2026-10-25. Decision: NO-GO for production SCTP.**
This is a release *preflight record*, not permission to create a tag, publish a package, or replace TurboMedia's usrsctp. Private research [PR #1281](https://github.com/qigao/salts/pull/1281) has merged to `master`; the standalone prototype now has an opaque installed C11 session target, but no production CNet/DTLS integration or Salts.Native publication. All SCTP work remains original C11, with no Rust/C++ SCTP engine, bridge, or runtime fallback.

## Version and immutable source authority

| Identity | Verified at preflight | Meaning |
| --- | --- | --- |
| Salts CMake / vcpkg | `2.3.0`; native SONAME `2.3` | Next intended **Salts** stable version, not an SCTP feature release |
| Published Salts prerelease | `v2.3.0-rc.15` at `21bdb1145fbb2b39ee3fa644173ec037adba7b87` | Official immutable six-RID candidate, NOT a release of this SCTP module |
| `master` | `c8a1a3d5e259a89ad42e75f1e4c3142e6b106ee1` | Common source ancestor |
| Draft SCTP preflight source | `0bbe3f0824e3368f2ed8a39bf555285fc487cb1b` | Private C11 tests only; documentation commits after this require exact-head requalification |
| Standalone C11 research CMake project | `CNetSctpPrototype 0.1.0` | **Only Wire** is exported as a separate test package. Not a Salts.Native installed SDK, release or stable SCTP ABI |

**Historical ancestry at 2026-10-13 preflight:** the published rc.15 tag was **13 commits ahead of** then-`master`; the SCTP Draft at that time was **82 ahead / 13 behind** relative to rc.15. These counts and the table above are not current branch comparisons. An SCTP-only green PR is NOT a validated release-tag ABI/source graph. Do not create an SCTP release tag from the Draft head, and do not publish stable Salts 2.3.0 from the older `master` without reviewing the immutable rc.15 dependency graph and release epic.

Release governance is already tracked in [Salts #1081](https://github.com/qigao/salts/issues/1081) (2.3 unified release) and [#1018](https://github.com/qigao/salts/issues/1018) (native ABI). This document adds **no new release authority** and does not supersede those issue gates.

## M0–M6 feature gate status

| Gate | Current evidence | Go/no-go |
| --- | --- | --- |
| M0 RFC and legal provenance | RFC matrix and C11-only module; known GmSSL Apache-2.0 provider for tests | **PARTIAL** — final release source/license/dependency audit open |
| M1 C11 wire + installed consumer | Strict Chunk framing, CRC32C and INIT TLVs; installed `CNetSctpPrototype::Session` uses an opaque C11 handle and versioned create config, while its independent consumer proves two-session handshake, DCEP OPEN/ACK, message transfer and teardown | **IMPLEMENTED IN STANDALONE PROTOTYPE** — not yet exported by production `Salts::CNet` or Salts.Native |
| M2 SCTP core | Owner-driven INIT/COOKIE/T1, HMAC-SM3, entropy/key rotation, bounded DATA/SACK/T3/Fast Recovery and zero-window recovery; SHUTDOWN/T2 with pending DATA and ULP RX drain; atomic bundled DATA/leading SACK receive; optional absolute T5 shutdown guard with ABORT-on-expiry and late-ACK preemption | **PARTIAL** — complete congestion/PMTU, restart, full ABORT/error-cause and peer-race behavior, full RFC wire surface and real Provider Owner integration remain open |
| M3 WebRTC extensions | PPID/DCEP OPEN/ACK with channel-type projection into the shared DATA reliability policy and pre-ACK ordering, explicit Owner-driven closing state coordinated with both reciprocal reset directions before SID reuse, ordered/unordered streams, 24 KiB DATA/I-DATA fragmentation, dedicated RFC 3758 negotiation with bounded fragmented timed/max-retransmit FORWARD-TSN/I-FORWARD-TSN and fixed Owner-pulled abandonment records, negotiated RFC 8260 MID/FSN receive interleaving, and a bounded negotiated RFC 6525 slice. The opaque installed ULP makes OPEN/SCTP admission transactional, retains peer ACK backpressure, binds close results to a 64-bit per-SID generation before reuse, and surfaces exactly-once generation-bound abandonment. | **PARTIAL** — production CNet/DTLS Generation leases, multi-message scheduling/partial delivery, remaining cross-parameter collisions, negotiated limits and complete browser qualification remain open |
| M4 independent interoperability | TurboMedia PR #269 (merged as `78e36536ca3cd39dbd0fdcbc8f6c5f636436a833`) drives the private C11 engine through real ICE/DTLS against public Janus and verifies DCEP, text/binary/20 KiB fragments, unordered, reset/reuse, controlled RTX1 and RTX0 loss, I-FORWARD-TSN and later ordered continuity | **PARTIAL** — public-peer WebRTC evidence exists, but it still uses a source-integrated test path rather than the installed Session ABI plus production CNet DTLS generation lease |
| M5 ownership, sanitizers and SDK | Linux/Windows/macOS standalone C11, source ASan/UBSan, pinned-GmSSL Cookie-MAC, and effective-source TSan for private Owner handoff + test-DSO callback/drain/dlclose all green at exact HEAD `1ebec510a7928864a74307a98aaa18a310abd0de` ([CNet C11 #39705821651](https://github.com/qigao/salts/actions/runs/39705821651)); Salts CI selected Android/iOS Release checks pass ([#39705821753](https://github.com/qigao/salts/actions/runs/39705821753)) | **PARTIAL** — production CNet Owner/DTLS Generation drain and real DTLS Provider unload race remain open; Android/iOS builds did not install/qualify the new SCTP session SDK; Linux arm64 and six-RID installed ABI checks remain open |
| M6 review/cutover | No independent protocol review or explicit release/cutover approval | **BLOCKED** |

**Exact HEAD lifecycle checkpoint (2026-10-22):** `1ebec510a7928864a74307a98aaa18a310abd0de` passes [CNet SCTP C11 #39705821651](https://github.com/qigao/salts/actions/runs/39705821651) 6/6, including effective-source TSan. The TSan job verifies compile instrumentation for `sctp_wire.c`, `sctp_assoc.c`, `sctp_data.c`, `sctp_init_params.c`, `sctp_collision.c`, `owner_thread_test.c`, `provider_dso_test.c`, and `provider_dso_fixture.c`, and repeats both Owner and test-DSO tests 10 times. The test DSO blocks inside an entropy callback, rejects new generation calls after cancellation, requires callback drain and Owner stop/drain/destroy before `dlclose`. This is test-provider evidence, **not** production GmSSL/DTLS application-data integration or independent browser/libwebrtc interoperability. [Salts CI #39705821753](https://github.com/qigao/salts/actions/runs/39705821753) and [immutable build provenance #39705821802](https://github.com/qigao/salts/actions/runs/39705821802) succeeded; Formal, package, benchmarks, installed ServiceHost ABI, six-RID ABI identity, and SDK publication were skipped. Those skips are not passes.

**Historical baseline evidence (2026-10-13):** [C11 #38358908402](https://github.com/qigao/salts/actions/runs/38358908402) 5/5 SUCCESS; [Salts CI #38358908624](https://github.com/qigao/salts/actions/runs/38358908624) SUCCESS. The latter's existing Android/iOS builds do **not** compile or install the new C11 SCTP engine. A green standalone Wire consumer is not a released SCTP session SDK.

**Current feature-source checkpoint (2026-10-19):** PR #1182 source/test HEAD is `ef2013585cc7c175def796207bead2a5b1dc038d`. Two bounded-shutdown retry rules are now covered. First, duplicate `SHUTDOWN` in `SHUTDOWN-ACK-SENT` retransmits the saved ACK transactionally and restarts T2 with the current backed-off RTO, but does **not** reset the bounded ACK retry budget. Second, DATA received in `SHUTDOWN-SENT` restarts T2 and receives the required response, but resets the retry counter only when the staged RX state demonstrates new peer progress (new buffered bytes, cumulative TSN movement, or a changed gap map); replay of an already-admitted DATA TSN cannot replenish the budget. The regression tests exercise duplicate SHUTDOWN after two ACK retries and duplicate DATA after two SHUTDOWN retries, then verify bounded expiry. Exact source/test-head evidence: [CNet SCTP C11 #39193007201](https://github.com/qigao/salts/actions/runs/39193007201) **SUCCESS 5/5** across Linux/macOS/Windows, source ASan/UBSan and pinned-GmSSL Cookie-MAC; [immutable provenance #39193007250](https://github.com/qigao/salts/actions/runs/39193007250) **SUCCESS**; [Salts CI #39193007321](https://github.com/qigao/salts/actions/runs/39193007321) **SUCCESS** for selected checks including Android ARM64 and iOS ARM64 Release builds. Salts CI skipped formal, package, benchmarks, installed ServiceHost ABI, six-RID identity and SDK publication; ACE candidate #39193007199 was skipped. These results are not full release or installed-SCTP-SDK qualification. Still unqualified: independent shutdown interoperability and complete SCTP shutdown/ABORT/restart semantics, complete RFC 9260 and WebRTC extensions, CNet/DTLS Generation leases and source-level TSan, actual independent browser interoperability, and six-RID installed SCTP session consumers.
## Production DTLS integration boundary — source audit 2026-10-22

A source-tree audit of the exact HEAD found `cnet/src/cnet_tls.h` and `cnet/src/cnet_tls.c`, which expose a **stream TLS** state machine (`cnet_tls_handshake`, `cnet_tls_feed_cipher`, `cnet_tls_take_cipher`, `cnet_tls_write`, `cnet_tls_read`, `cnet_tls_shutdown`). They do not expose a DTLS session API, datagram-preserving DTLS record pump, or DTLS generation lease that the private SCTP Association can consume. The SCTP research module currently remains a separate CMake project and does not link to the production CNet graph.

Therefore the production lifecycle gate cannot safely be closed by wiring SCTP directly to the existing stream-TLS API or by renaming a TLS callback as DTLS. Browser DataChannel requires SCTP over DTLS datagrams. The correct next step is a cross-repository integration contract with the existing DTLS owner in TurboMedia: define bounded datagram ingress/egress, immutable DTLS peer-generation identity, Owner-thread admission, cancellation/drain, and destroy-before-unload ordering, without adding a socket, scheduler, fallback, or a second production SCTP engine. If the current DTLS implementation has no stable callback contract, keep the SCTP engine isolated and track that missing contract explicitly rather than inventing an API here.

Acceptance should test this actual bridge with real DTLS records and the production owner/lease implementation under effective-source TSan; the existing test DSO remains a lifecycle fixture only. Independent browser/libwebrtc interoperability and the skipped installed-SDK gates remain separate blockers.

## Two mutually separate release lanes

### A. Salts.Native 2.3.0 — existing product

Can be prepared **without #1181** only through existing [release epic #1081](https://github.com/qigao/salts/issues/1081), [ABI gate #1018](https://github.com/qigao/salts/issues/1018), and exact approved candidate release workflow. The 2.3.0 tag does not currently exist in the checked repository. Before stable publication, choose an authoritative reviewed source that includes all required once ABI, ACE/CNet changes, satisfy six-RID installed SDK and published-byte downstream/TSan gates, and explicitly obtain release approval. The merged private SCTP sources are not an exported feature and their scoped CI does not qualify this release lane.

Separate known cross-repo qualification: [#1177](https://github.com/qigao/salts/issues/1177) tracks the rc.15 release-SDK TSan async-logging instrumentation visibility issue; diagnose it without suppressions. [#1160](https://github.com/qigao/salts/issues/1160) is another explicitly segregated GmSSL RFC8998 staged release lane, not automatically part of #1181 or 2.3.0.

### B. RFC-first CNet SCTP / TurboMedia DataChannel

**NO-GO** for a Salts.Native release or TurboMedia cutover today. Continue follow-up work from the merged research baseline and [TurboMedia #227](https://github.com/qigao/turbomedia/issues/227). The standalone opaque Session ABI must still be integrated with the production CNet DTLS generation/lease graph and pass full first-party sanitizer, six-RID installed consumers, performance comparison and immutable-package qualification. Only M6 written approval can permit production export and exclusive usrsctp removal. No dual-backend release and no automatic runtime fallback.

## Release preparation checklist (no release action authorized)

- [ ] Confirm approved Salts 2.3.0 source/ABI baseline under #1081; do not tag `master` opportunistically.
- [x] Merge the isolated private research snapshot through #1281 without changing production targets.
- [ ] Requalify every new SCTP follow-up SHA; feature acceptance, installed SDK qualification and production integration remain separate gates.
- [ ] Complete RFC 9260 association, data plane, reliability, shutdown, restart/tie-tag and exception paths within bounded single Owner.
- [ ] Complete WebRTC ULP channel-state/PPID coordination, complex RFC 6525 collision handling, multi-message scheduling/partial delivery and independent browser GmSSL/DTLS tests with captured wire evidence.
- [ ] Integrate CNet Owner + DTLS Generation leases, callback terminal once/drain, cancel vs input/timer/close races and full-source TSan.
- [ ] Qualify exact new SCTP installed C11 SDK for Linux x64/arm64, Windows x64, macOS arm64, Android arm64 and iOS arm64. Confirm no Rust/C++ SCTP runtime dependency.
- [ ] Obtain independent implementation/security/license review and maintainer approval for M6; **do not merely tick the gate because CI is green**.
- [ ] For an approved product source, run its existing **non-publishing** release qualification before any immutable tag. Verify six-RID package SHA, manifest version, commit provenance and native ABI.
- [ ] Obtain separate explicit permission for final version/tag publication and, later, the [TurboMedia #227](https://github.com/qigao/turbomedia/issues/227) exclusive cutover.


**Documentation HEAD requalification (2026-10-19, `e173e70130f54ad4cd41efe2bc86211dea8adbff`):** [CNet SCTP C11 #39194373002](https://github.com/qigao/salts/actions/runs/39194373002) **SUCCESS 5/5** (Linux/macOS/Windows C11 Wire, source ASan/UBSan, pinned-GmSSL Cookie-MAC); [immutable build provenance #39194372998](https://github.com/qigao/salts/actions/runs/39194372998) **SUCCESS**; [Salts CI #39194372988](https://github.com/qigao/salts/actions/runs/39194372988) **SUCCESS** for selected checks including Android ARM64 Release and iOS ARM64 Release. Formal, package, benchmarks, installed ServiceHost ABI, six-RID ABI identity and SDK publication were **SKIPPED**; ACE candidate was **SKIPPED**. This documentation-only checkpoint does not close M0–M6 blockers, qualify an installed SCTP SDK, or authorize merge/release/cutover. Any subsequent HEAD still requires its own exact-commit validation.

**PR #1281 is a merged private research baseline, not a release approval. No tag, NuGet push, GitHub Release or production cutover from this document.**

## M5 real-thread Owner lease / cancel checkpoint (2026-10-22)

A separate **opt-in** C11 POSIX pthread qualification target now exists:
`CNET_SCTP_TEST_OWNER_THREADS=ON` builds `cnet_sctp_owner_thread_test` and
uses the same private Association/Data sources. The default Windows/macOS
standalone build now also installs the opaque Session target; the Linux source
ASan+UBSan job enables the new test, while a dedicated Ubuntu Clang 18 job
builds the actual target and first-party dependencies with `-fsanitize=thread`,
`-fPIE`, and a TSan-linked PIE. The CI includes an explicit compilation-command
audit of all six relevant first-party C sources; the test is repeated ten times
with nonzero TSan-error exit status.

The test uses **two real pthreads**, caller-provided mutex/condition-variable
serialization and a single transferable Owner. For 256 cycles, ownership
moves across the threads while they exercise init, start, authenticated
synthetic peer-ABORT input, exactly-once ULP reason take versus stop-first
discard, drain and destroy. Non-Owner access is rejected without state
mutation. A separate 64-round test races an external cancellation **request**
against an in-flight Owner entropy callback, verifies callback reentrancy
returns BUSY, then stops/drains/destroys only after the callback returns.

**This is not permission to call the Association concurrently without an
external Owner lease.** It is also **not** a real Provider DSO unload/dlclose,
DTLS Generation barrier, or effective-source TSan qualification of the
third-party GmSSL implementation. Those, together with actual CNet Owner
integration and concurrent cancel/input/timer/close stress, remain open M5
gates. A passing scoped TSan run does not close the full M5 gate or authorize
release, publication, or production cutover.

## Real dynamic Provider DSO callback/drain/dlclose (2026-10-22)

A separate **private test-only** C11 Provider MODULE is now loaded through
`dlopen(RTLD_NOW|RTLD_LOCAL)`, its function table resolved by `dlsym`, and
its executable entropy callback invoked by the actual Owner's Association
`init()` call. Its MAC and verify callbacks are also invoked through the
real DSO entry table (intentionally non-cryptographic test implementations).
The module and test program are only built under the opt-in
`CNET_SCTP_TEST_OWNER_THREADS` gate and are **never installed/exported**
by the research Wire SDK or production Salts SDK.

A second thread posts cancellation intent while the DSO entropy callback is
blocked inside the mapped module. The externally synchronized Generation
lease rejects new callbacks after cancellation; `try_unload()` must refuse
`BUSY` in two separately scheduled states: **(A) in-flight callback** and
**(B) callback has returned but Owner TCB has not been stopped/destroyed**.
Only after the Owner performs `stop -> drained -> destroy`, signals its
Generation barrier, and the in-flight count is zero may the unload thread
clear all borrowed DSO function pointers and call `dlclose(handle)`.
Stale Generation tokens are denied both while the newer DSO is loaded and
after closing; double-`dlclose` is forbidden. The test actually performs
24 successive DSO open/cancel/drain/close generations in one process.

The Linux C11 sanitizer job builds and runs this real DSO test; the Clang 18
TSan job verifies **all eight first-party C sources**, including both the
DSO module and executable, are TSan-instrumented and repeats this test and
the pthread Owner handoff test **10 times each**. The isolated DSO fixture
uses no worker outside caller-created pthreads, no Rust or C++ runtime, and
no implicit production fallback.

**Qualification scope:** this is real *test-module* `dlopen/dlclose` under a
synthetic Provider callback and external Generation barrier. It does **not**
prove a production GmSSL/DTLS or CNet Provider DSO is unload-safe, nor
certify real CNet cancel/input/timer/close concurrent scheduling, TSan
instrumentation of external GmSSL, full RFC interoperability, six-RID
installed ABI, or authorization to merge/release/publish/cut over.
