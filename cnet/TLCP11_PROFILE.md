# TLCP 1.1 profile

## Protocol contract

`CNET_TLS_SECURITY_PROFILE_TLCP11` is an explicit CNet secure-transport
profile. It selects only TLCP 1.1 with `TLS_ECC_SM4_GCM_SM3`, SM2 and SM3.
It does not admit TLCP's CBC suite and never retries as standard TLS, RFC 8998
TLS 1.3, or plaintext. A caller still uses a `tls://` endpoint and selects
TLCP through the client/server TLS configuration; the URI alone never selects
TLCP.

The profile uses the same Owner-local NativeIO progress path and the same
bounded ciphertext rings as standard CNet TLS. GmSSL owns no socket, thread,
queue, cancellation, timeout, or close authority.

## Server identity layout

TLCP requires distinct signing and encryption identities. CNet consumes the
files synchronously when `cnet_tls_server_init()` creates its retained
context:

- `cert_file`: signing leaf certificate, encryption leaf certificate, then
  any issuer certificates, all in PEM order;
- `key_file`: matching signing private key, then matching encryption private
  key, in PEM order;
- `key_password`: the password used for both keys.

CNet requires the first leaf to have `digitalSignature` KeyUsage without
`keyEncipherment`, and the second leaf to have `keyEncipherment` without
`digitalSignature`. The pinned provider then independently verifies that each
private key matches the corresponding certificate. A malformed bundle,
reversed roles, or either key mismatch fails context creation.

Example:

```c
cnet_tls_server_config server_policy = {
    .size = sizeof(cnet_tls_server_config),
    .cert_file = "tlcp-sign-enc-chain.pem",
    .key_file = "tlcp-sign-enc-keys.pem",
    .key_password = password_from_secret_store,
    .security_profile = CNET_TLS_SECURITY_PROFILE_TLCP11
};

cnet_tls_client_config client_policy = {
    .size = sizeof(cnet_tls_client_config),
    .ca_file = "tlcp-root.pem",
    .server_name = "service.example",
    .security_profile = CNET_TLS_SECURITY_PROFILE_TLCP11
};
```

The caller retains ownership of all input strings. Client verification is
mandatory and includes the configured trust store and DNS/IP identity. The
pinned provider did not pass CNet's TLCP client-certificate verification gate,
so TLCP client certificates and `CNET_TLS_CLIENT_AUTH_REQUIRED` fail fast with
`SALTS_ENOTSUP`; they are never silently treated as one-way authentication.

## Negotiation and failures

After a completed connection, `cnet_tls_negotiated_version()` reports
`TLCPv1.1` and `cnet_tls_negotiated_cipher()` reports
`TLS_ECC_SM4_GCM_SM3`. `cnet_tls_state_set_protocol_range()` accepts only the
default/default pair for this profile because TLCP is not a TLS 1.2/1.3 range.

The deterministic qualification creates fresh SM2 roots and dual server
identities with the pinned vcpkg GmSSL tool. It covers:

- an independently launched `gmssl tlcp_server` / `tlcp_client` GCM exchange;
- CNet's bounded provider-ring handshake and bidirectional records;
- the public NativeIO client/listener route;
- wrong CA, wrong DNS identity, standard-TLS and RFC-8998 cross-profile peers;
- malformed certificates, mismatched keys, and reversed certificate roles;
- explicit fail-fast rejection of the unqualified TLCP mTLS mode;
- a truncated record followed by an invalid authentication tag, with no
  plaintext exposure.

The CTests are registered on native Windows, Linux, and macOS. Mobile targets
compile the CNet profile, but no Android/iOS on-device TLCP runtime result is
claimed. No second TLCP implementation is pinned in this repository; the
GmSSL CLI wire test is the available external process oracle.

