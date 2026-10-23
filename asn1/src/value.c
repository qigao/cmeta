#include <salts/asn1_value.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct salts_asn1_value {
    uint8_t tag_class;
    uint8_t constructed;
    uint32_t tag_number;
    uint8_t *content;
    size_t content_size;
    struct salts_asn1_value **children;
    size_t child_count;
    size_t child_capacity;
    struct salts_asn1_value *parent;
};

static size_t tag_octet_count(uint32_t tag_number) {
    size_t count = 1u;
    while (tag_number >= 128u) {
        tag_number >>= 7;
        ++count;
    }
    return count;
}

static size_t encoded_tag_size(uint32_t tag_number) {
    return tag_number < 31u ? 1u : 1u + tag_octet_count(tag_number);
}

static size_t encoded_length_size(size_t length, uint8_t indefinite) {
    size_t count = 0u;
    if (indefinite || length < 128u) return 1u;
    while (length != 0u) {
        length >>= 8;
        ++count;
    }
    return 1u + count;
}

static void write_tag(uint8_t *out, uint8_t tag_class, uint8_t constructed,
                      uint32_t tag_number) {
    size_t count, i;
    uint32_t value;
    if (tag_number < 31u) {
        out[0] = (uint8_t)((tag_class << 6) | (constructed << 5) |
                           (uint8_t)tag_number);
        return;
    }
    out[0] = (uint8_t)((tag_class << 6) | (constructed << 5) | 0x1fu);
    count = 0u;
    value = tag_number;
    do {
        out[1u + count] = (uint8_t)(value & 0x7fu);
        value >>= 7;
        ++count;
    } while (value != 0u);
    for (i = 0u; i < count / 2u; ++i) {
        uint8_t tmp = out[1u + i];
        out[1u + i] = out[count - i];
        out[count - i] = tmp;
    }
    for (i = 1u; i < count; ++i) out[i] |= 0x80u;
}

static size_t write_length(uint8_t *out, size_t length, uint8_t indefinite) {
    uint8_t reversed[sizeof(size_t)];
    size_t count = 0u, i;
    if (indefinite) {
        out[0] = 0x80u;
        return 1u;
    }
    if (length < 128u) {
        out[0] = (uint8_t)length;
        return 1u;
    }
    while (length != 0u) {
        reversed[count++] = (uint8_t)(length & 0xffu);
        length >>= 8;
    }
    out[0] = (uint8_t)(0x80u | (uint8_t)count);
    for (i = 0u; i < count; ++i) out[1u + i] = reversed[count - 1u - i];
    return 1u + count;
}

static int value_contains(const salts_asn1_value *root,
                          const salts_asn1_value *needle, size_t depth) {
    size_t i;
    if (root == needle) return 1;
    if (!root || depth >= SALTS_ASN1_MAX_DEPTH) return 0;
    for (i = 0u; i < root->child_count; ++i)
        if (value_contains(root->children[i], needle, depth + 1u)) return 1;
    return 0;
}

static salts_asn1_status value_height(const salts_asn1_value *value,
                                      size_t depth, size_t *height) {
    size_t i, max_child = 0u;
    if (!value || !height) return SALTS_ASN1_INVALID_ARGUMENT;
    if (depth >= SALTS_ASN1_MAX_DEPTH) return SALTS_ASN1_LIMIT_EXCEEDED;
    for (i = 0u; i < value->child_count; ++i) {
        size_t child_height = 0u;
        salts_asn1_status status =
            value_height(value->children[i], depth + 1u, &child_height);
        if (status != SALTS_ASN1_OK) return status;
        if (child_height > max_child) max_child = child_height;
    }
    *height = value->child_count != 0u ? max_child + 1u : 0u;
    return SALTS_ASN1_OK;
}

static salts_asn1_status value_depth_from_root(
    const salts_asn1_value *value, size_t *depth) {
    const salts_asn1_value *cursor;
    size_t count = 0u;
    if (!value || !depth) return SALTS_ASN1_INVALID_ARGUMENT;
    cursor = value;
    while (cursor->parent) {
        if (++count >= SALTS_ASN1_MAX_DEPTH)
            return SALTS_ASN1_LIMIT_EXCEEDED;
        cursor = cursor->parent;
    }
    *depth = count;
    return SALTS_ASN1_OK;
}

