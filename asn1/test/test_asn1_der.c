/**
 * @file test_asn1_der.c
 * @brief ASN.1 DER Encoder/Decoder Tests
 */

#include "tinytest.h"
#include "asn1_types.h"
#include <string.h>
#include <stdio.h>

spec("asn1_der") {
  describe("ASN.1 Value Creation") {
    it("should create a boolean value correctly") {
        asn1_value_t *v = asn1_create_boolean(1);
        check_not_null(v);
        check_equal(v->type, ASN1_TYPE_BOOLEAN);
        check_equal(v->value.boolean, 1);
        check_equal(v->tag, 0x01); // BOOLEAN tag
        asn1_free(v);
    }

    it("should create an integer value correctly") {
        asn1_value_t *v = asn1_create_integer(42);
        check_not_null(v);
        check_equal(v->type, ASN1_TYPE_INTEGER);
        check_equal((int)v->value.integer, 42);
        check_equal(v->tag, 0x02); // INTEGER tag
        asn1_free(v);
    }

    it("should create an octet string value correctly") {
        uint8_t data[] = {0x01, 0x02, 0x03};
        asn1_value_t *v = asn1_create_octet_string(data, sizeof(data));
        check_not_null(v);
        check_equal(v->type, ASN1_TYPE_OCTET_STRING);
        check_equal(v->value.octet_string.length, 3);
        check_equal(v->value.octet_string.data, data, 3);
        check_equal(v->tag, 0x04); // OCTET STRING tag
        asn1_free(v);
    }

    it("should create a null value correctly") {
        asn1_value_t *v = asn1_create_null();
        check_not_null(v);
        check_equal(v->type, ASN1_TYPE_NULL);
        check_equal(v->tag, 0x05); // NULL tag
        asn1_free(v);
    }

    it("should create a sequence and add a child correctly") {
        asn1_value_t *seq = asn1_create_sequence();
        check_not_null(seq);
        check_equal(seq->type, ASN1_TYPE_SEQUENCE);
        check_equal(seq->tag, 0x30); // SEQUENCE tag (constructed)

        asn1_value_t *child = asn1_create_integer(123);
        check_equal(asn1_sequence_add_child(seq, child), 0);
        check_equal(seq->value.sequence.count, 1);

        asn1_free(seq);
    }

    it("should create a set and add a child correctly") {
        asn1_value_t *set = asn1_create_set();
        check_not_null(set);
        check_equal(set->type, ASN1_TYPE_SET);
        check_equal(set->tag, 0x31); // SET tag (constructed)

        asn1_value_t *child = asn1_create_boolean(1);
        check_equal(asn1_set_add_child(set, child), 0);
        check_equal(set->value.set.count, 1);

        asn1_free(set);
    }
  }

  describe("String Type Creation") {
    it("should create a bit string value correctly") {
        uint8_t data[] = {0xAB, 0xCD};
        asn1_value_t *v = asn1_create_bit_string(data, 2, 4);  // 4 unused bits
        check_not_null(v);
        check_equal(v->type, ASN1_TYPE_BIT_STRING);
        check_equal(v->tag, 0x03); // BIT STRING tag
        // First byte should be unused bits count
        check_equal(v->value.octet_string.data[0], 4);
        check_equal(v->value.octet_string.data[1], 0xAB);
        check_equal(v->value.octet_string.data[2], 0xCD);
        asn1_free(v);
    }

    it("should create an OID from a string correctly") {
        asn1_value_t *v = asn1_create_oid_from_string("1.2.840.113549");
        check_not_null(v);
        check_equal(v->type, ASN1_TYPE_OBJECT_IDENTIFIER);
        check_equal(v->tag, 0x06); // OID tag
        check_equal(v->value.oid.count, 4);
        check_equal((int)v->value.oid.components[0], 1);
        check_equal((int)v->value.oid.components[1], 2);
        check_equal((int)v->value.oid.components[2], 840);
        check_equal((int)v->value.oid.components[3], 113549);
        asn1_free(v);
    }

    it("should create a printable string correctly") {
        asn1_value_t *v = asn1_create_printable_string("Hello World");
        check_not_null(v);
        check_equal(v->type, ASN1_TYPE_PRINTABLE_STRING);
        check_equal(v->tag, 0x13); // PrintableString tag
        check_equal(v->value.octet_string.length, 11);
        check_equal(v->value.octet_string.data, "Hello World", 11);
        asn1_free(v);
    }

    it("should create a UTF8 string correctly") {
        asn1_value_t *v = asn1_create_utf8_string("UTF8");
        check_not_null(v);
        check_equal(v->type, ASN1_TYPE_UTF8_STRING);
        check_equal(v->tag, 0x0C); // UTF8String tag
        check_equal(v->value.octet_string.length, 4);
        check_equal(v->value.octet_string.data, "UTF8", 4);
        asn1_free(v);
    }

    it("should create an IA5 string correctly") {
        asn1_value_t *v = asn1_create_ia5_string("test@example.com");
        check_not_null(v);
        check_equal(v->type, ASN1_TYPE_IA5_STRING);
        check_equal(v->tag, 0x16); // IA5String tag
        check_equal(v->value.octet_string.length, 16);
        check_equal(v->value.octet_string.data, "test@example.com", 16);
        asn1_free(v);
    }

    it("should create a UTC time string correctly") {
        asn1_value_t *v = asn1_create_utc_time("231231235959Z");
        check_not_null(v);
        check_equal(v->type, ASN1_TYPE_UTC_TIME);
        check_equal(v->tag, 0x17); // UTCTime tag
        check_equal(v->value.octet_string.length, 13);
        check_equal(v->value.octet_string.data, "231231235959Z", 13);
        asn1_free(v);
    }
  }

  describe("Binary Parsing") {
    it("should parse boolean TRUE correctly") {
        uint8_t data[] = {0x01, 0x01, 0xFF};  // BOOLEAN TRUE
        asn1_value_t *v = NULL;
        
        check_equal(scan_binary_asn1(data, sizeof(data), &v), 0);
        check_not_null(v);
        check_equal(v->type, ASN1_TYPE_BOOLEAN);
        check(v->value.boolean);
        
        asn1_free(v);
    }

    it("should parse boolean FALSE correctly") {
        uint8_t data[] = {0x01, 0x01, 0x00};  // BOOLEAN FALSE
        asn1_value_t *v = NULL;
        
        check_equal(scan_binary_asn1(data, sizeof(data), &v), 0);
        check_not_null(v);
        check_equal(v->type, ASN1_TYPE_BOOLEAN);
        check(!v->value.boolean);
        
        asn1_free(v);
    }

    it("should parse an integer correctly") {
        uint8_t data[] = {0x02, 0x01, 0x2A};  // INTEGER 42
        asn1_value_t *v = NULL;
        
        check_equal(scan_binary_asn1(data, sizeof(data), &v), 0);
        check_not_null(v);
        check_equal(v->type, ASN1_TYPE_INTEGER);
        check_equal((int)v->value.integer, 42);
        
        asn1_free(v);
    }

    it("should parse an octet string correctly") {
        uint8_t data[] = {0x04, 0x03, 0xAB, 0xCD, 0xEF};  // OCTET STRING
        asn1_value_t *v = NULL;
        
        check_equal(scan_binary_asn1(data, sizeof(data), &v), 0);
        check_not_null(v);
        check_equal(v->type, ASN1_TYPE_OCTET_STRING);
        check_equal(v->value.octet_string.length, 3);
        check_equal(v->value.octet_string.data[0], 0xAB);
        check_equal(v->value.octet_string.data[1], 0xCD);
        check_equal(v->value.octet_string.data[2], 0xEF);
        
        asn1_free(v);
    }

    it("should parse a null value correctly") {
        uint8_t data[] = {0x05, 0x00};  // NULL
        asn1_value_t *v = NULL;
        
        check_equal(scan_binary_asn1(data, sizeof(data), &v), 0);
        check_not_null(v);
        check_equal(v->type, ASN1_TYPE_NULL);
        
        asn1_free(v);
    }

    it("should parse a sequence correctly") {
        // SEQUENCE { INTEGER 42, BOOLEAN TRUE }
        // 30 06 02 01 2A 01 01 FF
        uint8_t data[] = {0x30, 0x06, 0x02, 0x01, 0x2A, 0x01, 0x01, 0xFF};
        asn1_value_t *v = NULL;
        
        check_equal(scan_binary_asn1(data, sizeof(data), &v), 0);
        check_not_null(v);
        check_equal(v->type, ASN1_TYPE_SEQUENCE);
        check_equal(v->value.sequence.count, 2);

        check_equal(v->value.sequence.children[0]->type, ASN1_TYPE_INTEGER);
        check_equal((int)v->value.sequence.children[0]->value.integer, 42);

        check_equal(v->value.sequence.children[1]->type, ASN1_TYPE_BOOLEAN);
        check(v->value.sequence.children[1]->value.boolean);
        
        asn1_free(v);
    }
  }

  describe("Complex Structures") {
    it("should handle nested sequences correctly") {
        // Create nested structure: SEQUENCE { SEQUENCE { INTEGER 1 }, BOOLEAN TRUE }
        asn1_value_t *outer = asn1_create_sequence();
        asn1_value_t *inner = asn1_create_sequence();
        
        asn1_sequence_add_child(inner, asn1_create_integer(1));
        asn1_sequence_add_child(outer, inner);
        asn1_sequence_add_child(outer, asn1_create_boolean(1));
        
        check_equal(outer->value.sequence.count, 2);
        check_equal(outer->value.sequence.children[0]->type, ASN1_TYPE_SEQUENCE);
        check_equal(outer->value.sequence.children[0]->value.sequence.count, 1);
        check_equal(outer->value.sequence.children[1]->type, ASN1_TYPE_BOOLEAN);
        
        asn1_free(outer);
    }

    it("should handle mixed types within a sequence correctly") {
        asn1_value_t *seq = asn1_create_sequence();
        
        asn1_sequence_add_child(seq, asn1_create_integer(123));
        asn1_sequence_add_child(seq, asn1_create_boolean(0));
        asn1_sequence_add_child(seq, asn1_create_null());
        
        uint8_t data[] = {0x01, 0x02, 0x03};
        asn1_sequence_add_child(seq, asn1_create_octet_string(data, 3));
        
        check_equal(seq->value.sequence.count, 4);
        check_equal(seq->value.sequence.children[0]->type, ASN1_TYPE_INTEGER);
        check_equal(seq->value.sequence.children[1]->type, ASN1_TYPE_BOOLEAN);
        check_equal(seq->value.sequence.children[2]->type, ASN1_TYPE_NULL);
        check_equal(seq->value.sequence.children[3]->type, ASN1_TYPE_OCTET_STRING);
        
        asn1_free(seq);
    }
  }

  describe("Error Handling") {
    it("should fail gracefully when parsing invalid data tag") {
        uint8_t data[] = {0xFF, 0xFF};  // Invalid tag
        asn1_value_t *v = NULL;
        
        // Should fail gracefully
        int result = scan_binary_asn1(data, sizeof(data), &v);
        check(result != 0);
        check_null(v);
    }

    it("should fail gracefully when parsing truncated data") {
        uint8_t data[] = {0x02, 0x05};  // INTEGER with length 5 but no data
        asn1_value_t *v = NULL;
        
        // Should fail gracefully
        int result = scan_binary_asn1(data, sizeof(data), &v);
        check(result != 0);
        check_null(v);
    }
  }

  describe("Utility Functions") {
    it("should correctly compare OIDs") {
        asn1_value_t *oid1 = asn1_create_oid_from_string("1.2.3.4");
        asn1_value_t *oid2 = asn1_create_oid_from_string("1.2.3.4");
        asn1_value_t *oid3 = asn1_create_oid_from_string("1.2.3.5");
        
        check_equal(asn1_compare_oid(&oid1->value.oid, &oid2->value.oid), 0);
        check(asn1_compare_oid(&oid1->value.oid, &oid3->value.oid) != 0);
        
        asn1_free(oid1);
        asn1_free(oid2);
        asn1_free(oid3);
    }

    it("should correctly convert OID structured value to string representation") {
        asn1_value_t *oid = asn1_create_oid_from_string("2.5.4.3");
        char *str = asn1_oid_to_string(&oid->value.oid);
        
        check_not_null(str);
        check_equal(str, "2.5.4.3");
        
        free(str);
        asn1_free(oid);
    }
  }
}