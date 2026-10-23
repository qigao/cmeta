#include <salts/asn1_value.h>

#include <cstdint>

int main() {
    salts_asn1_value *sequence = nullptr;
    salts_asn1_value *integer = nullptr;
    salts_asn1_value *decoded = nullptr;
    std::uint8_t encoded[32]{};
    std::size_t written = 0u;
    std::int64_t number = 0;
    int result = 1;

    if (salts_asn1_value_create_sequence(&sequence) != SALTS_ASN1_OK)
        goto done;
    if (salts_asn1_value_create_integer(128, &integer) != SALTS_ASN1_OK)
        goto done;
    if (salts_asn1_value_add_child(sequence, integer) != SALTS_ASN1_OK)
        goto done;
    integer = nullptr; /* ownership transferred to sequence */
    if (salts_asn1_value_encode(sequence, SALTS_ASN1_DER, 0u,
                                encoded, sizeof(encoded), &written) != SALTS_ASN1_OK)
        goto done;
    if (salts_asn1_value_decode(encoded, written, SALTS_ASN1_DER, &decoded) != SALTS_ASN1_OK)
        goto done;
    if (salts_asn1_value_child_count(decoded) != 1u)
        goto done;
    if (salts_asn1_value_get_integer_i64(
            salts_asn1_value_child_at(decoded, 0u), &number) != SALTS_ASN1_OK ||
        number != 128)
        goto done;
    result = 0;

done:
    salts_asn1_value_free(decoded);
    salts_asn1_value_free(sequence);
    salts_asn1_value_free(integer);
    return result;
}
