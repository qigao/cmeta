#include <salts/asn1_crypto.h>

#include <string.h>

static const uint8_t x25519_oid[] = {0x2bu, 0x65u, 0x6eu};
static const uint8_t ed25519_oid[] = {0x2bu, 0x65u, 0x70u};

#define ECDSA_INTEGER_CONTENT_MAX_BYTES \
    (SALTS_ASN1_ECDSA_COMPONENT_MAX_BYTES + 1u)
#define ECDSA_INTEGER_TLV_MAX_BYTES \
    (ECDSA_INTEGER_CONTENT_MAX_BYTES + 2u)
#define ECDSA_SEQUENCE_CONTENT_MAX_BYTES \
    (ECDSA_INTEGER_TLV_MAX_BYTES * 2u)

static salts_asn1_status algorithm_oid(salts_asn1_rfc8410_algorithm algorithm,
                                       const uint8_t **oid) {
    if (!oid) return SALTS_ASN1_INVALID_ARGUMENT;
    if (algorithm == SALTS_ASN1_RFC8410_X25519) *oid = x25519_oid;
    else if (algorithm == SALTS_ASN1_RFC8410_ED25519) *oid = ed25519_oid;
    else return SALTS_ASN1_INVALID_ARGUMENT;
    return SALTS_ASN1_OK;
}

static salts_asn1_status decode_algorithm(const salts_asn1_tlv *value,
                                          salts_asn1_rfc8410_algorithm *out) {
    salts_asn1_reader children;
    salts_asn1_tlv oid;
    salts_asn1_status status;
    salts_asn1_rfc8410_algorithm algorithm;
    if (!value || !out || value->tag_class != 0u || !value->constructed ||
        value->tag_number != 16u)
        return SALTS_ASN1_MALFORMED;
    status = salts_asn1_reader_init(&children, value->content,
                                    value->content_size, SALTS_ASN1_DER);
    if (status != SALTS_ASN1_OK) return status;
    status = salts_asn1_reader_next(&children, &oid);
    if (status != SALTS_ASN1_OK || oid.tag_class != 0u || oid.constructed ||
        oid.tag_number != 6u || oid.content_size != sizeof(x25519_oid))
        return SALTS_ASN1_MALFORMED;
    if (memcmp(oid.content, x25519_oid, sizeof(x25519_oid)) == 0)
        algorithm = SALTS_ASN1_RFC8410_X25519;
    else if (memcmp(oid.content, ed25519_oid, sizeof(ed25519_oid)) == 0)
        algorithm = SALTS_ASN1_RFC8410_ED25519;
    else
        return SALTS_ASN1_UNSUPPORTED;
    if (salts_asn1_reader_next(&children, &oid) != SALTS_ASN1_DONE)
        return SALTS_ASN1_MALFORMED;
    *out = algorithm;
    return SALTS_ASN1_OK;
}

static salts_asn1_status encode_integer_component(
    const uint8_t *input, size_t input_size,
    uint8_t output[ECDSA_INTEGER_TLV_MAX_BYTES],
    size_t *output_size) {
    uint8_t content[ECDSA_INTEGER_CONTENT_MAX_BYTES];
    size_t content_size, prefix = 0u;
    while (input_size > 0u && *input == 0u) {
        ++input;
        --input_size;
    }
    if (input_size > SALTS_ASN1_ECDSA_COMPONENT_MAX_BYTES)
        return SALTS_ASN1_LIMIT_EXCEEDED;
    if (input_size == 0u || (input[0] & 0x80u) != 0u) prefix = 1u;
    content_size = prefix + input_size;
    if (prefix != 0u) content[0] = 0u;
    if (input_size != 0u) memcpy(content + prefix, input, input_size);
    return salts_asn1_encode_tlv(0u, 0u, 2u, content, content_size,
                                 SALTS_ASN1_DER, 0u, output,
                                 ECDSA_INTEGER_TLV_MAX_BYTES,
                                 output_size);
}

