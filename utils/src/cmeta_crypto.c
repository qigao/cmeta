#include "cmeta_crypto.h"

#include <gmssl/aes.h>
#include <gmssl/des.h>
#include <gmssl/digest.h>
#include <gmssl/hmac.h>
#include <gmssl/hkdf.h>
#include <gmssl/md5.h>
#include <gmssl/mem.h>
#include <gmssl/sha1.h>
#include <gmssl/sha2.h>
#include <gmssl/sm3.h>
#include <gmssl/sm2.h>
#include <salts/asn1.h>
#include <gmssl/sm4.h>

#include <stdlib.h>
#include <string.h>

int cmeta_md5(const void *data, size_t size,
              uint8_t out[SALTS_MD5_DIGEST_BYTES]) {
  MD5_CTX context;

  if (out == NULL || (data == NULL && size != 0u)) return SALTS_EINVAL;

  md5_init(&context);
  if (size != 0u) md5_update(&context, (const uint8_t *)data, size);
  md5_finish(&context, out);
  gmssl_secure_clear(&context, sizeof(context));
  return SALTS_OK;
}

int cmeta_sha1(const void *data, size_t size,
               uint8_t out[SALTS_SHA1_DIGEST_BYTES]) {
  SHA1_CTX context;

  if (out == NULL || (data == NULL && size != 0u)) return SALTS_EINVAL;
  sha1_init(&context);
  if (size != 0u) sha1_update(&context, (const uint8_t *)data, size);
  sha1_finish(&context, out);
  memset(&context, 0, sizeof(context));
  return SALTS_OK;
}

struct cmeta_sha256_stream {
  SHA256_CTX context;
  int finished;
};

int cmeta_sha256(const void *data, size_t size,
                 uint8_t out[SALTS_SHA256_DIGEST_BYTES]) {
  SHA256_CTX context;

  if (out == NULL || (data == NULL && size != 0u)) return SALTS_EINVAL;
  sha256_init(&context);
  if (size != 0u) sha256_update(&context, (const uint8_t *)data, size);
  sha256_finish(&context, out);
  memset(&context, 0, sizeof(context));
  return SALTS_OK;
}

int cmeta_sha256_stream_create(cmeta_sha256_stream **out_stream) {
  cmeta_sha256_stream *stream;
  if (out_stream == NULL) return SALTS_EINVAL;
  *out_stream = NULL;
  stream = (cmeta_sha256_stream *)calloc(1u, sizeof(*stream));
  if (stream == NULL) return SALTS_ENOMEM;
  sha256_init(&stream->context);
  *out_stream = stream;
  return SALTS_OK;
}

int cmeta_sha256_stream_reset(cmeta_sha256_stream *stream) {
  if (stream == NULL) return SALTS_EINVAL;
  gmssl_secure_clear(&stream->context, sizeof(stream->context));
  sha256_init(&stream->context);
  stream->finished = 0;
  return SALTS_OK;
}

int cmeta_sha256_stream_update(cmeta_sha256_stream *stream,
                               const void *data, size_t size) {
  if (stream == NULL || stream->finished ||
      (data == NULL && size != 0u))
    return SALTS_EINVAL;
  if (size != 0u) sha256_update(&stream->context, (const uint8_t *)data, size);
  return SALTS_OK;
}

int cmeta_sha256_stream_finish(
    cmeta_sha256_stream *stream,
    uint8_t out[SALTS_SHA256_DIGEST_BYTES]) {
  if (stream == NULL || out == NULL || stream->finished) return SALTS_EINVAL;
  sha256_finish(&stream->context, out);
  gmssl_secure_clear(&stream->context, sizeof(stream->context));
  stream->finished = 1;
  return SALTS_OK;
}

void cmeta_sha256_stream_destroy(cmeta_sha256_stream *stream) {
  if (stream == NULL) return;
  gmssl_secure_clear(stream, sizeof(*stream));
  free(stream);
}

struct cmeta_sm3_stream {
  SM3_CTX context;
  int finished;
};

int cmeta_sm3(const void *data, size_t size,
              uint8_t out[SALTS_SM3_DIGEST_BYTES]) {
  SM3_CTX context;
  uint8_t digest[SALTS_SM3_DIGEST_BYTES];
  static const uint8_t empty = 0u;

  if (out == NULL || (data == NULL && size != 0u)) return SALTS_EINVAL;

  sm3_init(&context);
  if (size != 0u)
    sm3_update(&context, (const uint8_t *)data, size);
  else
    sm3_update(&context, data != NULL ? (const uint8_t *)data : &empty, 0u);
  sm3_finish(&context, digest);
  memcpy(out, digest, sizeof(digest));
  gmssl_secure_clear(&context, sizeof(context));
  gmssl_secure_clear(digest, sizeof(digest));
  return SALTS_OK;
}

int cmeta_sm3_stream_create(cmeta_sm3_stream **out_stream) {
  cmeta_sm3_stream *stream;
  if (out_stream == NULL) return SALTS_EINVAL;
  *out_stream = NULL;
  stream = (cmeta_sm3_stream *)calloc(1u, sizeof(*stream));
  if (stream == NULL) return SALTS_ENOMEM;
  sm3_init(&stream->context);
  *out_stream = stream;
  return SALTS_OK;
}