salts_asn1_status salts_asn1_value_create(
    uint8_t tag_class, uint8_t constructed, uint32_t tag_number,
    const void *content, size_t content_size, salts_asn1_value **out) {
    salts_asn1_value *value;
    if (!out || tag_class > 3u || constructed > 1u ||
        (!content && content_size != 0u) ||
        (constructed && content_size != 0u) ||
        (tag_class == 0u && tag_number == 0u))
        return SALTS_ASN1_INVALID_ARGUMENT;
    *out = NULL;
    value = (salts_asn1_value *)calloc(1u, sizeof(*value));
    if (!value) return SALTS_ASN1_OUT_OF_MEMORY;
    if (!constructed && content_size != 0u) {
        value->content = (uint8_t *)malloc(content_size);
        if (!value->content) {
            free(value);
            return SALTS_ASN1_OUT_OF_MEMORY;
        }
        memcpy(value->content, content, content_size);
    }
    value->tag_class = tag_class;
    value->constructed = constructed;
    value->tag_number = tag_number;
    value->content_size = content_size;
    *out = value;
    return SALTS_ASN1_OK;
}

salts_asn1_status salts_asn1_value_create_sequence(salts_asn1_value **out) {
    return salts_asn1_value_create(0u, 1u, 16u, NULL, 0u, out);
}

salts_asn1_status salts_asn1_value_create_set(salts_asn1_value **out) {
    return salts_asn1_value_create(0u, 1u, 17u, NULL, 0u, out);
}

salts_asn1_status salts_asn1_value_create_boolean(int value,
                                                   salts_asn1_value **out) {
    uint8_t content = value ? 0xffu : 0u;
    return salts_asn1_value_create(0u, 0u, 1u, &content, 1u, out);
}

salts_asn1_status salts_asn1_value_create_integer(int64_t value,
                                                   salts_asn1_value **out) {
    uint8_t encoded[sizeof(value)];
    uint64_t bits = (uint64_t)value;
    size_t i, start = 0u;
    for (i = 0u; i < sizeof(encoded); ++i)
        encoded[sizeof(encoded) - 1u - i] = (uint8_t)(bits >> (i * 8u));
    while (start + 1u < sizeof(encoded)) {
        if (encoded[start] == 0u && (encoded[start + 1u] & 0x80u) == 0u)
            ++start;
        else if (encoded[start] == 0xffu &&
                 (encoded[start + 1u] & 0x80u) != 0u)
            ++start;
        else
            break;
    }
    return salts_asn1_value_create(0u, 0u, 2u, encoded + start,
                                   sizeof(encoded) - start, out);
}

salts_asn1_status salts_asn1_value_create_octet_string(
    const void *data, size_t size, salts_asn1_value **out) {
    return salts_asn1_value_create(0u, 0u, 4u, data, size, out);
}

salts_asn1_status salts_asn1_value_create_null(salts_asn1_value **out) {
    return salts_asn1_value_create(0u, 0u, 5u, NULL, 0u, out);
}

static size_t encode_subidentifier(uint64_t value, uint8_t *out) {
    uint8_t reversed[10];
    size_t count = 0u, i;
    do {
        reversed[count++] = (uint8_t)(value & 0x7fu);
        value >>= 7;
    } while (value != 0u);
    for (i = 0u; i < count; ++i) {
        uint8_t octet = reversed[count - 1u - i];
        if (i + 1u < count) octet |= 0x80u;
        out[i] = octet;
    }
    return count;
}

salts_asn1_status salts_asn1_value_create_oid(
    const uint32_t *components, size_t count, salts_asn1_value **out) {
    uint8_t *content;
    size_t capacity, used = 0u, i;
    uint64_t first;
    salts_asn1_status status;
    if (!out || !components || count < 2u || components[0] > 2u ||
        (components[0] < 2u && components[1] > 39u))
        return SALTS_ASN1_INVALID_ARGUMENT;
    *out = NULL;
    if (count > SIZE_MAX / 5u) return SALTS_ASN1_LIMIT_EXCEEDED;
    capacity = count * 5u;
    content = (uint8_t *)malloc(capacity);
    if (!content) return SALTS_ASN1_OUT_OF_MEMORY;
    first = (uint64_t)components[0] * 40u + components[1];
    used += encode_subidentifier(first, content + used);
    for (i = 2u; i < count; ++i)
        used += encode_subidentifier((uint64_t)components[i], content + used);
    status = salts_asn1_value_create(0u, 0u, 6u, content, used, out);
    free(content);
    return status;
}


