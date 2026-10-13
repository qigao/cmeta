# Experimental CNet SCTP — pure C11

Independent RFC-first protocol workstream for [Salts #1181](https://github.com/qigao/salts/issues/1181) and upstream prerequisite for [TurboMedia #227](https://github.com/qigao/turbomedia/issues/227).

**Research only. This is not a complete SCTP engine, not authenticated production transport and not WebRTC-interoperable yet.** No Rust/C++ protocol source, FFI or runtime is involved. No production CNet or TurboMedia target uses these objects.

## Isolation and build

This directory is a self-contained C11 CMake project, not a subdirectory of the production Salts CMake graph. Its only installed/exported test package target is `CNetSctpPrototype::Wire`. The new M2 `cnet_sctp_assoc_proto` target, `src/sctp_assoc_internal.h` and its tests are **private** and NOT installed or included in `SaltsTargets`.

```sh
cmake -S cnet/sctp -B build-cnet-sctp -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build-cnet-sctp --config Release
ctest --test-dir build-cnet-sctp -C Release --output-on-failure
cmake --install build-cnet-sctp --config Release --prefix "$PWD/sdk-cnet-sctp"
cmake -S cnet/sctp/tests/installed -B build-cnet-sctp-consumer \
  -DCMAKE_PREFIX_PATH="$PWD/sdk-cnet-sctp"
cmake --build build-cnet-sctp-consumer --config Release
ctest --test-dir build-cnet-sctp-consumer -C Release --output-on-failure
```

## Implemented slices

- **M1 wire:** sync, bounded, allocation-free RFC 9260 common header / Chunk framing, Castagnoli CRC32C, basic INIT validation and full packet validation before borrowed views. Borrowed views are valid only while the caller retains packet bytes. Association semantics, unknown chunk reporting and extension negotiation are not implemented by this decoder.
- **M2 research handshake:** four-message INIT / INIT ACK / COOKIE ECHO / COOKIE ACK state transitions; explicit caller-provided monotonic clock, bounded T1-init/T1-cookie retransmit and timeout, capacity-before-state-change checks, fixed-size packet storage, one Owner verifier, stop/drain/destroy. Closed-side INIT ACK remains stateless until cookie validation; late duplicate COOKIE ECHO re-ACK does not re-open. Signed cookie claims include SCTP ports, tags, initial TSNs, stream/window parameters, creation time and a caller-supplied **opaque non-pointer DTLS peer-generation binding**.
- **M2 secure initialization:** the Association no longer takes caller-defined Verification Tag / Initial TSN values. A **required** C11 entropy callback generates 8 bytes under the Owner; missing, failed, zero-tag or reentrant initialization is explicitly rejected. Deterministic entropy in baseline unit tests is a **non-cryptographic fixture only**. The optional Linux GmSSL job invokes the real OS-backed `rand_bytes` C API for Tag/TSN generation; complete cross-platform provider/Owner integration and uniqueness across peer generations are still OPEN.
- **M2 HMAC and bounded rotation:** a required MAC signing and separate MAC verification callback allow a *private, test-only* two-key ring (current plus one prior epoch). GmSSL SM3-HMAC uses protocol-version domain separation and 32-byte owner-controlled secrets; both candidates are checked without early success, third-generation rotation fails until explicit prior-key retirement, and retired key bytes are wiped. Tests exercise actual in-flight Cookie completion across rotation, forgery/wrong-key/DTLS-Generation rejection and key retirement. These pass only the isolated crypto boundary: no auto-rotation/expiry, no actual CNet ComponentPlugin lease/drain barrier and no browser interoperability. **GmSSL and Association are not exported in the installed Wire SDK.**

- **M2 INIT/INIT ACK parameter reporting:** a separate allocation-free private C11 parser validates TLV lengths, padding, unique/mandatory State Cookie and a 128-TLV ceiling before Cookie signing or Association transitions. RFC 9260 unknown-parameter actions **00 stop, 01 stop+report, 10 skip and 11 skip+report** are implemented within a fixed **8-report / 512-byte** limit; excess reports fail closed without truncation. On INIT, 01/11 reports are copied into Type-8 *Unrecognized Parameter* fields of INIT ACK; on INIT ACK, they generate ERROR cause 8 bundled with COOKIE ECHO. Incoming bundled ERROR is strictly checked before admitting COOKIE ECHO; diagnostics are not yet surfaced to the ULP. All reporting is synchronous, with no retained views, heap allocation or asynchronous delivery.
- **M2 Host Name ABORT:** valid INIT or INIT ACK containing deprecated Host Name Address causes a 16-byte ABORT with the correct peer Initiate Tag and T-bit=0, without optional cause. A rejected 15-byte output capacity does not sign or transition the Association. INIT ACK ABORT seals the local handshake. Malformed mandatory fields and other known unsupported IPv4/IPv6, ECN, Cookie Preservative and Supported Address Types remain rejected in this **single-path experimental profile**; full ABORT causes, restart/tie-tag state, unknown chunk reports and browser interoperability remain open. End-to-end CRC-correct INIT/COOKIE/ERROR/ABORT tests and 10,000 deterministic TLV mutations exercise the boundary.

Every protocol entry point is synchronous and requires caller-provided Owner admission. No worker, independent UDP endpoint, process-global timer, retained external packet view, automatic application retry or raw runtime-object persistence was added.

## Optional real GmSSL cookie-MAC qualification

The regular C11 package/test build has **no GmSSL dependency**. The gated
`-DCNET_SCTP_TEST_GMSSL=ON` option builds only the private test target
`cnet_sctp_gmssl_mac_test`, using `find_package(GmSSL CONFIG REQUIRED)`;
it never exports the adapter into `CNetSctpPrototype::Wire`.

GitHub Actions [CNet SCTP C11 workflow](https://github.com/qigao/salts/actions/workflows/cnet-sctp-c11.yml)
pins `setup-vcpkg-cache` to SHA `d4afc8cd2f5ed3585c1e26469e379a087ce9ecf3`,
the GmSSL registry to `1f9d46f66436e3ce74677f393c25e72c008e0fc7`,
and the vcpkg baseline to `b1b19307e2d2ec1eefbdb7ea069de7d4bcd31f01`.
The observed Linux binary was `gmssl:x64-linux@3.2.0#9` (Apache-2.0).
It runs source-instrumented ASan/UBSan tests separately from a **clean Release**
installed C11 consumer and checks that GmSSL/Association internals do not ship.
A real MAC test is not a certification of the full association or its key/owner
lifetime under concurrent cancellation.

## Gaps that block production

The prototype handles a basic handshake with either one chunk or the narrowly validated COOKIE ECHO + ERROR bundle. It does **not** provide complete RFC 9260 parameter/unknown-chunk action support, simultaneous open, restart/tie-tag behavior, production cookie-key lifetime/rotation admission gates, AUTH/ASCONF, DATA, flow/congestion control, TSN/SACK, PR-SCTP, interleaving, stream reset, shutdown or browser compatibility. All gaps are tracked in [RFC_MATRIX.md](RFC_MATRIX.md). A successful self-to-self handshake with a test-only MAC is **not** independent interoperability and is not a security pass.

**Draft / DO NOT MERGE:** [PR #1182](https://github.com/qigao/salts/pull/1182). Preserve current production CNet, TurboMedia/usrsctp and published SDK unchanged.
