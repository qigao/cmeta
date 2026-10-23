#include "asn1_types.h"
#include <fmt.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// Global parse state for binary scanner
BinaryParseState g_binary_parse_state = {0};

asn1_value_t *asn1_create_value(int type, uint8_t tag_class, 
                                uint8_t constructed, uint32_t tag_number) {
    asn1_value_t *value = calloc(1, sizeof(asn1_value_t));
    if (!value) return NULL;
    
    value->type = type;
    value->tag_class = tag_class;
    value->constructed = constructed;
    value->tag_number = tag_number;
    
    // Calculate combined tag byte (for tag numbers < 31)
    if (tag_number < 31) {
        value->tag = (tag_class << 6) | (constructed << 5) | tag_number;
    } else {
        value->tag = (tag_class << 6) | (constructed << 5) | 0x1F; // Long form
    }
    
    return value;
}

asn1_value_t *asn1_create_boolean(int value) {
    asn1_value_t *val = asn1_create_value(ASN1_TYPE_BOOLEAN, 0, 0, 1);
    if (val) {
        val->value.boolean = value ? 1 : 0;
    }
    return val;
}

asn1_value_t *asn1_create_integer(int64_t value) {
    asn1_value_t *val = asn1_create_value(ASN1_TYPE_INTEGER, 0, 0, 2);
    if (val) {
        val->value.integer = value;
    }
    return val;
}

asn1_value_t *asn1_create_octet_string(const uint8_t *data, size_t len) {
    // If data is NULL but length > 0, this is invalid
    if (!data && len > 0) {
        return NULL;
    }
    
    asn1_value_t *val = asn1_create_value(ASN1_TYPE_OCTET_STRING, 0, 0, 4);
    if (val) {
        if (data && len > 0) {
            val->value.octet_string.data = malloc(len);
            if (val->value.octet_string.data) {
                memcpy(val->value.octet_string.data, data, len);
                val->value.octet_string.length = len;
            } else {
                free(val);
                return NULL;
            }
        } else {
            // Empty octet string (data is NULL or len is 0)
            val->value.octet_string.data = NULL;
            val->value.octet_string.length = 0;
        }
    }
    return val;
}

asn1_value_t *asn1_create_oid(const uint32_t *components, size_t count) {
    asn1_value_t *val = asn1_create_value(ASN1_TYPE_OBJECT_IDENTIFIER, 0, 0, 6);
    if (val && components && count > 0) {
        val->value.oid.components = malloc(count * sizeof(uint32_t));
        if (val->value.oid.components) {
            memcpy(val->value.oid.components, components, count * sizeof(uint32_t));
            val->value.oid.count = count;
        } else {
            free(val);
            return NULL;
        }
    }
    return val;
}

asn1_value_t *asn1_create_sequence(void) {
    asn1_value_t *seq = asn1_create_value(ASN1_TYPE_SEQUENCE, 0, 1, 16);
    if (seq) {
        seq->value.sequence.children = NULL;
        seq->value.sequence.count = 0;
        seq->value.sequence.capacity = 0;
    }
    return seq;
}

asn1_value_t *asn1_create_set(void) {
    asn1_value_t *set = asn1_create_value(ASN1_TYPE_SET, 0, 1, 17);
    if (set) {
        set->value.set.children = NULL;
        set->value.set.count = 0;
        set->value.set.capacity = 0;
    }
    return set;
}

int asn1_sequence_add_child(asn1_value_t *seq, asn1_value_t *child) {
    if (!seq || !child) return -1;
    if (seq->type != ASN1_TYPE_SEQUENCE && seq->type != TK_CONTEXT_SPECIFIC) return -1;
    if (seq->type == TK_CONTEXT_SPECIFIC && !seq->constructed) return -1;
    
    if (seq->value.sequence.count >= seq->value.sequence.capacity) {
        size_t new_capacity = seq->value.sequence.capacity ? 
                             seq->value.sequence.capacity * 2 : 8;
        asn1_value_t **new_children = realloc(seq->value.sequence.children,
                                             new_capacity * sizeof(asn1_value_t*));
        if (!new_children) return -1;
        
        seq->value.sequence.children = new_children;
        seq->value.sequence.capacity = new_capacity;
    }
    
    seq->value.sequence.children[seq->value.sequence.count++] = child;
    return 0;
}

int asn1_set_add_child(asn1_value_t *set, asn1_value_t *child) {
    if (!set || set->type != ASN1_TYPE_SET || !child) return -1;
    
    if (set->value.set.count >= set->value.set.capacity) {
        size_t new_capacity = set->value.set.capacity ? 
                             set->value.set.capacity * 2 : 8;
        asn1_value_t **new_children = realloc(set->value.set.children,
                                             new_capacity * sizeof(asn1_value_t*));
        if (!new_children) return -1;
        
        set->value.set.children = new_children;
        set->value.set.capacity = new_capacity;
    }
    
    set->value.set.children[set->value.set.count++] = child;
    return 0;
}

