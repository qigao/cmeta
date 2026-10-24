#include <salts/asn1.h>

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

typedef struct asn1_header {
    uint8_t tag_class;
    uint8_t constructed;
    uint32_t tag_number;
    size_t header_size;
    size_t content_size;
    bool indefinite;
} asn1_header;

static salts_asn1_status parse_one(const uint8_t *, size_t, salts_asn1_mode,
                                   size_t, salts_asn1_tlv *, bool *);

static salts_asn1_status parse_tag(const uint8_t *data, size_t size,
                                   asn1_header *header, size_t *used) {
    uint8_t first;
    uint32_t number;
    size_t pos = 1u;
    if (size == 0u) return SALTS_ASN1_UNEXPECTED_END;
    first = data[0];
    header->tag_class = (uint8_t)(first >> 6);
    header->constructed = (uint8_t)((first >> 5) & 1u);
    number = (uint32_t)(first & 0x1fu);
    if (number == 0x1fu) {
        size_t count = 0u;
        number = 0u;
        for (;;) {
            uint8_t octet;
            uint32_t payload;
            if (pos >= size) return SALTS_ASN1_UNEXPECTED_END;
            octet = data[pos++];
            payload = (uint32_t)(octet & 0x7fu);
            if (count == 0u && payload == 0u) return SALTS_ASN1_INVALID_TAG;
            if (number > (UINT32_MAX >> 7)) return SALTS_ASN1_LIMIT_EXCEEDED;
            number = (number << 7) | payload;
            if (++count > 5u) return SALTS_ASN1_LIMIT_EXCEEDED;
            if ((octet & 0x80u) == 0u) break;
        }
        if (number < 31u) return SALTS_ASN1_INVALID_TAG;
    }
    header->tag_number = number;
    *used = pos;
    return SALTS_ASN1_OK;
}

static salts_asn1_status parse_length(const uint8_t *data, size_t size,
                                      salts_asn1_mode mode, size_t *length,
                                      size_t *used, bool *indefinite) {
    uint8_t first;
    size_t count, value = 0u, i;
    if (size == 0u) return SALTS_ASN1_UNEXPECTED_END;
    first = data[0];
    if ((first & 0x80u) == 0u) {
        *length = first; *used = 1u; *indefinite = false;
        return SALTS_ASN1_OK;
    }
    count = (size_t)(first & 0x7fu);
    if (count == 0u) {
        if (mode == SALTS_ASN1_DER) return SALTS_ASN1_INVALID_LENGTH;
        *length = 0u; *used = 1u; *indefinite = true;
        return SALTS_ASN1_OK;
    }
    if (count > sizeof(size_t)) return SALTS_ASN1_LIMIT_EXCEEDED;
    if (size < 1u + count) return SALTS_ASN1_UNEXPECTED_END;
    if (data[1] == 0u) return SALTS_ASN1_INVALID_LENGTH;
    for (i = 0u; i < count; ++i) {
        if (value > (SIZE_MAX >> 8)) return SALTS_ASN1_LIMIT_EXCEEDED;
        value = (value << 8) | (size_t)data[1u + i];
    }
    if (value < 128u) return SALTS_ASN1_INVALID_LENGTH;
    *length = value; *used = 1u + count; *indefinite = false;
    return SALTS_ASN1_OK;
}

static int bytes_compare(const uint8_t *a, size_t an,
                         const uint8_t *b, size_t bn) {
    size_t n = an < bn ? an : bn;
    int cmp = n ? memcmp(a, b, n) : 0;
    if (cmp) return cmp;
    return an < bn ? -1 : an > bn ? 1 : 0;
}

static salts_asn1_status validate_oid(const uint8_t *data, size_t size) {
    size_t pos = 0u;
    if (size == 0u) return SALTS_ASN1_MALFORMED;
    while (pos < size) {
        bool first = true, end = false;
        while (pos < size) {
            uint8_t octet = data[pos++];
            if (first && octet == 0x80u) return SALTS_ASN1_MALFORMED;
            first = false;
            if ((octet & 0x80u) == 0u) { end = true; break; }
        }
        if (!end) return SALTS_ASN1_UNEXPECTED_END;
    }
    return SALTS_ASN1_OK;
}

