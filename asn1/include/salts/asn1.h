#ifndef SALTS_ASN1_H
#define SALTS_ASN1_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32) && defined(SALTS_ASN1_API)
#  define SALTS_ASN1_PUBLIC SALTS_ASN1_API
#elif !defined(_WIN32) && defined(__GNUC__) && __GNUC__ >= 4
#  define SALTS_ASN1_PUBLIC __attribute__((visibility("default")))
#else
#  define SALTS_ASN1_PUBLIC
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define SALTS_ASN1_ABI_VERSION 1u
#define SALTS_ASN1_MAX_DEPTH 32u

typedef enum salts_asn1_mode {
    SALTS_ASN1_BER = 1,
    SALTS_ASN1_DER = 2
} salts_asn1_mode;

typedef enum salts_asn1_status {
    SALTS_ASN1_OK = 0,
    SALTS_ASN1_DONE = 1,
    SALTS_ASN1_INVALID_ARGUMENT = 2,
    SALTS_ASN1_UNEXPECTED_END = 3,
    SALTS_ASN1_INVALID_TAG = 4,
    SALTS_ASN1_INVALID_LENGTH = 5,
    SALTS_ASN1_MALFORMED = 6,
    SALTS_ASN1_LIMIT_EXCEEDED = 7,
    SALTS_ASN1_UNSUPPORTED = 8,
    SALTS_ASN1_TRAILING_DATA = 9
} salts_asn1_status;

/* Borrowed view into caller-owned bytes. The source buffer must remain alive
 * and unchanged while the view or a child reader is in use. */
typedef struct salts_asn1_tlv {
    uint8_t tag_class;
    uint8_t constructed;
    uint8_t indefinite_length;
    uint8_t reserved;
    uint32_t tag_number;
    const uint8_t *content;
    size_t content_size;     /* Excludes EOC for indefinite-length values. */
    size_t encoded_size;     /* Includes tag, length, content and EOC. */
    size_t depth;            /* Parser-validated nesting depth; do not modify. */
} salts_asn1_tlv;

/* Stack-allocated reader facade. Treat fields as private after initialization. */
typedef struct salts_asn1_reader {
    const uint8_t *data;
    size_t size;
    size_t offset;
    salts_asn1_mode mode;
    size_t depth;
} salts_asn1_reader;

SALTS_ASN1_PUBLIC salts_asn1_status salts_asn1_reader_init(
    salts_asn1_reader *reader, const void *data, size_t size,
    salts_asn1_mode mode);

/* Reads one complete TLV. DONE means input exhaustion. On parse failure, the
 * reader offset and output TLV remain unchanged. Constructed contents are
 * recursively validated before a TLV is returned. */
SALTS_ASN1_PUBLIC salts_asn1_status salts_asn1_reader_next(
    salts_asn1_reader *reader, salts_asn1_tlv *out);

/* Opens validated children of a constructed TLV. The child reader borrows the
 * same buffer; indefinite-length EOC bytes are not exposed. */
SALTS_ASN1_PUBLIC salts_asn1_status salts_asn1_reader_open_children(
    const salts_asn1_reader *parent, const salts_asn1_tlv *value,
    salts_asn1_reader *children);

/* Validates exactly one complete encoded value and rejects trailing bytes.
 * DER mode rejects indefinite lengths and non-canonical universal encodings. */
SALTS_ASN1_PUBLIC salts_asn1_status salts_asn1_validate(
    const void *data, size_t size, salts_asn1_mode mode);

#ifdef __cplusplus
}
#endif

#endif /* SALTS_ASN1_H */
