/**
 * @file test_crypto_random.c
 * @brief Cryptographically Secure RNG Tests
 */

#include "crypto_random.h"
#include "tinytest.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

spec("crypto_random") {
  describe("Basic RNG Functionality") {
    it("should generate 32 random bytes and they should not be all zero") {
      uint8_t buf[32];
      memset(buf, 0, sizeof(buf));

      int ret = crypto_random_bytes(buf, 32);
      check_equal(ret, 0);

      // Verify buffer is not all zeros (extremely unlikely with CSPRNG)
      bool all_zero = true;
      for (int i = 0; i < 32; i++) {
        if (buf[i] != 0) {
          all_zero = false;
          break;
        }
      }
      check(!all_zero);
    }

    it("should produce different results on consecutive calls") {
      uint8_t buf1[32], buf2[32];

      check_equal(crypto_random_bytes(buf1, 32), 0);
      check_equal(crypto_random_bytes(buf2, 32), 0);

      // Two consecutive calls should produce different results
      check(memcmp(buf1, buf2, 32) != 0);
    }
  }

  describe("Variable Sizes") {
    it("should successfully generate random bytes for various buffer sizes") {
      uint8_t buf1[1], buf16[16], buf64[64], buf256[256];

      check_equal(crypto_random_bytes(buf1, 1), 0);
      check_equal(crypto_random_bytes(buf16, 16), 0);
      check_equal(crypto_random_bytes(buf64, 64), 0);
      check_equal(crypto_random_bytes(buf256, 256), 0);
    }
  }

  describe("Edge Cases") {
     it("should return -1 when given a NULL buffer or zero length") {
      uint8_t buf[32];
      check_equal(crypto_random_bytes(NULL, 32), -1);
      check_equal(crypto_random_bytes(buf, 0), -1);
    }
  }
}
