#ifndef SALTS_CRYPTO_H
#define SALTS_CRYPTO_H

#include "cmeta_api.h"
#include "cmeta_error.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SALTS_MD5_DIGEST_BYTES 16u
#define SALTS_SHA1_DIGEST_BYTES 20u
#define SALTS_SHA256_DIGEST_BYTES 32u
#define SALTS_SM3_DIGEST_BYTES 32u
#define SALTS_HKDF_SM3_MAX_OUTPUT_BYTES (255u * SALTS_SM3_DIGEST_BYTES)
#define SALTS_AES128_KEY_BYTES 16u
#define SALTS_AES_BLOCK_BYTES 16u
#define SALTS_DES_KEY_BYTES 8u
#define SALTS_DES_BLOCK_BYTES 8u

/** Opaque provider-neutral incremental SHA-256 context. */
typedef struct cmeta_sha256_stream cmeta_sha256_stream;

/** Opaque provider-neutral incremental SM3 context. */
typedef struct cmeta_sm3_stream cmeta_sm3_stream;

/**
 * Compute MD5 over one contiguous byte range.
 *
 * MD5 is exposed only for legacy protocol compatibility. New designs must not
 * use MD5 for integrity, signatures, password hashing, or collision-sensitive
 * identifiers.
 *
 * @param data Input bytes; NULL is allowed only when size is zero.
 * @param size Input byte count.
 * @param out Caller-owned 16-byte digest buffer.
 * @return SALTS_OK, SALTS_EINVAL, or SALTS_EIO.
 */
SALTS_C_API int cmeta_md5(const void *data, size_t size,
                          uint8_t out[SALTS_MD5_DIGEST_BYTES]);

/**
 * Compute SHA-1 over one contiguous byte range.
 *
 * SHA-1 is exposed only for legacy protocol compatibility. New designs should
 * use SHA-256 or stronger algorithms.
 *
 * @param data Input bytes; NULL is allowed only when size is zero.
 * @param size Input byte count.
 * @param out Caller-owned 20-byte digest buffer.
 * @return SALTS_OK or SALTS_EINVAL.
 */
SALTS_C_API int cmeta_sha1(const void *data, size_t size,
                           uint8_t out[SALTS_SHA1_DIGEST_BYTES]);

/**
 * Compute SHA-256 over one contiguous byte range.
 *
 * @param data Input bytes; NULL is allowed only when size is zero.
 * @param size Input byte count.
 * @param out Caller-owned 32-byte digest buffer.
 * @return SALTS_OK or SALTS_EINVAL.
 */
SALTS_C_API int cmeta_sha256(const void *data, size_t size,
                             uint8_t out[SALTS_SHA256_DIGEST_BYTES]);

/**
 * Create an incremental SHA-256 context.
 *
 * The returned context owns provider state privately; no provider type crosses
 * the public ABI. Destroy the context with cmeta_sha256_stream_destroy().
 */
SALTS_C_API int cmeta_sha256_stream_create(cmeta_sha256_stream **out_stream);

/** Reset an incremental SHA-256 context to the empty-message state. */
SALTS_C_API int cmeta_sha256_stream_reset(cmeta_sha256_stream *stream);

/**
 * Add one byte range to an incremental SHA-256 context.
 * NULL data is accepted only when size is zero.
 */
SALTS_C_API int cmeta_sha256_stream_update(cmeta_sha256_stream *stream,
                                           const void *data, size_t size);

/**
 * Finish the current digest. Further update/finish calls fail until reset.
 */
SALTS_C_API int cmeta_sha256_stream_finish(
    cmeta_sha256_stream *stream,
    uint8_t out[SALTS_SHA256_DIGEST_BYTES]);

/** Wipe provider state and release an incremental SHA-256 context. */
SALTS_C_API void cmeta_sha256_stream_destroy(cmeta_sha256_stream *stream);

/**
 * Compute SM3 over one contiguous byte range.
 *
 * Input is borrowed for this call. NULL data is allowed only when size is zero.
 * On success, out receives exactly SALTS_SM3_DIGEST_BYTES. On failure, out is
 * left unchanged. No provider type or state crosses the public ABI.
 */
