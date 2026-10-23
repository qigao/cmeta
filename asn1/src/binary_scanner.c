// Binary ASN.1 BER/DER scanner - handles parsing directly without complex grammar
// Supports full BER features: indefinite length, constructed primitives, long form tags
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include "platform.h"
#include "asn1_types.h"

// ASN.1 tag parsing
typedef struct {
    uint8_t tag_class;    // 0=universal, 1=application, 2=context, 3=private
    uint8_t constructed;  // 0=primitive, 1=constructed
    uint32_t tag_number;
    size_t tag_length;    // Length of tag encoding
} asn1_tag_t;

typedef struct {
    size_t length;
    size_t length_octets; // Number of octets used for length encoding
    int indefinite;       // 1 if indefinite length
} asn1_length_t;

// Forward declarations
static int parse_tag(const uint8_t *data, size_t len, asn1_tag_t *tag);
static int parse_length(const uint8_t *data, size_t len, asn1_length_t *length);
static asn1_value_t *parse_value(const uint8_t *data, size_t len, size_t *consumed);
static asn1_value_t *parse_constructed(const uint8_t *data, size_t len, asn1_tag_t *tag);
static asn1_value_t *parse_primitive(const uint8_t *data, size_t len, asn1_tag_t *tag);
static size_t find_end_of_contents(const uint8_t *data, size_t len);
static asn1_value_t *parse_constructed_primitive(const uint8_t *data, size_t len, asn1_tag_t *tag);

// Main parsing function - no complex grammar needed
int scan_binary_asn1(const uint8_t *data, size_t len, asn1_value_t **result) {
    if (!data || len == 0 || !result) return -1;
    
    size_t consumed = 0;
    *result = parse_value(data, len, &consumed);
    
    return *result ? 0 : -1;
}

static asn1_value_t *parse_value(const uint8_t *data, size_t len, size_t *consumed) {
    if (len < 2) return NULL; // Need at least tag + length
    
    asn1_tag_t tag;
    asn1_length_t length;
    
    // Parse tag
    if (parse_tag(data, len, &tag) < 0) return NULL;
    const uint8_t *cursor = data + tag.tag_length;
    size_t remaining = len - tag.tag_length;
    
    // Parse length
    if (parse_length(cursor, remaining, &length) < 0) return NULL;
    cursor += length.length_octets;
    remaining -= length.length_octets;
    
    size_t value_len;
    
    // Handle indefinite length (BER feature)
    if (length.indefinite) {
        value_len = find_end_of_contents(cursor, remaining);
        if (value_len == 0) return NULL; // No end-of-contents found
        *consumed = tag.tag_length + length.length_octets + value_len + 2; // +2 for end-of-contents
    } else {
        // Check if we have enough data for definite length
        if (length.length > remaining) return NULL;
        value_len = length.length;
        *consumed = tag.tag_length + length.length_octets + value_len;
    }
    
    // Parse based on constructed flag
    if (tag.constructed) {
        return parse_constructed(cursor, value_len, &tag);
    } else {
        return parse_primitive(cursor, value_len, &tag);
    }
}

static asn1_value_t *parse_constructed(const uint8_t *data, size_t len, asn1_tag_t *tag) {
    asn1_value_t *value = NULL;
    
    // Create appropriate constructed type
    if (tag->tag_class == 0) { // Universal
        switch (tag->tag_number) {
            case 3: // BIT STRING (constructed)
            case 4: // OCTET STRING (constructed)
                // These are constructed primitive types in BER
                return parse_constructed_primitive(data, len, tag);
                
            case 16: // SEQUENCE
                value = asn1_create_sequence();
                break;
            case 17: // SET
                value = asn1_create_set();
                break;
            default:
                return NULL;
        }
    } else {
        // Context-specific or application tag - treat as SEQUENCE for structural storage
        // but identify as TK_CONTEXT_SPECIFIC for type
        value = asn1_create_value(TK_CONTEXT_SPECIFIC, tag->tag_class, 
                                 tag->constructed, tag->tag_number);
    }
    
    if (!value) return NULL;
    
    // Parse children
    const uint8_t *cursor = data;
    size_t remaining = len;
    
    while (remaining > 0) {
        // Check for end-of-contents octets (0x00 0x00) in indefinite length
        if (remaining >= 2 && cursor[0] == 0x00 && cursor[1] == 0x00) {
            break; // End of indefinite length content
        }
        
        size_t child_consumed = 0;
        asn1_value_t *child = parse_value(cursor, remaining, &child_consumed);
        if (!child) break;
        
        // Add child to parent
        if (value->type == ASN1_TYPE_SEQUENCE || value->type == TK_CONTEXT_SPECIFIC) {
            asn1_sequence_add_child(value, child);
        } else if (value->type == ASN1_TYPE_SET) {
            asn1_set_add_child(value, child);
        }
        
        cursor += child_consumed;
        remaining -= child_consumed;
    }
    
    return value;
}

