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
- MAC is injected through a provider interface. The baseline Association test continues to use a **deliberately non-cryptographic** fixture, never valid for release. An independent optional **pure C11 GmSSL HMAC-SM3** test now exercises real provider-backed cookie signing/verification, with a domain-separated MAC, 32-byte owner-supplied key, GmSSL Linux RNG test keys, OpenSSL-derived independent known vector, tamper/key/generation/retire refusals and first-party ASan/UBSan. **The GmSSL adapter is private/test-only and NOT installed.** Production key lifecycle, safe overlapped rotation, unpredictable SCTP tags/TSNs and DTLS/browser authentication/interop remain unresolved.

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

The prototype currently handles only the simplest 1-chunk association startup profile. No complete RFC 9260 parameter/unknown-chunk action support, simultaneous open, restart/tie-tag behavior, secure cookie-key lifecycle, AUTH/ASCONF, DATA, flow/congestion control, TSN/SACK, PR-SCTP, interleaving, stream reset, shutdown or full browser compatibility is implemented. All gaps are tracked in [RFC_MATRIX.md](RFC_MATRIX.md). A successful self-to-self handshake with a test-only MAC is **not** independent interoperability and is not a security pass.

**Draft / DO NOT MERGE:** [PR #1182](https://github.com/qigao/salts/pull/1182). Preserve current production CNet, TurboMedia/usrsctp and published SDK unchanged.