int cmeta_sm3_stream_reset(cmeta_sm3_stream *stream) {
  if (stream == NULL) return SALTS_EINVAL;
  gmssl_secure_clear(&stream->context, sizeof(stream->context));
  sm3_init(&stream->context);
  stream->finished = 0;
  return SALTS_OK;
}

int cmeta_sm3_stream_update(cmeta_sm3_stream *stream,
                            const void *data, size_t size) {
  static const uint8_t empty = 0u;
  if (stream == NULL || stream->finished ||
      (data == NULL && size != 0u))
    return SALTS_EINVAL;
  sm3_update(&stream->context,
             data != NULL ? (const uint8_t *)data : &empty, size);
  return SALTS_OK;
}

int cmeta_sm3_stream_finish(
    cmeta_sm3_stream *stream,
    uint8_t out[SALTS_SM3_DIGEST_BYTES]) {
  uint8_t digest[SALTS_SM3_DIGEST_BYTES];
  if (stream == NULL || out == NULL || stream->finished) return SALTS_EINVAL;
  sm3_finish(&stream->context, digest);
  memcpy(out, digest, sizeof(digest));
  gmssl_secure_clear(digest, sizeof(digest));
  gmssl_secure_clear(&stream->context, sizeof(stream->context));
  stream->finished = 1;
  return SALTS_OK;
}

void cmeta_sm3_stream_destroy(cmeta_sm3_stream *stream) {
  if (stream == NULL) return;
  gmssl_secure_clear(stream, sizeof(*stream));
  free(stream);
}

int cmeta_hmac_sm3(
    const void *key, size_t key_size,
    const void *data, size_t data_size,
    uint8_t out[SALTS_SM3_DIGEST_BYTES]) {
  SM3_HMAC_CTX context;
  uint8_t mac[SALTS_SM3_DIGEST_BYTES];
  static const uint8_t empty = 0u;
  const uint8_t *key_bytes;
  const uint8_t *data_bytes;

  if (out == NULL || (key == NULL && key_size != 0u) ||
      (data == NULL && data_size != 0u))
    return SALTS_EINVAL;

  key_bytes = key != NULL ? (const uint8_t *)key : &empty;
  data_bytes = data != NULL ? (const uint8_t *)data : &empty;
  sm3_hmac_init(&context, key_bytes, key_size);
  sm3_hmac_update(&context, data_bytes, data_size);
  sm3_hmac_finish(&context, mac);
  memcpy(out, mac, sizeof(mac));
  gmssl_secure_clear(&context, sizeof(context));
  gmssl_secure_clear(mac, sizeof(mac));
  return SALTS_OK;
}

int cmeta_hkdf_sm3(
    const void *ikm, size_t ikm_size,
    const void *salt, size_t salt_size,
    const void *info, size_t info_size,
    void *output, size_t output_size) {
  uint8_t prk[SALTS_SM3_DIGEST_BYTES];
  uint8_t scratch[SALTS_HKDF_SM3_MAX_OUTPUT_BYTES];
  static const uint8_t empty = 0u;
  const uint8_t *ikm_bytes;
  const uint8_t *salt_bytes;
  const uint8_t *info_bytes;
  int status = SALTS_EIO;

  if ((ikm == NULL && ikm_size != 0u) ||
      (salt == NULL && salt_size != 0u) ||
      (info == NULL && info_size != 0u) ||
      (output == NULL && output_size != 0u))
    return SALTS_EINVAL;
  if (output_size > SALTS_HKDF_SM3_MAX_OUTPUT_BYTES) return SALTS_ERANGE;
  if (output_size == 0u) return SALTS_OK;

  ikm_bytes = ikm != NULL ? (const uint8_t *)ikm : &empty;
  salt_bytes = salt != NULL ? (const uint8_t *)salt : &empty;
  info_bytes = info != NULL ? (const uint8_t *)info : &empty;
  memset(prk, 0, sizeof(prk));
  memset(scratch, 0, sizeof(scratch));

  if (sm3_hkdf_extract(salt_size != 0u ? salt_bytes : NULL, salt_size,
                       ikm_bytes, ikm_size, prk) != 1)
    goto done;
  if (sm3_hkdf_expand(prk, info_bytes, info_size,
                      output_size, scratch) != 1)
    goto done;

  memcpy(output, scratch, output_size);
  status = SALTS_OK;
done:
  gmssl_secure_clear(prk, sizeof(prk));
  gmssl_secure_clear(scratch, sizeof(scratch));
  return status;
}

