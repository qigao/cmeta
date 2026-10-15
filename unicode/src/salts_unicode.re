// re2c --lang c --utf8 --encoding-policy fail
#include "salts_unicode.h"
#include "salts_unicode_emoji.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/*!include:re2c "unicode_properties.re" */
/*!include:re2c "unicode_categories.re" */

#define SALTS_UNICODE_MAX_UTF8_BYTES 4u
#define SALTS_UNICODE_SCANNER_PADDING 4u
#define SALTS_UNICODE_INVALID_PADDING_BYTE 0xffu
#define SALTS_UNICODE_DECIMAL_RADIX 10u

static int salts_unicode_is_decimal(uint32_t scalar) {
  const uint32_t *YYCURSOR = &scalar;
  /*!re2c
    re2c:define:YYCTYPE = "uint32_t";
    re2c:encoding:utf32 = 1;
    re2c:yyfill:enable = 0;
    Nd { return 1; }
    * { return 0; }
  */
}

salts_unicode_status salts_unicode_decimal_value(uint32_t scalar, uint32_t *out_value) {
  uint32_t first = scalar;
  if (out_value == NULL || scalar > 0x10ffffu || (scalar >= 0xd800u && scalar <= 0xdfffu))
    return SALTS_UNICODE_ERR_INVALID_ARGUMENT;
  if (!salts_unicode_is_decimal(scalar)) return SALTS_UNICODE_NO_MATCH;
  /* Nd runs consist of ordered zero-to-nine sets. Adjacent sets (notably the
   * mathematical digits) require modulo ten. O(L) time, O(1) space; the pinned
   * Unicode 17 data has L <= 50. No second numeric property table is maintained. */
  while (first != 0u && salts_unicode_is_decimal(first - 1u)) --first;
  *out_value = (scalar - first) % SALTS_UNICODE_DECIMAL_RADIX;
  return SALTS_UNICODE_OK;
}

/* Unicode 17 General_Category P* / S* query from the existing pinned re2c
 * unicode_categories.re. No second range table, locale or mutable state. */
salts_unicode_status salts_unicode_is_punctuation_or_symbol(uint32_t scalar, int *out_result) {
  const uint32_t *YYCURSOR = &scalar;

  if (out_result == NULL || scalar > 0x10ffffu ||
      (scalar >= 0xd800u && scalar <= 0xdfffu))
    return SALTS_UNICODE_ERR_INVALID_ARGUMENT;

  /*!re2c
    re2c:define:YYCTYPE = "uint32_t";
    re2c:encoding:utf32 = 1;
    re2c:yyfill:enable = 0;
    P { *out_result = 1; return SALTS_UNICODE_OK; }
    S { *out_result = 1; return SALTS_UNICODE_OK; }
    * { *out_result = 0; return SALTS_UNICODE_OK; }
  */
}

static uint32_t salts_unicode_decode_scalar(const unsigned char *bytes, size_t length) {
  if (length == 1u) return bytes[0];
  if (length == 2u)
    return ((uint32_t)(bytes[0] & 0x1fu) << 6u) | (uint32_t)(bytes[1] & 0x3fu);
  if (length == 3u)
    return ((uint32_t)(bytes[0] & 0x0fu) << 12u) | ((uint32_t)(bytes[1] & 0x3fu) << 6u) |
           (uint32_t)(bytes[2] & 0x3fu);
  return ((uint32_t)(bytes[0] & 0x07u) << 18u) | ((uint32_t)(bytes[1] & 0x3fu) << 12u) |
         ((uint32_t)(bytes[2] & 0x3fu) << 6u) | (uint32_t)(bytes[3] & 0x3fu);
}

