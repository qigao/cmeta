# RFC 8998 ShangMi TLS 1.3 profile — development contract

> **Draft only.** This profile lives on stacked Salts PR #1132 / #1133,
> depends on private GmSSL overlay PR #193 and qualification PR #1131, and
> has not been merged or published as a stable Salts.Native SDK.

## Scope and protocol policy

`CNET_TLS_SECURITY_PROFILE_SHANGMI_TLS13` is **explicitly opt-in**. It
selects only **TLS 1.3 + TLS_SM4_GCM_SM3 (0x00C6) + curveSM2 (41) +
sm2sig_sm3 (0x0708)**. Neither TLS 1.2, traditional AES/P-256 TLS,
`TLS_SM4_CCM_SM3`, TLCP 1.1, nor plaintext is an authorized fallback. The
cipher restriction is an intentional subset of RFC 8998, not a claim that
the RFC defines only GCM. TLCP is a *different* protocol, tracked by #1129.

`CNET_TLS_SECURITY_PROFILE_STANDARD = 0` preserves the independently verified
standard TLS 1.2–1.3 policy. Selecting the RFC 8998 profile does not change
the behavior of other connections; failure of one RFC 8998 handshake is
terminal. CNet never retries it with STANDARD or with a different transport.

The TLS provider remains a **private static** GmSSL implementation, pinned
through `vcpkg-configuration.json` in the stacked feature branches.
Public `<cnet/cnet.h>` exposes a versioned C configuration enum, not
`TLS_CTX`, GmSSL certificate objects, or direct GmSSL link requirements.

## Application admission

The public TLS configuration is consumed synchronously. The application
provides an explicit SM2 trust store and verified DNS identity; for servers,
the application supplies a matching SM2 server certificate and private key.
The following abbreviated C11 configuration snippets show the security
policy, not a complete socket lifecycle:

```c
cnet_tls_client_config client_policy = {
    .size = sizeof(cnet_tls_client_config),
    .ca_file = "sm2-root-ca.pem",
    .server_name = "service.example",
    .security_profile = CNET_TLS_SECURITY_PROFILE_SHANGMI_TLS13
};
cnet_tls_client client_tls = {0};
/* cnet_tls_client_init() copies/retains the validated policy. */
int status = cnet_tls_client_init(&client_tls, &client_policy);
```

```c
cnet_tls_server_config server_policy = {
    .size = sizeof(cnet_tls_server_config),
    .cert_file = "sm2-server-chain.pem",
    .key_file = "sm2-server-key.pem",
    .key_password = "supplied-securely-by-caller",
    .ca_file = "sm2-client-trust.pem",
    .client_auth = CNET_TLS_CLIENT_AUTH_REQUIRED,
    .security_profile = CNET_TLS_SECURITY_PROFILE_SHANGMI_TLS13
};
cnet_tls_server server_tls = {0};
int status = cnet_tls_server_init(&server_tls, &server_policy);
```

The client must bind the trusted CA, DNS/SAN identity, expected profile
and connection authority **before** calling `cnet_connect()` or
`cnet_start_tls()`. The server supplies the reusable `cnet_tls_server`
at its existing TLS accept admission. The examples intentionally omit event
loop/error handling; inspect existing CNet tests for complete Owner and
NativeIO lifetimes. Private key passwords must not be hardcoded in real
applications.

The provider's exact-version operation
`tls_ctx_set_strict_protocol(TLS_protocol_tls13)` executes *before* cipher,
group, signature, trust and certificate configuration. It only exists at the
private-provider boundary. A mismatch is rejected before any application
data is exposed. A selected RFC 8998 connection cannot be switched into the
standard TLS range after admission.

## Peer authentication and failure semantics

* Client certificate-chain verification, the selected server name and
  DNS subjectAltName remain mandatory; no insecure-verify toggle exists.
* An explicitly configured `CNET_TLS_CLIENT_AUTH_REQUIRED` server validates
  the client certificate, its issuer trust, and `clientAuth` purpose before
  admitting an authenticated server session. A client that has sent TLS 1.3
  Finished is **not** evidence that server-side mTLS authorization succeeded.
* The private provider uses the RFC 8998 SM2 identity
  `TLSv1.3+GM+Cipher+Suite` for TLS CertificateVerify. Verification of
  CA-issued SM2 certificate signatures involves the distinct certificate
  signing identity (normally `1234567812345678`). Configured trust must be
  explicit, not a hidden attempt with multiple identities.
* ALPN remains subject to peer negotiation and application policy. A
  successfully negotiated cipher does **not** imply that the requested
  application protocol or peer identity was accepted.
* Failed certificate, selected profile, record-authentication or handshake
  checks end the connection; CNet does not resume cleartext or re-negotiate
  weaker suites. NativeIO remains the sole socket/owner and keeps bounded
  ciphertext buffers, progress and close/cancel ownership.

## Exact-head qualification evidence

The most recent completed full-platform verification prior to further
record-tamper work is Salts SHA
[`9383e073`](https://github.com/qigao/salts/commit/9383e07302ea35a61bf3851d76e3d49a74ee6e3d):
[CI #38203877467](https://github.com/qigao/salts/actions/runs/38203877467)
**SUCCESS**. It includes real pinned-GmSSL RFC 8998 loopback and CNet
NativeIO/Owner tests on Windows IOCP, macOS Kqueue, and Linux epoll, plus
SM2 client-auth and negative verification, and installed C11/C++17 SDK
consumers. Mobile ARM64 was build-qualified, **not** claimed as device runtime
handshake validation.

The more recent encrypted-record tamper test at
[`8826792`](https://github.com/qigao/salts/commit/8826792de06f8c07dc2068d074cd87cbde4875d7)
changes an authenticated SM4-GCM record after successful bidirectional
exchange and demands `SALTS_EPROTO`, zero plaintext output length and no
changes to the caller buffer. It is a CNet bounded-ring crypto-integrity
qualification, not arbitrary attacker packet injection on a real network.
Its [CI #38205199140](https://github.com/qigao/salts/actions/runs/38205199140)
must be evaluated separately.

## Open gates — no stable release

- [#1134](https://github.com/qigao/salts/issues/1134) independently
  qualified Tongsuo strict RFC 8998 interoperability, in **both** CNet peer
  directions. Tongsuo's documented default may allow non-SM2 signatures in
  TLS 1.3; strict mode must be explicitly enabled and measured.
- Malformed/expired certificates, wrong-purpose server leaf, corrupted
  real TCP records, exact failure-stage evidence and repeated cancellation
  or close races need separate negative acceptance.
- Published SDK candidate, version pin, ABI and dependency graph must be
  independently qualified against the exact reviewed HEAD. Do not infer a
  release from a successful Draft PR build.

References: [RFC 8998](https://www.rfc-editor.org/rfc/rfc8998.html),
[Tongsuo TLS 1.3 ShangMi guide](https://www.tongsuo.net/docs/features/TLS-1-3/),
[#1128](https://github.com/qigao/salts/issues/1128),
[#1130](https://github.com/qigao/salts/issues/1130), and stacked PRs
[#1131](https://github.com/qigao/salts/pull/1131),
[#1132](https://github.com/qigao/salts/pull/1132),
[#1133](https://github.com/qigao/salts/pull/1133).