static salts_asn1_status validate_primitive(const uint8_t *data, size_t size,
                                            uint8_t cls, uint32_t tag,
                                            salts_asn1_mode mode) {
    if (cls != 0u) return SALTS_ASN1_OK;
    switch (tag) {
    case 0u: return SALTS_ASN1_INVALID_TAG;
    case 1u:
        if (size != 1u) return SALTS_ASN1_MALFORMED;
        if (mode == SALTS_ASN1_DER && data[0] != 0u && data[0] != 0xffu)
            return SALTS_ASN1_MALFORMED;
        return SALTS_ASN1_OK;
    case 2u:
    case 10u:
        if (size == 0u) return SALTS_ASN1_MALFORMED;
        if (mode == SALTS_ASN1_DER && size > 1u &&
            ((data[0] == 0u && (data[1] & 0x80u) == 0u) ||
             (data[0] == 0xffu && (data[1] & 0x80u) != 0u)))
            return SALTS_ASN1_MALFORMED;
        return SALTS_ASN1_OK;
    case 3u:
        if (size == 0u || data[0] > 7u) return SALTS_ASN1_MALFORMED;
        if (size == 1u && data[0] != 0u) return SALTS_ASN1_MALFORMED;
        if (size > 1u && data[0] != 0u) {
            uint8_t mask = (uint8_t)((1u << data[0]) - 1u);
            if ((data[size - 1u] & mask) != 0u) return SALTS_ASN1_MALFORMED;
        }
        return SALTS_ASN1_OK;
    case 5u: return size == 0u ? SALTS_ASN1_OK : SALTS_ASN1_MALFORMED;
    case 6u:
    case 13u: return validate_oid(data, size);
    default: return SALTS_ASN1_OK;
    }
}

static bool universal_tag_requires_primitive(uint32_t tag) {
    switch (tag) {
    case 1u: case 2u: case 3u: case 4u: case 5u: case 6u:
    case 9u: case 10u: case 12u: case 13u: case 14u:
    case 18u: case 19u: case 20u: case 21u: case 22u:
    case 23u: case 24u: case 25u: case 26u: case 27u:
    case 28u: case 30u:
        return true;
    default:
        return false;
    }
}

static bool universal_tag_is_constructible_string(uint32_t tag) {
    switch (tag) {
    case 3u: case 4u: case 12u: case 18u: case 19u: case 20u:
    case 21u: case 22u: case 25u: case 26u: case 27u: case 28u:
    case 30u:
        return true;
    default:
        return false;
    }
}

