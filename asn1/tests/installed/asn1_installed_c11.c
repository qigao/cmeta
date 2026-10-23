#include <salts/asn1_value.h>

#include <stdint.h>
#include <string.h>

int main(void) {
    static const uint8_t expected[] = {0x02u, 0x01u, 0x2au};
    uint8_t encoded[8];
    size_t written = 0u;
    int64_t decoded_integer = 0;
    salts_asn1_value *integer = NULL;
    salts_asn1_value *decoded = NULL;
    int result = 1;

    if (salts_asn1_value_create_integer(42, &integer) != SALTS_ASN1_OK)
        goto done;
    if (salts_asn1_value_encode(integer, SALTS_ASN1_DER, 0u,
                                encoded, sizeof(encoded), &written) != SALTS_ASN1_OK)
        goto done;
    if (written != sizeof(expected) || memcmp(encoded, expected, sizeof(expected)) != 0)
        goto done;
    if (salts_asn1_value_decode(encoded, written, SALTS_ASN1_DER, &decoded) != SALTS_ASN1_OK)
        goto done;
    if (salts_asn1_value_get_integer_i64(decoded, &decoded_integer) != SALTS_ASN1_OK ||
        decoded_integer != 42)
        goto done;
    result = 0;

done:
    salts_asn1_value_free(decoded);
    salts_asn1_value_free(integer);
    return result;
}
