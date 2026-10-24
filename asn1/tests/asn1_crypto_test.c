#include <salts/asn1_crypto.h>
#include "tinytest.h"

#include <stdint.h>
#include <string.h>

spec("Salts ASN.1 cryptographic structures") {
    it("round trips a canonical P-521-width ECDSA signature") {
        uint8_t r[66] = {0x80u};
        uint8_t s[66] = {0x80u};
        uint8_t der[SALTS_ASN1_ECDSA_SIGNATURE_MAX_BYTES];
        uint8_t sentinel[8];
        const uint8_t *decoded_r = NULL, *decoded_s = NULL;
        size_t decoded_r_size = 0u, decoded_s_size = 0u, written = 0u;
        check_equal(salts_asn1_ecdsa_signature_encode(
                        r, sizeof(r), s, sizeof(s), NULL, 0u, &written),
                    SALTS_ASN1_OK);
        check_equal(written, (size_t)SALTS_ASN1_ECDSA_SIGNATURE_MAX_BYTES);
        memset(sentinel, 0xa5, sizeof(sentinel));
        check_equal(salts_asn1_ecdsa_signature_encode(
                        r, sizeof(r), s, sizeof(s), sentinel, sizeof(sentinel),
                        &written),
                    SALTS_ASN1_BUFFER_TOO_SMALL);
        check_equal(sentinel[0], 0xa5u);
        check_equal(salts_asn1_ecdsa_signature_encode(
                        r, sizeof(r), s, sizeof(s), der, sizeof(der), &written),
                    SALTS_ASN1_OK);
        check_equal(salts_asn1_ecdsa_signature_decode(
                        der, written, &decoded_r, &decoded_r_size,
                        &decoded_s, &decoded_s_size),
                    SALTS_ASN1_OK);
        check_equal(decoded_r_size, sizeof(r));
        check_equal(decoded_s_size, sizeof(s));
        check_equal(decoded_r, r, sizeof(r));
        check_equal(decoded_s[0], 0x80u);
    }

    it("rejects non-canonical and over-wide ECDSA components") {
        const uint8_t negative[] = {0x30u,0x06u,0x02u,0x01u,0x80u,
                                    0x02u,0x01u,0x01u};
        uint8_t too_wide[67] = {1u};
        const uint8_t *r = NULL, *s = NULL;
        size_t r_size = 0u, s_size = 0u, written = 0u;
        check_equal(salts_asn1_ecdsa_signature_decode(
                        negative, sizeof(negative), &r, &r_size, &s, &s_size),
                    SALTS_ASN1_MALFORMED);
        check_equal(salts_asn1_ecdsa_signature_encode(
                        too_wide, sizeof(too_wide), too_wide, 1u,
                        NULL, 0u, &written),
                    SALTS_ASN1_LIMIT_EXCEEDED);
    }

    it("round trips RFC 8410 SPKI and PKCS8 without parameters") {
        uint8_t key[SALTS_ASN1_RFC8410_KEY_BYTES];
        uint8_t spki[SALTS_ASN1_RFC8410_SPKI_BYTES];
        uint8_t pkcs8[SALTS_ASN1_RFC8410_PKCS8_BYTES];
        salts_asn1_rfc8410_algorithm algorithm = 0;
        const uint8_t *decoded = NULL;
        size_t decoded_size = 0u, written = 0u, i;
        for (i = 0u; i < sizeof(key); ++i) key[i] = (uint8_t)i;

        check_equal(salts_asn1_rfc8410_spki_encode(
                        SALTS_ASN1_RFC8410_ED25519, key, spki, sizeof(spki),
                        &written),
                    SALTS_ASN1_OK);
        check_equal(written, sizeof(spki));
        check_equal(salts_asn1_rfc8410_spki_decode(
                        spki, sizeof(spki), &algorithm, &decoded, &decoded_size),
                    SALTS_ASN1_OK);
        check_equal(algorithm, SALTS_ASN1_RFC8410_ED25519);
        check_equal(decoded_size, sizeof(key));
        check_equal(decoded, key, sizeof(key));

        check_equal(salts_asn1_rfc8410_pkcs8_encode(
                        SALTS_ASN1_RFC8410_X25519, key, pkcs8, sizeof(pkcs8),
                        &written),
                    SALTS_ASN1_OK);
        check_equal(written, sizeof(pkcs8));
        check_equal(salts_asn1_rfc8410_pkcs8_decode(
                        pkcs8, sizeof(pkcs8), &algorithm, &decoded, &decoded_size),
                    SALTS_ASN1_OK);
        check_equal(algorithm, SALTS_ASN1_RFC8410_X25519);
        check_equal(decoded_size, sizeof(key));
        check_equal(decoded, key, sizeof(key));
    }

    it("rejects forbidden RFC 8410 AlgorithmIdentifier parameters") {
        uint8_t spki[SALTS_ASN1_RFC8410_SPKI_BYTES + 2u] = {
            0x30u,0x2cu,0x30u,0x07u,0x06u,0x03u,0x2bu,0x65u,0x70u,
            0x05u,0x00u,0x03u,0x21u,0x00u
        };
        salts_asn1_rfc8410_algorithm algorithm = 0;
        const uint8_t *key = NULL;
        size_t key_size = 0u;
        check_equal(salts_asn1_rfc8410_spki_decode(
                        spki, sizeof(spki), &algorithm, &key, &key_size),
                    SALTS_ASN1_MALFORMED);
    }
}