static salts_asn1_status parse_one(const uint8_t *data, size_t size,
                                   salts_asn1_mode mode, size_t depth,
                                   salts_asn1_tlv *out, bool *is_eoc) {
    asn1_header h;
    salts_asn1_tlv value;
    salts_asn1_status st;
    size_t tag_size = 0u, len_size = 0u, remaining;
    const uint8_t *content;
    if (!data || !out || !is_eoc) return SALTS_ASN1_INVALID_ARGUMENT;
    if (depth >= SALTS_ASN1_MAX_DEPTH) return SALTS_ASN1_LIMIT_EXCEEDED;
    if (size < 2u) return SALTS_ASN1_UNEXPECTED_END;
    memset(&h, 0, sizeof(h));
    st = parse_tag(data, size, &h, &tag_size);
    if (st != SALTS_ASN1_OK) return st;
    st = parse_length(data + tag_size, size - tag_size, mode,
                      &h.content_size, &len_size, &h.indefinite);
    if (st != SALTS_ASN1_OK) return st;
    h.header_size = tag_size + len_size;
    content = data + h.header_size;
    remaining = size - h.header_size;

    if (h.tag_class == 0u && h.tag_number == 0u) {
        if (h.constructed || h.indefinite || h.content_size != 0u)
            return SALTS_ASN1_INVALID_TAG;
        if (mode == SALTS_ASN1_DER) return SALTS_ASN1_MALFORMED;
        memset(&value, 0, sizeof(value));
        value.content = content; value.encoded_size = h.header_size;
        value.depth = depth;
        *out = value; *is_eoc = true;
        return SALTS_ASN1_OK;
    }
    if (h.indefinite && !h.constructed) return SALTS_ASN1_INVALID_LENGTH;
    if (h.indefinite && mode == SALTS_ASN1_DER) return SALTS_ASN1_INVALID_LENGTH;
    if (!h.indefinite && h.content_size > remaining)
        return SALTS_ASN1_UNEXPECTED_END;
    if (h.tag_class == 0u) {
        if ((h.tag_number == 8u || h.tag_number == 11u ||
             h.tag_number == 16u || h.tag_number == 17u ||
             h.tag_number == 29u) && !h.constructed)
            return SALTS_ASN1_MALFORMED;
        if (h.constructed && universal_tag_requires_primitive(h.tag_number) &&
            (mode == SALTS_ASN1_DER ||
             !universal_tag_is_constructible_string(h.tag_number)))
            return SALTS_ASN1_MALFORMED;
    }

    memset(&value, 0, sizeof(value));
    value.tag_class = h.tag_class;
    value.constructed = h.constructed;
    value.indefinite_length = h.indefinite ? 1u : 0u;
    value.tag_number = h.tag_number;
    value.content = content;
    value.depth = depth;
    *is_eoc = false;

    if (h.constructed) {
        size_t pos = 0u;
        const uint8_t *prev = NULL;
        size_t prev_size = 0u;
        size_t content_bound = h.indefinite ? remaining : h.content_size;
        while (pos < content_bound) {
            salts_asn1_tlv child;
            bool child_eoc = false;
            st = parse_one(content + pos, content_bound - pos, mode,
                           depth + 1u, &child, &child_eoc);
            if (st != SALTS_ASN1_OK) return st;
            if (child_eoc) {
                if (!h.indefinite) return SALTS_ASN1_MALFORMED;
                value.content_size = pos;
                value.encoded_size = h.header_size + pos + child.encoded_size;
                *out = value;
                return SALTS_ASN1_OK;
            }
            if (child.encoded_size == 0u ||
                child.encoded_size > content_bound - pos)
                return SALTS_ASN1_MALFORMED;
            if (mode == SALTS_ASN1_BER && h.tag_class == 0u &&
                universal_tag_is_constructible_string(h.tag_number) &&
                (child.tag_class != 0u || child.tag_number != h.tag_number))
                return SALTS_ASN1_MALFORMED;
            if (mode == SALTS_ASN1_DER && h.tag_class == 0u &&
                h.tag_number == 17u && prev &&
                bytes_compare(prev, prev_size, content + pos,
                              child.encoded_size) > 0)
                return SALTS_ASN1_MALFORMED;
            prev = content + pos;
            prev_size = child.encoded_size;
            pos += child.encoded_size;
        }
        if (h.indefinite) return SALTS_ASN1_UNEXPECTED_END;
        value.content_size = h.content_size;
        value.encoded_size = h.header_size + h.content_size;
    } else {
        st = validate_primitive(content, h.content_size, h.tag_class,
                                h.tag_number, mode);
        if (st != SALTS_ASN1_OK) return st;
        value.content_size = h.content_size;
        value.encoded_size = h.header_size + h.content_size;
    }
    *out = value;
    return SALTS_ASN1_OK;
}

salts_asn1_status salts_asn1_reader_init(salts_asn1_reader *reader,
                                          const void *data, size_t size,
                                          salts_asn1_mode mode) {
    if (!reader || (!data && size != 0u) ||
        (mode != SALTS_ASN1_BER && mode != SALTS_ASN1_DER))
        return SALTS_ASN1_INVALID_ARGUMENT;
    reader->data = (const uint8_t *)data;
    reader->size = size;
    reader->offset = 0u;
    reader->mode = mode;
    reader->depth = 0u;
    return SALTS_ASN1_OK;
}

