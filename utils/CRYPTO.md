# Provider-neutral cryptography contract

Salts::Core exposes a small C ABI in `cmeta_crypto.h`. GmSSL remains a private
implementation dependency. Public headers, imported targets, caller state and
serialized application data never contain GmSSL types or live provider objects.

## Ownership and errors

All one-shot inputs are borrowed only for the call. Output is caller-owned.
New secure APIs commit output only after the operation succeeds; invalid
arguments, range failures and provider failures leave caller output unchanged.

Opaque streaming contexts are created explicitly, own their private provider
state, and must be destroyed by the caller. Destroy wipes the whole context.
After `finish`, update/finish fail until an explicit reset.

The stable error classes for this surface are:

- `SALTS_EINVAL`: invalid pointer/length/lifecycle contract;
- `SALTS_ERANGE`: a valid request exceeds a fixed algorithm bound;
- `SALTS_ENOMEM`: opaque context allocation failed;
- `SALTS_EAUTH`: authentication, signature or contributory-key check failed;
- `SALTS_EIO`: the pinned provider failed after public preconditions passed.

No algorithm fallback is permitted. Existing MD5, SHA-1, DES-CBC and AES-CFB
remain compatibility-only and are not defaults for new secure protocols.

## SM3 family ABI

The first ShangMi public slice is deliberately narrow:

- `cmeta_sm3`: fixed 32-byte one-shot digest;
- `cmeta_sm3_stream_*`: opaque incremental SM3 with reset/finish lifecycle;
- `cmeta_hmac_sm3`: fixed 32-byte HMAC;
- `cmeta_hkdf_sm3`: RFC 5869 extract+expand with explicit IKM, salt and info.

HKDF output is bounded to `255 * 32 = 8160` bytes. A zero-length result is
valid. NULL is accepted only with the corresponding zero length. The API does
not generate salts/nonces, retain key material, select a provider, or expose an
expand-state object. The implementation uses bounded temporary storage so a
provider failure cannot partially publish an OKM.

Known-answer tests include the standard SM3 `abc` and 64-byte vectors, an
independent HMAC-SM3 vector, and an independently computed HKDF-SM3 vector.
Native CI also builds and runs independent installed-SDK C11 and C++17
consumers through `Salts::Core` only.

## Sequencing

This ABI freeze is the dependency for the remaining explicit additions:

1. SM4-GCM: fixed 128-bit key, 96-bit nonce, 128-bit tag; caller owns nonce
   uniqueness; authentication failure never publishes plaintext.
2. SM2 sign/verify: explicit caller identity, canonical key/signature encoding,
   qualified randomness and stable invalid-signature semantics.
3. CNet transport profiles consume these primitives only through reviewed
   profile-specific policy. RFC 8998 TLS and optional TLCP remain separate
   protocol gates; this crypto API never opportunistically selects either.

No dynamic crypto-provider registry, PKCS#11/SDF adapter, raw provider-state
persistence, or compatibility alias is introduced by this contract.

## Standard elliptic-curve slice

Core exposes three provider-neutral byte APIs needed by common protocols:

- ECDSA P-256 with SHA-256. Private keys are fixed-width big-endian scalars,
  public keys are 65-byte uncompressed SEC1 points, and signatures are strict
  X9.62 DER. GmSSL owns scalar multiplication, nonce generation and signing.
- RFC 8032 pure Ed25519. The private input is a 32-byte seed, the public key is
  32 bytes, and signatures are 64 bytes. The implementation uses Monocypher's
  optional Ed25519 4.0.3 source, including its signature timing-leak fix.
- RFC 7748 X25519. Inputs and results are 32 bytes. The caller must feed the raw
  shared secret to a protocol KDF. All-zero results are rejected with
  `SALTS_EAUTH` and are never published.

ASN.1 serialization remains in `Salts::ASN1`, not Core. `asn1_crypto.h`
provides strict ECDSA-Sig-Value DER and RFC 8410 Ed25519/X25519 SPKI and PKCS#8
codecs. RFC 8410 AlgorithmIdentifier parameters must be absent. Decoders return
borrowed views into caller-owned DER; encoders stage and publish only complete
results. This keeps format policy reusable without introducing a Core wrapper
or exposing a provider ASN.1 type.

