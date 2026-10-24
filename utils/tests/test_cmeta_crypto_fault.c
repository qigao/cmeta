#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "cmeta_crypto.h"
#include "tinytest.h"

static uint8_t fault_storage[256];
static size_t fault_storage_size;
static int fault_fail_next;
static int fault_in_use;
static int fault_release_count;
static int fault_release_was_cleared;

void *cmeta_crypto_test_alloc(size_t size) {
  if (fault_fail_next) {
    fault_fail_next = 0;
    return NULL;
  }
  if (fault_in_use || size > sizeof(fault_storage)) return NULL;
  memset(fault_storage, 0xa5, sizeof(fault_storage));
  fault_storage_size = size;
  fault_in_use = 1;
  return fault_storage;
}

void cmeta_crypto_test_free(void *data) {
  size_t index;
  fault_release_was_cleared = data == fault_storage && fault_in_use;
  for (index = 0u; fault_release_was_cleared && index < fault_storage_size; ++index) {
    if (fault_storage[index] != 0u) fault_release_was_cleared = 0;
  }
  fault_release_count += 1;
  fault_in_use = 0;
  fault_storage_size = 0u;
}

static void reset_fault_allocator(void) {
  memset(fault_storage, 0, sizeof(fault_storage));
  fault_storage_size = 0u;
  fault_fail_next = 0;
  fault_in_use = 0;
  fault_release_count = 0;
  fault_release_was_cleared = 0;
}

suite("CMeta crypto fault injection") {
  group("SM4-GCM staging ownership") {
    it("leaves seal outputs unchanged when staging allocation fails") {
      static const uint8_t key[SALTS_SM4_KEY_BYTES] = {0};
      static const uint8_t nonce[SALTS_SM4_GCM_NONCE_BYTES] = {0};
      static const uint8_t plaintext[] = "allocation failure";
      uint8_t ciphertext[sizeof(plaintext)];
      uint8_t ciphertext_before[sizeof(ciphertext)];
      uint8_t tag[SALTS_SM4_GCM_TAG_BYTES];
      uint8_t tag_before[sizeof(tag)];
      size_t ciphertext_size = 0x1234u;

      reset_fault_allocator();
      memset(ciphertext, 0x5a, sizeof(ciphertext));
      memset(tag, 0x6b, sizeof(tag));
      memcpy(ciphertext_before, ciphertext, sizeof(ciphertext));
      memcpy(tag_before, tag, sizeof(tag));
      fault_fail_next = 1;

      check_equal(cmeta_sm4_gcm_seal(key, nonce, NULL, 0u, plaintext, sizeof(plaintext), ciphertext,
                                     sizeof(ciphertext), &ciphertext_size, tag),
                  SALTS_ENOMEM);
      check_equal(memcmp(ciphertext, ciphertext_before, sizeof(ciphertext)), 0);
      check_equal(memcmp(tag, tag_before, sizeof(tag)), 0);
      check_equal(ciphertext_size, (size_t)0x1234u);
      check_equal(fault_release_count, 0);
    }

    it("leaves open outputs unchanged when staging allocation fails") {
      static const uint8_t key[SALTS_SM4_KEY_BYTES] = {0};
      static const uint8_t nonce[SALTS_SM4_GCM_NONCE_BYTES] = {0};
      static const uint8_t plaintext[] = "allocation failure";
      uint8_t ciphertext[sizeof(plaintext)];
      uint8_t output[sizeof(plaintext)];
      uint8_t output_before[sizeof(output)];
      uint8_t tag[SALTS_SM4_GCM_TAG_BYTES];
      size_t ciphertext_size = 0u;
      size_t output_size = 0x1234u;

      reset_fault_allocator();
      check_equal(cmeta_sm4_gcm_seal(key, nonce, NULL, 0u, plaintext, sizeof(plaintext), ciphertext,
                                     sizeof(ciphertext), &ciphertext_size, tag),
                  SALTS_OK);
      check_equal(fault_release_count, 1);
      check_true(fault_release_was_cleared);

      memset(output, 0x7c, sizeof(output));
      memcpy(output_before, output, sizeof(output));
      fault_fail_next = 1;
      check_equal(cmeta_sm4_gcm_open(key, nonce, NULL, 0u, ciphertext, ciphertext_size, tag, output,
                                     sizeof(output), &output_size),
                  SALTS_ENOMEM);
      check_equal(memcmp(output, output_before, sizeof(output)), 0);
      check_equal(output_size, (size_t)0x1234u);
      check_equal(fault_release_count, 1);
    }

    it("clears staging storage before every successful or rejected release") {
      static const uint8_t key[SALTS_SM4_KEY_BYTES] = {0};
      static const uint8_t nonce[SALTS_SM4_GCM_NONCE_BYTES] = {0};
      static const uint8_t plaintext[] = "clear before free";
      uint8_t ciphertext[sizeof(plaintext)];
      uint8_t output[sizeof(plaintext)];
      uint8_t tag[SALTS_SM4_GCM_TAG_BYTES];
      size_t ciphertext_size = 0u;
      size_t output_size = 0u;

      reset_fault_allocator();
      check_equal(cmeta_sm4_gcm_seal(key, nonce, NULL, 0u, plaintext, sizeof(plaintext), ciphertext,
                                     sizeof(ciphertext), &ciphertext_size, tag),
                  SALTS_OK);
      check_equal(fault_release_count, 1);
      check_true(fault_release_was_cleared);

      check_equal(cmeta_sm4_gcm_open(key, nonce, NULL, 0u, ciphertext, ciphertext_size, tag, output,
                                     sizeof(output), &output_size),
                  SALTS_OK);
      check_equal(fault_release_count, 2);
      check_true(fault_release_was_cleared);
      check_equal(output_size, sizeof(plaintext));
      check_equal(memcmp(output, plaintext, sizeof(plaintext)), 0);

      tag[0] ^= 1u;
      output_size = 0x1234u;
      check_equal(cmeta_sm4_gcm_open(key, nonce, NULL, 0u, ciphertext, ciphertext_size, tag, output,
                                     sizeof(output), &output_size),
                  SALTS_EAUTH);
      check_equal(fault_release_count, 3);
      check_true(fault_release_was_cleared);
      check_equal(output_size, (size_t)0x1234u);
    }
  }
}