int cmeta_hmac_md5(const void *key, size_t key_size,
                   const void *data, size_t data_size,
                   uint8_t out[SALTS_MD5_DIGEST_BYTES]) {
  MD5_CTX context;
  uint8_t key_block[MD5_BLOCK_SIZE] = {0};
  uint8_t inner_pad[MD5_BLOCK_SIZE];
  uint8_t outer_pad[MD5_BLOCK_SIZE];
  uint8_t inner_digest[SALTS_MD5_DIGEST_BYTES];
  size_t i;

  if (out == NULL || (key == NULL && key_size != 0u) ||
      (data == NULL && data_size != 0u))
    return SALTS_EINVAL;

  if (key_size > sizeof(key_block)) {
    md5_init(&context);
    md5_update(&context, (const uint8_t *)key, key_size);
    md5_finish(&context, key_block);
    gmssl_secure_clear(&context, sizeof(context));
  } else if (key_size != 0u) {
    memcpy(key_block, key, key_size);
  }

  for (i = 0u; i < sizeof(key_block); ++i) {
    inner_pad[i] = (uint8_t)(key_block[i] ^ 0x36u);
    outer_pad[i] = (uint8_t)(key_block[i] ^ 0x5cu);
  }

  md5_init(&context);
  md5_update(&context, inner_pad, sizeof(inner_pad));
  if (data_size != 0u) md5_update(&context, (const uint8_t *)data, data_size);
  md5_finish(&context, inner_digest);

  md5_init(&context);
  md5_update(&context, outer_pad, sizeof(outer_pad));
  md5_update(&context, inner_digest, sizeof(inner_digest));
  md5_finish(&context, out);

  gmssl_secure_clear(&context, sizeof(context));
  gmssl_secure_clear(key_block, sizeof(key_block));
  gmssl_secure_clear(inner_pad, sizeof(inner_pad));
  gmssl_secure_clear(outer_pad, sizeof(outer_pad));
  gmssl_secure_clear(inner_digest, sizeof(inner_digest));
  return SALTS_OK;
}

static int cmeta_hmac(const DIGEST *digest, size_t expected_size,
                      const void *key, size_t key_size,
                      const void *data, size_t data_size,
                      uint8_t *out) {
  size_t output_size = 0u;

  if (digest == NULL || out == NULL ||
      (key == NULL && key_size != 0u) ||
      (data == NULL && data_size != 0u))
    return SALTS_EINVAL;

  if (hmac(digest, (const uint8_t *)key, key_size,
           (const uint8_t *)data, data_size, out, &output_size) != 1 ||
      output_size != expected_size)
    return SALTS_EIO;

  return SALTS_OK;
}

int cmeta_hmac_sha1(const void *key, size_t key_size,
                    const void *data, size_t data_size,
                    uint8_t out[SALTS_SHA1_DIGEST_BYTES]) {
  return cmeta_hmac(DIGEST_sha1(), SALTS_SHA1_DIGEST_BYTES,
                    key, key_size, data, data_size, out);
}

int cmeta_hmac_sha256(const void *key, size_t key_size,
                      const void *data, size_t data_size,
                      uint8_t out[SALTS_SHA256_DIGEST_BYTES]) {
  return cmeta_hmac(DIGEST_sha256(), SALTS_SHA256_DIGEST_BYTES,
                    key, key_size, data, data_size, out);
}

static int cmeta_des_cbc_crypt(
    const uint8_t key[SALTS_DES_KEY_BYTES],
    const uint8_t iv[SALTS_DES_BLOCK_BYTES],
    const void *input, size_t size, void *output, int decrypt) {
  int result;

  if (key == NULL || iv == NULL ||
      (size % SALTS_DES_BLOCK_BYTES) != 0u ||
      (size != 0u && (input == NULL || output == NULL)))
    return SALTS_EINVAL;

  result = decrypt
      ? des_cbc_decrypt(key, iv, (const uint8_t *)input, size, (uint8_t *)output)
      : des_cbc_encrypt(key, iv, (const uint8_t *)input, size, (uint8_t *)output);
  return result == 1 ? SALTS_OK : SALTS_EIO;
}

int cmeta_des_cbc_encrypt(
    const uint8_t key[SALTS_DES_KEY_BYTES],
    const uint8_t iv[SALTS_DES_BLOCK_BYTES],
    const void *input, size_t size, void *output) {
  return cmeta_des_cbc_crypt(key, iv, input, size, output, 0);
}

int cmeta_des_cbc_decrypt(
    const uint8_t key[SALTS_DES_KEY_BYTES],
    const uint8_t iv[SALTS_DES_BLOCK_BYTES],
    const void *input, size_t size, void *output) {
  return cmeta_des_cbc_crypt(key, iv, input, size, output, 1);
}

static int cmeta_aes128_cfb_crypt(
    const uint8_t key[SALTS_AES128_KEY_BYTES],
    const uint8_t iv[SALTS_AES_BLOCK_BYTES],
    const void *input, size_t size, void *output, int decrypt) {
  AES_KEY aes_key;
  uint8_t feedback[SALTS_AES_BLOCK_BYTES];
  uint8_t stream[SALTS_AES_BLOCK_BYTES];
  const uint8_t *src = (const uint8_t *)input;
  uint8_t *dst = (uint8_t *)output;
  size_t offset = 0u;
  size_t i;
  int status = SALTS_OK;

  if (key == NULL || iv == NULL ||
      (size != 0u && (input == NULL || output == NULL)))
    return SALTS_EINVAL;
  if (size == 0u) return SALTS_OK;

  if (aes_set_encrypt_key(&aes_key, key, SALTS_AES128_KEY_BYTES) != 1)
    return SALTS_EIO;

  memcpy(feedback, iv, sizeof(feedback));
  memset(stream, 0, sizeof(stream));

  for (i = 0u; i < size; ++i) {
    uint8_t input_byte;
    uint8_t output_byte;

    if (offset == 0u) aes_encrypt(&aes_key, feedback, stream);
    input_byte = src[i];
    output_byte = (uint8_t)(input_byte ^ stream[offset]);
    dst[i] = output_byte;
    feedback[offset] = decrypt ? input_byte : output_byte;
    offset = (offset + 1u) % SALTS_AES_BLOCK_BYTES;
  }

  gmssl_secure_clear(&aes_key, sizeof(aes_key));
  gmssl_secure_clear(feedback, sizeof(feedback));
  gmssl_secure_clear(stream, sizeof(stream));
  return status;
}

