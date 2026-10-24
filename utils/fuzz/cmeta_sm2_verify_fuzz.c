#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "cmeta_crypto.h"

static const uint8_t SM2_PUBLIC_SPKI[] = {
    0x30, 0x59, 0x30, 0x13, 0x06, 0x07, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x02, 0x01, 0x06, 0x08, 0x2a,
    0x81, 0x1c, 0xcf, 0x55, 0x01, 0x82, 0x2d, 0x03, 0x42, 0x00, 0x04, 0x09, 0xf9, 0xdf, 0x31, 0x1e,
    0x54, 0x21, 0xa1, 0x50, 0xdd, 0x7d, 0x16, 0x1e, 0x4b, 0xc5, 0xc6, 0x72, 0x17, 0x9f, 0xad, 0x18,
    0x33, 0xfc, 0x07, 0x6b, 0xb0, 0x8f, 0xf3, 0x56, 0xf3, 0x50, 0x20, 0xcc, 0xea, 0x49, 0x0c, 0xe2,
    0x67, 0x75, 0xa5, 0x2d, 0xc6, 0xea, 0x71, 0x8c, 0xc1, 0xaa, 0x60, 0x0a, 0xed, 0x05, 0xfb, 0xf3,
    0x5e, 0x08, 0x4a, 0x66, 0x32, 0xf6, 0x07, 0x2d, 0xa9, 0xad, 0x13};
static const uint8_t SM2_ID[] = "1234567812345678";
static const uint8_t SM2_MESSAGE[] = "message digest";
static const uint8_t SM2_SIGNATURE[] = {
    0x30, 0x46, 0x02, 0x21, 0x00, 0xf5, 0xa0, 0x3b, 0x06, 0x48, 0xd2, 0xc4, 0x63, 0x0e, 0xea,
    0xc5, 0x13, 0xe1, 0xbb, 0x81, 0xa1, 0x59, 0x44, 0xda, 0x38, 0x27, 0xd5, 0xb7, 0x41, 0x43,
    0xac, 0x7e, 0xac, 0xee, 0xe7, 0x20, 0xb3, 0x02, 0x21, 0x00, 0xb1, 0xb6, 0xaa, 0x29, 0xdf,
    0x21, 0x2f, 0xd8, 0x76, 0x31, 0x82, 0xbc, 0x0d, 0x42, 0x1c, 0xa1, 0xbb, 0x90, 0x38, 0xfd,
    0x1f, 0x7f, 0x42, 0xd4, 0x84, 0x0b, 0x69, 0xc4, 0x85, 0xbb, 0xc1, 0xaa};

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  size_t key_size;
  size_t identity_size;
  size_t signature_size;
  size_t message_size;
  const uint8_t *key;
  const uint8_t *identity;
  const uint8_t *message;
  const uint8_t *signature;

  if (size > 4096u) return 0;
  if (size != 0u && data[0] == (uint8_t)'K') {
    uint8_t key_copy[sizeof(SM2_PUBLIC_SPKI)];
    uint8_t id_copy[sizeof(SM2_ID) - 1u];
    uint8_t message_copy[sizeof(SM2_MESSAGE) - 1u];
    uint8_t signature_copy[sizeof(SM2_SIGNATURE)];
    size_t index;
    const size_t total =
        sizeof(key_copy) + sizeof(id_copy) + sizeof(message_copy) + sizeof(signature_copy);
    int result;

    memcpy(key_copy, SM2_PUBLIC_SPKI, sizeof(key_copy));
    memcpy(id_copy, SM2_ID, sizeof(id_copy));
    memcpy(message_copy, SM2_MESSAGE, sizeof(message_copy));
    memcpy(signature_copy, SM2_SIGNATURE, sizeof(signature_copy));
    for (index = 1u; index < size; ++index) {
      size_t offset = (size_t)data[index] % total;
      uint8_t change = index + 1u < size ? data[++index] : 1u;
      if (change == 0u) change = 1u;
      if (offset < sizeof(key_copy)) {
        key_copy[offset] ^= change;
      } else if ((offset -= sizeof(key_copy)) < sizeof(id_copy)) {
        id_copy[offset] ^= change;
      } else if ((offset -= sizeof(id_copy)) < sizeof(message_copy)) {
        message_copy[offset] ^= change;
      } else {
        offset -= sizeof(message_copy);
        signature_copy[offset] ^= change;
      }
    }
    result = cmeta_sm2_verify(key_copy, sizeof(key_copy), id_copy, sizeof(id_copy), message_copy,
                              sizeof(message_copy), signature_copy, sizeof(signature_copy));
    if (size == 1u && result != SALTS_OK) abort();
    return 0;
  }

  if (size < 6u) return 0;
  key_size = ((size_t)data[0] | ((size_t)data[1] << 8u)) % (size - 3u);
  identity_size = (size_t)data[2] % (size - 3u - key_size + 1u);
  signature_size = (size_t)data[3];
  if (key_size + identity_size + signature_size > size - 4u) return 0;
  message_size = size - 4u - key_size - identity_size - signature_size;
  key = data + 4u;
  identity = key + key_size;
  message = identity + identity_size;
  signature = message + message_size;

  (void)cmeta_sm2_verify(key, key_size, identity, identity_size, message, message_size, signature,
                         signature_size);
  return 0;
}