static salts_unicode_status salts_unicode_scan_scalar(vstr input, size_t offset,
                                                       salts_unicode_scalar *result) {
  unsigned char scratch[SALTS_UNICODE_MAX_UTF8_BYTES + SALTS_UNICODE_SCANNER_PADDING];
  size_t available = input.len - offset;
  size_t copied = available < SALTS_UNICODE_MAX_UTF8_BYTES ? available
                                                           : SALTS_UNICODE_MAX_UTF8_BYTES;
  const unsigned char *YYCURSOR;
  const unsigned char *YYMARKER;
  const unsigned char *token_start;
  uint32_t properties;

  memset(scratch, SALTS_UNICODE_INVALID_PADDING_BYTE, sizeof(scratch));
  memcpy(scratch, input.data + offset, copied);
  YYCURSOR = scratch;
  token_start = scratch;

  /*!re2c
    re2c:define:YYCTYPE = "unsigned char";
    re2c:encoding:utf8 = 1;
    re2c:encoding-policy = fail;
    re2c:yyfill:enable = 0;

    XID_Start {
      properties = SALTS_UNICODE_PROPERTY_XID_START |
                   SALTS_UNICODE_PROPERTY_XID_CONTINUE;
      goto accept;
    }
    XID_Continue {
      properties = SALTS_UNICODE_PROPERTY_XID_CONTINUE;
      goto accept;
    }
    White_Space {
      properties = SALTS_UNICODE_PROPERTY_WHITE_SPACE;
      goto accept;
    }
    [^] {
      properties = SALTS_UNICODE_PROPERTY_NONE;
      goto accept;
    }
    * { return SALTS_UNICODE_ERR_INVALID_UTF8; }
  */

accept: {
    size_t length = (size_t)(YYCURSOR - token_start);
    if (length > copied) return SALTS_UNICODE_ERR_INVALID_UTF8;
    result->value = salts_unicode_decode_scalar(token_start, length);
    properties |= salts_unicode_emoji_properties(result->value);
    result->properties = properties;
    result->byte_offset = offset;
    result->byte_length = length;
    return SALTS_UNICODE_OK;
  }
}

salts_unicode_status salts_unicode_utf8_next(vstr input, size_t *cursor,
                                              salts_unicode_scalar *out_scalar) {
  salts_unicode_scalar result;
  salts_unicode_status status;
  size_t offset;

  if (cursor == NULL || out_scalar == NULL || (input.data == NULL && input.len != 0u))
    return SALTS_UNICODE_ERR_INVALID_ARGUMENT;
  offset = *cursor;
  if (offset > input.len) return SALTS_UNICODE_ERR_INVALID_ARGUMENT;
  if (offset == input.len) return SALTS_UNICODE_END;

  status = salts_unicode_scan_scalar(input, offset, &result);
  if (status != SALTS_UNICODE_OK) return status;

  *out_scalar = result;
  *cursor = offset + result.byte_length;
  return SALTS_UNICODE_OK;
}

static size_t salts_unicode_encode_scalar(uint32_t scalar, unsigned char output[4]) {
  if (scalar <= 0x7fu) {
    output[0] = (unsigned char)scalar;
    return 1u;
  }
  if (scalar <= 0x7ffu) {
    output[0] = (unsigned char)(0xc0u | (scalar >> 6u));
    output[1] = (unsigned char)(0x80u | (scalar & 0x3fu));
    return 2u;
  }
  if (scalar <= 0xffffu) {
    output[0] = (unsigned char)(0xe0u | (scalar >> 12u));
    output[1] = (unsigned char)(0x80u | ((scalar >> 6u) & 0x3fu));
    output[2] = (unsigned char)(0x80u | (scalar & 0x3fu));
    return 3u;
  }
  output[0] = (unsigned char)(0xf0u | (scalar >> 18u));
  output[1] = (unsigned char)(0x80u | ((scalar >> 12u) & 0x3fu));
  output[2] = (unsigned char)(0x80u | ((scalar >> 6u) & 0x3fu));
  output[3] = (unsigned char)(0x80u | (scalar & 0x3fu));
  return 4u;
}

