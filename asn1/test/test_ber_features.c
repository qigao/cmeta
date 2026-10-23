/**
 * @file test_ber_features.c
 * @brief BER-specific feature tests (indefinite length, constructed primitives)
 */

#include "tinytest.h"
#include "asn1_types.h"
#include <string.h>
#include <stdio.h>

spec("ber_features") {
  describe("Indefinite Length") {
    it("should parse an indefinite length sequence correctly") {
        // SEQUENCE with indefinite length containing INTEGER 42 and BOOLEAN TRUE
        // 30 80 02 01 2A 01 01 FF 00 00
        uint8_t data[] = {
            0x30, 0x80,           // SEQUENCE, indefinite length
            0x02, 0x01, 0x2A,     // INTEGER 42
            0x01, 0x01, 0xFF,     // BOOLEAN TRUE
            0x00, 0x00            // End-of-contents
        };
        
        asn1_value_t *v = NULL;
        check_equal(scan_binary_asn1(data, sizeof(data), &v), 0);
        check_not_null(v);
        
        if (v) {
            check_equal(v->type, ASN1_TYPE_SEQUENCE);
            check_equal(v->value.sequence.count, 2);
            
            // Check first child (INTEGER 42)
            check_equal(v->value.sequence.children[0]->type, ASN1_TYPE_INTEGER);
            check_equal((int)v->value.sequence.children[0]->value.integer, 42);
            
            // Check second child (BOOLEAN TRUE)
            check_equal(v->value.sequence.children[1]->type, ASN1_TYPE_BOOLEAN);
            check(v->value.sequence.children[1]->value.boolean);
            
            asn1_free(v);
        }
    }

    it("should handle nested indefinite length structures correctly") {
        // Nested indefinite length: SEQUENCE { SEQUENCE { INTEGER 1 } }
        // 30 80 30 80 02 01 01 00 00 00 00
        uint8_t data[] = {
            0x30, 0x80,           // Outer SEQUENCE, indefinite length
            0x30, 0x80,           // Inner SEQUENCE, indefinite length
            0x02, 0x01, 0x01,     // INTEGER 1
            0x00, 0x00,           // End-of-contents for inner SEQUENCE
            0x00, 0x00            // End-of-contents for outer SEQUENCE
        };
        
        asn1_value_t *v = NULL;
        check_equal(scan_binary_asn1(data, sizeof(data), &v), 0);
        check_not_null(v);
        
        if (v) {
            check_equal(v->type, ASN1_TYPE_SEQUENCE);
            check_equal(v->value.sequence.count, 1);
            
            // Check nested sequence
            asn1_value_t *inner = v->value.sequence.children[0];
            check_equal(inner->type, ASN1_TYPE_SEQUENCE);
            check_equal(inner->value.sequence.count, 1);
            
            // Check integer inside nested sequence
            check_equal(inner->value.sequence.children[0]->type, ASN1_TYPE_INTEGER);
            check_equal((int)inner->value.sequence.children[0]->value.integer, 1);
            
            asn1_free(v);
        }
    }
  }

  describe("Constructed Primitives (BER Features)") {
    it("should parse constructed octet strings by concatenating primitive chunks") {
        // Constructed OCTET STRING containing two primitive OCTET STRINGs
        // 24 80 04 03 AB CD EF 04 02 12 34 00 00
        uint8_t data[] = {
            0x24, 0x80,           // OCTET STRING, constructed, indefinite length
            0x04, 0x03, 0xAB, 0xCD, 0xEF,  // First chunk
            0x04, 0x02, 0x12, 0x34,        // Second chunk
            0x00, 0x00            // End-of-contents
        };
        
        asn1_value_t *v = NULL;
        check_equal(scan_binary_asn1(data, sizeof(data), &v), 0);
        check_not_null(v);
        
        if (v) {
            check_equal(v->type, ASN1_TYPE_OCTET_STRING);
            check_equal(v->value.octet_string.length, 5); // 3 + 2 bytes
            
            // Check concatenated content
            check_equal(v->value.octet_string.data[0], 0xAB);
            check_equal(v->value.octet_string.data[1], 0xCD);
            check_equal(v->value.octet_string.data[2], 0xEF);
            check_equal(v->value.octet_string.data[3], 0x12);
            check_equal(v->value.octet_string.data[4], 0x34);
            
            asn1_free(v);
        }
    }

    it("should parse constructed bit strings correctly") {
        // Constructed BIT STRING containing two primitive BIT STRINGs
        // 23 80 03 02 00 AB 03 03 04 CD EF 00 00
        uint8_t data[] = {
            0x23, 0x80,           // BIT STRING, constructed, indefinite length
            0x03, 0x02, 0x00, 0xAB,        // First chunk: 0 unused bits, data AB
            0x03, 0x03, 0x04, 0xCD, 0xEF,  // Second chunk: 4 unused bits, data CD EF
            0x00, 0x00            // End-of-contents
        };
        
        asn1_value_t *v = NULL;
        check_equal(scan_binary_asn1(data, sizeof(data), &v), 0);
        check_not_null(v);
        
        if (v) {
            check_equal(v->type, ASN1_TYPE_BIT_STRING);
            check_equal(v->value.octet_string.length, 4); // unused bits + 3 data bytes
            
            // Check unused bits (from first chunk only)
            check_equal(v->value.octet_string.data[0], 0x00);
            // Check concatenated data
            check_equal(v->value.octet_string.data[1], 0xAB);
            check_equal(v->value.octet_string.data[2], 0xCD);
            check_equal(v->value.octet_string.data[3], 0xEF);
            
            asn1_free(v);
        }
    }
  }

  describe("Mixed Length Encoding") {
    it("should handle mixed definite and indefinite length encodings") {
        // SEQUENCE (indefinite) containing SEQUENCE (definite) with INTEGER
        // 30 80 30 03 02 01 7B 00 00
        uint8_t data[] = {
            0x30, 0x80,           // Outer SEQUENCE, indefinite length
            0x30, 0x03,           // Inner SEQUENCE, definite length 3
            0x02, 0x01, 0x7B,     // INTEGER 123
            0x00, 0x00            // End-of-contents for outer SEQUENCE
        };
        
        asn1_value_t *v = NULL;
        check_equal(scan_binary_asn1(data, sizeof(data), &v), 0);
        check_not_null(v);
        
        if (v) {
            check_equal(v->type, ASN1_TYPE_SEQUENCE);
            check_equal(v->value.sequence.count, 1);
            
            // Check inner sequence
            asn1_value_t *inner = v->value.sequence.children[0];
            check_equal(inner->type, ASN1_TYPE_SEQUENCE);
            check_equal(inner->value.sequence.count, 1);
            
            // Check integer
            check_equal(inner->value.sequence.children[0]->type, ASN1_TYPE_INTEGER);
            check_equal((int)inner->value.sequence.children[0]->value.integer, 123);
            
            asn1_free(v);
        }
    }
  }

  describe("Long Form Length") {
    it("should successfully parse values with long form length encoding") {
        // OCTET STRING with long form length (length > 127)
        // Create 200-byte octet string: 04 81 C8 [200 bytes of 0xFF]
        uint8_t data[203];
        data[0] = 0x04;       // OCTET STRING tag
        data[1] = 0x81;       // Long form: 1 length octet follows
        data[2] = 0xC8;       // Length = 200
        
        // Fill with 0xFF bytes
        for (int i = 3; i < 203; i++) {
            data[i] = 0xFF;
        }
        
        asn1_value_t *v = NULL;
        check_equal(scan_binary_asn1(data, sizeof(data), &v), 0);
        check_not_null(v);
        
        if (v) {
            check_equal(v->type, ASN1_TYPE_OCTET_STRING);
            check_equal(v->value.octet_string.length, 200);
            
            // Check first and last bytes
            check_equal(v->value.octet_string.data[0], 0xFF);
            check_equal(v->value.octet_string.data[199], 0xFF);
            
            asn1_free(v);
        }
    }
  }

  describe("Error Handling") {
    it("should return an error when end-of-contents is missing for indefinite length") {
        // SEQUENCE with indefinite length but no end-of-contents
        uint8_t data[] = {
            0x30, 0x80,           // SEQUENCE, indefinite length
            0x02, 0x01, 0x2A      // INTEGER 42 (but no end-of-contents)
        };
        
        asn1_value_t *v = NULL;
        int result = scan_binary_asn1(data, sizeof(data), &v);
        check(result != 0); // Should fail
        check_null(v);
    }

    it("should handle malformed constructed primitive content gracefully") {
        // Constructed OCTET STRING with invalid content
        uint8_t data[] = {
            0x24, 0x80,           // OCTET STRING, constructed, indefinite length
            0xFF, 0xFF,           // Invalid tag/content
            0x00, 0x00            // End-of-contents
        };
        
        asn1_value_t *v = NULL;
        int result = scan_binary_asn1(data, sizeof(data), &v);
        // Should either fail or handle gracefully
        if (result == 0 && v) {
            asn1_free(v);
        }
        // Test passes if no crash occurs
        check(1);
    }
  }

  describe("BER vs DER Compatibility") {
    it("should successfully parse standard DER-encoded data using the BER parser") {
        // Test that DER-encoded data still works with BER parser
        // Standard DER SEQUENCE with definite length
        uint8_t data[] = {0x30, 0x06, 0x02, 0x01, 0x2A, 0x01, 0x01, 0xFF};
        
        asn1_value_t *v = NULL;
        check_equal(scan_binary_asn1(data, sizeof(data), &v), 0);
        check_not_null(v);
        
        if (v) {
            check_equal(v->type, ASN1_TYPE_SEQUENCE);
            check_equal(v->value.sequence.count, 2);
            
            asn1_free(v);
        }
    }
  }
}