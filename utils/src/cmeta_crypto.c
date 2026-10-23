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