void asn1_free(asn1_value_t *value) {
    if (!value) return;
    
    switch (value->type) {
        case ASN1_TYPE_OCTET_STRING:
        case ASN1_TYPE_BIT_STRING:
        case ASN1_TYPE_UTF8_STRING:
        case ASN1_TYPE_PRINTABLE_STRING:
        case ASN1_TYPE_IA5_STRING:
        case ASN1_TYPE_UTC_TIME:
        case ASN1_TYPE_GENERALIZED_TIME:
            free(value->value.octet_string.data);
            break;
            
        case ASN1_TYPE_OBJECT_IDENTIFIER:
            free(value->value.oid.components);
            break;
            
        case ASN1_TYPE_SEQUENCE:
            for (size_t i = 0; i < value->value.sequence.count; i++) {
                asn1_free(value->value.sequence.children[i]);
            }
            free(value->value.sequence.children);
            break;
            
        case ASN1_TYPE_SET:
            for (size_t i = 0; i < value->value.set.count; i++) {
                asn1_free(value->value.set.children[i]);
            }
            free(value->value.set.children);
            break;

        case TK_CONTEXT_SPECIFIC:
            if (value->constructed) {
                for (size_t i = 0; i < value->value.sequence.count; i++) {
                    asn1_free(value->value.sequence.children[i]);
                }
                free(value->value.sequence.children);
            } else {
                free(value->value.octet_string.data);
            }
            break;
    }
    
    free(value);
}

void asn1_print_value(const asn1_value_t *value, int indent) {
    if (!value) return;
    
    for (int i = 0; i < indent; i++) printf("  ");
    
    switch (value->type) {
        case ASN1_TYPE_BOOLEAN:
            printf("BOOLEAN: %s\n", value->value.boolean ? "TRUE" : "FALSE");
            break;
            
        case ASN1_TYPE_INTEGER:
            printf("INTEGER: %lld\n", (long long)value->value.integer);
            break;
            
        case ASN1_TYPE_OCTET_STRING:
            printf("OCTET STRING (%zu bytes): ", value->value.octet_string.length);
            for (size_t i = 0; i < value->value.octet_string.length && i < 16; i++) {
                printf("%02X ", value->value.octet_string.data[i]);
            }
            if (value->value.octet_string.length > 16) printf("...");
            printf("\n");
            break;
            
        case ASN1_TYPE_OBJECT_IDENTIFIER:
            printf("OBJECT IDENTIFIER: ");
            for (size_t i = 0; i < value->value.oid.count; i++) {
                if (i > 0) printf(".");
                printf("%u", value->value.oid.components[i]);
            }
            printf("\n");
            break;
            
        case ASN1_TYPE_SEQUENCE:
            printf("SEQUENCE (%zu elements):\n", value->value.sequence.count);
            for (size_t i = 0; i < value->value.sequence.count; i++) {
                asn1_print_value(value->value.sequence.children[i], indent + 1);
            }
            break;
            
        case ASN1_TYPE_SET:
            printf("SET (%zu elements):\n", value->value.set.count);
            for (size_t i = 0; i < value->value.set.count; i++) {
                asn1_print_value(value->value.set.children[i], indent + 1);
            }
            break;
            
        case TK_CONTEXT_SPECIFIC:
            printf("CONTEXT-SPECIFIC [%u] (%s):\n", (unsigned int)value->tag_number,
                   value->constructed ? "constructed" : "primitive");
            if (value->constructed) {
                for (size_t i = 0; i < value->value.sequence.count; i++) {
                    asn1_print_value(value->value.sequence.children[i], indent + 1);
                }
            } else {
                for (int i = 0; i < indent + 1; i++) printf("  ");
                printf("DATA: ");
                for (size_t i = 0; i < value->value.octet_string.length && i < 16; i++) {
                    printf("%02X ", value->value.octet_string.data[i]);
                }
                if (value->value.octet_string.length > 16) printf("...");
                printf("\n");
            }
            break;
            
        default:
            printf("UNKNOWN TYPE %d\n", value->type);
            break;
    }
}

int asn1_compare_oid(const asn1_oid_t *oid1, const asn1_oid_t *oid2) {
    if (!oid1 || !oid2) return -1;
    if (oid1->count != oid2->count) return (int)(oid1->count - oid2->count);
    
    for (size_t i = 0; i < oid1->count; i++) {
        if (oid1->components[i] != oid2->components[i]) {
            return (int)(oid1->components[i] - oid2->components[i]);
        }
    }
    return 0;
}

