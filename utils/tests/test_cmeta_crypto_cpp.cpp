#include "cmeta_crypto.h"
#include "tinytest.hpp"

#include <cstddef>
#include <cstdint>

static_assert(SALTS_MD5_DIGEST_BYTES == 16u, "MD5 ABI size changed");
static_assert(SALTS_SHA1_DIGEST_BYTES == 20u, "SHA-1 ABI size changed");
static_assert(SALTS_SHA256_DIGEST_BYTES == 32u, "SHA-256 ABI size changed");
static_assert(SALTS_SM3_DIGEST_BYTES == 32u, "SM3 ABI size changed");
static_assert(SALTS_HKDF_SM3_MAX_OUTPUT_BYTES == 8160u, "HKDF-SM3 output bound changed");
static_assert(SALTS_AES128_KEY_BYTES == 16u, "AES-128 key ABI size changed");
static_assert(SALTS_AES_BLOCK_BYTES == 16u, "AES block ABI size changed");
static_assert(SALTS_DES_KEY_BYTES == 8u, "DES key ABI size changed");
static_assert(SALTS_DES_BLOCK_BYTES == 8u, "DES block ABI size changed");

spec("cmeta_crypto C++ ABI") {
  it("exposes the provider-neutral C ABI to C++17 consumers") {
    std::uint8_t md5[SALTS_MD5_DIGEST_BYTES];
    std::uint8_t sha1_mac[SALTS_SHA1_DIGEST_BYTES];
    std::uint8_t sha256_mac[SALTS_SHA256_DIGEST_BYTES];
    std::uint8_t sm3[SALTS_SM3_DIGEST_BYTES];
    std::uint8_t hkdf[16] = {};
    std::uint8_t aes_key[SALTS_AES128_KEY_BYTES] = {};
    std::uint8_t aes_iv[SALTS_AES_BLOCK_BYTES] = {};
    std::uint8_t aes_data[1] = {};
    std::uint8_t des_key[SALTS_DES_KEY_BYTES] = {};
    std::uint8_t des_iv[SALTS_DES_BLOCK_BYTES] = {};
    std::uint8_t des_data[SALTS_DES_BLOCK_BYTES] = {};
    cmeta_sha256_stream *stream = nullptr;
    cmeta_sm3_stream *sm3_stream = nullptr;
    int equal = 0;

    check_equal(cmeta_md5("abc", static_cast<std::size_t>(3), md5), SALTS_OK);
    check_equal(cmeta_hmac_md5("key", static_cast<std::size_t>(3),
                               "data", static_cast<std::size_t>(4),
                               md5),
                SALTS_OK);
    check_equal(cmeta_hmac_sha1("key", static_cast<std::size_t>(3),
                                "data", static_cast<std::size_t>(4),
                                sha1_mac),
                SALTS_OK);
    check_equal(cmeta_hmac_sha256("key", static_cast<std::size_t>(3),
                                  "data", static_cast<std::size_t>(4),
                                  sha256_mac),
                SALTS_OK);
    check_equal(cmeta_sm3("abc", static_cast<std::size_t>(3), sm3), SALTS_OK);
    check_equal(cmeta_hmac_sm3("key", static_cast<std::size_t>(3),
                               "data", static_cast<std::size_t>(4), sm3),
                SALTS_OK);
    check_equal(cmeta_hkdf_sm3("ikm", static_cast<std::size_t>(3),
                               "salt", static_cast<std::size_t>(4),
                               "info", static_cast<std::size_t>(4),
                               hkdf, sizeof(hkdf)),
                SALTS_OK);
    check_equal(cmeta_sm3_stream_create(&sm3_stream), SALTS_OK);
    check_equal(cmeta_sm3_stream_update(sm3_stream, "abc", static_cast<std::size_t>(3)),
                SALTS_OK);
    check_equal(cmeta_sm3_stream_finish(sm3_stream, sm3), SALTS_OK);
    cmeta_sm3_stream_destroy(sm3_stream);
    sm3_stream = nullptr;
    check_equal(cmeta_sha256_stream_create(&stream), SALTS_OK);
    check_equal(cmeta_sha256_stream_update(stream, "abc", static_cast<std::size_t>(3)),
                SALTS_OK);
    check_equal(cmeta_sha256_stream_finish(stream, sha256_mac), SALTS_OK);
    cmeta_sha256_stream_destroy(stream);
    stream = nullptr;
    check_equal(cmeta_des_cbc_encrypt(des_key, des_iv, des_data,
                                      sizeof(des_data), des_data),
                SALTS_OK);
    check_equal(cmeta_des_cbc_decrypt(des_key, des_iv, des_data,
                                      sizeof(des_data), des_data),
                SALTS_OK);
    check_equal(cmeta_aes128_cfb_encrypt(aes_key, aes_iv, aes_data,
                                          sizeof(aes_data), aes_data),
                SALTS_OK);
    check_equal(cmeta_aes128_cfb_decrypt(aes_key, aes_iv, aes_data,
                                          sizeof(aes_data), aes_data),
                SALTS_OK);
    check_equal(cmeta_crypto_equal(md5, md5, sizeof(md5), &equal), SALTS_OK);
    check_equal(equal, 1);
    cmeta_crypto_clear(sha256_mac, sizeof(sha256_mac));
  }
}
