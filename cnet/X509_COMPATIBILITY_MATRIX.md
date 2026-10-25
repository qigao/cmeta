# CNet X.509 and secure-transport compatibility matrix

This matrix describes only the pinned GmSSL provider and CNet implementation
in this Salts source tree. `Verified` means an executable CNet or installed-SDK
test exercises the stated path. `Unsupported` means CNet deliberately does not
admit it. `Not yet verified` must not be inferred to work from a provider
primitive, OID, or parser alone.

CHttp HTTPS, SaltsNet protocol consumers, and Flowie MQTT/WSS/HTTPS are owned
and tested in their respective repositories. Their results are not CNet
results, and this document does not make claims for them.

## Profile matrix

| Profile | Status | CNet evidence and boundary |
| --- | --- | --- |
| Standard TLS 1.2/1.3, AES-128-GCM, RSA or ECDSA P-256 identity | Verified | Public client/listener tests cover real NativeIO handshake, hostname/trust rejection, ALPN and negotiated metadata. TLS 1.2 and 1.3 remain explicitly bounded by `cnet_tls_state_set_protocol_range()`. |
| P-256 leaf signed by a P-384/SHA-384 issuer | Verified | `cnet_installed_sdk_test` builds and runs the public installed C11 consumer from `tests/package_config/strategy/p384_tls.c`, including version agreement, wrong CA and wrong hostname. |
| RFC 8998 TLS 1.3, SM2/SM3/SM4-GCM | Verified on native desktop CTests | Explicit `CNET_TLS_SECURITY_PROFILE_SHANGMI_TLS13`; provider wire, bounded ring, public NativeIO, mTLS and fail-closed negatives. See `RFC8998_SHANGMI_PROFILE.md`. |
| TLCP 1.1, separate signing/encryption SM2 identities, SM4-GCM/SM3 | Verified on native desktop CTests | Explicit `CNET_TLS_SECURITY_PROFILE_TLCP11`; provider wire, bounded ring, public NativeIO, dual-role and authenticated-record negatives. TLCP mTLS is unsupported and rejected. See `TLCP11_PROFILE.md`. |
| RSA-3072 identity with RSA/SHA-256 PKCS#1 certificate signature | Verified | Deterministic public CNet fixtures authenticate and transfer application data with TLS 1.2 and TLS 1.3; the installed C11 SDK consumer also verifies the public NativeIO path. The independent OpenSSL workflow checks the chain, server purpose, SANs, key match, key size and signature encoding. TLS 1.3 uses RSA-PSS for `CertificateVerify`, independently of the certificate's PKCS#1 signature. |
| RSA-PSS certificate signature chain | Unsupported by pinned GmSSL 3.2.0#13 | The independent OpenSSL workflow verifies the RSASSA-PSS/SHA-256 leaf and chain; CNet then rejects its unsupported signature OID while creating the server context. This is distinct from supported TLS 1.3 RSA-PSS `CertificateVerify`; CNet does not switch providers or retry. |
| SHA-512 certificate signatures | Unsupported by pinned GmSSL 3.2.0#13 | OpenSSL independently validates the P-384→P-256 ECDSA/SHA-512 chain, purpose, SANs and key match. CNet rejects it during certificate verification under both TLS 1.2 and TLS 1.3, with no provider fallback. |
| Other mixed issuer/leaf curves | P-521 issuer unsupported by pinned GmSSL 3.2.0#13 | OpenSSL independently validates the P-521→P-256 ECDSA/SHA-512 chain. CNet rejects it during certificate verification with no provider fallback. The verified supported mixed case remains P-384→P-256 with SHA-384. |
| Ed25519 certificate or handshake signature | Unsupported | It is absent from CNet's admitted signature algorithms; the installed-SDK test verifies fail-closed server-context rejection with no provider fallback. |
| TLCP CBC suite | Unsupported | The TLCP profile admits only `TLS_ECC_SM4_GCM_SM3`. |
| Opportunistic provider/profile switching | Unsupported | Standard TLS, RFC 8998 and TLCP contexts are immutable and never retry as one another or plaintext. |

## Validation matrix

| Case | Standard TLS | RFC 8998 | TLCP 1.1 |
| --- | --- | --- | --- |
| Trusted chain and DNS identity | Verified | Verified | Verified |
| Wrong/untrusted CA | Verified | Verified | Verified |
| Wrong DNS hostname | Verified | Verified | Verified |
| Wrong IP SAN | Verified in CNet TLS tests | Not yet verified separately | Not yet verified separately |
| Malformed server certificate | Verified in CNet TLS tests | Verified | Verified |
| Server private-key mismatch | Verified in CNet TLS tests | Verified | Verified for either TLCP role |
| Signing/encryption role reversal | Not applicable | Not applicable | Verified rejection by KeyUsage and order |
| Truncated/tampered authenticated record | Verified in framing/provider tests | Verified | Verified with no plaintext exposure |
| Client certificate / mTLS | Verified | Verified positive and negative cases | Unsupported; fails with `SALTS_ENOTSUP` |
| Expired leaf | Verified rejection through installed C11 SDK | Not yet verified | Not yet verified |
| Not-yet-valid leaf | Not yet verified | Not yet verified | Not yet verified |
| Tampered certificate signature/DER | Verified rejection through installed C11 SDK | Not yet verified separately | Verified malformed-certificate rejection |
| Invalid CA constraints | Verified rejection through installed C11 SDK | Not yet verified | Not yet verified |
| Unsupported certificate OID | Not yet verified as a dedicated public CNet fixture | Not yet verified | Not yet verified |
| Root → intermediate → leaf full chain | Verified through installed C11 SDK | Not yet verified separately | Not yet verified separately |
| Missing intermediate | Verified rejection through installed C11 SDK | Not yet verified | Not yet verified |

## Platform and provenance boundary

The profile tests are registered for native Windows, Linux and macOS and use
the exact vcpkg-installed `gmssl` executable rather than a tool found on the
host `PATH`. Android and iOS build the same CNet sources, but an on-device
handshake result is not claimed. Installed C11/C++ consumers see only CNet's
public ABI; GmSSL headers and symbols remain private implementation details.

Downstream integration status belongs to:

- CHttp issue `qigao/chttp#248`;
- SaltsNet issue `qigao/salts-net#56`;
- Flowie issue `qigao/flowie#70` and its associated acceptance PR.

Those projects can consume this matrix as a provider/CNet baseline, but must
record their own real HTTPS, WSS, MQTT, S/MIME or other protocol evidence.