salts_unicode_status salts_unicode_scalar_properties(uint32_t scalar,
                                                      uint32_t *out_properties) {
  unsigned char encoded[4];
  salts_unicode_scalar result;
  size_t cursor = 0u;
  size_t length;
  salts_unicode_status status;

  if (out_properties == NULL || scalar > 0x10ffffu ||
      (scalar >= 0xd800u && scalar <= 0xdfffu))
    return SALTS_UNICODE_ERR_INVALID_ARGUMENT;

  length = salts_unicode_encode_scalar(scalar, encoded);
  status = salts_unicode_utf8_next(vstr_from_buf((const char *)encoded, length), &cursor, &result);
  if (status != SALTS_UNICODE_OK) return status;
  *out_properties = result.properties;
  return SALTS_UNICODE_OK;
}

salts_unicode_status salts_unicode_trim_whitespace(vstr input, vstr *output) {
  salts_unicode_scalar scalar;
  salts_unicode_status status;
  size_t cursor = 0u;
  size_t first_non_whitespace = 0u;
  size_t last_non_whitespace = 0u;
  int saw_non_whitespace = 0;
  vstr result;

  if (output == NULL || (input.data == NULL && input.len != 0u))
    return SALTS_UNICODE_ERR_INVALID_ARGUMENT;

  while (cursor < input.len) {
    status = salts_unicode_utf8_next(input, &cursor, &scalar);
    if (status != SALTS_UNICODE_OK) return status;
    if ((scalar.properties & SALTS_UNICODE_PROPERTY_WHITE_SPACE) == 0u) {
      if (!saw_non_whitespace) first_non_whitespace = scalar.byte_offset;
      last_non_whitespace = cursor;
      saw_non_whitespace = 1;
    }
  }

  if (saw_non_whitespace) {
    result = vstr_from_buf(input.data + first_non_whitespace,
                           last_non_whitespace - first_non_whitespace);
  } else if (input.data != NULL) {
    result = vstr_from_buf(input.data + input.len, 0u);
  } else {
    result = input;
  }
  *output = result;
  return SALTS_UNICODE_OK;
}

salts_unicode_status salts_unicode_xid_span(vstr input, size_t start, size_t *end) {
  salts_unicode_scalar scalar;
  salts_unicode_status status;
  size_t cursor;

  if (end == NULL || (input.data == NULL && input.len != 0u) || start > input.len)
    return SALTS_UNICODE_ERR_INVALID_ARGUMENT;
  if (start == input.len) return SALTS_UNICODE_END;

  cursor = start;
  status = salts_unicode_utf8_next(input, &cursor, &scalar);
  if (status != SALTS_UNICODE_OK) return status;
  if ((scalar.properties & SALTS_UNICODE_PROPERTY_XID_START) == 0u)
    return SALTS_UNICODE_NO_MATCH;

  while (cursor < input.len) {
    size_t candidate = cursor;
    status = salts_unicode_utf8_next(input, &candidate, &scalar);
    if (status != SALTS_UNICODE_OK) return status;
    if ((scalar.properties & SALTS_UNICODE_PROPERTY_XID_CONTINUE) == 0u) break;
    cursor = candidate;
  }

  *end = cursor;
  return SALTS_UNICODE_OK;
}

salts_unicode_status salts_unicode_is_xid(vstr input, int *result) {
  salts_unicode_scalar scalar;
  salts_unicode_status status;
  size_t cursor = 0u;
  size_t scalar_index = 0u;
  int is_xid = input.len != 0u;

  if (result == NULL || (input.data == NULL && input.len != 0u))
    return SALTS_UNICODE_ERR_INVALID_ARGUMENT;

  while (cursor < input.len) {
    status = salts_unicode_utf8_next(input, &cursor, &scalar);
    if (status != SALTS_UNICODE_OK) return status;
    if (scalar_index == 0u) {
      if ((scalar.properties & SALTS_UNICODE_PROPERTY_XID_START) == 0u) is_xid = 0;
    } else if ((scalar.properties & SALTS_UNICODE_PROPERTY_XID_CONTINUE) == 0u) {
      is_xid = 0;
    }
    ++scalar_index;
  }

  *result = is_xid;
  return SALTS_UNICODE_OK;
}

const char *salts_unicode_version(void) { return SALTS_UNICODE_VERSION_STRING; }
