#include <stdint.h>
#include <string.h>

#include <gmssl/mem.h>
#include <gmssl/sm2.h>

#include "cmeta_crypto.h"
#include "tinytest.h"

static const uint8_t SM2_GMT_PUBLIC_SPKI[] = {
    0x30, 0x59, 0x30, 0x13,
    0x06, 0x07, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x02, 0x01,
    0x06, 0x08, 0x2a, 0x81, 0x1c, 0xcf, 0x55, 0x01, 0x82, 0x2d,
    0x03, 0x42, 0x00, 0x04,
    0x09, 0xf9, 0xdf, 0x31, 0x1e, 0x54, 0x21, 0xa1,
    0x50, 0xdd, 0x7d, 0x16, 0x1e, 0x4b, 0xc5, 0xc6,
    0x72, 0x17, 0x9f, 0xad, 0x18, 0x33, 0xfc, 0x07,
    0x6b, 0xb0, 0x8f, 0xf3, 0x56, 0xf3, 0x50, 0x20,
    0xcc, 0xea, 0x49, 0x0c, 0xe2, 0x67, 0x75, 0xa5,
    0x2d, 0xc6, 0xea, 0x71, 0x8c, 0xc1, 0xaa, 0x60,
    0x0a, 0xed, 0x05, 0xfb, 0xf3, 0x5e, 0x08, 0x4a,
    0x66, 0x32, 0xf6, 0x07, 0x2d, 0xa9, 0xad, 0x13};

static const uint8_t SM2_GMT_SIGNATURE[] = {
    0x30, 0x46, 0x02, 0x21, 0x00,
    0xf5, 0xa0, 0x3b, 0x06, 0x48, 0xd2, 0xc4, 0x63,
    0x0e, 0xea, 0xc5, 0x13, 0xe1, 0xbb, 0x81, 0xa1,
    0x59, 0x44, 0xda, 0x38, 0x27, 0xd5, 0xb7, 0x41,
    0x43, 0xac, 0x7e, 0xac, 0xee, 0xe7, 0x20, 0xb3,
    0x02, 0x21, 0x00,
    0xb1, 0xb6, 0xaa, 0x29, 0xdf, 0x21, 0x2f, 0xd8,
    0x76, 0x31, 0x82, 0xbc, 0x0d, 0x42, 0x1c, 0xa1,
    0xbb, 0x90, 0x38, 0xfd, 0x1f, 0x7f, 0x42, 0xd4,
    0x84, 0x0b, 0x69, 0xc4, 0x85, 0xbb, 0xc1, 0xaa};

static const uint8_t SM2_GMT_USER_ID[] = "1234567812345678";
static const uint8_t SM2_GMT_MESSAGE[] = "message digest";

