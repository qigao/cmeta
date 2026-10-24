#include <stdint.h>
#include <string.h>

#include "cmeta_crypto.h"
#include "tinytest.h"

static const uint8_t SM4_GCM_RFC8998_KEY[SALTS_SM4_KEY_BYTES] = {
    0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef,
    0xfe, 0xdc, 0xba, 0x98, 0x76, 0x54, 0x32, 0x10};

static const uint8_t SM4_GCM_RFC8998_NONCE[SALTS_SM4_GCM_NONCE_BYTES] = {
    0x00, 0x00, 0x12, 0x34, 0x56, 0x78, 0x00, 0x00, 0x00, 0x00, 0xab, 0xcd};

static const uint8_t SM4_GCM_RFC8998_AAD[20] = {
    0xfe, 0xed, 0xfa, 0xce, 0xde, 0xad, 0xbe, 0xef, 0xfe, 0xed,
    0xfa, 0xce, 0xde, 0xad, 0xbe, 0xef, 0xab, 0xad, 0xda, 0xd2};

static const uint8_t SM4_GCM_RFC8998_PLAINTEXT[64] = {
    0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa,
    0xbb, 0xbb, 0xbb, 0xbb, 0xbb, 0xbb, 0xbb, 0xbb,
    0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc,
    0xdd, 0xdd, 0xdd, 0xdd, 0xdd, 0xdd, 0xdd, 0xdd,
    0xee, 0xee, 0xee, 0xee, 0xee, 0xee, 0xee, 0xee,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xee, 0xee, 0xee, 0xee, 0xee, 0xee, 0xee, 0xee,
    0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa};

static const uint8_t SM4_GCM_RFC8998_CIPHERTEXT[64] = {
    0x17, 0xf3, 0x99, 0xf0, 0x8c, 0x67, 0xd5, 0xee,
    0x19, 0xd0, 0xdc, 0x99, 0x69, 0xc4, 0xbb, 0x7d,
    0x5f, 0xd4, 0x6f, 0xd3, 0x75, 0x64, 0x89, 0x06,
    0x91, 0x57, 0xb2, 0x82, 0xbb, 0x20, 0x07, 0x35,
    0xd8, 0x27, 0x10, 0xca, 0x5c, 0x22, 0xf0, 0xcc,
    0xfa, 0x7c, 0xbf, 0x93, 0xd4, 0x96, 0xac, 0x15,
    0xa5, 0x68, 0x34, 0xcb, 0xcf, 0x98, 0xc3, 0x97,
    0xb4, 0x02, 0x4a, 0x26, 0x91, 0x23, 0x3b, 0x8d};

static const uint8_t SM4_GCM_RFC8998_TAG[SALTS_SM4_GCM_TAG_BYTES] = {
    0x83, 0xde, 0x35, 0x41, 0xe4, 0xc2, 0xb5, 0x81,
    0x77, 0xe0, 0x65, 0xa9, 0xbf, 0x7b, 0x62, 0xec};