int cmeta_aes128_cfb_encrypt(
    const uint8_t key[SALTS_AES128_KEY_BYTES],
    const uint8_t iv[SALTS_AES_BLOCK_BYTES],
    const void *input, size_t size, void *output) {
  return cmeta_aes128_cfb_crypt(key, iv, input, size, output, 0);
}

int cmeta_aes128_cfb_decrypt(
    const uint8_t key[SALTS_AES128_KEY_BYTES],
    const uint8_t iv[SALTS_AES_BLOCK_BYTES],
    const void *input, size_t size, void *output) {
  return cmeta_aes128_cfb_crypt(key, iv, input, size, output, 1);
}


static int cmeta_crypto_range_valid_v1(const void *ptr, size_t size) {
  uintptr_t start;

  if (size == 0u) return 1;
  if (ptr == NULL) return 0;
  start = (uintptr_t)ptr;
  return (uintmax_t)size <= (uintmax_t)(UINTPTR_MAX - start);
}

/* Returns 1 for overlap, 0 for disjoint ranges, and -1 for invalid ranges. */
static int cmeta_crypto_ranges_overlap_v1(const void *lhs, size_t lhs_size,
                                          const void *rhs, size_t rhs_size) {
  uintptr_t lhs_start;
  uintptr_t rhs_start;
  uintptr_t lhs_end;
  uintptr_t rhs_end;

  if (lhs_size == 0u || rhs_size == 0u) return 0;
  if (!cmeta_crypto_range_valid_v1(lhs, lhs_size) ||
      !cmeta_crypto_range_valid_v1(rhs, rhs_size))
    return -1;

  lhs_start = (uintptr_t)lhs;
  rhs_start = (uintptr_t)rhs;
  lhs_end = lhs_start + (uintptr_t)lhs_size;
  rhs_end = rhs_start + (uintptr_t)rhs_size;
  return lhs_start < rhs_end && rhs_start < lhs_end ? 1 : 0;
}

static int cmeta_crypto_ranges_conflict_v1(const void *lhs, size_t lhs_size,
                                           const void *rhs, size_t rhs_size) {
  return cmeta_crypto_ranges_overlap_v1(lhs, lhs_size, rhs, rhs_size) != 0;
}

static int cmeta_sm4_gcm_seal_ranges_valid(
    const uint8_t key[SALTS_SM4_KEY_BYTES],
    const uint8_t nonce[SALTS_SM4_GCM_NONCE_BYTES],
    const void *aad, size_t aad_size,
    const void *plaintext, size_t plaintext_size,
    void *ciphertext, size_t ciphertext_capacity,
    size_t *out_ciphertext_size,
    uint8_t tag[SALTS_SM4_GCM_TAG_BYTES]) {
  size_t output_range_size =
      ciphertext_capacity < plaintext_size ? ciphertext_capacity : plaintext_size;

  if (key == NULL || nonce == NULL || out_ciphertext_size == NULL || tag == NULL ||
      (aad == NULL && aad_size != 0u) ||
      (plaintext == NULL && plaintext_size != 0u) ||
      (ciphertext == NULL && ciphertext_capacity != 0u))
    return 0;
  if (!cmeta_crypto_range_valid_v1(key, SALTS_SM4_KEY_BYTES) ||
      !cmeta_crypto_range_valid_v1(nonce, SALTS_SM4_GCM_NONCE_BYTES) ||
      !cmeta_crypto_range_valid_v1(aad, aad_size) ||
      !cmeta_crypto_range_valid_v1(plaintext, plaintext_size) ||
      !cmeta_crypto_range_valid_v1(out_ciphertext_size, sizeof(*out_ciphertext_size)) ||
      !cmeta_crypto_range_valid_v1(tag, SALTS_SM4_GCM_TAG_BYTES) ||
      !cmeta_crypto_range_valid_v1(ciphertext, output_range_size))
    return 0;

  if (cmeta_crypto_ranges_conflict_v1(out_ciphertext_size, sizeof(*out_ciphertext_size),
                                      key, SALTS_SM4_KEY_BYTES) ||
      cmeta_crypto_ranges_conflict_v1(out_ciphertext_size, sizeof(*out_ciphertext_size),
                                      nonce, SALTS_SM4_GCM_NONCE_BYTES) ||
      cmeta_crypto_ranges_conflict_v1(out_ciphertext_size, sizeof(*out_ciphertext_size),
                                      aad, aad_size) ||
      cmeta_crypto_ranges_conflict_v1(out_ciphertext_size, sizeof(*out_ciphertext_size),
                                      plaintext, plaintext_size) ||
      cmeta_crypto_ranges_conflict_v1(out_ciphertext_size, sizeof(*out_ciphertext_size),
                                      ciphertext, output_range_size) ||
      cmeta_crypto_ranges_conflict_v1(out_ciphertext_size, sizeof(*out_ciphertext_size),
                                      tag, SALTS_SM4_GCM_TAG_BYTES) ||
      cmeta_crypto_ranges_conflict_v1(tag, SALTS_SM4_GCM_TAG_BYTES,
                                      key, SALTS_SM4_KEY_BYTES) ||
      cmeta_crypto_ranges_conflict_v1(tag, SALTS_SM4_GCM_TAG_BYTES,
                                      nonce, SALTS_SM4_GCM_NONCE_BYTES) ||
      cmeta_crypto_ranges_conflict_v1(tag, SALTS_SM4_GCM_TAG_BYTES,
                                      aad, aad_size) ||
      cmeta_crypto_ranges_conflict_v1(tag, SALTS_SM4_GCM_TAG_BYTES,
                                      plaintext, plaintext_size) ||
      cmeta_crypto_ranges_conflict_v1(tag, SALTS_SM4_GCM_TAG_BYTES,
                                      ciphertext, output_range_size))
    return 0;

  if (ciphertext != NULL && ciphertext != plaintext &&
      cmeta_crypto_ranges_conflict_v1(ciphertext, output_range_size,
                                      plaintext, plaintext_size))
    return 0;
  if (cmeta_crypto_ranges_conflict_v1(ciphertext, output_range_size,
                                      key, SALTS_SM4_KEY_BYTES) ||
      cmeta_crypto_ranges_conflict_v1(ciphertext, output_range_size,
                                      nonce, SALTS_SM4_GCM_NONCE_BYTES) ||
      cmeta_crypto_ranges_conflict_v1(ciphertext, output_range_size,
                                      aad, aad_size))
    return 0;
  return 1;
}

