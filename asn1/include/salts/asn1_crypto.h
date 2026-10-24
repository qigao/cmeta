#ifndef SALTS_ASN1_CRYPTO_H
#define SALTS_ASN1_CRYPTO_H

#include <salts/asn1.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SALTS_ASN1_ECDSA_COMPONENT_MAX_BYTES 66u
#define SALTS_ASN1_ECDSA_SIGNATURE_MAX_BYTES 141u
#define SALTS_ASN1_RFC8410_KEY_BYTES 32u
#define SALTS_ASN1_RFC8410_SPKI_BYTES 44u
#define SALTS_ASN1_RFC8410_PKCS8_BYTES 48u

typedef enum salts_asn1_rfc8410_algorithm {
    SALTS_ASN1_RFC8410_X25519 = 1,
    SALTS_ASN1_RFC8410_ED25519 = 2
} salts_asn1_rfc8410_algorithm;

/* Encodes an X9.62 ECDSA-Sig-Value from unsigned big-endian r and s.
 * Leading zero bytes are ignored. Components are limited to P-521 width.
 * A size query uses output=NULL/capacity=0. Caller output is unchanged on
 * error; written reports the required size for OK size queries and
 * BUFFER_TOO_SMALL results. */
SALTS_ASN1_PUBLIC salts_asn1_status salts_asn1_ecdsa_signature_encode(
    const void *r, size_t r_size, const void *s, size_t s_size,
    void *output, size_t output_capacity, size_t *written);

/* Strictly decodes one canonical DER ECDSA-Sig-Value. Returned component
 * views borrow the input buffer and exclude a DER sign-padding zero. */
SALTS_ASN1_PUBLIC salts_asn1_status salts_asn1_ecdsa_signature_decode(
    const void *der, size_t der_size,
    const uint8_t **r, size_t *r_size,
    const uint8_t **s, size_t *s_size);

/* RFC 8410 SubjectPublicKeyInfo. AlgorithmIdentifier parameters are absent,
 * as required by the RFC. The decoded key is a borrowed 32-byte view. */
SALTS_ASN1_PUBLIC salts_asn1_status salts_asn1_rfc8410_spki_encode(
    salts_asn1_rfc8410_algorithm algorithm,
    const uint8_t key[SALTS_ASN1_RFC8410_KEY_BYTES],
    void *output, size_t output_capacity, size_t *written);
SALTS_ASN1_PUBLIC salts_asn1_status salts_asn1_rfc8410_spki_decode(
    const void *der, size_t der_size,
    salts_asn1_rfc8410_algorithm *algorithm,
    const uint8_t **key, size_t *key_size);

/* RFC 8410 PKCS#8 PrivateKeyInfo version 0. The privateKey OCTET STRING
 * contains the RFC-required nested CurvePrivateKey OCTET STRING. */
SALTS_ASN1_PUBLIC salts_asn1_status salts_asn1_rfc8410_pkcs8_encode(
    salts_asn1_rfc8410_algorithm algorithm,
    const uint8_t seed[SALTS_ASN1_RFC8410_KEY_BYTES],
    void *output, size_t output_capacity, size_t *written);
SALTS_ASN1_PUBLIC salts_asn1_status salts_asn1_rfc8410_pkcs8_decode(
    const void *der, size_t der_size,
    salts_asn1_rfc8410_algorithm *algorithm,
    const uint8_t **seed, size_t *seed_size);

#ifdef __cplusplus
}
#endif

#endif /* SALTS_ASN1_CRYPTO_H */