GmSSL 3.2 is retained for P-256 because it is already the pinned Core provider.
Its build has no Ed25519 or X25519 API, so those operations use the existing
vendored Monocypher family instead of adding OpenSSL, a provider registry or a
second package dependency. CNet and Core compile independent hidden namespaces;
no generic `crypto_*` vendor symbol is exported from either DSO. Updating or
removing this slice is therefore reversible at the private target boundary and
does not change serialized keys or the public provider-neutral ABI.

Required qualification covers RFC 8032 and RFC 7748 known-answer vectors,
ECDSA derive/sign/verify and tampering, DER canonicality and width limits,
all-zero X25519 rejection, unchanged outputs on failure, C/C++ headers, private
symbol leakage, installed-SDK consumers and sanitizer builds.

## ABI v1 contract for SM4-GCM and SM2

These additions intentionally use byte buffers and fixed-width constants
rather than GmSSL objects or provider-specific key types. Implementations must
pass their native, sanitizer and installed-SDK qualification gates before the
symbols ship or CNet consumes them.

### SM4-GCM

Constants:

~~~c
#define SALTS_SM4_KEY_BYTES 16u
#define SALTS_SM4_GCM_NONCE_BYTES 12u
#define SALTS_SM4_GCM_TAG_BYTES 16u
#define SALTS_SM4_GCM_MAX_AAD_BYTES (16u * 1024u * 1024u)
#define SALTS_SM4_GCM_MAX_MESSAGE_BYTES ((UINT64_C(1) << 36) - UINT64_C(32))
~~~

Signatures:

~~~c
SALTS_C_API int cmeta_sm4_gcm_seal(
    const uint8_t key[SALTS_SM4_KEY_BYTES],
    const uint8_t nonce[SALTS_SM4_GCM_NONCE_BYTES],
    const void *aad, size_t aad_size,
    const void *plaintext, size_t plaintext_size,
    void *ciphertext, size_t ciphertext_capacity,
    size_t *out_ciphertext_size,
    uint8_t tag[SALTS_SM4_GCM_TAG_BYTES]);

SALTS_C_API int cmeta_sm4_gcm_open(
    const uint8_t key[SALTS_SM4_KEY_BYTES],
    const uint8_t nonce[SALTS_SM4_GCM_NONCE_BYTES],
    const void *aad, size_t aad_size,
    const void *ciphertext, size_t ciphertext_size,
    const uint8_t tag[SALTS_SM4_GCM_TAG_BYTES],
    void *plaintext, size_t plaintext_capacity,
    size_t *out_plaintext_size);
~~~

Contract:
- Key, nonce and tag lengths are fixed by the ABI; no variable tag length,
  implicit nonce generation, provider fallback, or automatic key selection.
- Caller owns nonce uniqueness: a key/nonce pair MUST NOT be reused.
  This API does not maintain process-global counters or persist nonce state.
- AAD and payload pointers are borrowed; NULL is valid only with size zero.
  AAD is bounded to SALTS_SM4_GCM_MAX_AAD_BYTES and payload is bounded to
  SALTS_SM4_GCM_MAX_MESSAGE_BYTES; validate lengths and integer arithmetic
  before calling the provider.
- Ciphertext and plaintext lengths are equal. out_*_size is required. On
  success it is set to the exact payload length. If capacity is too small,
  return SALTS_ERANGE, set the required payload length, and leave payload
  and tag buffers unchanged. On other errors, do not modify the size output.
- Exact in-place payload operation (plaintext == ciphertext or
  ciphertext == plaintext) is permitted. Any partial payload overlap, or
  overlap between the payload output and key, nonce, AAD, tag or size output,
  is invalid and returns SALTS_EINVAL.
- open MUST stage unauthenticated plaintext privately and commit it only
  after tag verification succeeds. A bad tag returns SALTS_EAUTH and leaves
  the caller's plaintext and size outputs unchanged. If private staging
  allocation fails, return SALTS_ENOMEM with caller output unchanged.
- SALTS_EINVAL means malformed pointers/lengths/overlap; SALTS_ERANGE means a
  valid request exceeds the fixed bound or output capacity; SALTS_ENOMEM means
  private staging allocation failed; SALTS_EIO means an unexpected provider
  failure; SALTS_EAUTH means authentication failed. Do not translate a tag
  failure into a generic provider error.

