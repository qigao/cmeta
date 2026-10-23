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