suite("CMeta SM4-GCM") {
  group("SM4-GCM contract") {
    it("matches the RFC 8998 known-answer vector") {
      uint8_t ciphertext[sizeof(SM4_GCM_RFC8998_CIPHERTEXT)] = {0};
      uint8_t plaintext[sizeof(SM4_GCM_RFC8998_PLAINTEXT)] = {0};
      uint8_t tag[SALTS_SM4_GCM_TAG_BYTES] = {0};
      size_t ciphertext_size = 0u;
      size_t plaintext_size = 0u;

      check_equal(cmeta_sm4_gcm_seal(
                      SM4_GCM_RFC8998_KEY, SM4_GCM_RFC8998_NONCE,
                      SM4_GCM_RFC8998_AAD, sizeof(SM4_GCM_RFC8998_AAD),
                      SM4_GCM_RFC8998_PLAINTEXT, sizeof(SM4_GCM_RFC8998_PLAINTEXT),
                      ciphertext, sizeof(ciphertext), &ciphertext_size, tag),
                  SALTS_OK);
      check_equal(ciphertext_size, sizeof(SM4_GCM_RFC8998_CIPHERTEXT));
      check_equal(memcmp(ciphertext, SM4_GCM_RFC8998_CIPHERTEXT, sizeof(ciphertext)), 0);
      check_equal(memcmp(tag, SM4_GCM_RFC8998_TAG, sizeof(tag)), 0);

      check_equal(cmeta_sm4_gcm_open(
                      SM4_GCM_RFC8998_KEY, SM4_GCM_RFC8998_NONCE,
                      SM4_GCM_RFC8998_AAD, sizeof(SM4_GCM_RFC8998_AAD),
                      ciphertext, ciphertext_size, tag,
                      plaintext, sizeof(plaintext), &plaintext_size),
                  SALTS_OK);
      check_equal(plaintext_size, sizeof(SM4_GCM_RFC8998_PLAINTEXT));
      check_equal(memcmp(plaintext, SM4_GCM_RFC8998_PLAINTEXT, sizeof(plaintext)), 0);
    }

    it("leaves plaintext and size unchanged when authentication fails") {
      uint8_t bad_tag[SALTS_SM4_GCM_TAG_BYTES];
      uint8_t plaintext[sizeof(SM4_GCM_RFC8998_PLAINTEXT)];
      uint8_t before[sizeof(plaintext)];
      size_t plaintext_size = 0x1234u;

      memcpy(bad_tag, SM4_GCM_RFC8998_TAG, sizeof(bad_tag));
      bad_tag[0] ^= 0x80u;
      memset(plaintext, 0xa5, sizeof(plaintext));
      memcpy(before, plaintext, sizeof(before));

      check_equal(cmeta_sm4_gcm_open(
                      SM4_GCM_RFC8998_KEY, SM4_GCM_RFC8998_NONCE,
                      SM4_GCM_RFC8998_AAD, sizeof(SM4_GCM_RFC8998_AAD),
                      SM4_GCM_RFC8998_CIPHERTEXT,
                      sizeof(SM4_GCM_RFC8998_CIPHERTEXT), bad_tag,
                      plaintext, sizeof(plaintext), &plaintext_size),
                  SALTS_EAUTH);
      check_equal(memcmp(plaintext, before, sizeof(plaintext)), 0);
      check_equal(plaintext_size, (size_t)0x1234u);
    }

    it("reports required capacity without changing ciphertext or tag") {
      uint8_t ciphertext[sizeof(SM4_GCM_RFC8998_PLAINTEXT)];
      uint8_t before[sizeof(ciphertext)];
      uint8_t tag[SALTS_SM4_GCM_TAG_BYTES];
      uint8_t tag_before[SALTS_SM4_GCM_TAG_BYTES];
      size_t ciphertext_size = 0u;

      memset(ciphertext, 0x5a, sizeof(ciphertext));
      memset(tag, 0xa5, sizeof(tag));
      memcpy(before, ciphertext, sizeof(before));
      memcpy(tag_before, tag, sizeof(tag_before));

      check_equal(cmeta_sm4_gcm_seal(
                      SM4_GCM_RFC8998_KEY, SM4_GCM_RFC8998_NONCE,
                      SM4_GCM_RFC8998_AAD, sizeof(SM4_GCM_RFC8998_AAD),
                      SM4_GCM_RFC8998_PLAINTEXT, sizeof(SM4_GCM_RFC8998_PLAINTEXT),
                      ciphertext, sizeof(ciphertext) - 1u, &ciphertext_size, tag),
                  SALTS_ERANGE);
      check_equal(ciphertext_size, sizeof(SM4_GCM_RFC8998_PLAINTEXT));
      check_equal(memcmp(ciphertext, before, sizeof(ciphertext)), 0);
      check_equal(memcmp(tag, tag_before, sizeof(tag)), 0);
    }

    it("supports exact in-place operation and rejects partial overlap") {
      uint8_t inplace[sizeof(SM4_GCM_RFC8998_PLAINTEXT)];
      uint8_t overlap[8] = {1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u};
      uint8_t overlap_before[sizeof(overlap)];
      uint8_t tag[SALTS_SM4_GCM_TAG_BYTES] = {0};
      uint8_t tag_before[SALTS_SM4_GCM_TAG_BYTES];
      size_t size = 0u;

      memcpy(inplace, SM4_GCM_RFC8998_PLAINTEXT, sizeof(inplace));
      check_equal(cmeta_sm4_gcm_seal(
                      SM4_GCM_RFC8998_KEY, SM4_GCM_RFC8998_NONCE,
                      SM4_GCM_RFC8998_AAD, sizeof(SM4_GCM_RFC8998_AAD),
                      inplace, sizeof(inplace), inplace, sizeof(inplace), &size, tag),
                  SALTS_OK);
      check_equal(size, sizeof(inplace));
      check_equal(memcmp(inplace, SM4_GCM_RFC8998_CIPHERTEXT, sizeof(inplace)), 0);

      check_equal(cmeta_sm4_gcm_open(
                      SM4_GCM_RFC8998_KEY, SM4_GCM_RFC8998_NONCE,
                      SM4_GCM_RFC8998_AAD, sizeof(SM4_GCM_RFC8998_AAD),
                      inplace, size, tag, inplace, sizeof(inplace), &size),
                  SALTS_OK);
      check_equal(memcmp(inplace, SM4_GCM_RFC8998_PLAINTEXT, sizeof(inplace)), 0);

      memcpy(overlap_before, overlap, sizeof(overlap));
      memcpy(tag_before, tag, sizeof(tag));
      size = 99u;
      check_equal(cmeta_sm4_gcm_seal(
                      SM4_GCM_RFC8998_KEY, SM4_GCM_RFC8998_NONCE,
                      NULL, 0u, overlap, 4u, overlap + 1u, 4u, &size, tag),
                  SALTS_EINVAL);
      check_equal(memcmp(overlap, overlap_before, sizeof(overlap)), 0);
      check_equal(memcmp(tag, tag_before, sizeof(tag)), 0);
      check_equal(size, (size_t)99u);
    }

    it("supports empty messages and validates bounded input") {
      uint8_t tag[SALTS_SM4_GCM_TAG_BYTES] = {0};
      uint8_t empty = 0u;
      size_t size = 7u;

      check_equal(cmeta_sm4_gcm_seal(
                      SM4_GCM_RFC8998_KEY, SM4_GCM_RFC8998_NONCE,
                      NULL, 0u, NULL, 0u, NULL, 0u, &size, tag),
                  SALTS_OK);
      check_equal(size, (size_t)0u);
      size = 7u;
      check_equal(cmeta_sm4_gcm_open(
                      SM4_GCM_RFC8998_KEY, SM4_GCM_RFC8998_NONCE,
                      NULL, 0u, NULL, 0u, tag, NULL, 0u, &size),
                  SALTS_OK);
      check_equal(size, (size_t)0u);

      check_equal(cmeta_sm4_gcm_seal(
                      SM4_GCM_RFC8998_KEY, SM4_GCM_RFC8998_NONCE,
                      &empty, SALTS_SM4_GCM_MAX_AAD_BYTES + 1u,
                      &empty, 0u, NULL, 0u, &size, tag),
                  SALTS_ERANGE);
      check_equal(cmeta_sm4_gcm_seal(
                      NULL, SM4_GCM_RFC8998_NONCE, NULL, 0u,
                      NULL, 0u, NULL, 0u, &size, tag),
                  SALTS_EINVAL);
    }
  }
}
