# GmSSL ShangMi private-provider qualification — #1123

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
| SM2 sign and verify | `gmssl/sm2.h`: `sm2_sign_*`, `sm2_verify_*` | generated-key sign/verify + wrong user ID | Not exposed | N/A |
| TLS 1.3 ShangMi | `gmssl/tls.h`: cipher `0x00c6`, curve `41`, signature `0x0708` | `TLS_CTX` configure-only | **Not exposed** | **NOT TESTED** |
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
- SM2 is a generated-key/ID semantic smoke, **not** yet a fixed-key signature
  KAT.

## Open qualification before marking #1123 complete

- [ ] Exact-head CTest compile/link/runtime on Linux, Windows and macOS;
      capture SHA, triplet, provider pin, CI run URL and CTest results.
- [x] Add independent RFC 8998 A.1 SM4-GCM ciphertext/tag vector as a CTest
      assertion (runtime success still awaits CI).
- [ ] Add fixed-key SM2 signing / verification tests (including DER
      lengths/canonicality).
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
