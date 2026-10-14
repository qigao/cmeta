/**
 * @file salts_unicode.h
 * @brief Strict UTF-8 scalar scanning and versioned Unicode properties.
 */

#ifndef SALTS_UNICODE_H
#define SALTS_UNICODE_H

#include <vstr.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SALTS_UNICODE_VERSION_MAJOR 17
#define SALTS_UNICODE_VERSION_MINOR 0
#define SALTS_UNICODE_VERSION_PATCH 0
#define SALTS_UNICODE_VERSION_STRING "17.0.0"

typedef enum salts_unicode_status {
  SALTS_UNICODE_OK = 0,
  SALTS_UNICODE_END = 1,
  SALTS_UNICODE_NO_MATCH = 2,
  SALTS_UNICODE_ERR_INVALID_ARGUMENT = -1,
  SALTS_UNICODE_ERR_INVALID_UTF8 = -2,
  SALTS_UNICODE_ERR_CALLBACK = -3
} salts_unicode_status;

typedef enum salts_unicode_case_mode {
  SALTS_UNICODE_CASE_UPPER,
  SALTS_UNICODE_CASE_LOWER,
  SALTS_UNICODE_CASE_TITLE
} salts_unicode_case_mode;

typedef int (*salts_unicode_case_write)(const char *bytes, size_t length, void *userdata);

typedef enum salts_unicode_property {
  SALTS_UNICODE_PROPERTY_NONE = 0u,
  SALTS_UNICODE_PROPERTY_XID_START = 1u << 0,
  SALTS_UNICODE_PROPERTY_XID_CONTINUE = 1u << 1,
  SALTS_UNICODE_PROPERTY_WHITE_SPACE = 1u << 2,
  SALTS_UNICODE_PROPERTY_EMOJI = 1u << 3,
  SALTS_UNICODE_PROPERTY_EMOJI_PRESENTATION = 1u << 4,
  SALTS_UNICODE_PROPERTY_EMOJI_MODIFIER = 1u << 5,
  SALTS_UNICODE_PROPERTY_EMOJI_MODIFIER_BASE = 1u << 6,
  SALTS_UNICODE_PROPERTY_EMOJI_COMPONENT = 1u << 7,
  SALTS_UNICODE_PROPERTY_EXTENDED_PICTOGRAPHIC = 1u << 8
} salts_unicode_property;

typedef struct salts_unicode_scalar {
  uint32_t value;
  uint32_t properties;
  size_t byte_offset;
  size_t byte_length;
} salts_unicode_scalar;

typedef enum salts_unicode_grapheme_break {
  SALTS_UNICODE_GRAPHEME_OTHER = 0,
  SALTS_UNICODE_GRAPHEME_CR,
  SALTS_UNICODE_GRAPHEME_LF,
  SALTS_UNICODE_GRAPHEME_CONTROL,
  SALTS_UNICODE_GRAPHEME_EXTEND,
  SALTS_UNICODE_GRAPHEME_ZWJ,
  SALTS_UNICODE_GRAPHEME_REGIONAL_INDICATOR,
  SALTS_UNICODE_GRAPHEME_PREPEND,
  SALTS_UNICODE_GRAPHEME_SPACING_MARK,
  SALTS_UNICODE_GRAPHEME_L,
  SALTS_UNICODE_GRAPHEME_V,
  SALTS_UNICODE_GRAPHEME_T,
  SALTS_UNICODE_GRAPHEME_LV,
  SALTS_UNICODE_GRAPHEME_LVT
} salts_unicode_grapheme_break;


