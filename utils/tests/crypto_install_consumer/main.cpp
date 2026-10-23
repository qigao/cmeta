#include <cmeta_crypto.h>
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
  return 0;
}