salts_asn1_status salts_asn1_reader_next(salts_asn1_reader *reader,
                                          salts_asn1_tlv *out) {
    salts_asn1_tlv value;
    bool is_eoc = false;
    salts_asn1_status st;
    if (!reader || !out || (!reader->data && reader->size != 0u) ||
        reader->offset > reader->size ||
        (reader->mode != SALTS_ASN1_BER && reader->mode != SALTS_ASN1_DER))
        return SALTS_ASN1_INVALID_ARGUMENT;
    if (reader->offset == reader->size) return SALTS_ASN1_DONE;
    st = parse_one(reader->data + reader->offset,
                   reader->size - reader->offset, reader->mode,
                   reader->depth, &value, &is_eoc);
    if (st != SALTS_ASN1_OK) return st;
    if (is_eoc) return SALTS_ASN1_MALFORMED;
    if (value.encoded_size == 0u ||
        value.encoded_size > reader->size - reader->offset)
        return SALTS_ASN1_MALFORMED;
    reader->offset += value.encoded_size;
    *out = value;
    return SALTS_ASN1_OK;
}

salts_asn1_status salts_asn1_reader_open_children(
    const salts_asn1_reader *parent, const salts_asn1_tlv *value,
    salts_asn1_reader *children) {
    if (!parent || !value || !children) return SALTS_ASN1_INVALID_ARGUMENT;
    if ((parent->mode != SALTS_ASN1_BER && parent->mode != SALTS_ASN1_DER) ||
        value->depth != parent->depth)
        return SALTS_ASN1_INVALID_ARGUMENT;
    if (!value->constructed) return SALTS_ASN1_INVALID_ARGUMENT;
    if ((!value->content && value->content_size != 0u) ||
        value->depth >= SALTS_ASN1_MAX_DEPTH - 1u)
        return SALTS_ASN1_LIMIT_EXCEEDED;
    children->data = value->content;
    children->size = value->content_size;
    children->offset = 0u;
    children->mode = parent->mode;
    children->depth = value->depth + 1u;
    return SALTS_ASN1_OK;
}

salts_asn1_status salts_asn1_validate(const void *data, size_t size,
                                      salts_asn1_mode mode) {
    salts_asn1_reader reader;
    salts_asn1_tlv value;
    salts_asn1_status st;
    if (!data || size == 0u) return SALTS_ASN1_INVALID_ARGUMENT;
    st = salts_asn1_reader_init(&reader, data, size, mode);
    if (st != SALTS_ASN1_OK) return st;
    st = salts_asn1_reader_next(&reader, &value);
    if (st != SALTS_ASN1_OK) return st;
    if (reader.offset != size) return SALTS_ASN1_TRAILING_DATA;
    return SALTS_ASN1_OK;
}


static salts_asn1_status validate_content_for_encoding(
    uint8_t tag_class, uint8_t constructed, uint32_t tag_number,
    const uint8_t *content, size_t content_size, salts_asn1_mode mode) {
    salts_asn1_status status;
    if (tag_class == 0u) {
        if (tag_number == 0u) return SALTS_ASN1_INVALID_TAG;
        if ((tag_number == 8u || tag_number == 11u || tag_number == 16u ||
             tag_number == 17u || tag_number == 29u) && !constructed)
            return SALTS_ASN1_MALFORMED;
        if (constructed && universal_tag_requires_primitive(tag_number) &&
            (mode == SALTS_ASN1_DER ||
             !universal_tag_is_constructible_string(tag_number)))
            return SALTS_ASN1_MALFORMED;
    }
    if (!constructed)
        return validate_primitive(content, content_size, tag_class, tag_number,
                                  mode);

    {
        salts_asn1_reader reader;
        salts_asn1_tlv child;
        const uint8_t *previous = NULL;
        size_t previous_size = 0u;
        status = salts_asn1_reader_init(&reader, content, content_size, mode);
        if (status != SALTS_ASN1_OK) return status;
        for (;;) {
            status = salts_asn1_reader_next(&reader, &child);
            if (status == SALTS_ASN1_DONE) return SALTS_ASN1_OK;
            if (status != SALTS_ASN1_OK) return status;
            if (mode == SALTS_ASN1_BER && tag_class == 0u &&
                universal_tag_is_constructible_string(tag_number) &&
                (child.tag_class != 0u || child.tag_number != tag_number))
                return SALTS_ASN1_MALFORMED;
            if (mode == SALTS_ASN1_DER && tag_class == 0u &&
                tag_number == 17u && previous &&
                bytes_compare(previous, previous_size,
                              content + reader.offset - child.encoded_size,
                              child.encoded_size) > 0)
                return SALTS_ASN1_MALFORMED;
            previous = content + reader.offset - child.encoded_size;
            previous_size = child.encoded_size;
        }
    }
}