typedef enum salts_unicode_word_break {
  SALTS_UNICODE_WORD_OTHER = 0,
  SALTS_UNICODE_WORD_CR,
  SALTS_UNICODE_WORD_LF,
  SALTS_UNICODE_WORD_NEWLINE,
  SALTS_UNICODE_WORD_EXTEND,
  SALTS_UNICODE_WORD_FORMAT,
  SALTS_UNICODE_WORD_ZWJ,
  SALTS_UNICODE_WORD_WSEG_SPACE,
  SALTS_UNICODE_WORD_ALETTER,
  SALTS_UNICODE_WORD_HEBREW_LETTER,
  SALTS_UNICODE_WORD_NUMERIC,
  SALTS_UNICODE_WORD_KATAKANA,
  SALTS_UNICODE_WORD_EXTEND_NUM_LET,
  SALTS_UNICODE_WORD_MID_LETTER,
  SALTS_UNICODE_WORD_MID_NUM,
  SALTS_UNICODE_WORD_MID_NUM_LET,
  SALTS_UNICODE_WORD_SINGLE_QUOTE,
  SALTS_UNICODE_WORD_DOUBLE_QUOTE,
  SALTS_UNICODE_WORD_REGIONAL_INDICATOR
} salts_unicode_word_break;

typedef enum salts_unicode_line_break {
  SALTS_UNICODE_LINE_BREAK_OP = 0,
  SALTS_UNICODE_LINE_BREAK_CL,
  SALTS_UNICODE_LINE_BREAK_CP,
  SALTS_UNICODE_LINE_BREAK_QU,
  SALTS_UNICODE_LINE_BREAK_GL,
  SALTS_UNICODE_LINE_BREAK_NS,
  SALTS_UNICODE_LINE_BREAK_EX,
  SALTS_UNICODE_LINE_BREAK_SY,
  SALTS_UNICODE_LINE_BREAK_IS,
  SALTS_UNICODE_LINE_BREAK_PR,
  SALTS_UNICODE_LINE_BREAK_PO,
  SALTS_UNICODE_LINE_BREAK_NU,
  SALTS_UNICODE_LINE_BREAK_AL,
  SALTS_UNICODE_LINE_BREAK_HL,
  SALTS_UNICODE_LINE_BREAK_ID,
  SALTS_UNICODE_LINE_BREAK_IN,
  SALTS_UNICODE_LINE_BREAK_HY,
  SALTS_UNICODE_LINE_BREAK_BA,
  SALTS_UNICODE_LINE_BREAK_BB,
  SALTS_UNICODE_LINE_BREAK_B2,
  SALTS_UNICODE_LINE_BREAK_ZW,
  SALTS_UNICODE_LINE_BREAK_CM,
  SALTS_UNICODE_LINE_BREAK_WJ,
  SALTS_UNICODE_LINE_BREAK_H2,
  SALTS_UNICODE_LINE_BREAK_H3,
  SALTS_UNICODE_LINE_BREAK_JL,
  SALTS_UNICODE_LINE_BREAK_JV,
  SALTS_UNICODE_LINE_BREAK_JT,
  SALTS_UNICODE_LINE_BREAK_RI,
  SALTS_UNICODE_LINE_BREAK_EB,
  SALTS_UNICODE_LINE_BREAK_EM,
  SALTS_UNICODE_LINE_BREAK_ZWJ,
  SALTS_UNICODE_LINE_BREAK_AK,
  SALTS_UNICODE_LINE_BREAK_AP,
  SALTS_UNICODE_LINE_BREAK_AS,
  SALTS_UNICODE_LINE_BREAK_VF,
  SALTS_UNICODE_LINE_BREAK_VI,
  SALTS_UNICODE_LINE_BREAK_HH,
  SALTS_UNICODE_LINE_BREAK_CB,
  SALTS_UNICODE_LINE_BREAK_AI,
  SALTS_UNICODE_LINE_BREAK_BK,
  SALTS_UNICODE_LINE_BREAK_CJ,
  SALTS_UNICODE_LINE_BREAK_CR,
  SALTS_UNICODE_LINE_BREAK_LF,
  SALTS_UNICODE_LINE_BREAK_NL,
  SALTS_UNICODE_LINE_BREAK_SA,
  SALTS_UNICODE_LINE_BREAK_SG,
  SALTS_UNICODE_LINE_BREAK_SP,
  SALTS_UNICODE_LINE_BREAK_XX
} salts_unicode_line_break;