suite("CMeta SM2") {
  group("known-answer verification") {
    it("verifies the GM/T sample using the explicit identity") {
      check_equal(cmeta_sm2_verify(
                      SM2_GMT_PUBLIC_SPKI, sizeof(SM2_GMT_PUBLIC_SPKI),
                      SM2_GMT_USER_ID, sizeof(SM2_GMT_USER_ID) - 1u,
                      SM2_GMT_MESSAGE, sizeof(SM2_GMT_MESSAGE) - 1u,
                      SM2_GMT_SIGNATURE, sizeof(SM2_GMT_SIGNATURE)),
                  SALTS_OK);
    }

    it("rejects a wrong identity, altered signature and trailing bytes") {
      static const uint8_t wrong_id[] = "1234567812345679";
      uint8_t signature[sizeof(SM2_GMT_SIGNATURE) + 1u];

      memcpy(signature, SM2_GMT_SIGNATURE, sizeof(SM2_GMT_SIGNATURE));
      check_equal(cmeta_sm2_verify(
                      SM2_GMT_PUBLIC_SPKI, sizeof(SM2_GMT_PUBLIC_SPKI),
                      wrong_id, sizeof(wrong_id) - 1u,
                      SM2_GMT_MESSAGE, sizeof(SM2_GMT_MESSAGE) - 1u,
                      signature, sizeof(SM2_GMT_SIGNATURE)),
                  SALTS_EAUTH);
      signature[sizeof(SM2_GMT_SIGNATURE) - 1u] ^= 1u;
      check_equal(cmeta_sm2_verify(
                      SM2_GMT_PUBLIC_SPKI, sizeof(SM2_GMT_PUBLIC_SPKI),
                      SM2_GMT_USER_ID, sizeof(SM2_GMT_USER_ID) - 1u,
                      SM2_GMT_MESSAGE, sizeof(SM2_GMT_MESSAGE) - 1u,
                      signature, sizeof(SM2_GMT_SIGNATURE)),
                  SALTS_EAUTH);
      memcpy(signature, SM2_GMT_SIGNATURE, sizeof(SM2_GMT_SIGNATURE));
      signature[sizeof(SM2_GMT_SIGNATURE)] = 0u;
      check_equal(cmeta_sm2_verify(
                      SM2_GMT_PUBLIC_SPKI, sizeof(SM2_GMT_PUBLIC_SPKI),
                      SM2_GMT_USER_ID, sizeof(SM2_GMT_USER_ID) - 1u,
                      SM2_GMT_MESSAGE, sizeof(SM2_GMT_MESSAGE) - 1u,
                      signature, sizeof(signature)),
                  SALTS_EAUTH);
    }

    it("rejects malformed public-key DER") {
      static const uint8_t malformed_key[] = {0x30, 0x81, 0x01, 0x00};
      check_equal(cmeta_sm2_verify(
                      malformed_key, sizeof(malformed_key),
                      SM2_GMT_USER_ID, sizeof(SM2_GMT_USER_ID) - 1u,
                      SM2_GMT_MESSAGE, sizeof(SM2_GMT_MESSAGE) - 1u,
                      SM2_GMT_SIGNATURE, sizeof(SM2_GMT_SIGNATURE)),
                  SALTS_EINVAL);
    }
  }

  group("signing contract") {
    it("signs and verifies with a generated key and explicit identity") {
      SM2_KEY provider_key = {0};
      uint8_t private_der[512] = {0};
      uint8_t public_der[256] = {0};
      uint8_t signature[SALTS_SM2_SIGNATURE_DER_MAX_BYTES] = {0};
      uint8_t signature_before[SALTS_SM2_SIGNATURE_DER_MAX_BYTES];
      uint8_t message[] = {0x61, 0x00, 0x62, 0x63};
      uint8_t user_id[] = {0x69, 0x64, 0x00, 0x31};
      uint8_t private_der_with_trailing[sizeof(private_der) + 1u];
      uint8_t private_der_bad[sizeof(private_der)];
      uint8_t *private_cursor = private_der;
      uint8_t *public_cursor = public_der;
      size_t private_size = 0u;
      size_t public_size = 0u;
      size_t signature_size = 0u;
      size_t required_size = 0u;

      check_equal(sm2_key_generate(&provider_key), 1);
      check_equal(sm2_private_key_info_to_der(&provider_key, &private_cursor,
                                               &private_size), 1);
      check_equal(sm2_public_key_info_to_der(&provider_key, &public_cursor,
                                              &public_size), 1);
      check_true(private_size > 0u && private_size <= sizeof(private_der));
      check_true(public_size > 0u && public_size <= sizeof(public_der));

      memset(signature, 0xa5, sizeof(signature));
      memcpy(signature_before, signature, sizeof(signature));
      check_equal(cmeta_sm2_sign(
                      private_der, private_size, user_id, sizeof(user_id),
                      message, sizeof(message), signature,
                      SALTS_SM2_SIGNATURE_DER_MAX_BYTES - 1u, &required_size),
                  SALTS_ERANGE);
      check_equal(required_size, (size_t)SALTS_SM2_SIGNATURE_DER_MAX_BYTES);
      check_equal(memcmp(signature, signature_before, sizeof(signature)), 0);

      check_equal(cmeta_sm2_sign(
                      private_der, private_size, user_id, sizeof(user_id),
                      message, sizeof(message), signature, sizeof(signature),
                      &signature_size),
                  SALTS_OK);
      check_true(signature_size > 0u && signature_size <= sizeof(signature));
      check_equal(cmeta_sm2_verify(
                      public_der, public_size, user_id, sizeof(user_id),
                      message, sizeof(message), signature, signature_size),
                  SALTS_OK);

      message[0] ^= 1u;
      check_equal(cmeta_sm2_verify(
                      public_der, public_size, user_id, sizeof(user_id),
                      message, sizeof(message), signature, signature_size),
                  SALTS_EAUTH);
      message[0] ^= 1u;

      memcpy(private_der_with_trailing, private_der, private_size);
      private_der_with_trailing[private_size] = 0u;
      required_size = 0x1234u;
      check_equal(cmeta_sm2_sign(
                      private_der_with_trailing, private_size + 1u,
                      user_id, sizeof(user_id), message, sizeof(message),
                      signature, sizeof(signature), &required_size),
                  SALTS_EINVAL);
      check_equal(required_size, (size_t)0x1234u);

      memcpy(private_der_bad, private_der, private_size);
      private_der_bad[0] ^= 0x80u;
      check_equal(cmeta_sm2_sign(
                      private_der_bad, private_size, user_id, sizeof(user_id),
                      message, sizeof(message), signature, sizeof(signature),
                      &required_size),
                  SALTS_EINVAL);

      gmssl_secure_clear(&provider_key, sizeof(provider_key));
      gmssl_secure_clear(private_der, sizeof(private_der));
      gmssl_secure_clear(public_der, sizeof(public_der));
      gmssl_secure_clear(signature, sizeof(signature));
      gmssl_secure_clear(signature_before, sizeof(signature_before));
      gmssl_secure_clear(private_der_with_trailing, sizeof(private_der_with_trailing));
      gmssl_secure_clear(private_der_bad, sizeof(private_der_bad));
    }
  }
}
