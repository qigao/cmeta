#include <salts/asn1_value.h>
#include "tinytest.h"

#include <limits.h>
#include <stdint.h>
#include <string.h>

spec("Salts ASN.1 owned value tree") {
    it("round trips a typed DER tree and releases the input buffer") {
        static const uint8_t payload[] = {0x00, 0x7f, 0x80, 0xff};
        const uint32_t oid_arcs[] = {2u, 4294967295u, 1u, 3u, 6u, 1u, 4u, 1u, 55555u};
        uint8_t encoded[256];
        uint8_t sentinel[8];
        size_t written = 0u, required = 0u, i;
        salts_asn1_value *root = NULL, *integer = NULL, *boolean = NULL;
        salts_asn1_value *octets = NULL, *oid = NULL, *null_value = NULL;
        salts_asn1_value *decoded = NULL;
        const salts_asn1_value *child;
        const uint8_t *content = NULL;
        size_t content_size = 0u;
        int64_t integer_value = 0;
        int boolean_value = 0;
        uint32_t arcs[9];
        size_t arc_count = 0u;

        check_equal(salts_asn1_value_create_sequence(&root), SALTS_ASN1_OK);
        check_equal(salts_asn1_value_create_integer(INT64_MIN, &integer), SALTS_ASN1_OK);
        check_equal(salts_asn1_value_add_child(root, integer), SALTS_ASN1_OK);
        check_equal(salts_asn1_value_create_boolean(1, &boolean), SALTS_ASN1_OK);
        check_equal(salts_asn1_value_add_child(root, boolean), SALTS_ASN1_OK);
        check_equal(salts_asn1_value_create_octet_string(payload, sizeof(payload), &octets), SALTS_ASN1_OK);
        check_equal(salts_asn1_value_add_child(root, octets), SALTS_ASN1_OK);
        check_equal(salts_asn1_value_create_oid(oid_arcs, 9u, &oid), SALTS_ASN1_OK);
        check_equal(salts_asn1_value_add_child(root, oid), SALTS_ASN1_OK);
        check_equal(salts_asn1_value_create_null(&null_value), SALTS_ASN1_OK);
        check_equal(salts_asn1_value_add_child(root, null_value), SALTS_ASN1_OK);

        check_equal(salts_asn1_value_encode(root, SALTS_ASN1_DER, 0u,
                                            NULL, 0u, &required), SALTS_ASN1_OK);
        check_true(required > 0u && required <= sizeof(encoded));
        memset(encoded, 0xa5, sizeof(encoded));
        check_equal(salts_asn1_value_encode(root, SALTS_ASN1_DER, 0u,
                                            encoded, required - 1u, &written),
                    SALTS_ASN1_BUFFER_TOO_SMALL);
        check_equal(written, required);
        check_equal(encoded[0], 0xa5u);
        check_equal(salts_asn1_value_encode(root, SALTS_ASN1_DER, 0u,
                                            encoded, sizeof(encoded), &written),
                    SALTS_ASN1_OK);
        check_equal(written, required);
        check_equal(salts_asn1_validate(encoded, written, SALTS_ASN1_DER),
                    SALTS_ASN1_OK);
        check_equal(salts_asn1_value_decode(encoded, written, SALTS_ASN1_DER,
                                            &decoded), SALTS_ASN1_OK);
        check_equal(salts_asn1_value_child_count(decoded), (size_t)5u);

        child = salts_asn1_value_child_at(decoded, 0u);
        check_equal(salts_asn1_value_get_integer_i64(child, &integer_value), SALTS_ASN1_OK);
        check_equal(integer_value, INT64_MIN);
        child = salts_asn1_value_child_at(decoded, 1u);
        check_equal(salts_asn1_value_get_boolean(child, &boolean_value), SALTS_ASN1_OK);
        check_equal(boolean_value, 1);
        child = salts_asn1_value_child_at(decoded, 2u);
        check_equal(salts_asn1_value_get_content(child, &content, &content_size), SALTS_ASN1_OK);
        check_equal(content_size, sizeof(payload));
        check_equal(content, payload, sizeof(payload));
        child = salts_asn1_value_child_at(decoded, 3u);
        check_equal(salts_asn1_value_get_oid(child, NULL, 0u, &arc_count), SALTS_ASN1_OK);
        check_equal(arc_count, (size_t)9u);
        check_equal(salts_asn1_value_get_oid(child, arcs, 9u, &arc_count), SALTS_ASN1_OK);
        for (i = 0u; i < 9u; ++i) check_equal(arcs[i], oid_arcs[i]);

        memset(sentinel, 0x5a, sizeof(sentinel));
        written = 0u;
        check_equal(salts_asn1_value_encode(decoded, SALTS_ASN1_DER, 0u,
                                            sentinel, sizeof(sentinel), &written),
                    SALTS_ASN1_BUFFER_TOO_SMALL);
        check_equal(sentinel[0], 0x5au);
        salts_asn1_value_free(decoded);
        salts_asn1_value_free(root);
    }

    it("encodes BER indefinite constructed values but rejects them in DER") {
        salts_asn1_value *root = NULL, *integer = NULL, *decoded = NULL;
        uint8_t encoded[32];
        size_t written = 0u;
        check_equal(salts_asn1_value_create_sequence(&root), SALTS_ASN1_OK);
        check_equal(salts_asn1_value_create_integer(42, &integer), SALTS_ASN1_OK);
        check_equal(salts_asn1_value_add_child(root, integer), SALTS_ASN1_OK);
        check_equal(salts_asn1_value_encode(root, SALTS_ASN1_BER, 1u,
                                            encoded, sizeof(encoded), &written),
                    SALTS_ASN1_OK);
        check_equal(encoded[1], 0x80u);
        check_equal(encoded[written - 2u], 0u);
        check_equal(encoded[written - 1u], 0u);
        check_equal(salts_asn1_value_decode(encoded, written, SALTS_ASN1_BER,
                                            &decoded), SALTS_ASN1_OK);
        check_equal(salts_asn1_value_child_count(decoded), (size_t)1u);
        check_equal(salts_asn1_value_encode(root, SALTS_ASN1_DER, 1u,
                                            encoded, sizeof(encoded), &written),
                    SALTS_ASN1_INVALID_LENGTH);
        salts_asn1_value_free(decoded);
        salts_asn1_value_free(root);
    }

    it("rejects cycles, reused children and non-canonical DER SET ordering") {
        salts_asn1_value *root = NULL, *child = NULL, *set = NULL;
        salts_asn1_value *two = NULL, *one = NULL;
        uint8_t output[32];
        size_t written = 0u;
        memset(output, 0x6b, sizeof(output));
        check_equal(salts_asn1_value_create_sequence(&root), SALTS_ASN1_OK);
        check_equal(salts_asn1_value_create_sequence(&child), SALTS_ASN1_OK);
        check_equal(salts_asn1_value_add_child(root, child), SALTS_ASN1_OK);
        check_equal(salts_asn1_value_add_child(child, root), SALTS_ASN1_INVALID_ARGUMENT);
        check_equal(salts_asn1_value_add_child(root, child), SALTS_ASN1_INVALID_ARGUMENT);
        check_equal(salts_asn1_value_create_set(&set), SALTS_ASN1_OK);
        check_equal(salts_asn1_value_create_integer(2, &two), SALTS_ASN1_OK);
        check_equal(salts_asn1_value_create_integer(1, &one), SALTS_ASN1_OK);
        check_equal(salts_asn1_value_add_child(set, two), SALTS_ASN1_OK);
        check_equal(salts_asn1_value_add_child(set, one), SALTS_ASN1_OK);
        check_equal(salts_asn1_value_encode(set, SALTS_ASN1_DER, 0u,
                                            output, sizeof(output), &written),
                    SALTS_ASN1_MALFORMED);
        check_equal(output[0], 0x6bu);
        salts_asn1_value_free(root);
        salts_asn1_value_free(set);
    }

    it("rejects integer overflow and invalid OID component requests") {
        const uint8_t too_large[] = {0x02, 0x09, 0x01, 0, 0, 0, 0, 0, 0, 0, 0};
        const uint32_t bad_oid[] = {1u, 40u};
        salts_asn1_value *integer = NULL, *oid = NULL;
        int64_t value = 0;
        check_equal(salts_asn1_value_decode(too_large, sizeof(too_large),
                                            SALTS_ASN1_DER, &integer), SALTS_ASN1_OK);
        check_equal(salts_asn1_value_get_integer_i64(integer, &value),
                    SALTS_ASN1_LIMIT_EXCEEDED);
        check_equal(salts_asn1_value_create_oid(bad_oid, 2u, &oid),
                    SALTS_ASN1_INVALID_ARGUMENT);
        salts_asn1_value_free(integer);
    }
}
