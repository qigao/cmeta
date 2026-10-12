#include <cmeta_crypto.h>
#include <cstdint>
#include <array>

int main() {
  std::array<std::uint8_t, 32> key{};
  std::array<std::uint8_t, 32> out{};
  key[0] = 7;
  return cmeta_hmac_sha256(key.data(), key.size(), "x", 1, out.data()) == 0 ? 0 : 1;
}
