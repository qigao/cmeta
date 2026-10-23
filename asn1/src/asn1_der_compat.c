#include <asn1/asn1_der_compat.h>
#include <stdlib.h>
#include <string.h>

// Legacy API compatibility implementations
int asn1_der_decode_legacy(const uint8_t *data, size_t len, asn1_value_t **result) {
    return scan_binary_asn1(data, len, result);
}

int asn1_der_encode_legacy(const asn1_value_t *value, uint8_t *buffer, size_t *buffer_len) {
    return asn1_der_encode(value, buffer, buffer_len);
}

void asn1_der_free_legacy(asn1_value_t *value) {
    asn1_free(value);
}

int asn1_der_get_tag(const asn1_value_t *value) {
    if (!value) return -1;
    
    // Map our types to legacy tag values
    switch (value->type) {
        case ASN1_TYPE_BOOLEAN: return ASN1_TAG_BOOLEAN;
        case ASN1_TYPE_INTEGER: return ASN1_TAG_INTEGER;
        case ASN1_TYPE_BIT_STRING: return ASN1_TAG_BIT_STRING;
        case ASN1_TYPE_OCTET_STRING: return ASN1_TAG_OCTET_STRING;
        case ASN1_TYPE_NULL: return ASN1_TAG_NULL;
        case ASN1_TYPE_OBJECT_IDENTIFIER: return ASN1_TAG_OBJECT_IDENTIFIER;
        case ASN1_TYPE_SEQUENCE: return ASN1_TAG_SEQUENCE;
        case ASN1_TYPE_SET: return ASN1_TAG_SET;
        default: return -1;
    }
}

const uint8_t *asn1_der_get_data(const asn1_value_t *value, size_t *len) {
    if (!value || !len) return NULL;
    
    switch (value->type) {
        case ASN1_TYPE_OCTET_STRING:
        case ASN1_TYPE_BIT_STRING:
            *len = value->value.octet_string.length;
            return value->value.octet_string.data;
        default:
            *len = 0;
            return NULL;
    }
}

size_t asn1_der_get_child_count(const asn1_value_t *value) {
    if (!value) return 0;
    
    switch (value->type) {
        case ASN1_TYPE_SEQUENCE:
            return value->value.sequence.count;
        case ASN1_TYPE_SET:
            return value->value.set.count;
        default:
            return 0;
    }
}

const asn1_value_t *asn1_der_get_child(const asn1_value_t *value, size_t index) {
    if (!value) return NULL;
    
    switch (value->type) {
        case ASN1_TYPE_SEQUENCE:
            if (index < value->value.sequence.count) {
                return value->value.sequence.children[index];
            }
            break;
        case ASN1_TYPE_SET:
            if (index < value->value.set.count) {
                return value->value.set.children[index];
            }
            break;
    }
    return NULL;
}