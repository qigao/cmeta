#include <salts/asn1_value.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    static const uint8_t expected[] = {0x02u, 0x01u, 0x2au};
    uint8_t encoded[8];
    size_t written = 0u;
    int64_t decoded_integer = 0;
    salts_asn1_value *integer = NULL;
    salts_asn1_value *decoded = NULL;
    int result = 1;

    {
        salts_asn1_status status = salts_asn1_value_create_integer(42, &integer);
        if (status != SALTS_ASN1_OK) {
            fprintf(stderr, "create_integer returned %d\n", (int)status);
            goto done;
        }
        status = salts_asn1_value_encode(integer, SALTS_ASN1_DER, 0u,
                                         encoded, sizeof(encoded), &written);
        if (status != SALTS_ASN1_OK) {
            fprintf(stderr, "value_encode returned %d\n", (int)status);
            goto done;
        }
        if (written != sizeof(expected) || memcmp(encoded, expected, sizeof(expected)) != 0) {
            size_t i;
            fprintf(stderr, "encoded length=%zu expected=%zu bytes=", written, sizeof(expected));
            for (i = 0u; i < written && i < sizeof(encoded); ++i)
                fprintf(stderr, "%02x", encoded[i]);
            fprintf(stderr, "\n");
            goto done;
        }
        status = salts_asn1_value_decode(encoded, written, SALTS_ASN1_DER, &decoded);
        if (status != SALTS_ASN1_OK) {
            fprintf(stderr, "value_decode returned %d\n", (int)status);
            goto done;
        }
        status = salts_asn1_value_get_integer_i64(decoded, &decoded_integer);
        if (status != SALTS_ASN1_OK || decoded_integer != 42) {
            fprintf(stderr, "get_integer returned %d value=%lld\n",
                    (int)status, (long long)decoded_integer);
            goto done;
        }
    }
    result = 0;

done:
    salts_asn1_value_free(decoded);
    salts_asn1_value_free(integer);
    return result;
}