SALTS_C_API int cmeta_sm3(const void *data, size_t size,
                          uint8_t out[SALTS_SM3_DIGEST_BYTES]);

/** Create an opaque incremental SM3 context owned by the caller. */
SALTS_C_API int cmeta_sm3_stream_create(cmeta_sm3_stream **out_stream);

/** Reset an incremental SM3 context to the empty-message state. */
SALTS_C_API int cmeta_sm3_stream_reset(cmeta_sm3_stream *stream);

/** Add one borrowed byte range; NULL data is allowed only when size is zero. */
SALTS_C_API int cmeta_sm3_stream_update(cmeta_sm3_stream *stream,
                                        const void *data, size_t size);

/**
 * Finish the current SM3 digest. Further update/finish calls fail until reset.
 * On failure, out is left unchanged.
 */
SALTS_C_API int cmeta_sm3_stream_finish(
    cmeta_sm3_stream *stream,
    uint8_t out[SALTS_SM3_DIGEST_BYTES]);

/** Wipe provider state and release an incremental SM3 context. */
SALTS_C_API void cmeta_sm3_stream_destroy(cmeta_sm3_stream *stream);

/**
 * Compute HMAC-SM3 over one contiguous byte range.
 *
 * key/data are borrowed. NULL is allowed only for a corresponding zero length.
 * On success, out receives exactly SALTS_SM3_DIGEST_BYTES. On failure, out is
 * left unchanged.
 */
SALTS_C_API int cmeta_hmac_sm3(
    const void *key, size_t key_size,
    const void *data, size_t data_size,
    uint8_t out[SALTS_SM3_DIGEST_BYTES]);

/**
 * RFC 5869 HKDF-SM3 extract+expand.
 *
 * ikm, salt and info are borrowed for this call. NULL is allowed only when the
 * corresponding size is zero. output_size may be zero with output == NULL.
 * output_size must not exceed SALTS_HKDF_SM3_MAX_OUTPUT_BYTES (8160). The
 * function performs no hidden nonce generation, provider selection or runtime
 * persistence. Output is committed only after the complete derivation succeeds;
 * every error leaves caller output unchanged.
 *
 * @return SALTS_OK, SALTS_EINVAL, SALTS_ERANGE, or SALTS_EIO.
 */
SALTS_C_API int cmeta_hkdf_sm3(
    const void *ikm, size_t ikm_size,
    const void *salt, size_t salt_size,
    const void *info, size_t info_size,
    void *output, size_t output_size);

#define SALTS_SM4_KEY_BYTES 16u
#define SALTS_SM4_GCM_NONCE_BYTES 12u
#define SALTS_SM4_GCM_TAG_BYTES 16u
#define SALTS_SM4_GCM_MAX_AAD_BYTES (16u * 1024u * 1024u)
#define SALTS_SM4_GCM_MAX_MESSAGE_BYTES ((UINT64_C(1) << 36) - UINT64_C(32))

/**
 * Encrypt one message with SM4-GCM using a fixed 96-bit nonce and 128-bit tag.
 *
 * The caller owns nonce generation and MUST NOT reuse a nonce with the same
 * key. AAD and plaintext are borrowed and NULL is valid only for zero length.
 * The GmSSL provider's 16 MiB AAD bound and GCM message bound are enforced.
 * Ciphertext length equals plaintext length; the tag is returned separately.
 *
 * The output size pointer is mandatory and must not overlap any other argument.
 * If capacity is too small, the function sets *out_ciphertext_size to the
 * required plaintext size and returns SALTS_ERANGE without modifying
 * ciphertext or tag. Exact in-place payload encryption is allowed; partial
 * overlap is rejected. Every other failure leaves all caller outputs unchanged.
 * A zero-length message may use ciphertext == NULL and capacity == 0.
 *
 * @return SALTS_OK, SALTS_EINVAL, SALTS_ERANGE, SALTS_ENOMEM, or SALTS_EIO.
 */