typedef enum salts_unicode_line_break_opportunity {
  SALTS_UNICODE_LINE_BREAK_ALLOWED = 0,
  SALTS_UNICODE_LINE_BREAK_MANDATORY = 1
} salts_unicode_line_break_opportunity;


typedef enum salts_unicode_bidi_class {
  SALTS_UNICODE_BIDI_L = 0,
  SALTS_UNICODE_BIDI_R,
  SALTS_UNICODE_BIDI_AL,
  SALTS_UNICODE_BIDI_EN,
  SALTS_UNICODE_BIDI_ES,
  SALTS_UNICODE_BIDI_ET,
  SALTS_UNICODE_BIDI_AN,
  SALTS_UNICODE_BIDI_CS,
  SALTS_UNICODE_BIDI_NSM,
  SALTS_UNICODE_BIDI_BN,
  SALTS_UNICODE_BIDI_B,
  SALTS_UNICODE_BIDI_S,
  SALTS_UNICODE_BIDI_WS,
  SALTS_UNICODE_BIDI_ON,
  SALTS_UNICODE_BIDI_LRE,
  SALTS_UNICODE_BIDI_LRO,
  SALTS_UNICODE_BIDI_RLE,
  SALTS_UNICODE_BIDI_RLO,
  SALTS_UNICODE_BIDI_PDF,
  SALTS_UNICODE_BIDI_LRI,
  SALTS_UNICODE_BIDI_RLI,
  SALTS_UNICODE_BIDI_FSI,
  SALTS_UNICODE_BIDI_PDI
} salts_unicode_bidi_class;

/**
 * Scan one Unicode scalar from an explicit-length borrowed UTF-8 view.
 *
 * `input` remains owned by the caller and must stay valid for the call. On
 * success, `*cursor` advances by one scalar and `*out_scalar` receives its
 * value, original byte range, and Unicode 17.0.0 property flags. At end,
 * `*cursor == input.len` and output is unchanged. On error both cursor and
 * output are unchanged; for invalid UTF-8 the cursor therefore identifies the
 * first invalid byte observed by this sequential scan.
 *
 * @param input Borrowed UTF-8 byte view; embedded NUL is supported.
 * @param cursor In/out byte offset in the inclusive range `[0, input.len]`.
 * @param out_scalar Output scalar record.
 * @return `SALTS_UNICODE_OK`, `SALTS_UNICODE_END`, or a negative error code.
 */
salts_unicode_status salts_unicode_utf8_next(vstr input, size_t *cursor,
                                             salts_unicode_scalar *out_scalar);

/**
 * Query the Unicode 17.0.0 Grapheme_Cluster_Break value for one scalar.
 * Scalars not explicitly assigned by the UCD return OTHER.
 */
salts_unicode_status salts_unicode_grapheme_break_class(
    uint32_t scalar, salts_unicode_grapheme_break *out_class);


/**
 * Query Unicode 17.0.0 Extended_Pictographic for one scalar.
 *
 * @param scalar Unicode scalar value; surrogates and values above U+10FFFF are invalid.
 * @param result Receives zero or one on success and is unchanged on error.
 */
salts_unicode_status salts_unicode_is_extended_pictographic(
    uint32_t scalar, int *result);

/**
 * Advance one Unicode 17.0.0 extended grapheme cluster (UAX #29).
 *
 * On success, *cursor advances to the exclusive end byte offset and *cluster
 * receives a borrowed subview of input. At end or on error both outputs remain
 * unchanged. Embedded NUL is ordinary text and no normalization is performed.
 */
salts_unicode_status salts_unicode_grapheme_next(vstr input, size_t *cursor,
                                                 vstr *cluster);

/**
 * Move backward one Unicode 17.0.0 extended grapheme cluster (UAX #29).
 *
 * The input cursor must be 0, input.len, or an extended-grapheme boundary. On
 * success it moves to the cluster start and *cluster receives a borrowed view.
 * At start or on error both outputs remain unchanged.
 */