static size_t encode_tag_bytes(uint8_t tag_class, uint8_t constructed,
                               uint32_t tag_number, uint8_t out[6]) {
    size_t count = 0u, i;
    uint32_t value;
    if (tag_number < 31u) {
        out[0] = (uint8_t)((tag_class << 6) | (constructed << 5) |
                           (uint8_t)tag_number);
        return 1u;
    }
    out[0] = (uint8_t)((tag_class << 6) | (constructed << 5) | 0x1fu);
    value = tag_number;
    do {
        out[1u + count] = (uint8_t)(value & 0x7fu);
        value >>= 7;
        ++count;
    } while (value != 0u && count < 5u);
    for (i = 0u; i < count / 2u; ++i) {
        uint8_t tmp = out[1u + i];
        out[1u + i] = out[count - i];
        out[count - i] = tmp;
    }
    for (i = 1u; i < count; ++i) out[i] |= 0x80u;
    return 1u + count;
}

static size_t encode_length_bytes(size_t length, uint8_t indefinite,
                                  uint8_t out[1u + sizeof(size_t)]) {
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

salts_asn1_status salts_asn1_encode_tlv(
    uint8_t tag_class, uint8_t constructed, uint32_t tag_number,
    const void *content, size_t content_size, salts_asn1_mode mode,
    uint8_t indefinite_length, void *output, size_t output_capacity,
    size_t *written) {
    uint8_t tag_bytes[6];
    uint8_t length_bytes[1u + sizeof(size_t)];
    size_t tag_size, length_size, header_size, eoc_size, required;
    salts_asn1_status status;
    uint8_t *destination = (uint8_t *)output;

    if (!written || tag_class > 3u || constructed > 1u ||
        indefinite_length > 1u || (!content && content_size != 0u) ||
        (mode != SALTS_ASN1_BER && mode != SALTS_ASN1_DER) ||
        (!output && output_capacity != 0u))
        return SALTS_ASN1_INVALID_ARGUMENT;
    if (indefinite_length && (mode != SALTS_ASN1_BER || !constructed))
        return SALTS_ASN1_INVALID_LENGTH;

    status = validate_content_for_encoding(tag_class, constructed, tag_number,
                                           (const uint8_t *)content,
                                           content_size, mode);
    if (status != SALTS_ASN1_OK) return status;

    tag_size = encode_tag_bytes(tag_class, constructed, tag_number, tag_bytes);
    length_size = encode_length_bytes(content_size, indefinite_length, length_bytes);
    header_size = tag_size + length_size;
    eoc_size = indefinite_length ? 2u : 0u;
    if (content_size > SIZE_MAX - header_size ||
        eoc_size > SIZE_MAX - header_size - content_size)
        return SALTS_ASN1_LIMIT_EXCEEDED;
    required = header_size + content_size + eoc_size;

    if (!output) {
        *written = required;
        return SALTS_ASN1_OK;
    }
    if (output_capacity < required) {
        *written = required;
        return SALTS_ASN1_BUFFER_TOO_SMALL;
    }

    /* Move content before writing the header, allowing safe in-place wrapping. */
    if (content_size != 0u)
        memmove(destination + header_size, content, content_size);
    memcpy(destination, tag_bytes, tag_size);
    memcpy(destination + tag_size, length_bytes, length_size);
    if (indefinite_length) {
        destination[header_size + content_size] = 0u;
        destination[header_size + content_size + 1u] = 0u;
    }
    *written = required;
    return SALTS_ASN1_OK;
}
