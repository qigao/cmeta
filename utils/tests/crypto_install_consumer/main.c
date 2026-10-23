#include <cmeta_crypto.h>
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
  return memcmp(digest, expected, sizeof(digest)) == 0 ? 0 : 8;
}