salts_unicode_status salts_unicode_grapheme_prev(vstr input, size_t *cursor,
                                                 vstr *cluster);


/**
 * Query the Unicode 17.0.0 Word_Break value for one scalar.
 * Scalars not explicitly assigned by the UCD return OTHER.
 */
salts_unicode_status salts_unicode_word_break_class(
    uint32_t scalar, salts_unicode_word_break *out_class);

/**
 * Advance one Unicode 17.0.0 default word-boundary segment (UAX #29).
 *
 * This is boundary segmentation, not lexical tokenization: whitespace and
 * punctuation can be returned as segments. On success, *cursor advances to the
 * exclusive end byte offset and *segment receives a borrowed subview of input.
 * At end or on error both outputs remain unchanged. No locale tailoring,
 * normalization, allocation, or source-lifetime extension is performed.
 */
salts_unicode_status salts_unicode_word_next(vstr input, size_t *cursor,
                                             vstr *segment);

/**
 * Move backward one Unicode 17.0.0 default word-boundary segment (UAX #29).
 *
 * The input cursor must be 0, input.len, or a default word boundary. On success
 * it moves to the segment start and *segment receives a borrowed input view.
 * At start or on error both outputs remain unchanged.
 */
salts_unicode_status salts_unicode_word_prev(vstr input, size_t *cursor,
                                             vstr *segment);

/**
 * Query the raw Unicode 17.0.0 Line_Break value for one scalar.
 */
salts_unicode_status salts_unicode_line_break_class(
    uint32_t scalar, salts_unicode_line_break *out_class);

/**
 * Find the next default Unicode 17.0.0 line-break opportunity (UAX #14).
 *
 * On success, *cursor and *break_offset receive the same exclusive byte
 * boundary and *opportunity reports whether the break is soft or mandatory.
 * At end or on error all outputs remain unchanged. The complete input is
 * validated before success is reported.
 */
salts_unicode_status salts_unicode_line_break_next(
    vstr input, size_t *cursor, size_t *break_offset,
    salts_unicode_line_break_opportunity *opportunity);


/** Query the Unicode 17.0.0 Bidi_Class value for one scalar. */
salts_unicode_status salts_unicode_bidi_class_of(
    uint32_t scalar, salts_unicode_bidi_class *out_class);

/**
 * Determine the default UAX #9 P2/P3 paragraph embedding level.
 *
 * Isolate contents are ignored while searching for the first strong type.
 * Returns level 0 for LTR/no-strong paragraphs and level 1 for R/AL.
 * The complete input is UTF-8 validated before success is reported.
 */
salts_unicode_status salts_unicode_bidi_paragraph_level(
    vstr input, uint8_t *out_level);

/**
 * Query Unicode 17.0.0 properties for one scalar value.
 *
 * @param scalar Unicode scalar value (surrogates are rejected).
 * @param out_properties Receives `salts_unicode_property` flags.
 * @return `SALTS_UNICODE_OK` or `SALTS_UNICODE_ERR_INVALID_ARGUMENT`.
 */
salts_unicode_status salts_unicode_scalar_properties(uint32_t scalar, uint32_t *out_properties);

/**
 * Query whether one Unicode 17.0.0 scalar belongs to General_Category P*
 * (punctuation) or S* (symbol). This is a category query, NOT Unicode
 * White_Space and not a locale-sensitive C punctuation predicate.
 *
 * The canonical pinned Unicode 17 re2c category table is the sole source.
 * The function is allocation-free, reentrant, and does not modify existing
 * salts_unicode_scalar layout, property flags or case transformation modes.
 * A valid scalar returns OK and sets *out_result to 0/1; a surrogate,
 * out-of-range scalar or NULL output returns INVALID_ARGUMENT and leaves
 * the supplied output unchanged.
 */
salts_unicode_status salts_unicode_is_punctuation_or_symbol(
    uint32_t scalar, int *out_result);