salts_asn1_status salts_asn1_value_add_child(salts_asn1_value *parent,
                                              salts_asn1_value *child) {
    size_t parent_depth = 0u, child_height = 0u, next_capacity;
    salts_asn1_value **children;
    salts_asn1_status status;
    if (!parent || !child || !parent->constructed || parent == child ||
        child->parent || value_contains(child, parent, 0u))
        return SALTS_ASN1_INVALID_ARGUMENT;
    status = value_depth_from_root(parent, &parent_depth);
    if (status != SALTS_ASN1_OK) return status;
    status = value_height(child, 0u, &child_height);
    if (status != SALTS_ASN1_OK) return status;
    if (parent_depth + 1u + child_height >= SALTS_ASN1_MAX_DEPTH)
        return SALTS_ASN1_LIMIT_EXCEEDED;

    if (parent->child_count == parent->child_capacity) {
        next_capacity = parent->child_capacity ? parent->child_capacity * 2u : 4u;
        if (next_capacity < parent->child_capacity ||
            next_capacity > SIZE_MAX / sizeof(*children))
            return SALTS_ASN1_LIMIT_EXCEEDED;
        children = (salts_asn1_value **)realloc(
            parent->children, next_capacity * sizeof(*children));
        if (!children) return SALTS_ASN1_OUT_OF_MEMORY;
        parent->children = children;
        parent->child_capacity = next_capacity;
    }
    parent->children[parent->child_count++] = child;
    child->parent = parent;
    return SALTS_ASN1_OK;
}

void salts_asn1_value_free(salts_asn1_value *value) {
    size_t i;
    if (!value) return;
    for (i = 0u; i < value->child_count; ++i)
        salts_asn1_value_free(value->children[i]);
    free(value->children);
    free(value->content);
    free(value);
}

size_t salts_asn1_value_child_count(const salts_asn1_value *value) {
    return value ? value->child_count : 0u;
}

const salts_asn1_value *salts_asn1_value_child_at(
    const salts_asn1_value *value, size_t index) {
    if (!value || index >= value->child_count) return NULL;
    return value->children[index];
}

salts_asn1_status salts_asn1_value_get_tag(
    const salts_asn1_value *value, uint8_t *tag_class,
    uint8_t *constructed, uint32_t *tag_number) {
    if (!value) return SALTS_ASN1_INVALID_ARGUMENT;
    if (tag_class) *tag_class = value->tag_class;
    if (constructed) *constructed = value->constructed;
    if (tag_number) *tag_number = value->tag_number;
    return SALTS_ASN1_OK;
}

salts_asn1_status salts_asn1_value_get_content(
    const salts_asn1_value *value, const uint8_t **data, size_t *size) {
    if (!value || !data || !size) return SALTS_ASN1_INVALID_ARGUMENT;
    if (value->constructed) return SALTS_ASN1_INVALID_ARGUMENT;
    *data = value->content;
    *size = value->content_size;
    return SALTS_ASN1_OK;
}