salts_asn1_status salts_asn1_ecdsa_signature_encode(
    const void *r, size_t r_size, const void *s, size_t s_size,
    void *output, size_t output_capacity, size_t *written) {
    uint8_t content[ECDSA_SEQUENCE_CONTENT_MAX_BYTES];
    uint8_t staged[SALTS_ASN1_ECDSA_SIGNATURE_MAX_BYTES];
    size_t r_encoded_size = 0u, s_encoded_size = 0u, required = 0u;
    salts_asn1_status status;
    if (!written || (!r && r_size != 0u) || (!s && s_size != 0u) ||
        (!output && output_capacity != 0u))
        return SALTS_ASN1_INVALID_ARGUMENT;
    status = encode_integer_component((const uint8_t *)r, r_size, content,
                                      &r_encoded_size);
    if (status != SALTS_ASN1_OK) return status;
    status = encode_integer_component((const uint8_t *)s, s_size,
                                      content + r_encoded_size,
                                      &s_encoded_size);
    if (status != SALTS_ASN1_OK) return status;
    status = salts_asn1_encode_tlv(0u, 1u, 16u, content,
                                   r_encoded_size + s_encoded_size,
                                   SALTS_ASN1_DER, 0u, staged, sizeof(staged),
                                   &required);
    if (status != SALTS_ASN1_OK) return status;
    if (!output) {
        *written = required;
        return SALTS_ASN1_OK;
    }
    if (output_capacity < required) {
        *written = required;
        return SALTS_ASN1_BUFFER_TOO_SMALL;
    }
    memcpy(output, staged, required);
    *written = required;
    return SALTS_ASN1_OK;
}

static salts_asn1_status decode_component(const salts_asn1_tlv *value,
                                          const uint8_t **magnitude,
                                          size_t *magnitude_size) {
    const uint8_t *data;
    size_t size;
    if (value->tag_class != 0u || value->constructed || value->tag_number != 2u ||
        value->content_size == 0u || (value->content[0] & 0x80u) != 0u)
        return SALTS_ASN1_MALFORMED;
    data = value->content;
    size = value->content_size;
    if (size > 1u && data[0] == 0u) {
        ++data;
        --size;
    }
    if (size > SALTS_ASN1_ECDSA_COMPONENT_MAX_BYTES)
        return SALTS_ASN1_LIMIT_EXCEEDED;
    *magnitude = data;
    *magnitude_size = size;
    return SALTS_ASN1_OK;
}

salts_asn1_status salts_asn1_ecdsa_signature_decode(
    const void *der, size_t der_size,
    const uint8_t **r, size_t *r_size,
    const uint8_t **s, size_t *s_size) {
    salts_asn1_reader reader, children;
    salts_asn1_tlv sequence, component;
    salts_asn1_status status;
    const uint8_t *decoded_r, *decoded_s;
    size_t decoded_r_size, decoded_s_size;
    if (!der || der_size == 0u || !r || !r_size || !s || !s_size)
        return SALTS_ASN1_INVALID_ARGUMENT;
    status = salts_asn1_reader_init(&reader, der, der_size, SALTS_ASN1_DER);
    if (status != SALTS_ASN1_OK) return status;
    status = salts_asn1_reader_next(&reader, &sequence);
    if (status != SALTS_ASN1_OK) return status;
    if (reader.offset != der_size || sequence.tag_class != 0u ||
        !sequence.constructed || sequence.tag_number != 16u)
        return SALTS_ASN1_MALFORMED;
    status = salts_asn1_reader_open_children(&reader, &sequence, &children);
    if (status != SALTS_ASN1_OK) return status;
    status = salts_asn1_reader_next(&children, &component);
    if (status != SALTS_ASN1_OK) return status;
    status = decode_component(&component, &decoded_r, &decoded_r_size);
    if (status != SALTS_ASN1_OK) return status;
    status = salts_asn1_reader_next(&children, &component);
    if (status != SALTS_ASN1_OK) return status;
    status = decode_component(&component, &decoded_s, &decoded_s_size);
    if (status != SALTS_ASN1_OK) return status;
    if (salts_asn1_reader_next(&children, &component) != SALTS_ASN1_DONE)
        return SALTS_ASN1_MALFORMED;
    *r = decoded_r;
    *r_size = decoded_r_size;
    *s = decoded_s;
    *s_size = decoded_s_size;
    return SALTS_ASN1_OK;
}

