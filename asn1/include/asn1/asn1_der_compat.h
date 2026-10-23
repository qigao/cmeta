#ifndef ASN1_DER_COMPAT_H
#define ASN1_DER_COMPAT_H


#include "asn1_api.h"
/**
 * @file asn1_der_compat.h
 * @brief Compatibility layer for old ASN.1 DER API
 * 
 * This provides compatibility for X.509 and crypto code that used
 * the old hand-written ASN.1 parser.
 */

#include "asn1_types.h"
#include "platform.h"

#ifdef __cplusplus
extern "C" {
#endif

// Legacy ASN.1 tag constants (for compatibility)
#define ASN1_TAG_BOOLEAN           0x01
#define ASN1_TAG_INTEGER           0x02
#define ASN1_TAG_BIT_STRING        0x03
#define ASN1_TAG_OCTET_STRING      0x04
#define ASN1_TAG_NULL              0x05
#define ASN1_TAG_OBJECT_IDENTIFIER 0x06
#define ASN1_TAG_UTF8_STRING       0x0C
#define ASN1_TAG_SEQUENCE          0x10  // Constructed: 0x30
#define ASN1_TAG_SET               0x11  // Constructed: 0x31
#define ASN1_TAG_PRINTABLE_STRING  0x13

// Legacy type mappings
typedef asn1_value_t asn1_der_value_t;

// Legacy API compatibility functions
SALTSNET_ASN1_C_API int asn1_der_decode_legacy(const uint8_t *data, size_t len, asn1_value_t **result);
SALTSNET_ASN1_C_API int asn1_der_encode_legacy(const asn1_value_t *value, uint8_t *buffer, size_t *buffer_len);
SALTSNET_ASN1_C_API void asn1_der_free_legacy(asn1_value_t *value);

// Legacy structure access helpers
SALTSNET_ASN1_C_API int asn1_der_get_tag(const asn1_value_t *value);
SALTSNET_ASN1_C_API const uint8_t *asn1_der_get_data(const asn1_value_t *value, size_t *len);
SALTSNET_ASN1_C_API size_t asn1_der_get_child_count(const asn1_value_t *value);
SALTSNET_ASN1_C_API const asn1_value_t *asn1_der_get_child(const asn1_value_t *value, size_t index);

#ifdef __cplusplus
}
#endif

#endif // ASN1_DER_COMPAT_H