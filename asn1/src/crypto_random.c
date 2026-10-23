/**
 * @file crypto_random.c
 * @brief Compatibility wrapper over Salts platform CSPRNG
 */

#include "crypto_random.h"

int crypto_random_bytes(uint8_t *buf, size_t len) {
    if (!buf || len == 0u) return -1;
    return cmeta_secure_random(buf, len) == 0 ? 0 : -1;
}