SALTS_C_API int cmeta_sm4_gcm_seal(
    const uint8_t key[SALTS_SM4_KEY_BYTES],
    const uint8_t nonce[SALTS_SM4_GCM_NONCE_BYTES],
    const void *aad, size_t aad_size,
    const void *plaintext, size_t plaintext_size,
    void *ciphertext, size_t ciphertext_capacity,
    size_t *out_ciphertext_size,
    uint8_t tag[SALTS_SM4_GCM_TAG_BYTES]);

/**
 * Authenticate and decrypt one SM4-GCM message.
 *
 * The caller owns the key and nonce. Ciphertext, AAD and tag are borrowed.
 * Plaintext is staged privately and copied to caller memory only after tag
 * verification succeeds; invalid authentication returns SALTS_EAUTH and leaves
 * both plaintext and *out_plaintext_size unchanged. If capacity is too small,
 * the function sets *out_plaintext_size to the required ciphertext size and
 * returns SALTS_ERANGE without modifying plaintext. Exact in-place decryption
 * is allowed; partial overlap is rejected. The output size pointer must not
 * overlap any other argument. A zero-length message may use plaintext == NULL
 * and capacity == 0.
 *
 * @return SALTS_OK, SALTS_EINVAL, SALTS_ERANGE, SALTS_ENOMEM, SALTS_EAUTH,
 *         or SALTS_EIO.
 */
SALTS_C_API int cmeta_sm4_gcm_open(
    const uint8_t key[SALTS_SM4_KEY_BYTES],
    const uint8_t nonce[SALTS_SM4_GCM_NONCE_BYTES],
    const void *aad, size_t aad_size,
    const void *ciphertext, size_t ciphertext_size,
    const uint8_t tag[SALTS_SM4_GCM_TAG_BYTES],
    void *plaintext, size_t plaintext_capacity,
    size_t *out_plaintext_size);

/**
 * Compute HMAC-MD5 over one contiguous byte range.
 *
 * HMAC-MD5 is exposed only for legacy protocol compatibility.
 *
 * @param key Key bytes; NULL is allowed only when key_size is zero.
 * @param key_size Key byte count.
 * @param data Input bytes; NULL is allowed only when data_size is zero.
 * @param data_size Input byte count.
 * @param out Caller-owned 16-byte MAC buffer.
 * @return SALTS_OK, SALTS_EINVAL, or SALTS_EIO.
 */
SALTS_C_API int cmeta_hmac_md5(const void *key, size_t key_size,
                               const void *data, size_t data_size,
                               uint8_t out[SALTS_MD5_DIGEST_BYTES]);

/**
 * Compute HMAC-SHA1 over one contiguous byte range.
 *
 * HMAC-SHA1 is exposed only for legacy protocol compatibility.
 *
 * @param key Key bytes; NULL is allowed only when key_size is zero.
 * @param key_size Key byte count.
 * @param data Input bytes; NULL is allowed only when data_size is zero.
 * @param data_size Input byte count.
 * @param out Caller-owned 20-byte MAC buffer.
 * @return SALTS_OK, SALTS_EINVAL, or SALTS_EIO.
 */
SALTS_C_API int cmeta_hmac_sha1(const void *key, size_t key_size,
                                const void *data, size_t data_size,
                                uint8_t out[SALTS_SHA1_DIGEST_BYTES]);

/**
 * Compute HMAC-SHA256 over one contiguous byte range.
 *
 * @param key Key bytes; NULL is allowed only when key_size is zero.
 * @param key_size Key byte count.
 * @param data Input bytes; NULL is allowed only when data_size is zero.
 * @param data_size Input byte count.
 * @param out Caller-owned 32-byte MAC buffer.
 * @return SALTS_OK, SALTS_EINVAL, or SALTS_EIO.
 */
SALTS_C_API int cmeta_hmac_sha256(const void *key, size_t key_size,
                                  const void *data, size_t data_size,
                                  uint8_t out[SALTS_SHA256_DIGEST_BYTES]);

