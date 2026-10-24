#ifndef SALTS_ASN1_VALUE_H
#define SALTS_ASN1_VALUE_H

#include <salts/asn1.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct salts_asn1_value salts_asn1_value;

/* Creates an owned value. Primitive content is copied; constructed values
 * must start empty and acquire children through add_child. */
SALTS_ASN1_PUBLIC salts_asn1_status salts_asn1_value_create(
    uint8_t tag_class, uint8_t constructed, uint32_t tag_number,
    const void *content, size_t content_size, salts_asn1_value **out);

SALTS_ASN1_PUBLIC salts_asn1_status salts_asn1_value_create_sequence(
    salts_asn1_value **out);
SALTS_ASN1_PUBLIC salts_asn1_status salts_asn1_value_create_set(
    salts_asn1_value **out);
SALTS_ASN1_PUBLIC salts_asn1_status salts_asn1_value_create_boolean(
    int value, salts_asn1_value **out);
SALTS_ASN1_PUBLIC salts_asn1_status salts_asn1_value_create_integer(
    int64_t value, salts_asn1_value **out);
/* Creates a non-negative DER INTEGER from an unsigned big-endian magnitude.
 * Leading zero bytes are ignored. A zero-length/all-zero magnitude encodes as
 * INTEGER 0. The input is copied and may be released after return. */
SALTS_ASN1_PUBLIC salts_asn1_status salts_asn1_value_create_unsigned_integer(
    const void *magnitude, size_t magnitude_size, salts_asn1_value **out);
SALTS_ASN1_PUBLIC salts_asn1_status salts_asn1_value_create_bit_string(
    const void *data, size_t size, uint8_t unused_bits,
    salts_asn1_value **out);
SALTS_ASN1_PUBLIC salts_asn1_status salts_asn1_value_create_octet_string(
    const void *data, size_t size, salts_asn1_value **out);
SALTS_ASN1_PUBLIC salts_asn1_status salts_asn1_value_create_null(
    salts_asn1_value **out);
SALTS_ASN1_PUBLIC salts_asn1_status salts_asn1_value_create_oid(
    const uint32_t *components, size_t count, salts_asn1_value **out);

/* Ownership transfers only when add_child returns OK. A child can have one
 * parent only. Freeing a parent recursively frees all children. */
SALTS_ASN1_PUBLIC salts_asn1_status salts_asn1_value_add_child(
    salts_asn1_value *parent, salts_asn1_value *child);
SALTS_ASN1_PUBLIC void salts_asn1_value_free(salts_asn1_value *value);

SALTS_ASN1_PUBLIC size_t salts_asn1_value_child_count(
    const salts_asn1_value *value);
SALTS_ASN1_PUBLIC const salts_asn1_value *salts_asn1_value_child_at(
    const salts_asn1_value *value, size_t index);
SALTS_ASN1_PUBLIC salts_asn1_status salts_asn1_value_get_tag(
    const salts_asn1_value *value, uint8_t *tag_class,
    uint8_t *constructed, uint32_t *tag_number);
/* Returns a borrowed view into value-owned primitive bytes. */
SALTS_ASN1_PUBLIC salts_asn1_status salts_asn1_value_get_content(
    const salts_asn1_value *value, const uint8_t **data, size_t *size);
SALTS_ASN1_PUBLIC salts_asn1_status salts_asn1_value_get_integer_i64(
    const salts_asn1_value *value, int64_t *out);
/* Returns a borrowed minimal unsigned magnitude. INTEGER 0 is returned as one
 * zero byte. Negative INTEGER values are rejected as MALFORMED. */
SALTS_ASN1_PUBLIC salts_asn1_status salts_asn1_value_get_unsigned_integer(
    const salts_asn1_value *value, const uint8_t **magnitude,
    size_t *magnitude_size);
/* Returns a borrowed view excluding the initial unused-bits octet. */
SALTS_ASN1_PUBLIC salts_asn1_status salts_asn1_value_get_bit_string(
    const salts_asn1_value *value, const uint8_t **data, size_t *size,
    uint8_t *unused_bits);
SALTS_ASN1_PUBLIC salts_asn1_status salts_asn1_value_get_boolean(
    const salts_asn1_value *value, int *out);
/* If components is NULL, capacity must be zero; required_count is always set
 * on success or BUFFER_TOO_SMALL. OID arcs are limited to uint32_t. */
SALTS_ASN1_PUBLIC salts_asn1_status salts_asn1_value_get_oid(
    const salts_asn1_value *value, uint32_t *components, size_t capacity,
    size_t *required_count);

/* Decodes exactly one complete BER/DER value into an owned tree. Primitive
 * payloads are preserved byte-for-byte; typed accessors interpret common
 * universal types. The input buffer can be released after successful decode. */
SALTS_ASN1_PUBLIC salts_asn1_status salts_asn1_value_decode(
    const void *data, size_t size, salts_asn1_mode mode,
    salts_asn1_value **out);

/* Encodes into caller-owned storage. If output is NULL and capacity is zero,
 * performs a validated size query. On error, caller output is not modified.
 * use_indefinite applies to all constructed nodes and is BER-only. */
SALTS_ASN1_PUBLIC salts_asn1_status salts_asn1_value_encode(
    const salts_asn1_value *value, salts_asn1_mode mode,
    uint8_t use_indefinite, void *output, size_t output_capacity,
    size_t *written);

#ifdef __cplusplus
}
#endif

#endif /* SALTS_ASN1_VALUE_H */
