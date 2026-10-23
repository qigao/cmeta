#include <stdint.h>
#include <string.h>

#include "cmeta_crypto.h"
#include "tinytest.h"

static const uint8_t MD5_ABC[SALTS_MD5_DIGEST_BYTES] = {
    0x90, 0x01, 0x50, 0x98, 0x3c, 0xd2, 0x4f, 0xb0,
    0xd6, 0x96, 0x3f, 0x7d, 0x28, 0xe1, 0x7f, 0x72};

static const uint8_t SHA1_ABC[SALTS_SHA1_DIGEST_BYTES] = {
    0xa9, 0x99, 0x3e, 0x36, 0x47, 0x06, 0x81, 0x6a, 0xba, 0x3e,
    0x25, 0x71, 0x78, 0x50, 0xc2, 0x6c, 0x9c, 0xd0, 0xd8, 0x9d};

static const uint8_t SHA256_ABC[SALTS_SHA256_DIGEST_BYTES] = {
    0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea,
    0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
    0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c,
    0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad};

static const uint8_t HMAC_MD5_HI_THERE[SALTS_MD5_DIGEST_BYTES] = {
    0x92, 0x94, 0x72, 0x7a, 0x36, 0x38, 0xbb, 0x1c,
    0x13, 0xf4, 0x8e, 0xf8, 0x15, 0x8b, 0xfc, 0x9d};

static const uint8_t DES_CBC_KEY[SALTS_DES_KEY_BYTES] = {
    0x01,0x23,0x45,0x67,0x89,0xab,0xcd,0xef};

static const uint8_t DES_CBC_IV[SALTS_DES_BLOCK_BYTES] = {
    0x12,0x34,0x56,0x78,0x90,0xab,0xcd,0xef};

static const uint8_t DES_CBC_PLAINTEXT[24] = {
    0x4e,0x6f,0x77,0x20,0x69,0x73,0x20,0x74,
    0x68,0x65,0x20,0x74,0x69,0x6d,0x65,0x20,
    0x66,0x6f,0x72,0x20,0x61,0x6c,0x6c,0x20};

static const uint8_t DES_CBC_CIPHERTEXT[24] = {
    0xe5,0xc7,0xcd,0xde,0x87,0x2b,0xf2,0x7c,
    0x43,0xe9,0x34,0x00,0x8c,0x38,0x9c,0x0f,
    0x68,0x37,0x88,0x49,0x9a,0x7c,0x05,0xf6};

static const uint8_t AES128_CFB_KEY[SALTS_AES128_KEY_BYTES] = {
    0x2b, 0x7e, 0x15, 0x16, 0x28, 0xae, 0xd2, 0xa6,
    0xab, 0xf7, 0x15, 0x88, 0x09, 0xcf, 0x4f, 0x3c};

static const uint8_t AES128_CFB_IV[SALTS_AES_BLOCK_BYTES] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f};

static const uint8_t AES128_CFB_PLAINTEXT[64] = {
    0x6b,0xc1,0xbe,0xe2,0x2e,0x40,0x9f,0x96,0xe9,0x3d,0x7e,0x11,0x73,0x93,0x17,0x2a,
    0xae,0x2d,0x8a,0x57,0x1e,0x03,0xac,0x9c,0x9e,0xb7,0x6f,0xac,0x45,0xaf,0x8e,0x51,
    0x30,0xc8,0x1c,0x46,0xa3,0x5c,0xe4,0x11,0xe5,0xfb,0xc1,0x19,0x1a,0x0a,0x52,0xef,
    0xf6,0x9f,0x24,0x45,0xdf,0x4f,0x9b,0x17,0xad,0x2b,0x41,0x7b,0xe6,0x6c,0x37,0x10};

static const uint8_t AES128_CFB_CIPHERTEXT[64] = {
    0x3b,0x3f,0xd9,0x2e,0xb7,0x2d,0xad,0x20,0x33,0x34,0x49,0xf8,0xe8,0x3c,0xfb,0x4a,
    0xc8,0xa6,0x45,0x37,0xa0,0xb3,0xa9,0x3f,0xcd,0xe3,0xcd,0xad,0x9f,0x1c,0xe5,0x8b,
    0x26,0x75,0x1f,0x67,0xa3,0xcb,0xb1,0x40,0xb1,0x80,0x8c,0xf1,0x87,0xa4,0xf4,0xdf,
    0xc0,0x4b,0x05,0x35,0x7c,0x5d,0x1c,0x0e,0xea,0xc4,0xc6,0x6f,0x9f,0xf7,0xf2,0xe6};

