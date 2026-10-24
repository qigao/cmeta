#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "cmeta_crypto.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  uint8_t key[SALTS_SM4_KEY_BYTES];
  uint8_t nonce[SALTS_SM4_GCM_NONCE_BYTES];
  uint8_t ciphertext[4096];
  uint8_t plaintext[4096];
  uint8_t unchanged[4096];
  uint8_t tag[SALTS_SM4_GCM_TAG_BYTES];
  size_t aad_size;
  size_t message_size;
  size_t ciphertext_size = 0u;
  size_t plaintext_size = 0u;
  const uint8_t *aad;
  const uint8_t *message;

  if (size < sizeof(key) + sizeof(nonce) + 1u || size > sizeof(ciphertext)) return 0;
  memcpy(key, data, sizeof(key));
  memcpy(nonce, data + sizeof(key), sizeof(nonce));
  data += sizeof(key) + sizeof(nonce);
  size -= sizeof(key) + sizeof(nonce);
  aad_size = (size_t)data[0] % size;
  aad = data + 1u;
  message = aad + aad_size;
  message_size = size - 1u - aad_size;

  if (cmeta_sm4_gcm_seal(key, nonce, aad, aad_size, message, message_size, ciphertext,
                         sizeof(ciphertext), &ciphertext_size, tag) != SALTS_OK ||
      ciphertext_size != message_size)
    abort();
  if (cmeta_sm4_gcm_open(key, nonce, aad, aad_size, ciphertext, ciphertext_size, tag, plaintext,
                         sizeof(plaintext), &plaintext_size) != SALTS_OK ||
      plaintext_size != message_size || memcmp(plaintext, message, message_size) != 0)
    abort();

  memset(plaintext, 0xa5, sizeof(plaintext));
  memcpy(unchanged, plaintext, sizeof(unchanged));
  plaintext_size = SIZE_MAX;
  tag[(size_t)data[0] % sizeof(tag)] ^= 1u;
  if (cmeta_sm4_gcm_open(key, nonce, aad, aad_size, ciphertext, ciphertext_size, tag, plaintext,
                         sizeof(plaintext), &plaintext_size) != SALTS_EAUTH ||
      plaintext_size != SIZE_MAX || memcmp(plaintext, unchanged, sizeof(plaintext)) != 0)
    abort();
  return 0;
}