/**
 * Query the decimal value of a Unicode 17.0.0 Nd scalar, without allocation.
 *
 * @param scalar Unicode scalar value; surrogates and values above U+10FFFF are invalid.
 * @param out_value Receives 0 through 9 on success; unchanged on all other results.
 * @return OK for Nd, NO_MATCH for other scalars, ERR_INVALID_ARGUMENT for an
 *         invalid scalar or NULL output. No shared mutable state is accessed.
 * Example: salts_unicode_decimal_value(0x0662u, &value) writes 2 and returns OK.
 */
salts_unicode_status salts_unicode_decimal_value(uint32_t scalar, uint32_t *out_value);

/**
 * Resolve a Unicode 17 character name or name alias to one scalar.
 *
 * @param name Borrowed explicit-length ASCII name. Spaces and hyphens are exact;
 * ordinary names/aliases ignore ASCII case. Hangul syllable and CJK unified
 * ideograph names require uppercase. Named sequences are not accepted.
 * @param out_scalar Receives the scalar on OK, unchanged otherwise.
 * @return OK, NO_MATCH for unknown/malformed names, or ERR_INVALID_ARGUMENT for
 * NULL output or a NULL input pointer with nonzero length. Allocation-free and
 * safe for concurrent calls; no storage is retained from name.
 * Example: salts_unicode_name_lookup(vstr_from_cstr("LF"), &scalar) writes 10.
 */
salts_unicode_status salts_unicode_name_lookup(vstr name, uint32_t *out_scalar);

/**
 * Trim Unicode White_Space scalars from both ends of a UTF-8 view.
 *
 * The complete input is validated before success is reported. `*output` is a
 * borrowed subview of `input`, so it is invalidated when the input storage is
 * mutated or released. Embedded NUL bytes are preserved. No allocation occurs.
 *
 * @param input Borrowed UTF-8 byte view.
 * @param output Receives the trimmed borrowed view; unchanged on error.
 * @return `SALTS_UNICODE_OK` or a negative error code.
 */
salts_unicode_status salts_unicode_trim_whitespace(vstr input, vstr *output);

/** Transform UTF-8 using Unicode 17.0.0 default full case mappings. The input
 * is fully validated before callbacks begin. Mappings can expand a scalar;
 * lowercase applies Greek final sigma, while title maps every scalar's default
 * titlecase mapping. Locale-specific mappings are excluded. Callback chunks
 * borrow valid UTF-8 and a nonzero return stops with ERR_CALLBACK. */
salts_unicode_status salts_unicode_case_transform(vstr input, salts_unicode_case_mode mode,
                                                  salts_unicode_case_write write, void *userdata);

/**
 * Scan one Unicode XID identifier starting at a byte offset.
 *
 * The first scalar must have XID_Start and subsequent scalars must have
 * XID_Continue. Language-specific additions such as `_` are intentionally not
 * included. On success `*end` is the exclusive byte offset. Outputs are
 * unchanged for `END`, `NO_MATCH`, and errors.
 *
 * @param input Borrowed UTF-8 byte view.
 * @param start Byte offset in the inclusive range `[0, input.len]`.
 * @param end Receives the exclusive byte offset on success.
 * @return `SALTS_UNICODE_OK`, `SALTS_UNICODE_END`,
 *         `SALTS_UNICODE_NO_MATCH`, or a negative error code.
 */
salts_unicode_status salts_unicode_xid_span(vstr input, size_t start, size_t *end);

/**
 * Validate that a complete UTF-8 view is one Unicode XID identifier.
 *
 * Empty input and syntactically non-XID input return `SALTS_UNICODE_OK` with
 * `*result == 0`. The complete byte view is always UTF-8 validated, including
 * bytes after a scalar that already made the XID predicate false. Output is
 * unchanged on error.
 *
 * @param input Borrowed UTF-8 byte view.
 * @param result Receives zero or one on success.
 * @return `SALTS_UNICODE_OK` or a negative error code.
 */
salts_unicode_status salts_unicode_is_xid(vstr input, int *result);

/** Return the immutable Unicode Character Database version string. */
const char *salts_unicode_version(void);

#ifdef __cplusplus
}
#endif

#endif
