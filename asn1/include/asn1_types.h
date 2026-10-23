#ifndef ASN1_TYPES_H
#define ASN1_TYPES_H


#include "asn1_api.h"
#include <stdint.h>
#include <stddef.h>
#include "platform.h"

#ifdef __cplusplus
extern "C" {
#endif

// ASN.1 Universal tag numbers
#define ASN1_TYPE_BOOLEAN           1
#define ASN1_TYPE_INTEGER           2
#define ASN1_TYPE_BIT_STRING        3
#define ASN1_TYPE_OCTET_STRING      4
#define ASN1_TYPE_NULL              5
#define ASN1_TYPE_OBJECT_IDENTIFIER 6
#define ASN1_TYPE_UTF8_STRING       12
#define ASN1_TYPE_SEQUENCE          16
#define ASN1_TYPE_SET               17
#define ASN1_TYPE_PRINTABLE_STRING  19
#define ASN1_TYPE_IA5_STRING        22
#define ASN1_TYPE_UTC_TIME          23
#define ASN1_TYPE_GENERALIZED_TIME  24

// Token types for parsers
#define TK_BOOLEAN           1001
#define TK_INTEGER           1002
#define TK_BIT_STRING        1003
#define TK_OCTET_STRING      1004
#define TK_NULL              1005
#define TK_OBJECT_IDENTIFIER 1006
#define TK_SEQUENCE          1016
#define TK_SET               1017
#define TK_CONTEXT_SPECIFIC  1100
#define TK_UNKNOWN           1999

// Binary parser tokens (different namespace)
#define BIN_TK_BOOLEAN           2001
#define BIN_TK_INTEGER           2002
#define BIN_TK_BIT_STRING        2003
#define BIN_TK_OCTET_STRING      2004
#define BIN_TK_NULL              2005
#define BIN_TK_OBJECT_IDENTIFIER 2006
#define BIN_TK_SEQUENCE          2016
#define BIN_TK_SET               2017
#define BIN_TK_CONTEXT_SPECIFIC  2100

// ASN.1 value structure
typedef struct asn1_value asn1_value_t;

typedef struct {
    uint8_t *data;
    size_t length;
} asn1_octet_string_t;

typedef struct {
    uint32_t *components;
    size_t count;
} asn1_oid_t;

typedef struct {
    asn1_value_t **children;
    size_t count;
    size_t capacity;
} asn1_sequence_t;

typedef struct {
    asn1_value_t **children;
    size_t count;
    size_t capacity;
} asn1_set_t;

struct asn1_value {
    int type;
    uint8_t tag_class;    // 0=universal, 1=application, 2=context, 3=private
    uint8_t constructed;  // 0=primitive, 1=constructed
    uint32_t tag_number;
    uint8_t tag;          // Combined tag byte for compatibility
    
    union {
        int boolean;
        int64_t integer;
        asn1_octet_string_t octet_string;
        asn1_oid_t oid;
        asn1_sequence_t sequence;
        asn1_set_t set;
    } value;
};

// Binary parsing state
typedef struct {
    asn1_value_t *root;
    int error;
    char error_msg[256];
} BinaryParseState;

// API functions
SALTSNET_ASN1_C_API asn1_value_t *asn1_create_value(int type, uint8_t tag_class,
                                          uint8_t constructed, uint32_t tag_number);
SALTSNET_ASN1_C_API asn1_value_t *asn1_create_boolean(int value);
SALTSNET_ASN1_C_API asn1_value_t *asn1_create_integer(int64_t value);
SALTSNET_ASN1_C_API asn1_value_t *asn1_create_octet_string(const uint8_t *data, size_t len);
SALTSNET_ASN1_C_API asn1_value_t *asn1_create_oid(const uint32_t *components, size_t count);
SALTSNET_ASN1_C_API asn1_value_t *asn1_create_sequence(void);
SALTSNET_ASN1_C_API asn1_value_t *asn1_create_set(void);
SALTSNET_ASN1_C_API asn1_value_t *asn1_create_null(void);
SALTSNET_ASN1_C_API asn1_value_t *asn1_create_bit_string(const uint8_t *data, size_t len, int unused_bits);
SALTSNET_ASN1_C_API asn1_value_t *asn1_create_oid_from_string(const char *oid_str);
SALTSNET_ASN1_C_API asn1_value_t *asn1_create_printable_string(const char *str);
SALTSNET_ASN1_C_API asn1_value_t *asn1_create_utf8_string(const char *str);
SALTSNET_ASN1_C_API asn1_value_t *asn1_create_ia5_string(const char *str);
SALTSNET_ASN1_C_API asn1_value_t *asn1_create_utc_time(const char *time_str);
SALTSNET_ASN1_C_API asn1_value_t *asn1_create_generalized_time(const char *time_str);

SALTSNET_ASN1_C_API int asn1_sequence_add_child(asn1_value_t *seq, asn1_value_t *child);
SALTSNET_ASN1_C_API int asn1_set_add_child(asn1_value_t *set, asn1_value_t *child);

SALTSNET_ASN1_C_API void asn1_free(asn1_value_t *value);

// Binary parsing
SALTSNET_ASN1_C_API int scan_binary_asn1(const uint8_t *data, size_t len, asn1_value_t **result);

// DER encoding/decoding (compatibility layer)
SALTSNET_ASN1_C_API int asn1_der_encode(const asn1_value_t *value, uint8_t *buffer, size_t *buffer_len);
SALTSNET_ASN1_C_API int asn1_der_decode(const uint8_t *data, size_t len, asn1_value_t **result);

// BER encoding/decoding (full BER support including indefinite length and constructed primitives)
SALTSNET_ASN1_C_API int asn1_ber_encode(const asn1_value_t *value, uint8_t *buffer, size_t *buffer_len, int use_indefinite);
SALTSNET_ASN1_C_API int asn1_ber_decode(const uint8_t *data, size_t len, asn1_value_t **result);

// Utility functions
SALTSNET_ASN1_C_API void asn1_print_value(const asn1_value_t *value, int indent);
SALTSNET_ASN1_C_API int asn1_compare_oid(const asn1_oid_t *oid1, const asn1_oid_t *oid2);
SALTSNET_ASN1_C_API char *asn1_oid_to_string(const asn1_oid_t *oid);

#ifdef __cplusplus
}
#endif

#endif // ASN1_TYPES_H
