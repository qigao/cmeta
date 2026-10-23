/**
 * @file asn1_encoder.c
 * @brief ASN.1 BER/DER encoding implementation
 */

#include "asn1_types.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// Forward declarations
static int encode_tag(uint8_t tag_class, uint8_t constructed, uint32_t tag_number, 
                     uint8_t *buffer, size_t *pos, size_t max_len);
static int encode_length(size_t length, int indefinite, uint8_t *buffer, 
                        size_t *pos, size_t max_len);
static int encode_value(const asn1_value_t *value, uint8_t *buffer, 
                       size_t *pos, size_t max_len, int use_indefinite);
static size_t calculate_encoded_length(const asn1_value_t *value, int use_indefinite);
static int encode_integer(int64_t value, uint8_t *buffer, size_t *pos, size_t max_len);
static int encode_oid(const asn1_oid_t *oid, uint8_t *buffer, size_t *pos, size_t max_len);

// Main DER encoding function (definite length only)
int asn1_der_encode(const asn1_value_t *value, uint8_t *buffer, size_t *buffer_len) {
    if (!value || !buffer_len) return -1;
    
    // Calculate required buffer size
    size_t required_len = calculate_encoded_length(value, 0); // DER = no indefinite length
    if (required_len == 0) return -1;
    
    if (!buffer) {
        // Just return required size
        *buffer_len = required_len;
        return 0;
    }
    
    if (*buffer_len < required_len) {
        *buffer_len = required_len;
        return -1; // Buffer too small
    }
    
    size_t pos = 0;
    int result = encode_value(value, buffer, &pos, *buffer_len, 0); // DER = no indefinite
    if (result == 0) {
        *buffer_len = pos;
    }
    
    return result;
}

// BER encoding function (supports indefinite length)
int asn1_ber_encode(const asn1_value_t *value, uint8_t *buffer, size_t *buffer_len, 
                    int use_indefinite) {
    if (!value || !buffer_len) return -1;
    
    // Calculate required buffer size (estimate for indefinite length)
    size_t required_len = calculate_encoded_length(value, use_indefinite);
    if (required_len == 0) return -1;
    
    if (!buffer) {
        *buffer_len = required_len;
        return 0;
    }
    
    if (*buffer_len < required_len) {
        *buffer_len = required_len;
        return -1;
    }
    
    size_t pos = 0;
    int result = encode_value(value, buffer, &pos, *buffer_len, use_indefinite);
    if (result == 0) {
        *buffer_len = pos;
    }
    
    return result;
}

static int encode_value(const asn1_value_t *value, uint8_t *buffer, 
                       size_t *pos, size_t max_len, int use_indefinite) {
    if (!value || !buffer || !pos) return -1;
    
    // Encode tag
    uint8_t constructed = value->constructed;
    
    if (encode_tag(value->tag_class, constructed, value->tag_number, 
                   buffer, pos, max_len) < 0) {
        return -1;
    }
    
    // Calculate content length
    size_t content_len = 0;
    size_t content_start = *pos;
    
    // For constructed types with indefinite length, we need to encode differently
    int use_indef_for_this = use_indefinite && constructed;
    
    if (use_indef_for_this) {
        // Encode indefinite length marker
        if (encode_length(0, 1, buffer, pos, max_len) < 0) return -1;
    } else {
        // Reserve space for length (we'll fill it in later)
        size_t length_pos = *pos;
        *pos += 4; // Reserve maximum space for length encoding
        if (*pos > max_len) return -1;
        
        content_start = *pos;
    }
    
    // Encode content based on type
    switch (value->type) {
        case ASN1_TYPE_BOOLEAN:
            if (*pos >= max_len) return -1;
            buffer[(*pos)++] = value->value.boolean ? 0xFF : 0x00;
            content_len = 1;
            break;
            
        case ASN1_TYPE_INTEGER:
            if (encode_integer(value->value.integer, buffer, pos, max_len) < 0) return -1;
            content_len = *pos - content_start;
            break;
            
        case ASN1_TYPE_OCTET_STRING:
        case ASN1_TYPE_BIT_STRING:
        case ASN1_TYPE_UTF8_STRING:
        case ASN1_TYPE_PRINTABLE_STRING:
        case ASN1_TYPE_IA5_STRING:
        case ASN1_TYPE_UTC_TIME:
        case ASN1_TYPE_GENERALIZED_TIME:
            if (*pos + value->value.octet_string.length > max_len) return -1;
            memcpy(buffer + *pos, value->value.octet_string.data, 
                   value->value.octet_string.length);
            *pos += value->value.octet_string.length;
            content_len = value->value.octet_string.length;
            break;
            
        case ASN1_TYPE_NULL:
            content_len = 0; // NULL has no content
            break;
            
        case ASN1_TYPE_OBJECT_IDENTIFIER:
            if (encode_oid(&value->value.oid, buffer, pos, max_len) < 0) return -1;
            content_len = *pos - content_start;
            break;
            
        case ASN1_TYPE_SEQUENCE:
        case ASN1_TYPE_SET:
        case TK_CONTEXT_SPECIFIC:
            if (constructed) {
                // Encode all children
                size_t child_count = (value->type == ASN1_TYPE_SET) ? 
                                     value->value.set.count : value->value.sequence.count;
                asn1_value_t **children = (value->type == ASN1_TYPE_SET) ? 
                                         value->value.set.children : value->value.sequence.children;
                                         
                for (size_t i = 0; i < child_count; i++) {
                    if (encode_value(children[i], buffer, 
                                   pos, max_len, use_indefinite) < 0) {
                        return -1;
                    }
                }
                content_len = *pos - content_start;
            } else if (value->type == TK_CONTEXT_SPECIFIC || value->type == BIN_TK_CONTEXT_SPECIFIC) {
                // Primitive context tag - encode as octet string
                if (*pos + value->value.octet_string.length > max_len) return -1;
                if (value->value.octet_string.length > 0 && value->value.octet_string.data) {
                    memcpy(buffer + *pos, value->value.octet_string.data, 
                           value->value.octet_string.length);
                    *pos += value->value.octet_string.length;
                }
                content_len = value->value.octet_string.length;
            } else {
                return -1;
            }
            break;
            
        default:
            return -1; // Unsupported type
    }
    
    if (use_indef_for_this) {
        // Add end-of-contents octets
        if (*pos + 2 > max_len) return -1;
        buffer[(*pos)++] = 0x00;
        buffer[(*pos)++] = 0x00;
    } else {
        // Go back and fill in the actual length
        size_t length_pos = content_start - 4; // Where we reserved space
        size_t saved_pos = *pos;
        *pos = length_pos;
        
        if (encode_length(content_len, 0, buffer, pos, max_len) < 0) return -1;
        
        // If we used less space for length than reserved, shift content
        size_t actual_length_size = *pos - length_pos;
        if (actual_length_size < 4) {
            size_t shift = 4 - actual_length_size;
            memmove(buffer + content_start - shift, buffer + content_start, content_len);
            *pos = saved_pos - shift;
        } else {
            *pos = saved_pos;
        }
    }
    
    return 0;
}