/**
 * Encrypt whole DES-CBC blocks.
 *
 * DES is exposed only for legacy protocol compatibility. Padding remains the
 * responsibility of the protocol layer.
 *
 * @param key Caller-owned 8-byte DES key.
 * @param iv Caller-owned 8-byte initial CBC block.
 * @param input Input bytes; NULL is allowed only when size is zero.
 * @param size Input/output byte count; must be a multiple of 8.
 * @param output Output bytes; NULL is allowed only when size is zero. Input and
 * output may alias exactly for in-place operation.
 * @return SALTS_OK, SALTS_EINVAL, or SALTS_EIO.
 */
SALTS_C_API int cmeta_des_cbc_encrypt(
    const uint8_t key[SALTS_DES_KEY_BYTES],
    const uint8_t iv[SALTS_DES_BLOCK_BYTES],
    const void *input, size_t size, void *output);

/**
 * Decrypt whole DES-CBC blocks.
 *
 * DES is exposed only for legacy protocol compatibility. Padding remains the
 * responsibility of the protocol layer.
 *
 * @param key Caller-owned 8-byte DES key.
 * @param iv Caller-owned 8-byte initial CBC block.
 * @param input Input bytes; NULL is allowed only when size is zero.
 * @param size Input/output byte count; must be a multiple of 8.
 * @param output Output bytes; NULL is allowed only when size is zero. Input and
 * output may alias exactly for in-place operation.
 * @return SALTS_OK, SALTS_EINVAL, or SALTS_EIO.
 */
SALTS_C_API int cmeta_des_cbc_decrypt(
    const uint8_t key[SALTS_DES_KEY_BYTES],
    const uint8_t iv[SALTS_DES_BLOCK_BYTES],
    const void *input, size_t size, void *output);

/**
 * Encrypt one byte range with AES-128-CFB128.
 *
 * @param key Caller-owned 16-byte AES key.
 * @param iv Caller-owned 16-byte initial feedback block.
 * @param input Input bytes; NULL is allowed only when size is zero.
 * @param size Input/output byte count.
 * @param output Output bytes; NULL is allowed only when size is zero. Input and
 * output may alias exactly for in-place operation.
 * @return SALTS_OK, SALTS_EINVAL, or SALTS_EIO.
 */
SALTS_C_API int cmeta_aes128_cfb_encrypt(
    const uint8_t key[SALTS_AES128_KEY_BYTES],
    const uint8_t iv[SALTS_AES_BLOCK_BYTES],
    const void *input, size_t size, void *output);

/**
 * Decrypt one byte range with AES-128-CFB128.
 *
 * @param key Caller-owned 16-byte AES key.
 * @param iv Caller-owned 16-byte initial feedback block.
 * @param input Input bytes; NULL is allowed only when size is zero.
 * @param size Input/output byte count.
 * @param output Output bytes; NULL is allowed only when size is zero. Input and
 * output may alias exactly for in-place operation.
 * @return SALTS_OK, SALTS_EINVAL, or SALTS_EIO.
 */
SALTS_C_API int cmeta_aes128_cfb_decrypt(
    const uint8_t key[SALTS_AES128_KEY_BYTES],
    const uint8_t iv[SALTS_AES_BLOCK_BYTES],
    const void *input, size_t size, void *output);

/**
 * Compare two fixed-length byte ranges in constant time with respect to their
 * contents.
 *
 * The comparison length is public. NULL inputs are accepted only when size is
 * zero. On success, out_equal receives 1 for equality and 0 for inequality.
 *
 * @param lhs First byte range.
 * @param rhs Second byte range.
 * @param size Number of bytes to compare.
 * @param out_equal Caller-owned equality result.
 * @return SALTS_OK or SALTS_EINVAL.
 */
SALTS_C_API int cmeta_crypto_equal(const void *lhs, const void *rhs, size_t size,
                                   int *out_equal);

/**
 * Securely clear one caller-owned byte range.
 *
 * A zero-length clear accepts NULL. Non-zero size requires a valid buffer.
 */
SALTS_C_API void cmeta_crypto_clear(void *data, size_t size);

#ifdef __cplusplus
}
#endif

#endif /* SALTS_CRYPTO_H */