static asn1_value_t *parse_primitive(const uint8_t *data, size_t len, asn1_tag_t *tag) {
    asn1_value_t *value = NULL;
    
    if (tag->tag_class == 0) { // Universal
        switch (tag->tag_number) {
            case 1: // BOOLEAN
                value = asn1_create_value(ASN1_TYPE_BOOLEAN, 0, 0, 1);
                if (value && len >= 1) {
                    value->value.boolean = data[0] ? 1 : 0;
                }
                break;
                
            case 2: // INTEGER
                value = asn1_create_value(ASN1_TYPE_INTEGER, 0, 0, 2);
                if (value) {
                    value->value.integer = 0;
                    for (size_t i = 0; i < len && i < sizeof(int64_t); i++) {
                        value->value.integer = (value->value.integer << 8) | data[i];
                    }
                    // Handle negative numbers (two's complement)
                    if (len > 0 && (data[0] & 0x80)) {
                        // Sign extend for negative numbers
                        for (size_t i = len; i < sizeof(int64_t); i++) {
                            value->value.integer |= (0xFFLL << (i * 8));
                        }
                    }
                }
                break;

            case 10: // ENUMERATED (same encoding as INTEGER)
                value = asn1_create_value(ASN1_TYPE_INTEGER, 0, 0, 10);
                if (value) {
                    value->value.integer = 0;
                    for (size_t i = 0; i < len && i < sizeof(int64_t); i++) {
                        value->value.integer = (value->value.integer << 8) | data[i];
                    }
                    // Handle negative numbers (two's complement)
                    if (len > 0 && (data[0] & 0x80)) {
                        for (size_t i = len; i < sizeof(int64_t); i++) {
                            value->value.integer |= (0xFFLL << (i * 8));
                        }
                    }
                }
                break;

            case 3: // BIT STRING
                value = asn1_create_value(ASN1_TYPE_BIT_STRING, 0, 0, 3);
                if (value && len > 0) {
                    value->value.octet_string.data = malloc(len);
                    if (value->value.octet_string.data) {
                        memcpy(value->value.octet_string.data, data, len);
                        value->value.octet_string.length = len;
                    } else {
                        // Memory allocation failed
                        asn1_free(value);
                        return NULL;
                    }
                } else if (value) {
                    // Empty BIT STRING
                    value->value.octet_string.data = NULL;
                    value->value.octet_string.length = 0;
                }
                break;
                
            case 4: // OCTET STRING
                value = asn1_create_value(ASN1_TYPE_OCTET_STRING, 0, 0, 4);
                if (value && len > 0) {
                    value->value.octet_string.data = malloc(len);
                    if (value->value.octet_string.data) {
                        memcpy(value->value.octet_string.data, data, len);
                        value->value.octet_string.length = len;
                    } else {
                        // Memory allocation failed
                        asn1_free(value);
                        return NULL;
                    }
                } else if (value) {
                    // Empty OCTET STRING
                    value->value.octet_string.data = NULL;
                    value->value.octet_string.length = 0;
                }
                break;
                
            case 5: // NULL
                value = asn1_create_value(ASN1_TYPE_NULL, 0, 0, 5);
                break;
                
            case 6: // OBJECT IDENTIFIER
                value = asn1_create_value(ASN1_TYPE_OBJECT_IDENTIFIER, 0, 0, 6);
                if (value && len > 0) {
                    // Parse OID from DER encoding
                    // This is simplified - real OID parsing is complex
                    value->value.oid.components = malloc(64 * sizeof(uint32_t));
                    value->value.oid.count = 0;
                    
                    if (value->value.oid.components && len > 0) {
                        // First byte encodes first two components: 40*first + second
                        uint32_t first_two = data[0];
                        value->value.oid.components[0] = first_two / 40;
                        value->value.oid.components[1] = first_two % 40;
                        value->value.oid.count = 2;
                        
                        // Parse remaining components
                        size_t i = 1;
                        while (i < len && value->value.oid.count < 64) {
                            uint32_t component = 0;
                            while (i < len) {
                                uint8_t byte = data[i++];
                                component = (component << 7) | (byte & 0x7F);
                                if (!(byte & 0x80)) break; // Last byte of component
                            }
                            value->value.oid.components[value->value.oid.count++] = component;
                        }
                    }
                }
                break;
                
            case 23: // UTC TIME
                value = asn1_create_value(ASN1_TYPE_UTC_TIME, 0, 0, 23);
                if (value) {
                    value->value.octet_string.data = malloc(len);
                    if (value->value.octet_string.data) {
                        memcpy(value->value.octet_string.data, data, len);
                        value->value.octet_string.length = len;
                    }
                }
                break;
                
            case 24: // GENERALIZED TIME
                value = asn1_create_value(ASN1_TYPE_GENERALIZED_TIME, 0, 0, 24);
                if (value) {
                    value->value.octet_string.data = malloc(len);
                    if (value->value.octet_string.data) {
                        memcpy(value->value.octet_string.data, data, len);
                        value->value.octet_string.length = len;
                    }
                }
                break;
                
            default:
                // Unknown universal type
                value = asn1_create_value(TK_UNKNOWN, 0, 0, tag->tag_number);
                break;
        }
    } else {
        // Context-specific, application, or private tag
        value = asn1_create_value(TK_CONTEXT_SPECIFIC, tag->tag_class, 
                                 tag->constructed, tag->tag_number);
        if (value) {
            value->value.octet_string.data = malloc(len);
            if (value->value.octet_string.data) {
                memcpy(value->value.octet_string.data, data, len);
                value->value.octet_string.length = len;
            }
        }
    }
    
    return value;
}

