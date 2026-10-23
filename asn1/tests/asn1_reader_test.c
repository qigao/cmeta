#include <salts/asn1.h>
#include "tinytest.h"
#include <stddef.h>
#include <stdint.h>

spec("Salts ASN.1 bounded BER/DER reader") {
    it("reads a TLV and opens validated constructed children") {
        static const uint8_t encoded[] = {0x30,0x06,0x02,0x01,0x2a,0x04,0x01,0xaa};
        salts_asn1_reader reader, children;
        salts_asn1_tlv root, integer, octets;
        check_equal(salts_asn1_reader_init(&reader, encoded, sizeof(encoded), SALTS_ASN1_DER), SALTS_ASN1_OK);
        check_equal(salts_asn1_reader_next(&reader, &root), SALTS_ASN1_OK);
        check_equal(root.tag_number, 16u);
        check_true(root.constructed != 0u);
        check_equal(root.content_size, (size_t)6u);
        check_equal(root.encoded_size, sizeof(encoded));
        check_equal(salts_asn1_reader_next(&reader, &octets), SALTS_ASN1_DONE);
        check_equal(salts_asn1_reader_open_children(&reader, &root, &children), SALTS_ASN1_OK);
        check_equal(salts_asn1_reader_next(&children, &integer), SALTS_ASN1_OK);
        check_equal(integer.tag_number, 2u);
        check_equal(integer.content[0], 0x2au);
        check_equal(salts_asn1_reader_next(&children, &octets), SALTS_ASN1_OK);
        check_equal(octets.tag_number, 4u);
        check_equal(octets.content[0], 0xaau);
        check_equal(salts_asn1_reader_next(&children, &integer), SALTS_ASN1_DONE);
    }
    it("accepts BER indefinite length and excludes EOC from content") {
        static const uint8_t encoded[] = {0x30,0x80,0x02,0x01,0x01,0x00,0x00};
        salts_asn1_reader reader, children;
        salts_asn1_tlv root, child;
        check_equal(salts_asn1_validate(encoded, sizeof(encoded), SALTS_ASN1_BER), SALTS_ASN1_OK);
        check_equal(salts_asn1_validate(encoded, sizeof(encoded), SALTS_ASN1_DER), SALTS_ASN1_INVALID_LENGTH);
        check_equal(salts_asn1_reader_init(&reader, encoded, sizeof(encoded), SALTS_ASN1_BER), SALTS_ASN1_OK);
        check_equal(salts_asn1_reader_next(&reader, &root), SALTS_ASN1_OK);
        check_true(root.indefinite_length != 0u);
        check_equal(root.content_size, (size_t)3u);
        check_equal(root.encoded_size, sizeof(encoded));
        check_equal(salts_asn1_reader_open_children(&reader, &root, &children), SALTS_ASN1_OK);
        check_equal(salts_asn1_reader_next(&children, &child), SALTS_ASN1_OK);
        check_equal(child.tag_number, 2u);
        check_equal(salts_asn1_reader_next(&children, &child), SALTS_ASN1_DONE);
    }
    it("preserves high tag numbers") {
        static const uint8_t encoded[] = {0x9f,0x81,0x00,0x00};
        salts_asn1_reader reader; salts_asn1_tlv value;
        check_equal(salts_asn1_reader_init(&reader, encoded, sizeof(encoded), SALTS_ASN1_BER), SALTS_ASN1_OK);
        check_equal(salts_asn1_reader_next(&reader, &value), SALTS_ASN1_OK);
        check_equal(value.tag_class, 2u);
        check_equal(value.tag_number, 128u);
        check_equal(value.content_size, (size_t)0u);
    }
    it("rejects malformed children transactionally") {
        static const uint8_t malformed[] = {0x30,0x03,0x02,0x02,0x01};
        salts_asn1_reader reader; salts_asn1_tlv value = {0};
        value.tag_number = 77u;
        check_equal(salts_asn1_reader_init(&reader, malformed, sizeof(malformed), SALTS_ASN1_BER), SALTS_ASN1_OK);
        check_equal(salts_asn1_reader_next(&reader, &value), SALTS_ASN1_UNEXPECTED_END);
        check_equal(reader.offset, (size_t)0u);
        check_equal(value.tag_number, 77u);
    }
    it("rejects trailing data when validating a single value") {
        static const uint8_t encoded[] = {0x02,0x01,0x01,0x05,0x00};
        check_equal(salts_asn1_validate(encoded, sizeof(encoded), SALTS_ASN1_DER), SALTS_ASN1_TRAILING_DATA);
    }
    it("rejects malformed high tags, lengths and truncated input") {
        static const uint8_t bad_tag[] = {0x1f,0x1e,0x00};
        static const uint8_t bad_length[] = {0x04,0x82,0x01};
        static const uint8_t truncated[] = {0x04};
        check_equal(salts_asn1_validate(bad_tag, sizeof(bad_tag), SALTS_ASN1_BER), SALTS_ASN1_INVALID_TAG);
        check_equal(salts_asn1_validate(bad_length, sizeof(bad_length), SALTS_ASN1_BER), SALTS_ASN1_UNEXPECTED_END);
        check_equal(salts_asn1_validate(truncated, sizeof(truncated), SALTS_ASN1_BER), SALTS_ASN1_UNEXPECTED_END);
    }
    it("enforces DER integer, BOOLEAN and SET canonical rules") {
        static const uint8_t padded_int[] = {0x02,0x02,0x00,0x01};
        static const uint8_t noncanonical_true[] = {0x01,0x01,0x01};
        static const uint8_t unsorted_set[] = {0x31,0x06,0x02,0x01,0x02,0x02,0x01,0x01};
        static const uint8_t sorted_set[] = {0x31,0x06,0x02,0x01,0x01,0x02,0x01,0x02};
        check_equal(salts_asn1_validate(padded_int, sizeof(padded_int), SALTS_ASN1_DER), SALTS_ASN1_MALFORMED);
        check_equal(salts_asn1_validate(noncanonical_true, sizeof(noncanonical_true), SALTS_ASN1_DER), SALTS_ASN1_MALFORMED);
        check_equal(salts_asn1_validate(unsorted_set, sizeof(unsorted_set), SALTS_ASN1_DER), SALTS_ASN1_MALFORMED);
        check_equal(salts_asn1_validate(sorted_set, sizeof(sorted_set), SALTS_ASN1_DER), SALTS_ASN1_OK);
    }
    it("rejects invalid initialization and top-level EOC") {
        static const uint8_t eoc[] = {0x00,0x00};
        salts_asn1_reader reader; salts_asn1_tlv value;
        check_equal(salts_asn1_reader_init(&reader, NULL, 1u, SALTS_ASN1_BER), SALTS_ASN1_INVALID_ARGUMENT);
        check_equal(salts_asn1_reader_init(&reader, eoc, sizeof(eoc), SALTS_ASN1_BER), SALTS_ASN1_OK);
        check_equal(salts_asn1_reader_next(&reader, &value), SALTS_ASN1_MALFORMED);
    }
}
