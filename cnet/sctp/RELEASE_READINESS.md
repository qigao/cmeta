# Release readiness — RFC-first C11 SCTP (#1181)

**Initial preflight: 2026-10-13. Feature-checkpoint update: 2026-10-17. Decision: NO-GO for public or production SCTP.**
This is a release *preflight record*, not permission to merge Draft [PR #1182](https://github.com/qigao/salts/pull/1182), create a tag, publish a package, or replace TurboMedia's usrsctp. All SCTP work remains original C11, with no Rust/C++ SCTP engine, bridge, or runtime fallback.

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
| M1 C11 wire + installed consumer | Strict Chunk framing, CRC32C, INIT TLVs; independent `CNetSctpPrototype::Wire` installed C11 consumer | **PARTIAL** — production opaque session ABI and Salts SDK export absent |
| M2 SCTP core | Owner-driven INIT/COOKIE/T1, HMAC-SM3, entropy/key rotation, bounded DATA/SACK/T3/Fast Recovery and zero-window recovery; SHUTDOWN/T2 with pending DATA and ULP RX drain; atomic bundled DATA/leading SACK receive | **PARTIAL** — complete congestion/PMTU, restart, T5/ABORT exceptions, full RFC wire surface and real Provider Owner integration remain open |
| M3 WebRTC extensions | Private PPID/DCEP OPEN/ACK, ordered/unordered streams, 24 KiB DATA fragmentation and independent CRC-correct post-Cookie tests | **PARTIAL** — PR-SCTP/FORWARD-TSN, stream reset/reuse, I-DATA interleaving, negotiated limits and browser interop remain blocked |
| M4 independent interoperability | No real GmSSL SCTP-over-DTLS + independent browser/libwebrtc peer proof | **BLOCKED** |
| M5 ownership, sanitizers and SDK | Linux/Windows/macOS standalone C11 and Linux ASan/UBSan green; actual GmSSL test with source instrumentation | **PARTIAL** — effective-source TSan, real CNet Owner/DTLS Generation drain, Linux arm64 and Android/iOS **new SCTP** installed SDK remain open |
| M6 review/cutover | No independent protocol review or explicit release/cutover approval | **BLOCKED** |

**Historical baseline evidence (2026-10-13):** [C11 #38358908402](https://github.com/qigao/salts/actions/runs/38358908402) 5/5 SUCCESS; [Salts CI #38358908624](https://github.com/qigao/salts/actions/runs/38358908624) SUCCESS. The latter's existing Android/iOS builds do **not** compile or install the new C11 SCTP engine. A green standalone Wire consumer is not a released SCTP session SDK.

**Current feature-source checkpoint (2026-10-17):** PR #1182 introduced retransmission priority, bounded zero-window probes with peer-silence bounds, RX-slot-aware advertised window and explicit Owner window-update SACK. These are private prototype features, not the released `Salts::CNet` module. Refer to the exact HEAD CI run for acceptance; the earlier M2/M3 absence statements above have been replaced by partial implementation evidence. The following remain unqualified: complete SCTP shutdown/ABORT/T5/restart, complete RFC 9260 and WebRTC extensions, CNet/DTLS Generation leases and source-level TSan, actual independent browser interoperability, and six-RID installed SCTP session consumers.

## Two mutually separate release lanes

### A. Salts.Native 2.3.0 — existing product

Can be prepared **without #1181** only through existing [release epic #1081](https://github.com/qigao/salts/issues/1081), [ABI gate #1018](https://github.com/qigao/salts/issues/1018), and exact approved candidate release workflow. The 2.3.0 tag does not currently exist in the checked repository. Before stable publication, choose an authoritative reviewed source that includes all required once ABI, ACE/CNet changes, satisfy six-RID installed SDK and published-byte downstream/TSan gates, and explicitly obtain release approval. **Do not include the SCTP Draft source in this baseline** or infer its PR CI qualifies it.

Separate known cross-repo qualification: [#1177](https://github.com/qigao/salts/issues/1177) tracks the rc.15 release-SDK TSan async-logging instrumentation visibility issue; diagnose it without suppressions. [#1160](https://github.com/qigao/salts/issues/1160) is another explicitly segregated GmSSL RFC8998 staged release lane, not automatically part of #1181 or 2.3.0.

### B. RFC-first CNet SCTP / TurboMedia DataChannel

**NO-GO** for any public Salts SDK, release tag, or TurboMedia cutover today. Continue on [Draft #1182](https://github.com/qigao/salts/pull/1182) and [TurboMedia #227](https://github.com/qigao/turbomedia/issues/227). When M0–M5 and independent browser/DTLS interoperability pass, review a **new, exact SHA-qualified integration candidate based on the approved Salts ABI graph**; reconcile ancestry and run full first-party sanitizer, six-RID *SCTP* installed consumers and immutable packages. Only M6 written approval can permit a public SCTP session ABI and exclusive usrsctp removal. No dual-backend release and no automatic runtime fallback.

## Release preparation checklist (no release action authorized)

- [ ] Confirm approved Salts 2.3.0 source/ABI baseline under #1081; do not tag `master` or SCTP Draft opportunistically.
- [ ] Resolve rc.15 / Draft #1182 ancestry via a separately reviewed integration plan, **after** feature acceptance. Any new SHA requires new testing and provenance.
- [ ] Complete RFC 9260 association, data plane, reliability, shutdown, restart/tie-tag and exception paths within bounded single Owner.
- [ ] Complete WebRTC DCEP/PPID, PR-SCTP, stream reset, interleaving and independent browser GmSSL/DTLS tests with captured wire evidence.
- [ ] Integrate CNet Owner + DTLS Generation leases, callback terminal once/drain, cancel vs input/timer/close races and full-source TSan.
- [ ] Qualify exact new SCTP installed C11 SDK for Linux x64/arm64, Windows x64, macOS arm64, Android arm64 and iOS arm64. Confirm no Rust/C++ SCTP runtime dependency.
- [ ] Obtain independent implementation/security/license review and maintainer approval for M6; **do not merely tick the gate because CI is green**.
- [ ] For an approved product source, run its existing **non-publishing** release qualification before any immutable tag. Verify six-RID package SHA, manifest version, commit provenance and native ABI.
- [ ] Obtain separate explicit permission for final version/tag publication and, later, the [TurboMedia #227](https://github.com/qigao/turbomedia/issues/227) exclusive cutover.

**Keep #1181 Open. Keep PR #1182 Draft / DO NOT MERGE. No tag, NuGet push, GitHub Release or production source mutation from this document.**