static const uint8_t HMAC_SHA1_HI_THERE[SALTS_SHA1_DIGEST_BYTES] = {
    0xb6, 0x17, 0x31, 0x86, 0x55, 0x05, 0x72, 0x64, 0xe2, 0x8b,
    0xc0, 0xb6, 0xfb, 0x37, 0x8c, 0x8e, 0xf1, 0x46, 0xbe, 0x00};

static const uint8_t HMAC_SHA256_HI_THERE[SALTS_SHA256_DIGEST_BYTES] = {
    0xb0, 0x34, 0x4c, 0x61, 0xd8, 0xdb, 0x38, 0x53,
    0x5c, 0xa8, 0xaf, 0xce, 0xaf, 0x0b, 0xf1, 0x2b,
    0x88, 0x1d, 0xc2, 0x00, 0xc9, 0x83, 0x3d, 0xa7,
    0x26, 0xe9, 0x37, 0x6c, 0x2e, 0x32, 0xcf, 0xf7};

static const uint8_t SM3_ABC[SALTS_SM3_DIGEST_BYTES] = {
    0x66,0xc7,0xf0,0xf4,0x62,0xee,0xed,0xd9,
    0xd1,0xf2,0xd4,0x6b,0xdc,0x10,0xe4,0xe2,
    0x41,0x67,0xc4,0x87,0x5c,0xf2,0xf7,0xa2,
    0x29,0x7d,0xa0,0x2b,0x8f,0x4b,0xa8,0xe0};

static const uint8_t SM3_ABCD_16[SALTS_SM3_DIGEST_BYTES] = {
    0xde,0xbe,0x9f,0xf9,0x22,0x75,0xb8,0xa1,
    0x38,0x60,0x48,0x89,0xc1,0x8e,0x5a,0x4d,
    0x6f,0xdb,0x70,0xe5,0x38,0x7e,0x57,0x65,
    0x29,0x3d,0xcb,0xa3,0x9c,0x0c,0x57,0x32};

static const uint8_t HMAC_SM3_HI_THERE[SALTS_SM3_DIGEST_BYTES] = {
    0x51,0xb0,0x0d,0x1f,0xb4,0x98,0x32,0xbf,
    0xb0,0x1c,0x3c,0xe2,0x78,0x48,0xe5,0x9f,
    0x87,0x1d,0x9b,0xa9,0x38,0xdc,0x56,0x3b,
    0x33,0x8c,0xa9,0x64,0x75,0x5c,0xce,0x70};

static const uint8_t HKDF_SM3_OKM_32[SALTS_SM3_DIGEST_BYTES] = {
    0x18,0x1a,0xd1,0x15,0x65,0x7b,0x29,0xaa,
    0x30,0x6e,0xc1,0x54,0x87,0xf8,0x3d,0x95,
    0xf6,0xa9,0xe1,0x23,0xb7,0x8d,0xe5,0xfa,
    0x2c,0x2a,0x4d,0x6a,0x29,0x9e,0xc6,0x53};

static const uint8_t SM3_EMPTY[SALTS_SM3_DIGEST_BYTES] = {
    0x1a,0xb2,0x1d,0x83,0x55,0xcf,0xa1,0x7f,
    0x8e,0x61,0x19,0x48,0x31,0xe8,0x1a,0x8f,
    0x22,0xbe,0xc8,0xc7,0x28,0xfe,0xfb,0x74,
    0x7e,0xd0,0x35,0xeb,0x50,0x82,0xaa,0x2b};

static const uint8_t SM3_EMBEDDED_NUL[SALTS_SM3_DIGEST_BYTES] = {
    0xcf,0x51,0xd7,0x99,0xee,0x61,0x3d,0xae,
    0xb2,0x66,0xee,0x73,0x20,0x3c,0x6b,0xde,
    0xfd,0xc3,0xfd,0xaa,0x20,0xbf,0x62,0xd1,
    0x4a,0xbe,0x01,0xcc,0x6e,0x8d,0x61,0x2d};

