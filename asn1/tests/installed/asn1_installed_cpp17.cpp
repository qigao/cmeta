#include <salts/asn1_value.h>

#include <cstdint>
#include <cstdio>

int main() {
    salts_asn1_value *sequence = nullptr;
    salts_asn1_value *integer = nullptr;
    salts_asn1_value *decoded = nullptr;
    std::uint8_t encoded[32]{};
    std::size_t written = 0u;
    std::int64_t number = 0;
    int result = 1;

    {
        auto status = salts_asn1_value_create_sequence(&sequence);
        if (status != SALTS_ASN1_OK) { std::fprintf(stderr, "create_sequence=%d\n", (int)status); goto done; }
        status = salts_asn1_value_create_integer(128, &integer);
        if (status != SALTS_ASN1_OK) { std::fprintf(stderr, "create_integer=%d\n", (int)status); goto done; }
        status = salts_asn1_value_add_child(sequence, integer);
        if (status != SALTS_ASN1_OK) { std::fprintf(stderr, "add_child=%d\n", (int)status); goto done; }
        integer = nullptr; /* ownership transferred to sequence */
        status = salts_asn1_value_encode(sequence, SALTS_ASN1_DER, 0u,
                                          encoded, sizeof(encoded), &written);
        if (status != SALTS_ASN1_OK) { std::fprintf(stderr, "value_encode=%d\n", (int)status); goto done; }
        status = salts_asn1_value_decode(encoded, written, SALTS_ASN1_DER, &decoded);
        if (status != SALTS_ASN1_OK) { std::fprintf(stderr, "value_decode=%d\n", (int)status); goto done; }
        if (salts_asn1_value_child_count(decoded) != 1u) {
            std::fprintf(stderr, "child_count=%zu\n", salts_asn1_value_child_count(decoded)); goto done;
        }
        status = salts_asn1_value_get_integer_i64(
            salts_asn1_value_child_at(decoded, 0u), &number);
        if (status != SALTS_ASN1_OK || number != 128) {
            std::fprintf(stderr, "get_integer=%d number=%lld\n", (int)status, (long long)number); goto done;
        }
    }
    result = 0;

done:
    salts_asn1_value_free(decoded);
    salts_asn1_value_free(sequence);
    salts_asn1_value_free(integer);
    return result;
}