### SM2 sign/verify

Constants and signatures:

~~~c
#define SALTS_SM2_MAX_KEY_DER_BYTES (64u * 1024u)
#define SALTS_SM2_MAX_USER_ID_BYTES 8191u
#define SALTS_SM2_SIGNATURE_DER_MAX_BYTES 72u

SALTS_C_API int cmeta_sm2_sign(
    const void *private_key_pkcs8_der, size_t private_key_der_size,
    const void *user_id, size_t user_id_size,
    const void *message, size_t message_size,
    uint8_t *signature_der, size_t signature_capacity,
    size_t *out_signature_size);

SALTS_C_API int cmeta_sm2_verify(
    const void *public_key_spki_der, size_t public_key_der_size,
    const void *user_id, size_t user_id_size,
    const void *message, size_t message_size,
    const void *signature_der, size_t signature_der_size);
~~~

Contract:
- Private keys are DER-encoded PKCS#8 PrivateKeyInfo containing an SM2 key.
  Public keys are DER-encoded X.509 SubjectPublicKeyInfo containing an SM2
  public key. Reject other key algorithms, malformed DER, trailing bytes and
  invalid/off-curve public points. PEM and raw scalar/point encodings are not
  part of ABI v1.
- user_id is mandatory, non-empty, borrowed, and at most 8191 bytes (the
  largest whole-byte identity representable by SM2's 16-bit ENTL bit count).
  There is no implicit default identity, process-global identity, or hidden
  identity substitution. Sign and verify use the exact same supplied bytes.
- Signature output/input is strict canonical ASN.1 DER SEQUENCE(INTEGER r,
  INTEGER s), with both values in the valid SM2 group range and no trailing
  bytes. Validation uses the public Salts ASN.1 API directly; no crypto-local
  parser, wrapper or compatibility layer is introduced. The maximum DER
  encoding is 72 bytes. Signing requires a non-NULL
  out_signature_size and a capacity of at least
  SALTS_SM2_SIGNATURE_DER_MAX_BYTES; on insufficient capacity return
  SALTS_ERANGE, set the required capacity to 72, and do not sign or modify
  signature bytes. On success, set the actual DER length.
- Signing uses only the pinned provider's cryptographically secure randomness;
  no caller-supplied nonce, deterministic fallback, or weak random source is
  accepted. Private key bytes and all intermediate key/signature state are
  cleared on every exit path.
- cmeta_sm2_verify returns SALTS_OK only for a valid signature over the exact
  message and explicit identity. A malformed or invalid signature returns
  SALTS_EAUTH; malformed arguments or key DER return SALTS_EINVAL; unexpected
  provider failure returns SALTS_EIO. Invalid signatures are a normal negative
  result, not a provider fallback condition.
- The error taxonomy adds the stable code SALTS_EAUTH = -4054. Existing
  numeric values remain unchanged. The shared error table and its metadata
  test must land with the implementation before either API ships.

### Cross-API ownership and failure rules

- Inputs are immutable borrowed byte ranges. Output buffers belong to callers.
  Do not retain caller pointers after return; do not serialize provider state.
- Validate NULL/zero semantics, maximum lengths, capacity, range arithmetic,
  and forbidden overlap before provider calls. Payload and signature bytes are
  committed only on success. Output-size metadata changes only on success or
  the explicitly documented SALTS_ERANGE capacity response.
- No public header or installed target may include gmssl/*, require
  GmSSL::GmSSL, or expose provider structs. No C++ runtime is required by
  this C11 ABI; C++17 consumers compile against the same C declarations.
- Required negative tests: null/nonzero pairs, zero-length input, exact and
  partial overlap, undersized capacity, max/max+1 bounds, malformed/trailing
  DER, wrong SM2 identity, wrong key algorithm, invalid/off-curve public key,
  bad signature, bad GCM tag, tampered AAD/ciphertext, allocation failure,
  output-unchanged assertions and ASan/UBSan.
- CNet profile policy is a separate layer. RFC 8998 TLS 1.3 ShangMi and TLCP
  must remain distinct explicit profiles; neither primitive API selects a
  protocol, cipher suite, certificate identity or fallback policy.