salts_asn1_status salts_asn1_rfc8410_spki_encode(
    salts_asn1_rfc8410_algorithm algorithm,
    const uint8_t key[SALTS_ASN1_RFC8410_KEY_BYTES],
    void *output, size_t output_capacity, size_t *written) {
    const uint8_t *oid;
    uint8_t staged[SALTS_ASN1_RFC8410_SPKI_BYTES] = {
        0x30u,0x2au,0x30u,0x05u,0x06u,0x03u,0,0,0,
        0x03u,0x21u,0x00u
    };
    if (!written || !key || (!output && output_capacity != 0u))
        return SALTS_ASN1_INVALID_ARGUMENT;
    if (algorithm_oid(algorithm, &oid) != SALTS_ASN1_OK)
        return SALTS_ASN1_INVALID_ARGUMENT;
    if (!output) {
        *written = sizeof(staged);
        return SALTS_ASN1_OK;
    }
    if (output_capacity < sizeof(staged)) {
        *written = sizeof(staged);
        return SALTS_ASN1_BUFFER_TOO_SMALL;
    }
    memcpy(staged + 6u, oid, 3u);
    memcpy(staged + 12u, key, SALTS_ASN1_RFC8410_KEY_BYTES);
    memcpy(output, staged, sizeof(staged));
    *written = sizeof(staged);
    return SALTS_ASN1_OK;
}

salts_asn1_status salts_asn1_rfc8410_spki_decode(
    const void *der, size_t der_size,
    salts_asn1_rfc8410_algorithm *algorithm,
    const uint8_t **key, size_t *key_size) {
    salts_asn1_reader reader, children;
    salts_asn1_tlv root, value;
    salts_asn1_status status;
    salts_asn1_rfc8410_algorithm decoded_algorithm;
    const uint8_t *decoded_key;
    if (!der || !algorithm || !key || !key_size)
        return SALTS_ASN1_INVALID_ARGUMENT;
    status = salts_asn1_reader_init(&reader, der, der_size, SALTS_ASN1_DER);
    if (status != SALTS_ASN1_OK) return status;
    status = salts_asn1_reader_next(&reader, &root);
    if (status != SALTS_ASN1_OK) return status;
    if (reader.offset != der_size || root.tag_class != 0u || !root.constructed ||
        root.tag_number != 16u) return SALTS_ASN1_MALFORMED;
    status = salts_asn1_reader_open_children(&reader, &root, &children);
    if (status != SALTS_ASN1_OK) return status;
    status = salts_asn1_reader_next(&children, &value);
    if (status != SALTS_ASN1_OK) return status;
    status = decode_algorithm(&value, &decoded_algorithm);
    if (status != SALTS_ASN1_OK) return status;
    status = salts_asn1_reader_next(&children, &value);
    if (status != SALTS_ASN1_OK || value.tag_class != 0u || value.constructed ||
        value.tag_number != 3u || value.content_size != 33u ||
        value.content[0] != 0u) return SALTS_ASN1_MALFORMED;
    decoded_key = value.content + 1u;
    if (salts_asn1_reader_next(&children, &value) != SALTS_ASN1_DONE)
        return SALTS_ASN1_MALFORMED;
    *algorithm = decoded_algorithm;
    *key = decoded_key;
    *key_size = SALTS_ASN1_RFC8410_KEY_BYTES;
    return SALTS_ASN1_OK;
}

