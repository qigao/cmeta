/**
 * @file crypto_random.h
 * @brief Cryptographically Secure Random Number Generator
 *
 * Cross-platform CSPRNG for cryptographic key generation.
 */

#ifndef CRYPTO_RANDOM_H
#define CRYPTO_RANDOM_H


#include "asn1_api.h"
#include "platform.h"
#include <stdint.h>
#include <stddef.h>

/**
 * Fill buffer with cryptographically secure random bytes
 * @param buf Output buffer
 * @param len Number of bytes to generate
 * @return 0 on success, -1 on error
 */
SALTSNET_ASN1_C_API int crypto_random_bytes(uint8_t *buf, size_t len);

#endif // CRYPTO_RANDOM_H