static const uint8_t SM3_BYTES_0_TO_255[SALTS_SM3_DIGEST_BYTES] = {
    0x59,0xd1,0x71,0xdb,0xfd,0x25,0x1d,0x5a,
    0x4c,0xd7,0x7d,0x6b,0xa2,0xb7,0x10,0x9b,
    0x7d,0x64,0xa4,0xcd,0x7f,0xa8,0x18,0x2b,
    0xeb,0x10,0x0a,0x01,0x6f,0xa3,0xac,0x44};

static const uint8_t HMAC_SM3_EMPTY[SALTS_SM3_DIGEST_BYTES] = {
    0x0d,0x23,0xf7,0x2b,0xa1,0x5e,0x9c,0x18,
    0x9a,0x87,0x9a,0xef,0xc7,0x09,0x96,0xb0,
    0x60,0x91,0xde,0x6e,0x64,0xd3,0x1b,0x7a,
    0x84,0x00,0x43,0x56,0xdd,0x91,0x52,0x61};

static const uint8_t HMAC_SM3_EMBEDDED_NUL[SALTS_SM3_DIGEST_BYTES] = {
    0x91,0xd5,0x43,0xd3,0x95,0x0d,0xac,0xbe,
    0x5b,0x3f,0x4e,0x0d,0x48,0xa5,0xdf,0x43,
    0x18,0x1c,0x63,0x71,0x21,0xfd,0xab,0x27,
    0x51,0xab,0x4c,0xe0,0x78,0x0c,0x1a,0x34};

static const uint8_t HKDF_SM3_EMBEDDED_NUL_32[SALTS_SM3_DIGEST_BYTES] = {
    0x86,0x97,0x35,0x0e,0xee,0xf5,0x86,0xf4,
    0x16,0x2c,0x18,0x0f,0xb6,0x51,0x5b,0x1b,
    0xa4,0x8c,0xe2,0x9f,0x82,0xad,0xa2,0xaf,
    0xd8,0xfd,0xe8,0x0b,0x07,0x6f,0x86,0x30};

/* SM3 digest of the full 8160-byte empty-input HKDF-SM3 output, independently
 * cross-checked with OpenSSL 3.5.5. */
static const uint8_t HKDF_SM3_EMPTY_MAX_OUTPUT_DIGEST[SALTS_SM3_DIGEST_BYTES] = {
    0xa6,0x6a,0xeb,0xc3,0x27,0x29,0x06,0xb5,
    0x1b,0x10,0x2a,0x13,0xf6,0x5e,0x10,0x9d,
    0xeb,0x79,0x23,0xa1,0xce,0x2b,0xdc,0xa2,
    0xf3,0xd9,0x9b,0x7a,0x1c,0x72,0xa9,0xce};