static int cmeta_sm4_gcm_open_ranges_valid(
    const uint8_t key[SALTS_SM4_KEY_BYTES],
    const uint8_t nonce[SALTS_SM4_GCM_NONCE_BYTES],
    const void *aad, size_t aad_size,
    const void *ciphertext, size_t ciphertext_size,
    const uint8_t tag[SALTS_SM4_GCM_TAG_BYTES],
    void *plaintext, size_t plaintext_capacity,
    size_t *out_plaintext_size) {
  size_t output_range_size =
      plaintext_capacity < ciphertext_size ? plaintext_capacity : ciphertext_size;

  if (key == NULL || nonce == NULL || tag == NULL || out_plaintext_size == NULL ||
      (aad == NULL && aad_size != 0u) ||
      (ciphertext == NULL && ciphertext_size != 0u) ||
      (plaintext == NULL && plaintext_capacity != 0u))
    return 0;
  if (!cmeta_crypto_range_valid_v1(key, SALTS_SM4_KEY_BYTES) ||
      !cmeta_crypto_range_valid_v1(nonce, SALTS_SM4_GCM_NONCE_BYTES) ||
      !cmeta_crypto_range_valid_v1(aad, aad_size) ||
      !cmeta_crypto_range_valid_v1(ciphertext, ciphertext_size) ||
      !cmeta_crypto_range_valid_v1(tag, SALTS_SM4_GCM_TAG_BYTES) ||
      !cmeta_crypto_range_valid_v1(plaintext, output_range_size) ||
      !cmeta_crypto_range_valid_v1(out_plaintext_size, sizeof(*out_plaintext_size)))
    return 0;

  if (cmeta_crypto_ranges_conflict_v1(out_plaintext_size, sizeof(*out_plaintext_size),
                                      key, SALTS_SM4_KEY_BYTES) ||
      cmeta_crypto_ranges_conflict_v1(out_plaintext_size, sizeof(*out_plaintext_size),
                                      nonce, SALTS_SM4_GCM_NONCE_BYTES) ||
      cmeta_crypto_ranges_conflict_v1(out_plaintext_size, sizeof(*out_plaintext_size),
                                      aad, aad_size) ||
      cmeta_crypto_ranges_conflict_v1(out_plaintext_size, sizeof(*out_plaintext_size),
                                      ciphertext, ciphertext_size) ||
      cmeta_crypto_ranges_conflict_v1(out_plaintext_size, sizeof(*out_plaintext_size),
                                      tag, SALTS_SM4_GCM_TAG_BYTES) ||
      cmeta_crypto_ranges_conflict_v1(out_plaintext_size, sizeof(*out_plaintext_size),
                                      plaintext, output_range_size) ||
      cmeta_crypto_ranges_conflict_v1(plaintext, output_range_size,
                                      key, SALTS_SM4_KEY_BYTES) ||
      cmeta_crypto_ranges_conflict_v1(plaintext, output_range_size,
                                      nonce, SALTS_SM4_GCM_NONCE_BYTES) ||
      cmeta_crypto_ranges_conflict_v1(plaintext, output_range_size,
                                      aad, aad_size) ||
      cmeta_crypto_ranges_conflict_v1(plaintext, output_range_size,
                                      tag, SALTS_SM4_GCM_TAG_BYTES))
    return 0;

  if (plaintext != NULL && plaintext != ciphertext &&
      cmeta_crypto_ranges_conflict_v1(plaintext, output_range_size,
                                      ciphertext, ciphertext_size))
    return 0;
  return 1;
}

