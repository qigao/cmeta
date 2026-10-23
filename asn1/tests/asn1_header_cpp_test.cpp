#include <salts/asn1.h>
#include <salts/asn1_value.h>
#include "tinytest.hpp"
#include <cstdint>
#include <cstddef>
#include <type_traits>

static_assert(std::is_standard_layout<salts_asn1_tlv>::value, "TLV ABI");
static_assert(std::is_standard_layout<salts_asn1_reader>::value, "reader ABI");
static_assert(SALTS_ASN1_ABI_VERSION == 1u, "ASN.1 ABI version");

spec("Salts ASN.1 C++17 public header") {
    it("links the C ABI and validates DER") {
        const std::uint8_t encoded[] = {0x02,0x01,0x2a};
        check_equal(salts_asn1_validate(encoded, sizeof(encoded), SALTS_ASN1_DER), SALTS_ASN1_OK);
    }
    it("creates and releases an owned value through the C ABI") {
        salts_asn1_value *sequence = nullptr;
        salts_asn1_value *integer = nullptr;
        check_equal(salts_asn1_value_create_sequence(&sequence), SALTS_ASN1_OK);
        check_equal(salts_asn1_value_create_integer(42, &integer), SALTS_ASN1_OK);
        check_equal(salts_asn1_value_add_child(sequence, integer), SALTS_ASN1_OK);
        check_equal(salts_asn1_value_child_count(sequence), static_cast<std::size_t>(1u));
        salts_asn1_value_free(sequence);
    }
}
