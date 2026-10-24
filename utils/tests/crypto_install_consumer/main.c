#include <cmeta_crypto.h>
#include "crypto_test_vectors.h"
#include <stdint.h>
#include <string.h>

int main(void) {
  static const uint8_t expected[SALTS_SM3_DIGEST_BYTES] = {
      0x66,0xc7,0xf0,0xf4,0x62,0xee,0xed,0xd9,
      0xd1,0xf2,0xd4,0x6b,0xdc,0x10,0xe4,0xe2,
      0x41,0x67,0xc4,0x87,0x5c,0xf2,0xf7,0xa2,
      0x29,0x7d,0xa0,0x2b,0x8f,0x4b,0xa8,0xe0};
  uint8_t digest[SALTS_SM3_DIGEST_BYTES];
  uint8_t key[16];
  cmeta_sm3_stream *stream = NULL;

  if (cmeta_sm3("abc", 3u, digest) != SALTS_OK) return 1;
  if (memcmp(digest, expected, sizeof(digest)) != 0) return 2;
  if (cmeta_hmac_sm3("key", 3u, "data", 4u, digest) != SALTS_OK) return 3;
  if (cmeta_hkdf_sm3("ikm", 3u, "salt", 4u, "info", 4u,
                      key, sizeof(key)) != SALTS_OK) return 4;
  if (cmeta_sm3_stream_create(&stream) != SALTS_OK || stream == NULL) return 5;
  if (cmeta_sm3_stream_update(stream, "abc", 3u) != SALTS_OK) return 6;
  if (cmeta_sm3_stream_finish(stream, digest) != SALTS_OK) return 7;
  cmeta_sm3_stream_destroy(stream);
  if (memcmp(digest, expected, sizeof(digest)) != 0) return 8;

  if (cmeta_sm2_verify(
          SALTS_INSTALL_SM2_PUBLIC_SPKI, sizeof(SALTS_INSTALL_SM2_PUBLIC_SPKI),
          SALTS_INSTALL_SM2_USER_ID, sizeof(SALTS_INSTALL_SM2_USER_ID) - 1u,
          SALTS_INSTALL_SM2_MESSAGE, sizeof(SALTS_INSTALL_SM2_MESSAGE) - 1u,
          SALTS_INSTALL_SM2_SIGNATURE, sizeof(SALTS_INSTALL_SM2_SIGNATURE)) != SALTS_OK)
    return 9;

  {
    uint8_t ciphertext[sizeof(SALTS_INSTALL_SM4_GCM_CIPHERTEXT)] = {0};
    uint8_t plaintext[sizeof(SALTS_INSTALL_SM4_GCM_PLAINTEXT)] = {0};
    uint8_t tag[SALTS_SM4_GCM_TAG_BYTES] = {0};
    uint8_t bad_tag[SALTS_SM4_GCM_TAG_BYTES];
    uint8_t before[sizeof(plaintext)];
    size_t ciphertext_size = 0u;
    size_t plaintext_size = 0x1234u;

    if (cmeta_sm4_gcm_seal(
            SALTS_INSTALL_SM4_GCM_KEY, SALTS_INSTALL_SM4_GCM_NONCE,
            SALTS_INSTALL_SM4_GCM_AAD, sizeof(SALTS_INSTALL_SM4_GCM_AAD),
            SALTS_INSTALL_SM4_GCM_PLAINTEXT, sizeof(SALTS_INSTALL_SM4_GCM_PLAINTEXT),
            ciphertext, sizeof(ciphertext), &ciphertext_size, tag) != SALTS_OK)
      return 14;
    if (ciphertext_size != sizeof(ciphertext) ||
        memcmp(ciphertext, SALTS_INSTALL_SM4_GCM_CIPHERTEXT, sizeof(ciphertext)) != 0 ||
        memcmp(tag, SALTS_INSTALL_SM4_GCM_TAG, sizeof(tag)) != 0)
      return 15;
    if (cmeta_sm4_gcm_open(
            SALTS_INSTALL_SM4_GCM_KEY, SALTS_INSTALL_SM4_GCM_NONCE,
            SALTS_INSTALL_SM4_GCM_AAD, sizeof(SALTS_INSTALL_SM4_GCM_AAD),
            ciphertext, ciphertext_size, tag,
            plaintext, sizeof(plaintext), &plaintext_size) != SALTS_OK)
      return 16;
    if (plaintext_size != sizeof(plaintext) ||
        memcmp(plaintext, SALTS_INSTALL_SM4_GCM_PLAINTEXT, sizeof(plaintext)) != 0)
      return 17;

    memcpy(bad_tag, tag, sizeof(bad_tag));
    bad_tag[0] ^= 1u;
    memset(plaintext, 0xa5, sizeof(plaintext));
    memcpy(before, plaintext, sizeof(before));
    plaintext_size = 0x1234u;
    if (cmeta_sm4_gcm_open(
            SALTS_INSTALL_SM4_GCM_KEY, SALTS_INSTALL_SM4_GCM_NONCE,
            SALTS_INSTALL_SM4_GCM_AAD, sizeof(SALTS_INSTALL_SM4_GCM_AAD),
            ciphertext, ciphertext_size, bad_tag,
            plaintext, sizeof(plaintext), &plaintext_size) != SALTS_EAUTH)
      return 18;
    if (memcmp(plaintext, before, sizeof(plaintext)) != 0 || plaintext_size != (size_t)0x1234u)
      return 19;
  }
  {
    static const uint8_t wrong_id[] = "1234567812345679";
    uint8_t bad_signature[sizeof(SALTS_INSTALL_SM2_SIGNATURE)];
    uint8_t private_key_stub[] = {0x30u};
    uint8_t output[SALTS_SM2_SIGNATURE_DER_MAX_BYTES];
    uint8_t before[sizeof(output)];
    size_t signature_size = 0x1234u;

    if (cmeta_sm2_verify(
            SALTS_INSTALL_SM2_PUBLIC_SPKI, sizeof(SALTS_INSTALL_SM2_PUBLIC_SPKI),
            wrong_id, sizeof(wrong_id) - 1u,
            SALTS_INSTALL_SM2_MESSAGE, sizeof(SALTS_INSTALL_SM2_MESSAGE) - 1u,
            SALTS_INSTALL_SM2_SIGNATURE, sizeof(SALTS_INSTALL_SM2_SIGNATURE)) != SALTS_EAUTH)
      return 10;
    memcpy(bad_signature, SALTS_INSTALL_SM2_SIGNATURE, sizeof(bad_signature));
    bad_signature[sizeof(bad_signature) - 1u] ^= 1u;
    if (cmeta_sm2_verify(
            SALTS_INSTALL_SM2_PUBLIC_SPKI, sizeof(SALTS_INSTALL_SM2_PUBLIC_SPKI),
            SALTS_INSTALL_SM2_USER_ID, sizeof(SALTS_INSTALL_SM2_USER_ID) - 1u,
            SALTS_INSTALL_SM2_MESSAGE, sizeof(SALTS_INSTALL_SM2_MESSAGE) - 1u,
            bad_signature, sizeof(bad_signature)) != SALTS_EAUTH)
      return 11;

    memset(output, 0xa5, sizeof(output));
    memcpy(before, output, sizeof(before));
    if (cmeta_sm2_sign(
            private_key_stub, sizeof(private_key_stub),
            SALTS_INSTALL_SM2_USER_ID, sizeof(SALTS_INSTALL_SM2_USER_ID) - 1u,
            SALTS_INSTALL_SM2_MESSAGE, sizeof(SALTS_INSTALL_SM2_MESSAGE) - 1u,
            output, sizeof(output) - 1u, &signature_size) != SALTS_ERANGE)
      return 12;
    if (signature_size != SALTS_SM2_SIGNATURE_DER_MAX_BYTES ||
        memcmp(output, before, sizeof(output)) != 0)
      return 13;
  }
  return 0;
}