int cmeta_sm4_gcm_seal(
    const uint8_t key[SALTS_SM4_KEY_BYTES],
    const uint8_t nonce[SALTS_SM4_GCM_NONCE_BYTES],
    const void *aad, size_t aad_size,
    const void *plaintext, size_t plaintext_size,
    void *ciphertext, size_t ciphertext_capacity,
    size_t *out_ciphertext_size,
    uint8_t tag[SALTS_SM4_GCM_TAG_BYTES]) {
  SM4_KEY provider_key;
  uint8_t staged_tag[SALTS_SM4_GCM_TAG_BYTES] = {0};
  uint8_t empty_input = 0u;
  uint8_t *staged_ciphertext;
  int provider_result;

  if (aad_size > SALTS_SM4_GCM_MAX_AAD_BYTES ||
      (uint64_t)plaintext_size > SALTS_SM4_GCM_MAX_MESSAGE_BYTES)
    return SALTS_ERANGE;
  if (!cmeta_sm4_gcm_seal_ranges_valid(
          key, nonce, aad, aad_size, plaintext, plaintext_size,
          ciphertext, ciphertext_capacity, out_ciphertext_size, tag))
    return SALTS_EINVAL;

  if (ciphertext_capacity < plaintext_size) {
    *out_ciphertext_size = plaintext_size;
    return SALTS_ERANGE;
  }

  staged_ciphertext = (uint8_t *)malloc(plaintext_size == 0u ? 1u : plaintext_size);
  if (staged_ciphertext == NULL) return SALTS_ENOMEM;

  sm4_set_encrypt_key(&provider_key, key);
  provider_result = sm4_gcm_encrypt(
      &provider_key, nonce, SALTS_SM4_GCM_NONCE_BYTES,
      (const uint8_t *)aad, aad_size,
      plaintext_size == 0u ? &empty_input : (const uint8_t *)plaintext,
      plaintext_size, staged_ciphertext, SALTS_SM4_GCM_TAG_BYTES, staged_tag);
  gmssl_secure_clear(&provider_key, sizeof(provider_key));

  if (provider_result != 1) {
    gmssl_secure_clear(staged_ciphertext, plaintext_size == 0u ? 1u : plaintext_size);
    free(staged_ciphertext);
    gmssl_secure_clear(staged_tag, sizeof(staged_tag));
    return SALTS_EIO;
  }

  if (plaintext_size != 0u) memcpy(ciphertext, staged_ciphertext, plaintext_size);
  memcpy(tag, staged_tag, sizeof(staged_tag));
  *out_ciphertext_size = plaintext_size;

  gmssl_secure_clear(staged_ciphertext, plaintext_size == 0u ? 1u : plaintext_size);
  free(staged_ciphertext);
  gmssl_secure_clear(staged_tag, sizeof(staged_tag));
  return SALTS_OK;
}

int cmeta_sm4_gcm_open(
    const uint8_t key[SALTS_SM4_KEY_BYTES],
    const uint8_t nonce[SALTS_SM4_GCM_NONCE_BYTES],
    const void *aad, size_t aad_size,
    const void *ciphertext, size_t ciphertext_size,
    const uint8_t tag[SALTS_SM4_GCM_TAG_BYTES],
    void *plaintext, size_t plaintext_capacity,
    size_t *out_plaintext_size) {
  SM4_KEY provider_key;
  uint8_t empty_input = 0u;
  uint8_t *staged_plaintext;
  int provider_result;

  if (aad_size > SALTS_SM4_GCM_MAX_AAD_BYTES ||
      (uint64_t)ciphertext_size > SALTS_SM4_GCM_MAX_MESSAGE_BYTES)
    return SALTS_ERANGE;
  if (!cmeta_sm4_gcm_open_ranges_valid(
          key, nonce, aad, aad_size, ciphertext, ciphertext_size, tag,
          plaintext, plaintext_capacity, out_plaintext_size))
    return SALTS_EINVAL;

  if (plaintext_capacity < ciphertext_size) {
    *out_plaintext_size = ciphertext_size;
    return SALTS_ERANGE;
  }

  staged_plaintext = (uint8_t *)malloc(ciphertext_size == 0u ? 1u : ciphertext_size);
  if (staged_plaintext == NULL) return SALTS_ENOMEM;

  sm4_set_encrypt_key(&provider_key, key);
  provider_result = sm4_gcm_decrypt(
      &provider_key, nonce, SALTS_SM4_GCM_NONCE_BYTES,
      (const uint8_t *)aad, aad_size,
      ciphertext_size == 0u ? &empty_input : (const uint8_t *)ciphertext,
      ciphertext_size, tag, SALTS_SM4_GCM_TAG_BYTES, staged_plaintext);
  gmssl_secure_clear(&provider_key, sizeof(provider_key));

  if (provider_result != 1) {
    gmssl_secure_clear(staged_plaintext, ciphertext_size == 0u ? 1u : ciphertext_size);
    free(staged_plaintext);
    return SALTS_EAUTH;
  }

  if (ciphertext_size != 0u) memcpy(plaintext, staged_plaintext, ciphertext_size);
  *out_plaintext_size = ciphertext_size;

  gmssl_secure_clear(staged_plaintext, ciphertext_size == 0u ? 1u : ciphertext_size);
  free(staged_plaintext);
  return SALTS_OK;
}