salts_asn1_status salts_asn1_rfc8410_pkcs8_encode(
    salts_asn1_rfc8410_algorithm algorithm,
    const uint8_t seed[SALTS_ASN1_RFC8410_KEY_BYTES],
    void *output, size_t output_capacity, size_t *written) {
    const uint8_t *oid;
    uint8_t staged[SALTS_ASN1_RFC8410_PKCS8_BYTES] = {
        0x30u,0x2eu,0x02u,0x01u,0x00u,0x30u,0x05u,0x06u,0x03u,0,0,0,
        0x04u,0x22u,0x04u,0x20u
    };
    if (!written || !seed || (!output && output_capacity != 0u))
        return SALTS_ASN1_INVALID_ARGUMENT;
    if (algorithm_oid(algorithm, &oid) != SALTS_ASN1_OK)
        return SALTS_ASN1_INVALID_ARGUMENT;
    if (!output) {
        *written = sizeof(staged);
        return SALTS_ASN1_OK;
    }
    if (output_capacity < sizeof(staged)) {
        *written = sizeof(staged);
        return SALTS_ASN1_BUFFER_TOO_SMALL;
    }
    memcpy(staged + 9u, oid, 3u);
    memcpy(staged + 16u, seed, SALTS_ASN1_RFC8410_KEY_BYTES);
    memcpy(output, staged, sizeof(staged));
    *written = sizeof(staged);
    return SALTS_ASN1_OK;
}

salts_asn1_status salts_asn1_rfc8410_pkcs8_decode(
    const void *der, size_t der_size,
    salts_asn1_rfc8410_algorithm *algorithm,
    const uint8_t **seed, size_t *seed_size) {
    salts_asn1_reader reader, children, inner;
    salts_asn1_tlv root, value, nested;
    salts_asn1_status status;
    salts_asn1_rfc8410_algorithm decoded_algorithm;
    const uint8_t *decoded_seed;
    if (!der || !algorithm || !seed || !seed_size)
        return SALTS_ASN1_INVALID_ARGUMENT;
    status = salts_asn1_reader_init(&reader, der, der_size, SALTS_ASN1_DER);
    if (status != SALTS_ASN1_OK) return status;
    status = salts_asn1_reader_next(&reader, &root);
    if (status != SALTS_ASN1_OK) return status;
    if (reader.offset != der_size || root.tag_class != 0u || !root.constructed ||
        root.tag_number != 16u) return SALTS_ASN1_MALFORMED;
    status = salts_asn1_reader_open_children(&reader, &root, &children);
    if (status != SALTS_ASN1_OK) return status;
    status = salts_asn1_reader_next(&children, &value);
    if (status != SALTS_ASN1_OK || value.tag_class != 0u || value.constructed ||
        value.tag_number != 2u || value.content_size != 1u ||
        value.content[0] != 0u) return SALTS_ASN1_MALFORMED;
    status = salts_asn1_reader_next(&children, &value);
    if (status != SALTS_ASN1_OK) return status;
    status = decode_algorithm(&value, &decoded_algorithm);
    if (status != SALTS_ASN1_OK) return status;
    status = salts_asn1_reader_next(&children, &value);
    if (status != SALTS_ASN1_OK || value.tag_class != 0u || value.constructed ||
        value.tag_number != 4u) return SALTS_ASN1_MALFORMED;
    status = salts_asn1_reader_init(&inner, value.content, value.content_size,
                                    SALTS_ASN1_DER);
    if (status != SALTS_ASN1_OK) return status;
    status = salts_asn1_reader_next(&inner, &nested);
    if (status != SALTS_ASN1_OK || inner.offset != value.content_size ||
        nested.tag_class != 0u || nested.constructed || nested.tag_number != 4u ||
        nested.content_size != SALTS_ASN1_RFC8410_KEY_BYTES)
        return SALTS_ASN1_MALFORMED;
    decoded_seed = nested.content;
    if (salts_asn1_reader_next(&children, &value) != SALTS_ASN1_DONE)
        return SALTS_ASN1_MALFORMED;
    *algorithm = decoded_algorithm;
    *seed = decoded_seed;
    *seed_size = SALTS_ASN1_RFC8410_KEY_BYTES;
    return SALTS_ASN1_OK;
}
