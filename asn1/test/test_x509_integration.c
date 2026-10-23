/**
 * @file test_x509_integration.c
 * @brief X.509 Integration Tests
 */

#include "tinytest.h"
#include "asn1_types.h"
#include "asn1/x509_cert.h"
#include "asn1/asn1_der_compat.h"
#include <string.h>
#include <stdio.h>

spec("x509_integration") {
  describe("X.509 Certificate Structure") {
    it("should correctly handle a simplified X.509 certificate structure creation") {
        // Create a simplified X.509 certificate structure
        asn1_value_t *cert = asn1_create_sequence();
        
        // TBSCertificate (simplified)
        asn1_value_t *tbs = asn1_create_sequence();
        asn1_sequence_add_child(tbs, asn1_create_integer(1)); // version
        asn1_sequence_add_child(tbs, asn1_create_integer(12345)); // serial
        
        // Add TBS to certificate
        asn1_sequence_add_child(cert, tbs);
        
        // Algorithm Identifier (simplified)
        asn1_value_t *alg = asn1_create_sequence();
        asn1_sequence_add_child(alg, asn1_create_oid_from_string("1.2.840.113549.1.1.11")); // SHA256-RSA
        asn1_sequence_add_child(cert, alg);
        
        // Signature (dummy)
        uint8_t sig_data[] = {0x00, 0x01, 0x02, 0x03}; // Dummy signature
        asn1_value_t *signature = asn1_create_bit_string(sig_data, sizeof(sig_data), 0);
        asn1_sequence_add_child(cert, signature);
        
        // Verify structure
        check_not_null(cert);
        check_equal(cert->type, ASN1_TYPE_SEQUENCE);
        check_equal(cert->value.sequence.count, 3);
        
        // Check TBS
        asn1_value_t *tbs_check = cert->value.sequence.children[0];
        check_equal(tbs_check->type, ASN1_TYPE_SEQUENCE);
        check_equal(tbs_check->value.sequence.count, 2);
        
        // Check algorithm
        asn1_value_t *alg_check = cert->value.sequence.children[1];
        check_equal(alg_check->type, ASN1_TYPE_SEQUENCE);
        check_equal(alg_check->value.sequence.count, 1);
        
        // Check signature
        asn1_value_t *sig_check = cert->value.sequence.children[2];
        check_equal(sig_check->type, ASN1_TYPE_BIT_STRING);
        
        asn1_free(cert);
    }

    it("should correctly handle an X.509 Name structure creation") {
        // Create an X.509 Name structure: SEQUENCE OF RDN
        asn1_value_t *name = asn1_create_sequence();
        
        // RDN for CN=Test
        asn1_value_t *rdn = asn1_create_set();
        asn1_value_t *attr = asn1_create_sequence();
        
        // Add CN OID
        asn1_sequence_add_child(attr, asn1_create_oid_from_string("2.5.4.3")); // CN
        asn1_sequence_add_child(attr, asn1_create_utf8_string("Test"));
        
        asn1_set_add_child(rdn, attr);
        asn1_sequence_add_child(name, rdn);
        
        // Verify structure
        check_not_null(name);
        check_equal(name->type, ASN1_TYPE_SEQUENCE);
        check_equal(name->value.sequence.count, 1);
        
        // Check RDN
        asn1_value_t *rdn_check = name->value.sequence.children[0];
        check_equal(rdn_check->type, ASN1_TYPE_SET);
        check_equal(rdn_check->value.set.count, 1);
        
        // Check attribute
        asn1_value_t *attr_check = rdn_check->value.set.children[0];
        check_equal(attr_check->type, ASN1_TYPE_SEQUENCE);
        check_equal(attr_check->value.sequence.count, 2);
        
        // Check OID
        asn1_value_t *oid_check = attr_check->value.sequence.children[0];
        check_equal(oid_check->type, ASN1_TYPE_OBJECT_IDENTIFIER);
        
        // Check value
        asn1_value_t *val_check = attr_check->value.sequence.children[1];
        check_equal(val_check->type, ASN1_TYPE_UTF8_STRING);
        
        asn1_free(name);
    }

    it("should correctly handle an X.509 Extension structure creation") {
        // Create an X.509 Extension structure
        asn1_value_t *ext = asn1_create_sequence();
        
        // Extension OID (Basic Constraints)
        asn1_sequence_add_child(ext, asn1_create_oid_from_string("2.5.29.19"));
        
        // Critical flag
        asn1_sequence_add_child(ext, asn1_create_boolean(1));
        
        // Extension value (DER-encoded)
        uint8_t ext_value[] = {0x30, 0x03, 0x01, 0x01, 0xFF}; // SEQUENCE { BOOLEAN TRUE }
        asn1_sequence_add_child(ext, asn1_create_octet_string(ext_value, sizeof(ext_value)));
        
        // Verify structure
        check_not_null(ext);
        check_equal(ext->type, ASN1_TYPE_SEQUENCE);
        check_equal(ext->value.sequence.count, 3);
        
        // Check OID
        asn1_value_t *oid = ext->value.sequence.children[0];
        check_equal(oid->type, ASN1_TYPE_OBJECT_IDENTIFIER);
        
        // Check critical flag
        asn1_value_t *critical = ext->value.sequence.children[1];
        check_equal(critical->type, ASN1_TYPE_BOOLEAN);
        check(critical->value.boolean);
        
        // Check value
        asn1_value_t *value = ext->value.sequence.children[2];
        check_equal(value->type, ASN1_TYPE_OCTET_STRING);
        check_equal(value->value.octet_string.length, 5);
        
        asn1_free(ext);
    }
  }

  describe("Context-Specific Tags") {
    it("should correctly handle creation of context-specific tags used in X.509") {
        // Test context-specific tags used in X.509
        
        // Create a version field [0] EXPLICIT
        asn1_value_t *version_ctx = asn1_create_sequence();
        version_ctx->tag = 0xA0;  // [0] EXPLICIT
        version_ctx->tag_class = 2; // Context-specific
        version_ctx->constructed = 1; // Constructed
        version_ctx->tag_number = 0; // [0]
        
        asn1_sequence_add_child(version_ctx, asn1_create_integer(2)); // v3
        
        check_equal(version_ctx->tag, 0xA0);
        check_equal(version_ctx->tag_class, 2);
        check_equal(version_ctx->constructed, 1);
        check_equal(version_ctx->tag_number, 0);
        
        asn1_free(version_ctx);
    }
  }

  describe("OID Functionality") {
    it("should verify common X.509 Object Identifiers correctly") {
        // Test common X.509 OIDs
        struct {
            const char *oid_str;
            const char *name;
        } test_oids[] = {
            {"2.5.4.3", "CN"},
            {"2.5.4.6", "C"},
            {"2.5.4.10", "O"},
            {"1.2.840.113549.1.1.11", "SHA256-RSA"},
            {"2.5.29.19", "Basic Constraints"},
            {"2.5.29.15", "Key Usage"},
            {NULL, NULL}
        };
        
        for (int i = 0; test_oids[i].oid_str != NULL; i++) {
            asn1_value_t *oid = asn1_create_oid_from_string(test_oids[i].oid_str);
            check_not_null(oid);
            if (oid) {
                check_equal(oid->type, ASN1_TYPE_OBJECT_IDENTIFIER);
                
                // Convert back to string and compare
                char *str = asn1_oid_to_string(&oid->value.oid);
                check_not_null(str);
                if (str) {
                    check_equal(str, test_oids[i].oid_str);
                    free(str);
                }
                asn1_free(oid);
            }
        }
    }
  }

  describe("Binary Parsing Integration") {
    it("should allow creating an X.509-like structure and verify it") {
        // Create a simple X.509-like structure (without full roundtrip encoding check)
        asn1_value_t *cert = asn1_create_sequence();
        asn1_value_t *tbs = asn1_create_sequence();
        
        asn1_sequence_add_child(tbs, asn1_create_integer(1));
        asn1_sequence_add_child(tbs, asn1_create_integer(12345));
        asn1_sequence_add_child(cert, tbs);
        
        check_not_null(cert);
        check_equal(cert->type, ASN1_TYPE_SEQUENCE);
        check_equal(cert->value.sequence.count, 1);
        
        asn1_free(cert);
    }
  }

  describe("Error Handling and Parameters Management") {
    it("should return NULL for invalid OID string patterns") {
        // Test invalid OID strings
        check_null(asn1_create_oid_from_string("invalid"));
        check_null(asn1_create_oid_from_string("1.2.3.a"));
        check_null(asn1_create_oid_from_string(""));
        check_null(asn1_create_oid_from_string(NULL));
    }

    it("should fail gracefully when given NULL parameters to string types") {
        // Test that functions handle NULL parameters gracefully
        check_null(asn1_create_octet_string(NULL, 10)); // NULL data with non-zero length should fail
        check_null(asn1_create_utf8_string(NULL));
        check_null(asn1_create_printable_string(NULL));
        check_null(asn1_create_ia5_string(NULL));
        check_null(asn1_create_utc_time(NULL));
    }

    it("should succeed with empty octet strings (NULL data and zero length)") {
        // Test that empty octet string works (NULL data with zero length)
        asn1_value_t *empty_octet = asn1_create_octet_string(NULL, 0);
        check_not_null(empty_octet);
        if (empty_octet) {
            check_equal(empty_octet->type, ASN1_TYPE_OCTET_STRING);
            check_equal(empty_octet->value.octet_string.length, 0);
            check_null(empty_octet->value.octet_string.data);
            asn1_free(empty_octet);
        }
    }

    it("should handle NULL parent or child in sequence manipulation functions") {
        // Test add_child with NULL
        asn1_value_t *seq = asn1_create_sequence();
        check_equal(asn1_sequence_add_child(NULL, seq), -1);
        check_equal(asn1_sequence_add_child(seq, NULL), -1);
        asn1_free(seq);
    }
  }
}