salts_asn1_status salts_asn1_value_get_integer_i64(
    const salts_asn1_value *value, int64_t *out) {
    size_t start = 0u, size, i;
    uint64_t magnitude = 0u;
    const uint8_t *data;
    int negative;
    if (!value || !out || value->tag_class != 0u ||
        value->tag_number != 2u || value->constructed)
        return SALTS_ASN1_INVALID_ARGUMENT;
    data = value->content;
    size = value->content_size;
    if (size == 0u) return SALTS_ASN1_MALFORMED;
    negative = (data[0] & 0x80u) != 0u;
    while (start + 1u < size) {
        if (data[start] == 0u && (data[start + 1u] & 0x80u) == 0u)
            ++start;
        else if (data[start] == 0xffu &&
                 (data[start + 1u] & 0x80u) != 0u)
            ++start;
        else
            break;
    }
    data += start;
    size -= start;
    if (size > sizeof(uint64_t)) return SALTS_ASN1_LIMIT_EXCEEDED;
    if (!negative) {
        for (i = 0u; i < size; ++i)
            magnitude = (magnitude << 8) | (uint64_t)data[i];
        if (magnitude > (uint64_t)INT64_MAX)
            return SALTS_ASN1_LIMIT_EXCEEDED;
        *out = (int64_t)magnitude;
        return SALTS_ASN1_OK;
    }
    for (i = 0u; i < size; ++i)
        magnitude = (magnitude << 8) | (uint64_t)(uint8_t)~data[i];
    ++magnitude;
    if (magnitude > (uint64_t)INT64_MAX + 1u)
        return SALTS_ASN1_LIMIT_EXCEEDED;
    if (magnitude == (uint64_t)INT64_MAX + 1u)
        *out = INT64_MIN;
    else
        *out = -(int64_t)magnitude;
    return SALTS_ASN1_OK;
}

salts_asn1_status salts_asn1_value_get_boolean(
    const salts_asn1_value *value, int *out) {
    if (!value || !out || value->tag_class != 0u ||
        value->tag_number != 1u || value->constructed)
        return SALTS_ASN1_INVALID_ARGUMENT;
    if (value->content_size != 1u) return SALTS_ASN1_MALFORMED;
    *out = value->content[0] != 0u;
    return SALTS_ASN1_OK;
}

static salts_asn1_status decode_oid_content(
    const uint8_t *data, size_t size, uint32_t *components,
    size_t capacity, size_t *required_count) {
    size_t pos = 0u, count = 0u;
    int first_subidentifier = 1;
    if ((!data && size != 0u) || !required_count)
        return SALTS_ASN1_INVALID_ARGUMENT;
    if (size == 0u) return SALTS_ASN1_MALFORMED;
    while (pos < size) {
        uint64_t subidentifier = 0u;
        int first_octet = 1, terminated = 0;
        while (pos < size) {
            uint8_t octet = data[pos++];
            if (first_octet && octet == 0x80u)
                return SALTS_ASN1_MALFORMED;
            first_octet = 0;
            if (subidentifier > (UINT64_MAX >> 7))
                return SALTS_ASN1_LIMIT_EXCEEDED;
            subidentifier = (subidentifier << 7) | (uint64_t)(octet & 0x7fu);
            if ((octet & 0x80u) == 0u) {
                terminated = 1;
                break;
            }
        }
        if (!terminated) return SALTS_ASN1_UNEXPECTED_END;
        if (first_subidentifier) {
            uint32_t first_arc;
            uint64_t second_arc;
            if (subidentifier < 40u) first_arc = 0u;
            else if (subidentifier < 80u) first_arc = 1u;
            else first_arc = 2u;
            second_arc = subidentifier - (uint64_t)first_arc * 40u;
            if (second_arc > UINT32_MAX)
                return SALTS_ASN1_LIMIT_EXCEEDED;
            if (components) {
                if (count > capacity || capacity - count < 2u)
                    return SALTS_ASN1_BUFFER_TOO_SMALL;
                components[count] = first_arc;
                components[count + 1u] = (uint32_t)second_arc;
            }
            count += 2u;
            first_subidentifier = 0;
        } else {
            if (subidentifier > UINT32_MAX)
                return SALTS_ASN1_LIMIT_EXCEEDED;
            if (components) {
                if (count >= capacity) return SALTS_ASN1_BUFFER_TOO_SMALL;
                components[count] = (uint32_t)subidentifier;
            }
            ++count;
        }
    }
    *required_count = count;
    return SALTS_ASN1_OK;
}

salts_asn1_status salts_asn1_value_get_oid(
    const salts_asn1_value *value, uint32_t *components, size_t capacity,
    size_t *required_count) {
    salts_asn1_status status;
    size_t count = 0u;
    if (!value || !required_count || value->tag_class != 0u ||
        value->tag_number != 6u || value->constructed ||
        (!components && capacity != 0u))
        return SALTS_ASN1_INVALID_ARGUMENT;
    status = decode_oid_content(value->content, value->content_size,
                                NULL, 0u, &count);
    if (status != SALTS_ASN1_OK) return status;
    *required_count = count;
    if (!components) return SALTS_ASN1_OK;
    if (capacity < count) return SALTS_ASN1_BUFFER_TOO_SMALL;
    return decode_oid_content(value->content, value->content_size,
                              components, capacity, required_count);
}