int cmeta_sm2_sign(
    const void *private_key_pkcs8_der, size_t private_key_der_size,
    const void *user_id, size_t user_id_size,
    const void *message, size_t message_size,
    uint8_t *signature_der, size_t signature_capacity,
    size_t *out_signature_size) {
  SM2_KEY key;
  SM2_SIGN_CTX sign_ctx;
  SM2_SIGNATURE parsed_signature;
  uint8_t staged_signature[SALTS_SM2_SIGNATURE_DER_MAX_BYTES] = {0};
  const uint8_t *key_cursor;
  const uint8_t *attributes = NULL;
  size_t key_remaining;
  size_t attributes_size = 0u;
  size_t signature_size = 0u;
  size_t signature_range_size =
      signature_capacity < SALTS_SM2_SIGNATURE_DER_MAX_BYTES
          ? signature_capacity
          : SALTS_SM2_SIGNATURE_DER_MAX_BYTES;
  int result = SALTS_EIO;

  memset(&key, 0, sizeof(key));
  memset(&sign_ctx, 0, sizeof(sign_ctx));
  memset(&parsed_signature, 0, sizeof(parsed_signature));

  if (private_key_pkcs8_der == NULL || private_key_der_size == 0u ||
      user_id == NULL || user_id_size == 0u ||
      (message == NULL && message_size != 0u) ||
      out_signature_size == NULL ||
      (signature_der == NULL && signature_capacity != 0u))
    return SALTS_EINVAL;
  if (private_key_der_size > SALTS_SM2_MAX_KEY_DER_BYTES ||
      user_id_size > SALTS_SM2_MAX_USER_ID_BYTES)
    return SALTS_ERANGE;
  if (!cmeta_crypto_range_valid_v1(private_key_pkcs8_der, private_key_der_size) ||
      !cmeta_crypto_range_valid_v1(user_id, user_id_size) ||
      !cmeta_crypto_range_valid_v1(message, message_size) ||
      !cmeta_crypto_range_valid_v1(out_signature_size, sizeof(*out_signature_size)) ||
      !cmeta_crypto_range_valid_v1(signature_der, signature_range_size))
    return SALTS_EINVAL;

  if (cmeta_crypto_ranges_conflict_v1(out_signature_size, sizeof(*out_signature_size),
                                      private_key_pkcs8_der, private_key_der_size) ||
      cmeta_crypto_ranges_conflict_v1(out_signature_size, sizeof(*out_signature_size),
                                      user_id, user_id_size) ||
      cmeta_crypto_ranges_conflict_v1(out_signature_size, sizeof(*out_signature_size),
                                      message, message_size) ||
      cmeta_crypto_ranges_conflict_v1(out_signature_size, sizeof(*out_signature_size),
                                      signature_der, signature_range_size) ||
      cmeta_crypto_ranges_conflict_v1(signature_der, signature_range_size,
                                      private_key_pkcs8_der, private_key_der_size) ||
      cmeta_crypto_ranges_conflict_v1(signature_der, signature_range_size,
                                      user_id, user_id_size) ||
      cmeta_crypto_ranges_conflict_v1(signature_der, signature_range_size,
                                      message, message_size))
    return SALTS_EINVAL;

  if (signature_capacity < SALTS_SM2_SIGNATURE_DER_MAX_BYTES) {
    *out_signature_size = SALTS_SM2_SIGNATURE_DER_MAX_BYTES;
    return SALTS_ERANGE;
  }
  if (signature_der == NULL) return SALTS_EINVAL;
  if (salts_asn1_validate(private_key_pkcs8_der, private_key_der_size,
                          SALTS_ASN1_DER) != SALTS_ASN1_OK)
    return SALTS_EINVAL;

  key_cursor = (const uint8_t *)private_key_pkcs8_der;
  key_remaining = private_key_der_size;
  if (sm2_private_key_info_from_der(&key, &attributes, &attributes_size,
                                    &key_cursor, &key_remaining) != 1 ||
      key_remaining != 0u) {
    result = SALTS_EINVAL;
    goto cleanup;
  }

  if (sm2_sign_init(&sign_ctx, &key, (const char *)user_id, user_id_size) != 1) {
    result = SALTS_EIO;
    goto cleanup;
  }
  if (sm2_sign_update(&sign_ctx, (const uint8_t *)message, message_size) != 1) {
    result = SALTS_EIO;
    goto cleanup;
  }
  if (sm2_sign_finish(&sign_ctx, staged_signature, &signature_size) != 1 ||
      signature_size == 0u ||
      signature_size > SALTS_SM2_SIGNATURE_DER_MAX_BYTES ||
      salts_asn1_validate(staged_signature, signature_size,
                          SALTS_ASN1_DER) != SALTS_ASN1_OK) {
    result = SALTS_EIO;
    goto cleanup;
  }

  /* Validate the generated signature structure before publishing any output. */
  {
    const uint8_t *signature_cursor = staged_signature;
    size_t signature_remaining = signature_size;
    if (sm2_signature_from_der(&parsed_signature, &signature_cursor,
                               &signature_remaining) != 1 ||
        signature_remaining != 0u) {
      result = SALTS_EIO;
      goto cleanup;
    }
  }

  memcpy(signature_der, staged_signature, signature_size);
  *out_signature_size = signature_size;
  result = SALTS_OK;

cleanup:
  gmssl_secure_clear(&sign_ctx, sizeof(sign_ctx));
  gmssl_secure_clear(&key, sizeof(key));
  gmssl_secure_clear(&parsed_signature, sizeof(parsed_signature));
  gmssl_secure_clear(staged_signature, sizeof(staged_signature));
  return result;
}