static int parse_tag(const uint8_t *data, size_t len, asn1_tag_t *tag) {
    if (len < 1) return -1;
    
    uint8_t first_octet = data[0];
    tag->tag_class = (first_octet >> 6) & 0x03;
    tag->constructed = (first_octet >> 5) & 0x01;
    tag->tag_number = first_octet & 0x1F;
    tag->tag_length = 1;
    
    // Long form tag
    if (tag->tag_number == 0x1F) {
        tag->tag_number = 0;
        size_t i = 1;
        
        while (i < len) {
            uint8_t octet = data[i];
            tag->tag_number = (tag->tag_number << 7) | (octet & 0x7F);
            i++;
            
            if (!(octet & 0x80)) { // Last octet
                break;
            }
            
            if (i >= len) return -1; // Truncated
        }
        tag->tag_length = i;
    }
    
    return 0;
}

static int parse_length(const uint8_t *data, size_t len, asn1_length_t *length) {
    if (len < 1) return -1;
    
    uint8_t first_octet = data[0];
    
    if (first_octet & 0x80) {
        // Long form or indefinite
        uint8_t num_octets = first_octet & 0x7F;
        
        if (num_octets == 0) {
            // Indefinite length
            length->indefinite = 1;
            length->length = 0;
            length->length_octets = 1;
            return 0;
        }
        
        if (num_octets > 4 || len < 1 + num_octets) {
            return -1; // Too long or truncated
        }
        
        length->length = 0;
        for (int i = 0; i < num_octets; i++) {
            length->length = (length->length << 8) | data[1 + i];
        }
        length->length_octets = 1 + num_octets;
        length->indefinite = 0;
    } else {
        // Short form
        length->length = first_octet;
        length->length_octets = 1;
        length->indefinite = 0;
    }
    
    return 0;
}

// Find end-of-contents octets (0x00 0x00) for indefinite length
static size_t find_end_of_contents(const uint8_t *data, size_t len) {
    size_t depth = 1; // We're already inside one indefinite length
    size_t pos = 0;
    
    while (pos < len - 1) {
        if (data[pos] == 0x00 && data[pos + 1] == 0x00) {
            depth--;
            if (depth == 0) {
                return pos; // Found our end-of-contents
            }
            pos += 2;
            continue;
        }
        
        // Parse tag and length to skip over nested structures
        asn1_tag_t tag;
        asn1_length_t length;
        
        if (parse_tag(data + pos, len - pos, &tag) < 0) break;
        pos += tag.tag_length;
        
        if (pos >= len) break;
        
        if (parse_length(data + pos, len - pos, &length) < 0) break;
        pos += length.length_octets;
        
        if (length.indefinite) {
            depth++; // Nested indefinite length
        } else {
            pos += length.length; // Skip definite length content
        }
    }
    
    return 0; // End-of-contents not found
}