static salts_asn1_status measure_value(
    const salts_asn1_value *value, salts_asn1_mode mode,
    uint8_t use_indefinite, size_t depth,
    size_t *encoded_size, size_t *content_size) {
    size_t content = 0u, total, i, tag_size, length_size, eoc_size;
    salts_asn1_status status;
    if (!value || !encoded_size || !content_size ||
        value->tag_class > 3u || value->constructed > 1u ||
        (value->tag_class == 0u && value->tag_number == 0u))
        return SALTS_ASN1_INVALID_ARGUMENT;
    if (depth >= SALTS_ASN1_MAX_DEPTH)
        return SALTS_ASN1_LIMIT_EXCEEDED;
    if (mode != SALTS_ASN1_BER && mode != SALTS_ASN1_DER)
        return SALTS_ASN1_INVALID_ARGUMENT;
    if (use_indefinite && mode == SALTS_ASN1_DER)
        return SALTS_ASN1_INVALID_LENGTH;

    if (!value->constructed) {
        size_t required = 0u;
        status = salts_asn1_encode_tlv(
            value->tag_class, 0u, value->tag_number, value->content,
            value->content_size, mode, 0u, NULL, 0u, &required);
        if (status != SALTS_ASN1_OK) return status;
        *encoded_size = required;
        *content_size = value->content_size;
        return SALTS_ASN1_OK;
    }
    if (value->content_size != 0u || value->content != NULL)
        return SALTS_ASN1_MALFORMED;

    for (i = 0u; i < value->child_count; ++i) {
        size_t child_encoded = 0u, child_content = 0u;
        status = measure_value(value->children[i], mode, use_indefinite,
                               depth + 1u, &child_encoded, &child_content);
        if (status != SALTS_ASN1_OK) return status;
        if (child_encoded > SIZE_MAX - content)
            return SALTS_ASN1_LIMIT_EXCEEDED;
        content += child_encoded;
    }
    tag_size = encoded_tag_size(value->tag_number);
    length_size = encoded_length_size(content, use_indefinite);
    eoc_size = use_indefinite ? 2u : 0u;
    if (tag_size > SIZE_MAX - length_size ||
        content > SIZE_MAX - tag_size - length_size ||
        eoc_size > SIZE_MAX - tag_size - length_size - content)
        return SALTS_ASN1_LIMIT_EXCEEDED;
    total = tag_size + length_size + content + eoc_size;
    *encoded_size = total;
    *content_size = content;
    return SALTS_ASN1_OK;
}

static salts_asn1_status encode_value_into(
    const salts_asn1_value *value, salts_asn1_mode mode,
    uint8_t use_indefinite, size_t depth, uint8_t *output,
    size_t output_capacity, size_t *written) {
    size_t encoded_size = 0u, content_size = 0u, tag_size, length_size, offset, i;
    uint8_t indefinite;
    salts_asn1_status status;
    if (!output || !written) return SALTS_ASN1_INVALID_ARGUMENT;
    status = measure_value(value, mode, use_indefinite, depth,
                           &encoded_size, &content_size);
    if (status != SALTS_ASN1_OK) return status;
    if (output_capacity < encoded_size) return SALTS_ASN1_BUFFER_TOO_SMALL;
    indefinite = value->constructed && use_indefinite ? 1u : 0u;
    tag_size = encoded_tag_size(value->tag_number);
    write_tag(output, value->tag_class, value->constructed, value->tag_number);
    length_size = write_length(output + tag_size, content_size, indefinite);
    offset = tag_size + length_size;

    if (!value->constructed) {
        if (content_size != 0u)
            memcpy(output + offset, value->content, content_size);
    } else {
        for (i = 0u; i < value->child_count; ++i) {
            size_t child_written = 0u;
            status = encode_value_into(value->children[i], mode, use_indefinite,
                                       depth + 1u, output + offset,
                                       output_capacity - offset, &child_written);
            if (status != SALTS_ASN1_OK) return status;
            offset += child_written;
        }
        if (indefinite) {
            output[offset++] = 0u;
            output[offset++] = 0u;
        }
    }
    if (offset != encoded_size) return SALTS_ASN1_MALFORMED;
    *written = offset;
    return SALTS_ASN1_OK;
}