int cmeta_sm2_verify(
    const void *public_key_spki_der, size_t public_key_der_size,
    const void *user_id, size_t user_id_size,
    const void *message, size_t message_size,
    const void *signature_der, size_t signature_der_size) {
  SM2_KEY key;
  SM2_VERIFY_CTX verify_ctx;
  SM2_SIGNATURE parsed_signature;
  const uint8_t *key_cursor;
  const uint8_t *signature_cursor;
  size_t key_remaining;
  size_t signature_remaining;
  int result = SALTS_EIO;

  memset(&key, 0, sizeof(key));
  memset(&verify_ctx, 0, sizeof(verify_ctx));
  memset(&parsed_signature, 0, sizeof(parsed_signature));

  if (public_key_spki_der == NULL || public_key_der_size == 0u ||
      user_id == NULL || user_id_size == 0u ||
      (message == NULL && message_size != 0u) ||
      signature_der == NULL)
    return SALTS_EINVAL;
  if (public_key_der_size > SALTS_SM2_MAX_KEY_DER_BYTES ||
      user_id_size > SALTS_SM2_MAX_USER_ID_BYTES)
    return SALTS_ERANGE;
  if (!cmeta_crypto_range_valid_v1(public_key_spki_der, public_key_der_size) ||
      !cmeta_crypto_range_valid_v1(user_id, user_id_size) ||
      !cmeta_crypto_range_valid_v1(message, message_size) ||
      !cmeta_crypto_range_valid_v1(signature_der, signature_der_size))
    return SALTS_EINVAL;
  if (signature_der_size == 0u ||
      signature_der_size > SALTS_SM2_SIGNATURE_DER_MAX_BYTES)
    return SALTS_EAUTH;

  if (salts_asn1_validate(public_key_spki_der, public_key_der_size,
                          SALTS_ASN1_DER) != SALTS_ASN1_OK)
    return SALTS_EINVAL;
  if (salts_asn1_validate(signature_der, signature_der_size,
                          SALTS_ASN1_DER) != SALTS_ASN1_OK)
    return SALTS_EAUTH;

  key_cursor = (const uint8_t *)public_key_spki_der;
  key_remaining = public_key_der_size;
  if (sm2_public_key_info_from_der(&key, &key_cursor, &key_remaining) != 1 ||
      key_remaining != 0u) {
    result = SALTS_EINVAL;
    goto cleanup;
  }

  signature_cursor = (const uint8_t *)signature_der;
  signature_remaining = signature_der_size;
  if (sm2_signature_from_der(&parsed_signature, &signature_cursor,
                             &signature_remaining) != 1 ||
      signature_remaining != 0u) {
    result = SALTS_EAUTH;
    goto cleanup;
  }

  if (sm2_verify_init(&verify_ctx, &key, (const char *)user_id, user_id_size) != 1) {
    result = SALTS_EIO;
    goto cleanup;
  }
  if (sm2_verify_update(&verify_ctx, (const uint8_t *)message, message_size) != 1) {
    result = SALTS_EIO;
    goto cleanup;
  }

  result = sm2_verify_finish(&verify_ctx, (const uint8_t *)signature_der,
                             signature_der_size) == 1
               ? SALTS_OK
               : SALTS_EAUTH;

cleanup:
  gmssl_secure_clear(&verify_ctx, sizeof(verify_ctx));
  gmssl_secure_clear(&key, sizeof(key));
  gmssl_secure_clear(&parsed_signature, sizeof(parsed_signature));
  return result;
}

int cmeta_crypto_equal(const void *lhs, const void *rhs, size_t size,
                       int *out_equal) {
  if (out_equal == NULL || (size != 0u && (lhs == NULL || rhs == NULL)))
    return SALTS_EINVAL;

  if (size == 0u) {
    *out_equal = 1;
    return SALTS_OK;
  }

  *out_equal = gmssl_secure_memcmp(lhs, rhs, size) == 0 ? 1 : 0;
  return SALTS_OK;
}

void cmeta_crypto_clear(void *data, size_t size) {
  if (data == NULL || size == 0u) return;
  gmssl_secure_clear(data, size);
}