spec("cmeta_crypto") {
  it("computes legacy MD5 for the standard abc vector") {
    uint8_t digest[SALTS_MD5_DIGEST_BYTES];
    check_equal(cmeta_md5("abc", 3u, digest), SALTS_OK);
    check_equal(memcmp(digest, MD5_ABC, sizeof(digest)), 0);
  }

  it("computes SHA-1 for the standard abc vector") {
    uint8_t digest[SALTS_SHA1_DIGEST_BYTES];
    check_equal(cmeta_sha1("abc", 3u, digest), SALTS_OK);
    check_equal(memcmp(digest, SHA1_ABC, sizeof(digest)), 0);
  }

  it("computes SHA-256 for the standard abc vector") {
    uint8_t digest[SALTS_SHA256_DIGEST_BYTES];
    check_equal(cmeta_sha256("abc", 3u, digest), SALTS_OK);
    check_equal(memcmp(digest, SHA256_ABC, sizeof(digest)), 0);
  }

  it("computes SHA-256 incrementally with bounded chunks") {
    cmeta_sha256_stream *stream = NULL;
    uint8_t digest[SALTS_SHA256_DIGEST_BYTES];

    check_equal(cmeta_sha256_stream_create(&stream), SALTS_OK);
    check(stream != NULL);
    check_equal(cmeta_sha256_stream_update(stream, "a", 1u), SALTS_OK);
    check_equal(cmeta_sha256_stream_update(stream, NULL, 0u), SALTS_OK);
    check_equal(cmeta_sha256_stream_update(stream, "bc", 2u), SALTS_OK);
    check_equal(cmeta_sha256_stream_finish(stream, digest), SALTS_OK);
    check_equal(memcmp(digest, SHA256_ABC, sizeof(digest)), 0);
    check_equal(cmeta_sha256_stream_update(stream, "x", 1u), SALTS_EINVAL);
    check_equal(cmeta_sha256_stream_finish(stream, digest), SALTS_EINVAL);

    check_equal(cmeta_sha256_stream_reset(stream), SALTS_OK);
    check_equal(cmeta_sha256_stream_update(stream, "abc", 3u), SALTS_OK);
    check_equal(cmeta_sha256_stream_finish(stream, digest), SALTS_OK);
    check_equal(memcmp(digest, SHA256_ABC, sizeof(digest)), 0);
    cmeta_sha256_stream_destroy(stream);
  }

  it("validates streaming SHA-256 lifecycle") {
    cmeta_sha256_stream *stream = NULL;
    uint8_t digest[SALTS_SHA256_DIGEST_BYTES];

    check_equal(cmeta_sha256_stream_create(NULL), SALTS_EINVAL);
    check_equal(cmeta_sha256_stream_create(&stream), SALTS_OK);
    check_equal(cmeta_sha256_stream_update(stream, NULL, 1u), SALTS_EINVAL);
    check_equal(cmeta_sha256_stream_finish(stream, NULL), SALTS_EINVAL);
    check_equal(cmeta_sha256_stream_reset(NULL), SALTS_EINVAL);
    cmeta_sha256_stream_destroy(stream);
    cmeta_sha256_stream_destroy(NULL);
  }

  it("computes official SM3 abc and 64-byte standard vectors") {
    static const uint8_t long_message[] =
        "abcdabcdabcdabcdabcdabcdabcdabcd"
        "abcdabcdabcdabcdabcdabcdabcdabcd";
    uint8_t digest[SALTS_SM3_DIGEST_BYTES];

    check_equal(cmeta_sm3("abc", 3u, digest), SALTS_OK);
    check_equal(memcmp(digest, SM3_ABC, sizeof(digest)), 0);
    check_equal(cmeta_sm3(long_message, sizeof(long_message) - 1u, digest), SALTS_OK);
    check_equal(memcmp(digest, SM3_ABCD_16, sizeof(digest)), 0);
  }

  it("computes empty, embedded-NUL and binary SM3 vectors") {
    static const uint8_t embedded_nul[] = {'a', 0u, 'b', 'c'};
    uint8_t binary_message[256];
    uint8_t digest[SALTS_SM3_DIGEST_BYTES];

    for (size_t i = 0u; i < sizeof(binary_message); ++i)
      binary_message[i] = (uint8_t)i;

    check_equal(cmeta_sm3(NULL, 0u, digest), SALTS_OK);
    check_equal(memcmp(digest, SM3_EMPTY, sizeof(digest)), 0);
    check_equal(cmeta_sm3(embedded_nul, sizeof(embedded_nul), digest), SALTS_OK);
    check_equal(memcmp(digest, SM3_EMBEDDED_NUL, sizeof(digest)), 0);
    check_equal(cmeta_sm3(binary_message, sizeof(binary_message), digest), SALTS_OK);
    check_equal(memcmp(digest, SM3_BYTES_0_TO_255, sizeof(digest)), 0);
  }

  it("computes SM3 incrementally and enforces reset-after-finish") {
    cmeta_sm3_stream *stream = NULL;
    uint8_t digest[SALTS_SM3_DIGEST_BYTES];

    check_equal(cmeta_sm3_stream_create(&stream), SALTS_OK);
    check_not_null(stream);
    check_equal(cmeta_sm3_stream_update(stream, "a", 1u), SALTS_OK);
    check_equal(cmeta_sm3_stream_update(stream, NULL, 0u), SALTS_OK);
    check_equal(cmeta_sm3_stream_update(stream, "bc", 2u), SALTS_OK);
    check_equal(cmeta_sm3_stream_finish(stream, digest), SALTS_OK);
    check_equal(memcmp(digest, SM3_ABC, sizeof(digest)), 0);
    check_equal(cmeta_sm3_stream_update(stream, "x", 1u), SALTS_EINVAL);
    check_equal(cmeta_sm3_stream_finish(stream, digest), SALTS_EINVAL);
    check_equal(cmeta_sm3_stream_reset(stream), SALTS_OK);
    check_equal(cmeta_sm3_stream_update(stream, "abc", 3u), SALTS_OK);
    check_equal(cmeta_sm3_stream_finish(stream, digest), SALTS_OK);
    check_equal(memcmp(digest, SM3_ABC, sizeof(digest)), 0);
    cmeta_sm3_stream_destroy(stream);
  }

  it("computes independent HMAC-SM3 and HKDF-SM3 vectors") {
    static const uint8_t hkdf_salt[] = "salt";
    static const uint8_t hkdf_ikm[] = "input key material";
    static const uint8_t hkdf_info[] = "context";
    uint8_t key[20];
    uint8_t mac[SALTS_SM3_DIGEST_BYTES];
    uint8_t okm[SALTS_SM3_DIGEST_BYTES];

    memset(key, 0x0b, sizeof(key));
    check_equal(cmeta_hmac_sm3(key, sizeof(key), "Hi There", 8u, mac), SALTS_OK);
    check_equal(memcmp(mac, HMAC_SM3_HI_THERE, sizeof(mac)), 0);

    check_equal(cmeta_hkdf_sm3(
                    hkdf_ikm, sizeof(hkdf_ikm) - 1u,
                    hkdf_salt, sizeof(hkdf_salt) - 1u,
                    hkdf_info, sizeof(hkdf_info) - 1u,
                    okm, sizeof(okm)),
                SALTS_OK);
    check_equal(memcmp(okm, HKDF_SM3_OKM_32, sizeof(okm)), 0);
  }

  it("keeps SM3 family outputs unchanged on rejected arguments") {
    uint8_t digest[SALTS_SM3_DIGEST_BYTES];
    uint8_t before[SALTS_SM3_DIGEST_BYTES];
    cmeta_sm3_stream *stream = NULL;

    memset(digest, 0xa5, sizeof(digest));
    memcpy(before, digest, sizeof(before));
    check_equal(cmeta_sm3(NULL, 1u, digest), SALTS_EINVAL);
    check_equal(memcmp(digest, before, sizeof(digest)), 0);
    check_equal(cmeta_hmac_sm3(NULL, 1u, NULL, 0u, digest), SALTS_EINVAL);
    check_equal(memcmp(digest, before, sizeof(digest)), 0);
    check_equal(cmeta_hkdf_sm3(
                    "ikm", 3u, NULL, 0u, NULL, 0u,
                    digest, SALTS_HKDF_SM3_MAX_OUTPUT_BYTES + 1u),
                SALTS_ERANGE);
    check_equal(memcmp(digest, before, sizeof(digest)), 0);

    check_equal(cmeta_sm3_stream_create(NULL), SALTS_EINVAL);
    check_equal(cmeta_sm3_stream_create(&stream), SALTS_OK);
    check_equal(cmeta_sm3_stream_update(stream, NULL, 1u), SALTS_EINVAL);
    check_equal(cmeta_sm3_stream_finish(stream, NULL), SALTS_EINVAL);
    check_equal(cmeta_sm3_stream_reset(NULL), SALTS_EINVAL);
    cmeta_sm3_stream_destroy(stream);
    cmeta_sm3_stream_destroy(NULL);

    check_equal(cmeta_hmac_sm3(NULL, 0u, NULL, 0u, digest), SALTS_OK);
    check_equal(cmeta_hkdf_sm3(NULL, 0u, NULL, 0u, NULL, 0u, NULL, 0u), SALTS_OK);
  }

  it("computes RFC 2202 HMAC-MD5") {
    uint8_t key[16];
    uint8_t mac[SALTS_MD5_DIGEST_BYTES];
    memset(key, 0x0b, sizeof(key));

    check_equal(cmeta_hmac_md5(key, sizeof(key), "Hi There", 8u, mac), SALTS_OK);
    check_equal(memcmp(mac, HMAC_MD5_HI_THERE, sizeof(mac)), 0);
  }

  it("computes legacy DES-CBC known vector and decrypt round-trip") {
    uint8_t encrypted[sizeof(DES_CBC_PLAINTEXT)];
    uint8_t decrypted[sizeof(DES_CBC_PLAINTEXT)];

    check_equal(cmeta_des_cbc_encrypt(
                    DES_CBC_KEY, DES_CBC_IV,
                    DES_CBC_PLAINTEXT, sizeof(DES_CBC_PLAINTEXT), encrypted),
                SALTS_OK);
    check_equal(memcmp(encrypted, DES_CBC_CIPHERTEXT, sizeof(encrypted)), 0);

    check_equal(cmeta_des_cbc_decrypt(
                    DES_CBC_KEY, DES_CBC_IV,
                    encrypted, sizeof(encrypted), decrypted),
                SALTS_OK);
    check_equal(memcmp(decrypted, DES_CBC_PLAINTEXT, sizeof(decrypted)), 0);
  }

  it("supports in-place legacy DES-CBC") {
    uint8_t buffer[sizeof(DES_CBC_PLAINTEXT)];
    memcpy(buffer, DES_CBC_PLAINTEXT, sizeof(buffer));

    check_equal(cmeta_des_cbc_encrypt(
                    DES_CBC_KEY, DES_CBC_IV, buffer, sizeof(buffer), buffer),
                SALTS_OK);
    check_equal(memcmp(buffer, DES_CBC_CIPHERTEXT, sizeof(buffer)), 0);
    check_equal(cmeta_des_cbc_decrypt(
                    DES_CBC_KEY, DES_CBC_IV, buffer, sizeof(buffer), buffer),
                SALTS_OK);
    check_equal(memcmp(buffer, DES_CBC_PLAINTEXT, sizeof(buffer)), 0);
  }

  it("computes NIST AES-128-CFB128 encrypt and decrypt vectors") {
    uint8_t encrypted[sizeof(AES128_CFB_PLAINTEXT)];
    uint8_t decrypted[sizeof(AES128_CFB_PLAINTEXT)];

    check_equal(cmeta_aes128_cfb_encrypt(
                    AES128_CFB_KEY, AES128_CFB_IV,
                    AES128_CFB_PLAINTEXT, sizeof(AES128_CFB_PLAINTEXT), encrypted),
                SALTS_OK);
    check_equal(memcmp(encrypted, AES128_CFB_CIPHERTEXT, sizeof(encrypted)), 0);

    check_equal(cmeta_aes128_cfb_decrypt(
                    AES128_CFB_KEY, AES128_CFB_IV,
                    encrypted, sizeof(encrypted), decrypted),
                SALTS_OK);
    check_equal(memcmp(decrypted, AES128_CFB_PLAINTEXT, sizeof(decrypted)), 0);
  }

  it("supports in-place AES-128-CFB128") {
    uint8_t buffer[sizeof(AES128_CFB_PLAINTEXT)];
    memcpy(buffer, AES128_CFB_PLAINTEXT, sizeof(buffer));

    check_equal(cmeta_aes128_cfb_encrypt(
                    AES128_CFB_KEY, AES128_CFB_IV, buffer, sizeof(buffer), buffer),
                SALTS_OK);
    check_equal(memcmp(buffer, AES128_CFB_CIPHERTEXT, sizeof(buffer)), 0);
    check_equal(cmeta_aes128_cfb_decrypt(
                    AES128_CFB_KEY, AES128_CFB_IV, buffer, sizeof(buffer), buffer),
                SALTS_OK);
    check_equal(memcmp(buffer, AES128_CFB_PLAINTEXT, sizeof(buffer)), 0);
  }

  it("computes RFC 2202 HMAC-SHA1") {
    uint8_t key[20];
    uint8_t mac[SALTS_SHA1_DIGEST_BYTES];
    memset(key, 0x0b, sizeof(key));

    check_equal(cmeta_hmac_sha1(key, sizeof(key), "Hi There", 8u, mac), SALTS_OK);
    check_equal(memcmp(mac, HMAC_SHA1_HI_THERE, sizeof(mac)), 0);
  }

  it("computes RFC 4231 HMAC-SHA256") {
    uint8_t key[20];
    uint8_t mac[SALTS_SHA256_DIGEST_BYTES];
    memset(key, 0x0b, sizeof(key));

    check_equal(cmeta_hmac_sha256(key, sizeof(key), "Hi There", 8u, mac), SALTS_OK);
    check_equal(memcmp(mac, HMAC_SHA256_HI_THERE, sizeof(mac)), 0);
  }

  it("compares fixed ranges without exposing provider semantics") {
    const uint8_t same_a[] = {1u, 2u, 3u, 4u};
    const uint8_t same_b[] = {1u, 2u, 3u, 4u};
    const uint8_t different[] = {1u, 2u, 3u, 5u};
    int equal = 0;

    check_equal(cmeta_crypto_equal(same_a, same_b, sizeof(same_a), &equal), SALTS_OK);
    check_equal(equal, 1);
    check_equal(cmeta_crypto_equal(same_a, different, sizeof(same_a), &equal), SALTS_OK);
    check_equal(equal, 0);
    check_equal(cmeta_crypto_equal(NULL, NULL, 0u, &equal), SALTS_OK);
    check_equal(equal, 1);
  }

  it("securely clears caller-owned memory") {
    uint8_t secret[] = {1u, 2u, 3u, 4u};
    uint8_t zero[sizeof(secret)] = {0};

    cmeta_crypto_clear(secret, sizeof(secret));
    check_equal(memcmp(secret, zero, sizeof(secret)), 0);
    cmeta_crypto_clear(NULL, 0u);
  }

  it("validates digest arguments") {
    uint8_t digest[SALTS_SHA256_DIGEST_BYTES];

    check_equal(cmeta_md5(NULL, 0u, digest), SALTS_OK);
    check_equal(cmeta_md5(NULL, 1u, digest), SALTS_EINVAL);
    check_equal(cmeta_md5("x", 1u, NULL), SALTS_EINVAL);

    check_equal(cmeta_sha256(NULL, 0u, digest), SALTS_OK);
    check_equal(cmeta_sha256(NULL, 1u, digest), SALTS_EINVAL);
    check_equal(cmeta_sha256("x", 1u, NULL), SALTS_EINVAL);
  }

  it("validates HMAC and compare arguments") {
    uint8_t mac[SALTS_SHA256_DIGEST_BYTES];
    int equal = 0;

    check_equal(cmeta_hmac_md5(NULL, 0u, NULL, 0u, mac), SALTS_OK);
    check_equal(cmeta_hmac_md5(NULL, 1u, "", 0u, mac), SALTS_EINVAL);
    check_equal(cmeta_hmac_md5("", 0u, NULL, 1u, mac), SALTS_EINVAL);
    check_equal(cmeta_hmac_md5("", 0u, "", 0u, NULL), SALTS_EINVAL);

    check_equal(cmeta_hmac_sha256(NULL, 0u, NULL, 0u, mac), SALTS_OK);
    check_equal(cmeta_hmac_sha256(NULL, 1u, "", 0u, mac), SALTS_EINVAL);
    check_equal(cmeta_hmac_sha256("", 0u, NULL, 1u, mac), SALTS_EINVAL);
    check_equal(cmeta_hmac_sha256("", 0u, "", 0u, NULL), SALTS_EINVAL);

    check_equal(cmeta_des_cbc_encrypt(
                    DES_CBC_KEY, DES_CBC_IV, NULL, 0u, NULL),
                SALTS_OK);
    check_equal(cmeta_des_cbc_encrypt(
                    NULL, DES_CBC_IV, NULL, 0u, NULL),
                SALTS_EINVAL);
    check_equal(cmeta_des_cbc_encrypt(
                    DES_CBC_KEY, NULL, NULL, 0u, NULL),
                SALTS_EINVAL);
    check_equal(cmeta_des_cbc_encrypt(
                    DES_CBC_KEY, DES_CBC_IV, DES_CBC_PLAINTEXT, 7u, mac),
                SALTS_EINVAL);
    check_equal(cmeta_des_cbc_encrypt(
                    DES_CBC_KEY, DES_CBC_IV, NULL, SALTS_DES_BLOCK_BYTES, mac),
                SALTS_EINVAL);

    check_equal(cmeta_aes128_cfb_encrypt(
                    AES128_CFB_KEY, AES128_CFB_IV, NULL, 0u, NULL),
                SALTS_OK);
    check_equal(cmeta_aes128_cfb_encrypt(
                    NULL, AES128_CFB_IV, NULL, 0u, NULL),
                SALTS_EINVAL);
    check_equal(cmeta_aes128_cfb_encrypt(
                    AES128_CFB_KEY, NULL, NULL, 0u, NULL),
                SALTS_EINVAL);
    check_equal(cmeta_aes128_cfb_encrypt(
                    AES128_CFB_KEY, AES128_CFB_IV, NULL, 1u, mac),
                SALTS_EINVAL);
    check_equal(cmeta_aes128_cfb_encrypt(
                    AES128_CFB_KEY, AES128_CFB_IV, mac, 1u, NULL),
                SALTS_EINVAL);

    check_equal(cmeta_crypto_equal(NULL, NULL, 1u, &equal), SALTS_EINVAL);
    check_equal(cmeta_crypto_equal("", "", 0u, NULL), SALTS_EINVAL);
  }
}