char *asn1_oid_to_string(const asn1_oid_t *oid) {
    if (!oid || !oid->components || oid->count == 0) return NULL;
    
    // Estimate buffer size (each component can be up to 10 digits + dot)
    size_t buffer_size = oid->count * 12;
    char *buffer = malloc(buffer_size);
    if (!buffer) return NULL;
    
    size_t pos = 0;
    for (size_t i = 0; i < oid->count; i++) {
        if (i > 0) {
            buffer[pos++] = '.';
        }
        int written = fmt(buffer + pos, buffer_size - pos, "{}", oid->components[i]);
        if (written < 0 || (size_t)written >= buffer_size - pos) {
            break;
        }
        pos += (size_t)written;
    }
    
    return buffer;
}

// Wrapper for DER decoding using binary scanner
int asn1_der_decode(const uint8_t *data, size_t len, asn1_value_t **result) {
    return scan_binary_asn1(data, len, result);
}

// BER decoding (alias for binary scanner - now supports full BER)
int asn1_ber_decode(const uint8_t *data, size_t len, asn1_value_t **result) {
    return scan_binary_asn1(data, len, result);
}

// Additional ASN.1 type creation functions
asn1_value_t *asn1_create_null(void) {
    return asn1_create_value(ASN1_TYPE_NULL, 0, 0, 5);
}

asn1_value_t *asn1_create_bit_string(const uint8_t *data, size_t len, int unused_bits) {
    asn1_value_t *val = asn1_create_value(ASN1_TYPE_BIT_STRING, 0, 0, 3);
    if (val && data && len > 0) {
        // For bit strings, we need to store the unused bits count and the data
        val->value.octet_string.data = malloc(len + 1);
        if (val->value.octet_string.data) {
            val->value.octet_string.data[0] = unused_bits;
            memcpy(val->value.octet_string.data + 1, data, len);
            val->value.octet_string.length = len + 1;
        }
    }
    return val;
}

asn1_value_t *asn1_create_oid_from_string(const char *oid_str) {
    if (!oid_str || strlen(oid_str) == 0) return NULL;
    
    // Parse OID string like "1.2.840.113549"
    uint32_t components[32]; // Max 32 components
    size_t count = 0;
    
    const char *p = oid_str;
    while (*p && count < 32) {
        char *endptr;
        unsigned long val = strtoul(p, &endptr, 10);
        if (endptr == p) return NULL; // No digits found
        
        components[count++] = (uint32_t)val;
        
        if (*endptr == '.') {
            p = endptr + 1;
        } else if (*endptr == '\0') {
            break;
        } else {
            return NULL; // Invalid character
        }
    }
    
    if (count == 0) return NULL;
    
    return asn1_create_oid(components, count);
}

asn1_value_t *asn1_create_printable_string(const char *str) {
    if (!str) return NULL;
    size_t len = strlen(str);
    
    // PrintableString has tag 19
    asn1_value_t *val = asn1_create_value(19, 0, 0, 19);
    if (val) {
        val->value.octet_string.data = malloc(len);
        if (val->value.octet_string.data) {
            memcpy(val->value.octet_string.data, str, len);
            val->value.octet_string.length = len;
        }
    }
    return val;
}

asn1_value_t *asn1_create_utf8_string(const char *str) {
    if (!str) return NULL;
    size_t len = strlen(str);
    
    // UTF8String has tag 12
    asn1_value_t *val = asn1_create_value(12, 0, 0, 12);
    if (val) {
        val->value.octet_string.data = malloc(len);
        if (val->value.octet_string.data) {
            memcpy(val->value.octet_string.data, str, len);
            val->value.octet_string.length = len;
        }
    }
    return val;
}

asn1_value_t *asn1_create_ia5_string(const char *str) {
    if (!str) return NULL;
    size_t len = strlen(str);
    
    // IA5String has tag 22
    asn1_value_t *val = asn1_create_value(22, 0, 0, 22);
    if (val) {
        val->value.octet_string.data = malloc(len);
        if (val->value.octet_string.data) {
            memcpy(val->value.octet_string.data, str, len);
            val->value.octet_string.length = len;
        }
    }
    return val;
}

asn1_value_t *asn1_create_utc_time(const char *time_str) {
    if (!time_str) return NULL;
    size_t len = strlen(time_str);
    
    // UTCTime has tag 23
    asn1_value_t *val = asn1_create_value(ASN1_TYPE_UTC_TIME, 0, 0, 23);
    if (val) {
        val->value.octet_string.data = malloc(len);
        if (val->value.octet_string.data) {
            memcpy(val->value.octet_string.data, time_str, len);
            val->value.octet_string.length = len;
        }
    }
    return val;
}
asn1_value_t *asn1_create_generalized_time(const char *time_str) {
    if (!time_str) return NULL;
    size_t len = strlen(time_str);
    
    // GeneralizedTime has tag 24
    asn1_value_t *val = asn1_create_value(ASN1_TYPE_GENERALIZED_TIME, 0, 0, 24);
    if (val) {
        val->value.octet_string.data = malloc(len);
        if (val->value.octet_string.data) {
            memcpy(val->value.octet_string.data, time_str, len);
            val->value.octet_string.length = len;
        }
    }
    return val;
}
