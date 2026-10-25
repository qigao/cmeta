# CNet / GmSSL 3.2.0#14 TLS 1.3 CCS + HRR qualification

Status: **Draft / DO NOT MERGE / no production SDK cutover**.

Tracking:
- [Provider PR #209](https://github.com/qigao/vcpkg-cache/pull/209)
- [Salts #1317](https://github.com/qigao/salts/issues/1317)
- [MyGames M4 #5](https://github.com/qigao/mygames/issues/5)
- [MyGames PR #1](https://github.com/qigao/mygames/pull/1)

This branch changes only the **gmssl** entry of `vcpkg-configuration.json`
to the exact unmerged registry commit
`59693dfdb4e6830df838edf9da109f855fd2ea24`,
which stages GmSSL 3.2.0 port revision **14**. All other custom
registry entries and the Microsoft vcpkg scripts baseline remain unchanged.

## Verified at source provider layer, not yet downstream

The vcpkg-cache source and installed-provider CI qualifies a bounded
RFC 8446 D.4 unencrypted dummy TLS 1.3 ChangeCipherSpec, including
HRR second ClientHello. It also checks RFC 8446 4.1.2 padding
add/remove/resize on retry with zero-filled RFC 7685 payloads.
The source profile requires an independently authenticated
OpenSSL TLS1.3/P-256/X25519-first client, real verified ECDSA P-256
CA/server/client certs and a live mTLS data echo. The provider C11
contract verifies malformed, duplicate and partial CCS refusal.

**This changes GmSSL's public TLS_CONNECT size** by adding a private
connection-owned CCS admission count. It does NOT change Salts CNet's
published C ABI or its production TLS policy. Do not substitute the
port14 provider .a file into already compiled Salts/CNet/CHttp SDKs:
they embed or depend on the old TLS_CONNECT layout.

## Exact source qualification gates

- [ ] Rebuild Salts::CNet and the entire selected installed SDK from this
  branch's exact registry revision, headers and static GmSSL library.
- [ ] Run existing `cnet_gmssl_contract_test`, which checks
  `sizeof(TLS_CTX) == tls_ctx_sizeof()` and
  `sizeof(TLS_CONNECT) == tls_connect_sizeof()`, before invoking any
  real provider callback.
- [ ] Run `cnet_tls_test`, `cnet_tls_handshake_framing_test`,
  `cnet_tls_signature_test`, public provider/close/ALPN tests,
  installed C11 SDK consumer, and Linux ASan/UBSan/TSan where
  applicable. Keep TLS1.2, TLCP and RFC8998 tests enabled.
- [ ] Real independent OpenSSL TLS1.3 default ClientHello, including
  X25519-first HRR and dummy CCS, reaches the CNet server under
  required mTLS and completes application data; negative unknown CA,
  missing/revoked client certificate, forbidden SAN/EKU, invalid CCS,
  repeated CCS, invalid HRR and failed shutdown remain fail-closed.
- [ ] Qualify Linux/Windows/macOS and supported cross-compiled
  Android/iOS SDK dependency closures separately; mere package build
  success does not establish network interoperability.
- [ ] Rebuild CHttp from the qualified Salts/CNet SDK and then run
  MyGames PR #1's real end-to-end authenticated recovery GET tests.
- [ ] Do **not** mark MyGames recovery production-ready or merged
  while Flowie #22 delayed inbound ACK or durable external consumer
  replay supervision remains unqualified.

## Outstanding independent issue

TLS1.2 legacy secure renegotiation compatibility observed with
OpenSSL remains separately tracked in Salts #1317.
The TLS1.3 CCS/HRR change must never silently disable CA verification,
fall back to plaintext or grant non-authenticated read authority.

The branch and its dependency pin are a qualification candidate only.