static int encode_tag(uint8_t tag_class, uint8_t constructed, uint32_t tag_number, 
                     uint8_t *buffer, size_t *pos, size_t max_len) {
    if (*pos >= max_len) return -1;
    
    if (tag_number < 31) {
        // Short form
        buffer[(*pos)++] = (tag_class << 6) | (constructed << 5) | tag_number;
    } else {
        // Long form
        buffer[(*pos)++] = (tag_class << 6) | (constructed << 5) | 0x1F;
        
        // Encode tag number in base 128
        uint8_t octets[5];
        int num_octets = 0;
        
        uint32_t temp = tag_number;
        do {
            octets[num_octets++] = temp & 0x7F;
            temp >>= 7;
        } while (temp > 0 && num_octets < 5);
        
        // Write octets in reverse order with continuation bits
        for (int i = num_octets - 1; i >= 0; i--) {
            if (*pos >= max_len) return -1;
            uint8_t octet = octets[i];
            if (i > 0) octet |= 0x80; // Continuation bit
            buffer[(*pos)++] = octet;
        }
    }
    
    return 0;
}

static int encode_length(size_t length, int indefinite, uint8_t *buffer, 
                        size_t *pos, size_t max_len) {
    if (indefinite) {
        // Indefinite length
        if (*pos >= max_len) return -1;
        buffer[(*pos)++] = 0x80;
        return 0;
    }
    
    if (length < 128) {
        // Short form
        if (*pos >= max_len) return -1;
        buffer[(*pos)++] = (uint8_t)length;
    } else {
        // Long form
        uint8_t octets[4];
        int num_octets = 0;
        
        size_t temp = length;
        while (temp > 0 && num_octets < 4) {
            octets[num_octets++] = temp & 0xFF;
            temp >>= 8;
        }
        
        if (*pos + 1 + num_octets > max_len) return -1;
        
        buffer[(*pos)++] = 0x80 | num_octets;
        
        // Write octets in reverse order
        for (int i = num_octets - 1; i >= 0; i--) {
            buffer[(*pos)++] = octets[i];
        }
    }
    
    return 0;
}