// Parse constructed primitive types (BER allows BIT STRING and OCTET STRING to be constructed)
static asn1_value_t *parse_constructed_primitive(const uint8_t *data, size_t len, asn1_tag_t *tag) {
    asn1_value_t *value = NULL;
    
    // Create the appropriate primitive type
    switch (tag->tag_number) {
        case 3: // BIT STRING
            value = asn1_create_value(ASN1_TYPE_BIT_STRING, 0, 0, 3);
            break;
        case 4: // OCTET STRING
            value = asn1_create_value(ASN1_TYPE_OCTET_STRING, 0, 0, 4);
            break;
        default:
            return NULL;
    }
    
    if (!value) return NULL;
    
    // Parse constructed content and concatenate
    const uint8_t *cursor = data;
    size_t remaining = len;
    size_t total_content_len = 0;
    uint8_t *content_buffer = NULL;
    size_t buffer_capacity = 0;
    int unused_bits = 0; // For BIT STRING
    int first_chunk = 1;
    
    while (remaining > 0) {
        // Check for end-of-contents octets
        if (remaining >= 2 && cursor[0] == 0x00 && cursor[1] == 0x00) {
            break;
        }
        
        size_t chunk_consumed = 0;
        asn1_value_t *chunk = parse_value(cursor, remaining, &chunk_consumed);
        if (!chunk) break;
        
        // Verify chunk type matches expected
        if (chunk->type != value->type) {
            asn1_free(chunk);
            break;
        }
        
        // Handle BIT STRING unused bits (only from first chunk)
        if (tag->tag_number == 3 && first_chunk && chunk->value.octet_string.length > 0) {
            unused_bits = chunk->value.octet_string.data[0];
        }
        
        // Determine content to copy
        const uint8_t *chunk_data = chunk->value.octet_string.data;
        size_t chunk_len = chunk->value.octet_string.length;
        
        // For BIT STRING, skip unused bits byte in ALL chunks
        if (tag->tag_number == 3 && chunk_len > 0) {
            chunk_data++; // Skip unused bits byte
            chunk_len--;
        }
        
        // Mark that we've processed the first chunk
        if (first_chunk) {
            first_chunk = 0;
        }
        
        // Expand buffer if needed
        if (total_content_len + chunk_len > buffer_capacity) {
            buffer_capacity = (buffer_capacity == 0) ? 256 : buffer_capacity * 2;
            while (buffer_capacity < total_content_len + chunk_len) {
                buffer_capacity *= 2;
            }
            
            uint8_t *new_buffer = realloc(content_buffer, buffer_capacity);
            if (!new_buffer) {
                free(content_buffer);
                asn1_free(chunk);
                asn1_free(value);
                return NULL;
            }
            content_buffer = new_buffer;
        }
        
        // Copy chunk content
        if (chunk_len > 0) {
            memcpy(content_buffer + total_content_len, chunk_data, chunk_len);
            total_content_len += chunk_len;
        }
        
        asn1_free(chunk);
        cursor += chunk_consumed;
        remaining -= chunk_consumed;
    }
    
    // Set final content
    if (tag->tag_number == 3) {
        // BIT STRING: prepend unused bits count
        value->value.octet_string.data = malloc(total_content_len + 1);
        if (value->value.octet_string.data) {
            value->value.octet_string.data[0] = unused_bits;
            if (total_content_len > 0) {
                memcpy(value->value.octet_string.data + 1, content_buffer, total_content_len);
            }
            value->value.octet_string.length = total_content_len + 1;
        }
    } else {
        // OCTET STRING: direct copy
        if (total_content_len > 0 && content_buffer) {
            value->value.octet_string.data = content_buffer;
            value->value.octet_string.length = total_content_len;
            content_buffer = NULL; // Transfer ownership
        } else {
            // No content or buffer allocation failed
            value->value.octet_string.data = NULL;
            value->value.octet_string.length = 0;
        }
    }
    
    free(content_buffer); // Safe to call with NULL
    return value;
}