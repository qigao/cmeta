#include <cmeta_crypto.h>
#include "crypto_test_vectors.h"
#include <cstring>
#include <cstddef>
#include <cstdint>

static_assert(SALTS_SM3_DIGEST_BYTES == 32u, "SM3 ABI mismatch");
static_assert(SALTS_HKDF_SM3_MAX_OUTPUT_BYTES == 8160u, "HKDF-SM3 bound mismatch");

int main() {
  std::uint8_t digest[SALTS_SM3_DIGEST_BYTES] = {};
  std::uint8_t okm[16] = {};
  cmeta_sm3_stream *stream = nullptr;
  if (cmeta_sm3("abc", std::size_t{3}, digest) != SALTS_OK) return 1;
  if (cmeta_hmac_sm3("key", std::size_t{3}, "data", std::size_t{4}, digest) != SALTS_OK) return 2;
  if (cmeta_hkdf_sm3("ikm", std::size_t{3}, nullptr, std::size_t{0},
                      nullptr, std::size_t{0}, okm, sizeof(okm)) != SALTS_OK) return 3;
  if (cmeta_sm3_stream_create(&stream) != SALTS_OK || stream == nullptr) return 4;
  if (cmeta_sm3_stream_reset(stream) != SALTS_OK) return 5;
  cmeta_sm3_stream_destroy(stream);

  if (cmeta_sm2_verify(
          SALTS_INSTALL_SM2_PUBLIC_SPKI, sizeof(SALTS_INSTALL_SM2_PUBLIC_SPKI),
          SALTS_INSTALL_SM2_USER_ID, sizeof(SALTS_INSTALL_SM2_USER_ID) - 1u,
          SALTS_INSTALL_SM2_MESSAGE, sizeof(SALTS_INSTALL_SM2_MESSAGE) - 1u,
          SALTS_INSTALL_SM2_SIGNATURE, sizeof(SALTS_INSTALL_SM2_SIGNATURE)) != SALTS_OK)
    return 6;

  {
    std::uint8_t ciphertext[sizeof(SALTS_INSTALL_SM4_GCM_CIPHERTEXT)] = {};
    std::uint8_t tag[SALTS_SM4_GCM_TAG_BYTES] = {};
    std::size_t ciphertext_size = 0u;
    if (cmeta_sm4_gcm_seal(
            SALTS_INSTALL_SM4_GCM_KEY, SALTS_INSTALL_SM4_GCM_NONCE,
            SALTS_INSTALL_SM4_GCM_AAD, sizeof(SALTS_INSTALL_SM4_GCM_AAD),
            SALTS_INSTALL_SM4_GCM_PLAINTEXT, sizeof(SALTS_INSTALL_SM4_GCM_PLAINTEXT),
            ciphertext, sizeof(ciphertext), &ciphertext_size, tag) != SALTS_OK)
      return 10;
    if (ciphertext_size != sizeof(ciphertext) ||
        std::memcmp(ciphertext, SALTS_INSTALL_SM4_GCM_CIPHERTEXT, sizeof(ciphertext)) != 0 ||
        std::memcmp(tag, SALTS_INSTALL_SM4_GCM_TAG, sizeof(tag)) != 0)
      return 11;
  }
  {
    static const std::uint8_t wrong_id[] = "1234567812345679";
    std::uint8_t private_key_stub[] = {0x30u};
    std::uint8_t output[SALTS_SM2_SIGNATURE_DER_MAX_BYTES] = {};
    std::uint8_t before[sizeof(output)] = {};
    std::size_t signature_size = 0x1234u;

    if (cmeta_sm2_verify(
            SALTS_INSTALL_SM2_PUBLIC_SPKI, sizeof(SALTS_INSTALL_SM2_PUBLIC_SPKI),
            wrong_id, sizeof(wrong_id) - 1u,
            SALTS_INSTALL_SM2_MESSAGE, sizeof(SALTS_INSTALL_SM2_MESSAGE) - 1u,
            SALTS_INSTALL_SM2_SIGNATURE, sizeof(SALTS_INSTALL_SM2_SIGNATURE)) != SALTS_EAUTH)
      return 7;
    std::memset(output, 0xa5, sizeof(output));
    std::memcpy(before, output, sizeof(before));
    if (cmeta_sm2_sign(
            private_key_stub, sizeof(private_key_stub),
            SALTS_INSTALL_SM2_USER_ID, sizeof(SALTS_INSTALL_SM2_USER_ID) - 1u,
            SALTS_INSTALL_SM2_MESSAGE, sizeof(SALTS_INSTALL_SM2_MESSAGE) - 1u,
            output, sizeof(output) - 1u, &signature_size) != SALTS_ERANGE)
      return 8;
    if (signature_size != SALTS_SM2_SIGNATURE_DER_MAX_BYTES ||
        std::memcmp(output, before, sizeof(output)) != 0)
      return 9;
  }
  return 0;
}