salts_asn1_status salts_asn1_value_encode(
    const salts_asn1_value *value, salts_asn1_mode mode,
    uint8_t use_indefinite, void *output, size_t output_capacity,
    size_t *written) {
    uint8_t *temporary;
    size_t required = 0u, content_size = 0u, produced = 0u;
    salts_asn1_status status;
    if (!value || !written || use_indefinite > 1u ||
        (!output && output_capacity != 0u))
        return SALTS_ASN1_INVALID_ARGUMENT;
    status = measure_value(value, mode, use_indefinite, 0u,
                           &required, &content_size);
    if (status != SALTS_ASN1_OK) return status;
    temporary = (uint8_t *)malloc(required);
    if (!temporary) return SALTS_ASN1_OUT_OF_MEMORY;
    status = encode_value_into(value, mode, use_indefinite, 0u,
                               temporary, required, &produced);
    if (status == SALTS_ASN1_OK && produced != required)
        status = SALTS_ASN1_MALFORMED;
    if (status == SALTS_ASN1_OK)
        status = salts_asn1_validate(temporary, produced, mode);
    if (status != SALTS_ASN1_OK) {
        free(temporary);
        return status;
    }
    *written = required;
    if (output && output_capacity < required) {
        free(temporary);
        return SALTS_ASN1_BUFFER_TOO_SMALL;
    }
    if (output) memcpy(output, temporary, required);
    free(temporary);
    return SALTS_ASN1_OK;
}

static salts_asn1_status decode_value_node(
    const salts_asn1_reader *parent, const salts_asn1_tlv *tlv,
    salts_asn1_value **out) {
    salts_asn1_value *value = NULL;
    salts_asn1_status status;
    if (!parent || !tlv || !out) return SALTS_ASN1_INVALID_ARGUMENT;
    *out = NULL;
    status = salts_asn1_value_create(
        tlv->tag_class, tlv->constructed, tlv->tag_number,
        tlv->constructed ? NULL : tlv->content,
        tlv->constructed ? 0u : tlv->content_size, &value);
    if (status != SALTS_ASN1_OK) return status;
    if (tlv->constructed) {
        salts_asn1_reader children;
        status = salts_asn1_reader_open_children(parent, tlv, &children);
        if (status != SALTS_ASN1_OK) {
            salts_asn1_value_free(value);
            return status;
        }
        for (;;) {
            salts_asn1_tlv child_tlv;
            salts_asn1_value *child = NULL;
            status = salts_asn1_reader_next(&children, &child_tlv);
            if (status == SALTS_ASN1_DONE) break;
            if (status != SALTS_ASN1_OK) {
                salts_asn1_value_free(value);
                return status;
            }
            status = decode_value_node(&children, &child_tlv, &child);
            if (status != SALTS_ASN1_OK) {
                salts_asn1_value_free(value);
                return status;
            }
            status = salts_asn1_value_add_child(value, child);
            if (status != SALTS_ASN1_OK) {
                salts_asn1_value_free(child);
                salts_asn1_value_free(value);
                return status;
            }
        }
    }
    *out = value;
    return SALTS_ASN1_OK;
}

salts_asn1_status salts_asn1_value_decode(
    const void *data, size_t size, salts_asn1_mode mode,
    salts_asn1_value **out) {
    salts_asn1_reader reader;
    salts_asn1_tlv tlv;
    salts_asn1_status status;
    if (!out) return SALTS_ASN1_INVALID_ARGUMENT;
    *out = NULL;
    status = salts_asn1_validate(data, size, mode);
    if (status != SALTS_ASN1_OK) return status;
    status = salts_asn1_reader_init(&reader, data, size, mode);
    if (status != SALTS_ASN1_OK) return status;
    status = salts_asn1_reader_next(&reader, &tlv);
    if (status != SALTS_ASN1_OK) return status;
    return decode_value_node(&reader, &tlv, out);
}
