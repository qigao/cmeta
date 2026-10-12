# GmSSL ShangMi private-provider qualification — #1123

## Current integration candidate (not released)

As of the latest Draft #1131 implementation branch, the **GmSSL-only custom
registry pin** in `vcpkg-configuration.json` has been advanced from the
original `3.2.0#4` (shown in the historical baseline below) to **candidate
`3.2.0#10`**, exact registry commit
`8de71e653c378f948b7616185f71605a15ee1b98`.
This is the head of [qigao/vcpkg-cache Draft PR #193](https://github.com/qigao/vcpkg-cache/pull/193),
not a published stable dependency or a merged provider overlay.

The new private GmSSL function `tls_ctx_set_strict_protocol` was added by
[qigao/vcpkg-cache#192](https://github.com/qigao/vcpkg-cache/issues/192).
The Salts provider-only probe now requires it, checks wrong-protocol rejection
without mutating the old offer, requires one supported version on success,
and rejects re-application after configuring cipher/group/signature choices.
The test does NOT modify GmSSL `TLS_CTX` members directly.

Candidate provider validation: [registry](https://github.com/qigao/vcpkg-cache/actions/runs/38195084665),
[port](https://github.com/qigao/vcpkg-cache/actions/runs/38195084765), and
[cache contract](https://github.com/qigao/vcpkg-cache/actions/runs/38195084874)
are SUCCESS. **Salts's exact `3.2.0#10` runtime/CTest CI remains an
independent open gate.** Neither RFC 8998 nor TLCP has passed real CNet
on-wire interop, and no new production transport profile is enabled.

## Evidence scope

This document records a **source-level inventory** for the exact GmSSL vcpkg
registry selected by `qigao/salts`, plus the CTest provider-only executable
`cnet_gmssl_shangmi_provider_test`. **The existence of headers or a passing
standalone provider probe is not equivalent to public Salts crypto API
qualification or a CNet RFC 8998/TLCP handshake.** Do not advertise either
protocol profile on the strength of this document.

Source manifest (at issue opening, 2026-10-12):

- Salts `vcpkg.json`: manifest version `2.3.0`, dependency `gmssl`.
- `vcpkg-configuration.json`: `gmssl` registry `qigao/vcpkg-cache`
  **`1f9d46f66436e3ce74677f393c25e72c008e0fc7`**.
- Registry `ports/gmssl/vcpkg.json`: **GmSSL `3.2.0#4`**.
- Registry `ports/gmssl/portfile.cmake`: upstream Git commit
  **`7c9f02904ef33e59c87b4f16621cc8fd434e7579`**, with the
  overlay CMake adaptations applied after fetching it.
- Registry config forces a **static** GmSSL provider, `ENABLE_TLS=ON`,
  `ENABLE_SECP256R1=ON`, `ENABLE_SHA2=ON`, `ENABLE_AES=ON`;
  `ENABLE_SM9=OFF`, `ENABLE_GHASH=OFF` (the latter disables the
  **standalone GHASH command/test**, not the GHASH primitive used by GCM).
- This pin and its patches must be re-qualified if any reference, SHA, port
  revision, triplet or build option changes.

Exact registry:
https://github.com/qigao/vcpkg-cache/tree/1f9d46f66436e3ce74677f393c25e72c008e0fc7/ports/gmssl

## Layer and readiness matrix

| Capability | Found in pinned upstream source | Provider-only test | Salts public API | CNet negotiated wire |
| --- | --- | --- | --- | --- |
| SM3 one-shot and streaming | `gmssl/sm3.h`: `sm3_init/update/finish` | `abc` independent digest KAT | Not exposed | N/A |
| HMAC-SM3 | `gmssl/sm3.h`: `sm3_hmac_*` | independent vector | Not exposed | N/A |
| HKDF-SM3 | `gmssl/hkdf.h`: `sm3_hkdf_extract/expand` | independently computed PRK/OKM vectors | Not exposed | N/A |
| SM4-GCM | `gmssl/sm4.h`: `sm4_gcm_encrypt/decrypt` | RFC 8998 Appendix A.1 KAT, roundtrip, wrong tag and wrong AAD preserve caller output | Not exposed | N/A |
| SM2 sign and verify | `gmssl/sm2.h`: `sm2_sign_*`, `sm2_verify_*` | fixed independent OpenSSL DER SPKI/signature verify + wrong ID/message/signature; generated-key signing smoke | Not exposed | N/A |
| TLS 1.3 ShangMi | `gmssl/tls.h`: cipher `0x00c6`, curve `41`, signature `0x0708` | `TLS_CTX` strict version, suite, group, signature and wrong-suite reject (configure-only) | **Not exposed** | **NOT TESTED** |
| TLCP 1.1 | `gmssl/tls.h`: `TLS_protocol_tlcp`, TLCP ECC/SM4 suites, TLCP cert/key setup | `TLS_CTX` configure-only | **Not exposed** | **NOT TESTED** |

Provider-only tests compile and link directly against `GmSSL::GmSSL`. This is
**intentionally private test code**, not a change to the public consumer target
contract. The production CNet/Salts targets already link GmSSL privately.

### Important decrypt safety observation

At upstream SHA
`7c9f02904ef33e59c87b4f16621cc8fd434e7579`,
`src/sm4_gcm.c` `sm4_gcm_decrypt()` checks the computed GHASH tag using
`gmssl_secure_memcmp()` and **only then** calls `sm4_ctr32_encrypt()` to
write plaintext. This enables a no-unauthenticated-output wrapper if the
buffer and alias rules are strictly constrained. #1126 must still test failed
output integrity against the **installed and patched** provider, including
overlap, truncated lengths and integer boundaries.

### TLCP separation

Pinned `src/tlcp.c` uses `tls_send_record()` / `tls_recv_record()`
owned by GmSSL `src/tls.c`, and the vcpkg `external-io.cmake` patch adapts
those record I/O primitives to a per-connection `TLS_IO` callback.
This is **not** evidence that TLCP works end-to-end over CNet's owner rings:
dual-cert loading, verification, fragmentation, close and untrusted input
remain unqualified. #1129 is a separate gate.

## Current executable gate

Registered in `cnet/tests/CMakeLists.txt` and included in the
`cnet_tls_test` build dependency closure:

```sh
cmake --preset linux-release-ci -DENABLE_TESTS=ON -DBUILD_TESTS=ON
cmake --build --preset linux-release-ci --target cnet_tls_test
ctest --test-dir build/linux-gcc-release \
  -R '^cnet_gmssl_shangmi_provider_test$' --output-on-failure
```

**Check the actual resolved build directory and preset names** in the CI
runner; the commands above express the intended CMake target/CTest contract,
not a record of an executed local test. Other platforms should use their
repository-defined release presets. The probe fails closed when the pinned
provider misses symbols or returns unexpected behavior.

KAT provenance:
- SM3(`abc`): published SM3 example; verified independently.
- HMAC-SM3(`0b` repeated 20 bytes, `Hi There`): independently computed
  with Python/OpenSSL SM3-backed HMAC.
- HKDF-SM3(salt=`salt`, ikm=`input key material`, info=`context`):
  independently derived with RFC 5869 extract/expand operations using SM3 HMAC.
- SM4-GCM: **RFC 8998 Appendix A.1** ciphertext + tag known-answer vector,
  plus separate provider roundtrip/auth-failure smoke.
- SM2 has a fixed **independent OpenSSL 3.5.5** SPKI/DER signature
  verification vector for message `CNet-ShangMi-SM2-KAT-v1` and ID
  `1234567812345678`. The fixture contains a public key and signature only;
  a generated-key signing smoke separately tests provider signing. Fixed
  deterministic signing with controlled `k` is **not** claimed.

## Open qualification before marking #1123 complete

- [ ] Exact-head CTest compile/link/runtime on Linux, Windows and macOS;
      capture SHA, triplet, provider pin, CI run URL and CTest results.
- [x] Add independent RFC 8998 A.1 SM4-GCM ciphertext/tag vector as a CTest
      assertion (runtime success still awaits CI).
- [x] Add fixed-key external-OpenSSL SM2 signature verification with wrong-ID,
      wrong-message and corrupted-signature negatives (runtime evidence awaits CI).
- [ ] Add SM2 DER boundary/noncanonical signatures, invalid curve/key and
      fixed private-key signing contract qualification.
- [ ] Compare SM4 decrypt no-output-on-failure and alias/size contracts under
      sanitizers, including AES-GCM/TLS regressions.
- [ ] RFC 8998 end-to-end ClientHello/ServerHello cipher, SM2 curve, signature,
      certificate and full record exchange against independently launched
      client/server; wrong CA/name, downgrade and malformed-record negatives.
- [ ] TLCP full handshake with sign and encryption cert/key pairs and positive
      + malformed/wrong-cert interop, with explicit external-I/O/close coverage.
- [ ] Platform installed SDK consumer gate (C11/C++17), no private GmSSL
      include/link dependency or provider DLL leak, and mobile status declared
      separately.

## Boundaries

No fallback to default TLS, to plaintext, to a different cipher/profile or to
Monocypher Secure KCP. The existing `cmeta_crypto.h` ABI and standard CNet
TLS/NativeIO ownership stay unchanged by this provider-only qualification.
#1124 freezes any *new* public ABI after the provider matrix is verified.

Tracking umbrella: https://github.com/qigao/salts/issues/1122
P0 provider test: https://github.com/qigao/salts/issues/1123
Public ABI: https://github.com/qigao/salts/issues/1124

## Strict RFC 8998 identity and version observation

RFC 8998 §3.2.1 distinguishes:
- SM2 CertificateVerify/key-exchange signing identity:
  **`TLSv1.3+GM+Cipher+Suite`**.
- SM2 certificate signature verification identity: the RFC's
  **`1234567812345678`** requirement, with an interoperability caveat that
  CAs may have issued signatures with a different configured identity.
  CNet must not guess/try multiple identities as a hidden fallback; explicit
  trust policy and concrete CA provenance belong in #1128.

The upstream provider exports `TLS13_SM2_ID` and uses it in TLS 1.3
CertificateVerify sign/verify. The provider-only CTest asserts the exact
identifier plus RFC 8998 cipher `0x00c6`, signature `0x0708` and curve
`41`. It tests the exported `tls_ctx_init` protocol/min/max values,
configured suite/group/signature and cross-profile cipher rejection. **The
previous 3.2.0#4 GmSSL header does NOT expose `tls_ctx_set_supported_versions`.**
The provider initializes `supported_versions` with several versions even for
single-protocol contexts; an API to make the advertised/accepted version list
strict must be qualified (or added in the pinned provider overlay) before
RFC 8998 may be advertised as fail-closed. No direct `TLS_CTX` field mutation
is permitted in production CNet; avoid assuming `min/max` alone enforce
on-wire downgrade rejection. These remain configuration gates only.

Independent SM2 fixture provenance (test-only, public data):
```sh
openssl genpkey -algorithm SM2 -out sm2-private.pem
openssl pkey -in sm2-private.pem -pubout -outform DER -out sm2-public.der
printf 'CNet-ShangMi-SM2-KAT-v1' > message.bin
openssl pkeyutl -sign -rawin -inkey sm2-private.pem \
  -in message.bin -out sm2.sig -digest sm3 \
  -pkeyopt distid:1234567812345678
openssl pkeyutl -verify -rawin -pubin -inkey sm2-public.pem \
  -in message.bin -sigfile sm2.sig -digest sm3 \
  -pkeyopt distid:1234567812345678
```
`sm2-public.pem` in the verification command is the PEM public key
exported from the same fixture private key. **The private key is deliberately
not committed**. The test embeds fixed, independently signed public SPKI
and DER signature bytes.

RFC: https://www.rfc-editor.org/rfc/rfc8998#section-3.2.1

## Actual compiler failure and corrected boundary (2026-10-12)

[CI #38194332832](https://github.com/qigao/salts/actions/runs/38194332832)
showed macOS and macOS-Clang build failure in the new probe:
`tls_ctx_set_supported_versions` was undeclared by the pinned public GmSSL
header. Although its implementation exists in the fetched upstream source,
it is not a supported exported provider API. Fixed on the feature branch at
[`f5d081d`](https://github.com/qigao/salts/commit/f5d081d638190970562da08e9eccc6df6265be76)
by removing this call, verifying only the actual exported provider protocol
configuration, and retaining the strict version setter/on-wire proof as an
**open blocker** for #1128. The previous CI did not prove runtime success.

The fixed independent SM2 public SPKI + DER signature fixture was additionally
rechecked with OpenSSL 3.5.5: `pkeyutl -verify -rawin -digest sm3` with the
specified `distid` returned Signature Verified Successfully. The SM3/HMAC-SM3
and HKDF-SM3 KAT outputs were independently recalculated using OpenSSL-backed
Python `hashlib`/`hmac`. These are *reference-vector* checks, not evidence
that the pinned GmSSL installed CTest has passed.