static int encode_integer(int64_t value, uint8_t *buffer, size_t *pos, size_t max_len) {
    // Determine minimum number of octets needed
    uint8_t octets[8];
    int num_octets = 0;
    
    if (value == 0) {
        octets[0] = 0;
        num_octets = 1;
    } else {
        uint64_t temp = (value < 0) ? -value : value;
        
        // Extract octets
        while (temp > 0 && num_octets < 8) {
            octets[num_octets++] = temp & 0xFF;
            temp >>= 8;
        }
        
        // Handle negative numbers (two's complement)
        if (value < 0) {
            // Invert all bits and add 1
            int carry = 1;
            for (int i = 0; i < num_octets; i++) {
                octets[i] = ~octets[i];
                if (carry) {
                    octets[i]++;
                    if (octets[i] != 0) carry = 0;
                }
            }
            
            // Ensure sign bit is set
            if (num_octets < 8 && !(octets[num_octets - 1] & 0x80)) {
                octets[num_octets++] = 0xFF;
            }
        } else {
            // Ensure sign bit is not set for positive numbers
            if (octets[num_octets - 1] & 0x80) {
                if (num_octets < 8) {
                    octets[num_octets++] = 0x00;
                }
            }
        }
    }
    
    if (*pos + num_octets > max_len) return -1;
    
    // Write octets in reverse order (big-endian)
    for (int i = num_octets - 1; i >= 0; i--) {
        buffer[(*pos)++] = octets[i];
    }
    
    return 0;
}

static int encode_oid(const asn1_oid_t *oid, uint8_t *buffer, size_t *pos, size_t max_len) {
    if (!oid || oid->count < 2) return -1;
    
    // First octet encodes first two components
    uint32_t first_octet = oid->components[0] * 40 + oid->components[1];
    if (*pos >= max_len) return -1;
    buffer[(*pos)++] = (uint8_t)first_octet;
    
    // Encode remaining components
    for (size_t i = 2; i < oid->count; i++) {
        uint32_t component = oid->components[i];
        
        if (component < 128) {
            if (*pos >= max_len) return -1;
            buffer[(*pos)++] = (uint8_t)component;
        } else {
            // Multi-octet encoding
            uint8_t octets[5];
            int num_octets = 0;
            
            uint32_t temp = component;
            do {
                octets[num_octets++] = temp & 0x7F;
                temp >>= 7;
            } while (temp > 0 && num_octets < 5);
            
            if (*pos + num_octets > max_len) return -1;
            
            // Write octets in reverse order with continuation bits
            for (int j = num_octets - 1; j >= 0; j--) {
                uint8_t octet = octets[j];
                if (j > 0) octet |= 0x80;
                buffer[(*pos)++] = octet;
            }
        }
    }
    
    return 0;
}

static size_t calculate_encoded_length(const asn1_value_t *value, int use_indefinite) {
    if (!value) return 0;
    
    size_t total = 0;
    
    // Tag length
    if (value->tag_number < 31) {
        total += 1;
    } else {
        total += 2; // Simplified estimate for long form tags
    }
    
    // Content length
    size_t content_len = 0;
    
    switch (value->type) {
        case ASN1_TYPE_BOOLEAN:
            content_len = 1;
            break;
            
        case ASN1_TYPE_INTEGER:
            // Estimate: up to 8 bytes for int64_t
            content_len = 8;
            break;
            
        case ASN1_TYPE_OCTET_STRING:
        case ASN1_TYPE_BIT_STRING:
        case ASN1_TYPE_UTF8_STRING:
        case ASN1_TYPE_PRINTABLE_STRING:
        case ASN1_TYPE_IA5_STRING:
        case ASN1_TYPE_UTC_TIME:
        case ASN1_TYPE_GENERALIZED_TIME:
            content_len = value->value.octet_string.length;
            break;
            
        case ASN1_TYPE_NULL:
            content_len = 0;
            break;
            
        case ASN1_TYPE_OBJECT_IDENTIFIER:
            // Estimate: 1 byte per component + overhead
            content_len = value->value.oid.count * 2;
            break;
            
        case ASN1_TYPE_SEQUENCE:
        case ASN1_TYPE_SET:
        case TK_CONTEXT_SPECIFIC:
            if (value->constructed) {
                size_t child_count = (value->type == ASN1_TYPE_SET) ? 
                                     value->value.set.count : value->value.sequence.count;
                asn1_value_t **children = (value->type == ASN1_TYPE_SET) ? 
                                         value->value.set.children : value->value.sequence.children;
                                         
                for (size_t i = 0; i < child_count; i++) {
                    content_len += calculate_encoded_length(children[i], use_indefinite);
                }
            } else if (value->type == TK_CONTEXT_SPECIFIC) {
                content_len = value->value.octet_string.length;
            }
            break;
            
        default:
            return 0;
    }
    
    // Length encoding
    if (use_indefinite && (value->type == ASN1_TYPE_SEQUENCE || value->type == ASN1_TYPE_SET)) {
        total += 1; // Indefinite length marker
        total += content_len;
        total += 2; // End-of-contents
    } else {
        if (content_len < 128) {
            total += 1; // Short form length
        } else {
            total += 5; // Long form length (estimate)
        }
        total += content_len;
    }
    
    return total;
}