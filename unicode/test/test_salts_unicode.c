#include "salts_unicode.h"
#include "tinytest.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

static void check_scalar(const unsigned char *bytes, size_t length, uint32_t value,
                         uint32_t properties) {
  salts_unicode_scalar scalar = {0};
  size_t cursor = 0u;
  salts_unicode_status status =
      salts_unicode_utf8_next(vstr_from_buf((const char *)bytes, length), &cursor, &scalar);

  check_equal(status, SALTS_UNICODE_OK);
  check_equal(scalar.value, value);
  check_equal(scalar.properties, properties);
  check_equal(scalar.byte_offset, 0u);
  check_equal(scalar.byte_length, length);
  check_equal(cursor, length);
}

typedef struct CASE_OUTPUT {
  char bytes[128];
  size_t length;
  size_t fail_after;
} CASE_OUTPUT;

static int case_write(const char *bytes, size_t length, void *opaque) {
  CASE_OUTPUT *output = (CASE_OUTPUT *)opaque;
  if (output == NULL || length > sizeof(output->bytes) - output->length ||
      output->length + length > output->fail_after) return -1;
  memcpy(output->bytes + output->length, bytes, length);
  output->length += length;
  return 0;
}

suite("salts_unicode") {
  it("resolves names aliases and derived character names") {
    static const struct { const char *name; uint32_t scalar; } cases[] = {
        {"LATIN CAPITAL LETTER A", 0x41u}, {"latin capital letter a", 0x41u},
        {"LF", 0xau}, {"line feed", 0xau}, {"BOM", 0xfeffu}, {"NULL", 0u},
        {"HANGUL SYLLABLE GA", 0xac00u}, {"HANGUL SYLLABLE HIH", 0xd7a3u},
        {"CJK UNIFIED IDEOGRAPH-4E00", 0x4e00u}, {"CJK UNIFIED IDEOGRAPH-04E00", 0x4e00u},
        {"cjk compatibility ideograph-fa0e", 0xfa0eu}, {"nushu character-1b170", 0x1b170u},
        {"TANGUT IDEOGRAPH-17000", 0x17000u}};
    for (size_t i = 0u; i < sizeof(cases) / sizeof(cases[0]); ++i) {
      uint32_t scalar = UINT32_MAX;
      check_equal(salts_unicode_name_lookup(vstr_from_cstr(cases[i].name), &scalar), SALTS_UNICODE_OK);
      check_equal(scalar, cases[i].scalar);
    }
  }
  it("rejects unknown names sequences and noncanonical spelling atomically") {
    static const char *names[] = {"", "UNKNOWN CHARACTER", "KEYCAP DIGIT ONE", "LATIN  CAPITAL LETTER A",
        "hangul syllable ga", "CJK UNIFIED IDEOGRAPH-4e00", "CJK UNIFIED IDEOGRAPH-110000",
        "CJK UNIFIED IDEOGRAPH-0041", "NUSHU CHARACTER-01B170"};
    uint32_t scalar = UINT32_MAX;
    for (size_t i = 0u; i < sizeof(names) / sizeof(names[0]); ++i) {
      check_equal(salts_unicode_name_lookup(vstr_from_cstr(names[i]), &scalar), SALTS_UNICODE_NO_MATCH);
      check_equal(scalar, UINT32_MAX);
    }
    const char exact[] = {'L','F'};
    check_equal(salts_unicode_name_lookup(vstr_from_buf(exact, sizeof(exact)), &scalar), SALTS_UNICODE_OK);
    check_equal(scalar, (uint32_t)10u);
    check_equal(salts_unicode_name_lookup(vstr_from_buf(exact, 1u), &scalar), SALTS_UNICODE_NO_MATCH);
    check_equal(scalar, (uint32_t)10u);
    const vstr invalid = {NULL, 1u};
    check_equal(salts_unicode_name_lookup(invalid, &scalar), SALTS_UNICODE_ERR_INVALID_ARGUMENT);
    check_equal(salts_unicode_name_lookup(vstr_from_cstr("LF"), NULL), SALTS_UNICODE_ERR_INVALID_ARGUMENT);
    const char embedded_nul[] = {'L', 'F', '\0'};
    check_equal(salts_unicode_name_lookup(vstr_from_buf(embedded_nul, sizeof(embedded_nul)), &scalar), SALTS_UNICODE_NO_MATCH);
    check_equal(salts_unicode_name_lookup(vstr_from_cstr("\u4e00"), &scalar), SALTS_UNICODE_NO_MATCH);
    const vstr oversized = {exact, SIZE_MAX};
    check_equal(salts_unicode_name_lookup(oversized, &scalar), SALTS_UNICODE_NO_MATCH);
    check_equal(scalar, (uint32_t)10u);
  }
  it("maps decimal digits across scripts and adjacent mathematical digit sets") {
    static const uint32_t zeros[] = {0x30u, 0x660u, 0x6f0u, 0xff10u, 0x104a0u,
        0x116d0u, 0x116dau, 0x1d7ceu, 0x1d7d8u, 0x1d7e2u, 0x1d7ecu, 0x1d7f6u, 0x11de0u};
    for (size_t i = 0u; i < sizeof(zeros) / sizeof(zeros[0]); ++i) {
      for (uint32_t digit = 0u; digit < 10u; ++digit) {
        uint32_t value = UINT32_MAX;
        check_equal(salts_unicode_decimal_value(zeros[i] + digit, &value), SALTS_UNICODE_OK);
        check_equal(value, digit);
      }
    }
  }
  it("distinguishes nondecimal scalars from invalid arguments without changing output") {
    static const uint32_t others[] = {0u, 0x2fu, 0x3au, 0xb2u, 0x2167u, 0x3007u, 0x10ffffu};
    uint32_t value = UINT32_MAX;
    for (size_t i = 0u; i < sizeof(others) / sizeof(others[0]); ++i) {
      check_equal(salts_unicode_decimal_value(others[i], &value), SALTS_UNICODE_NO_MATCH);
      check_equal(value, UINT32_MAX);
    }
    check_equal(salts_unicode_decimal_value(0xd800u, &value), SALTS_UNICODE_ERR_INVALID_ARGUMENT);
    check_equal(salts_unicode_decimal_value(0xdfffu, &value), SALTS_UNICODE_ERR_INVALID_ARGUMENT);
    check_equal(salts_unicode_decimal_value(0x110000u, &value), SALTS_UNICODE_ERR_INVALID_ARGUMENT);
    check_equal(salts_unicode_decimal_value(UINT32_MAX, &value), SALTS_UNICODE_ERR_INVALID_ARGUMENT);
    check_equal(salts_unicode_decimal_value('0', NULL), SALTS_UNICODE_ERR_INVALID_ARGUMENT);
    check_equal(value, UINT32_MAX);
  }
  it("reports the pinned Unicode data version") {
    check_equal(SALTS_UNICODE_VERSION_MAJOR, 17);
    check_equal(SALTS_UNICODE_VERSION_MINOR, 0);
    check_equal(SALTS_UNICODE_VERSION_PATCH, 0);
    check_equal(salts_unicode_version(), SALTS_UNICODE_VERSION_STRING);
  }

  it("transforms full case mappings and validates input before writing") {
    CASE_OUTPUT output = {.fail_after = sizeof(output.bytes)};
    const char *upper = "STRASSE FFI İ ΣΣ ΣA Σ\xcc\x81";
    const char *lower = "straße ﬃ i̇ ος οσα ος́ σa";
    check_equal(salts_unicode_case_transform(vstr_from_cstr("Straße ﬃ İ ΣΣ ΣA Σ\xcc\x81"),
        SALTS_UNICODE_CASE_UPPER, case_write, &output), SALTS_UNICODE_OK);
    check_equal(output.length, strlen(upper));
    check_equal(memcmp(output.bytes, upper, output.length), 0);
    output = (CASE_OUTPUT){.fail_after = sizeof(output.bytes)};
    check_equal(salts_unicode_case_transform(vstr_from_cstr("Straße ﬃ İ ΟΣ ΟΣΑ ΟΣ́ ΣA"),
        SALTS_UNICODE_CASE_LOWER, case_write, &output), SALTS_UNICODE_OK);
    check_equal(output.length, strlen(lower));
    check_equal(memcmp(output.bytes, lower, output.length), 0);
    output = (CASE_OUTPUT){.fail_after = sizeof(output.bytes)};
    check_equal(salts_unicode_case_transform(vstr_from_cstr("\xc7\xb3\xc3\x9f"),
        SALTS_UNICODE_CASE_TITLE, case_write, &output), SALTS_UNICODE_OK);
    check_equal(output.length, sizeof("\xc7\xb2Ss") - 1u);
    check_equal(memcmp(output.bytes, "\xc7\xb2Ss", output.length), 0);
    static const unsigned char invalid[] = {'A', 0xffu};
    output = (CASE_OUTPUT){.fail_after = sizeof(output.bytes)};
    check_equal(salts_unicode_case_transform(vstr_from_buf((const char *)invalid, sizeof(invalid)),
        SALTS_UNICODE_CASE_LOWER, case_write, &output), SALTS_UNICODE_ERR_INVALID_UTF8);
    check_equal(output.length, 0u);
    output = (CASE_OUTPUT){.fail_after = 1u};
    check_equal(salts_unicode_case_transform(vstr_from_cstr("ß"), SALTS_UNICODE_CASE_UPPER,
        case_write, &output), SALTS_UNICODE_ERR_CALLBACK);
    check_equal(salts_unicode_case_transform(vstr_from_cstr("x"),
        (salts_unicode_case_mode)99, case_write, &output), SALTS_UNICODE_ERR_INVALID_ARGUMENT);
  }

  it("classifies ASCII identifier characters and embedded NUL") {
    static const unsigned char upper[] = {'A'};
    static const unsigned char underscore[] = {'_'};
    static const unsigned char digit[] = {'7'};
    static const unsigned char nul[] = {0x00};
    const uint32_t identifier =
        SALTS_UNICODE_PROPERTY_XID_START | SALTS_UNICODE_PROPERTY_XID_CONTINUE;

    check_scalar(upper, sizeof(upper), 'A', identifier);
    check_scalar(underscore, sizeof(underscore), '_', SALTS_UNICODE_PROPERTY_XID_CONTINUE);
    check_scalar(digit, sizeof(digit), '7', SALTS_UNICODE_PROPERTY_XID_CONTINUE |
        SALTS_UNICODE_PROPERTY_EMOJI | SALTS_UNICODE_PROPERTY_EMOJI_COMPONENT);
    check_scalar(nul, sizeof(nul), 0u, SALTS_UNICODE_PROPERTY_NONE);
  }

  it("classifies BMP and non-BMP identifier scalars") {
    static const unsigned char greek_alpha[] = {0xce, 0xb1};
    static const unsigned char han_user[] = {0xe7, 0x94, 0xa8};
    static const unsigned char deseret_letter[] = {0xf0, 0x90, 0x90, 0x80};
    static const unsigned char combining_acute[] = {0xcc, 0x81};
    const uint32_t identifier =
        SALTS_UNICODE_PROPERTY_XID_START | SALTS_UNICODE_PROPERTY_XID_CONTINUE;

    check_scalar(greek_alpha, sizeof(greek_alpha), 0x03b1u, identifier);
    check_scalar(han_user, sizeof(han_user), 0x7528u, identifier);
    check_scalar(deseret_letter, sizeof(deseret_letter), 0x10400u, identifier);
    check_scalar(combining_acute, sizeof(combining_acute), 0x0301u,
                 SALTS_UNICODE_PROPERTY_XID_CONTINUE);
  }

  it("classifies Unicode whitespace without treating zero-width space as whitespace") {
    static const unsigned char nbsp[] = {0xc2, 0xa0};
    static const unsigned char em_space[] = {0xe2, 0x80, 0x83};
    static const unsigned char line_separator[] = {0xe2, 0x80, 0xa8};
    static const unsigned char zero_width_space[] = {0xe2, 0x80, 0x8b};

    check_scalar(nbsp, sizeof(nbsp), 0x00a0u, SALTS_UNICODE_PROPERTY_WHITE_SPACE);
    check_scalar(em_space, sizeof(em_space), 0x2003u, SALTS_UNICODE_PROPERTY_WHITE_SPACE);
    check_scalar(line_separator, sizeof(line_separator), 0x2028u,
                 SALTS_UNICODE_PROPERTY_WHITE_SPACE);
    check_scalar(zero_width_space, sizeof(zero_width_space), 0x200bu,
                 SALTS_UNICODE_PROPERTY_NONE);
  }

  it("classifies other valid scalars") {
    static const unsigned char emoji[] = {0xf0, 0x9f, 0x98, 0x80};
    check_scalar(emoji, sizeof(emoji), 0x1f600u, SALTS_UNICODE_PROPERTY_EMOJI |
        SALTS_UNICODE_PROPERTY_EMOJI_PRESENTATION | SALTS_UNICODE_PROPERTY_EXTENDED_PICTOGRAPHIC);
  }

  it("advances a byte cursor one scalar at a time") {
    static const unsigned char input[] = {'A', 0x00, 0xce, 0xb1, 0xf0, 0x9f, 0x98, 0x80};
    static const uint32_t values[] = {'A', 0u, 0x03b1u, 0x1f600u};
    static const size_t offsets[] = {0u, 1u, 2u, 4u};
    static const size_t lengths[] = {1u, 1u, 2u, 4u};
    vstr view = vstr_from_buf((const char *)input, sizeof(input));
    salts_unicode_scalar scalar = {0};
    size_t cursor = 0u;
    size_t index;

    for (index = 0u; index < sizeof(values) / sizeof(values[0]); ++index) {
      check_equal(salts_unicode_utf8_next(view, &cursor, &scalar), SALTS_UNICODE_OK);
      check_equal(scalar.value, values[index]);
      check_equal(scalar.byte_offset, offsets[index]);
      check_equal(scalar.byte_length, lengths[index]);
    }
    check_equal(cursor, sizeof(input));
    check_equal(salts_unicode_utf8_next(view, &cursor, &scalar), SALTS_UNICODE_END);
    check_equal(cursor, sizeof(input));
  }

  it("rejects malformed UTF-8 and preserves cursor and output") {
    static const unsigned char isolated_continuation[] = {0x80};
    static const unsigned char overlong[] = {0xc0, 0xaf};
    static const unsigned char surrogate[] = {0xed, 0xa0, 0x80};
    static const unsigned char above_max[] = {0xf4, 0x90, 0x80, 0x80};
    static const unsigned char truncated[] = {0xe2, 0x82};
    const struct {
      const unsigned char *bytes;
      size_t length;
    } cases[] = {{isolated_continuation, sizeof(isolated_continuation)},
                 {overlong, sizeof(overlong)},
                 {surrogate, sizeof(surrogate)},
                 {above_max, sizeof(above_max)},
                 {truncated, sizeof(truncated)}};
    size_t index;

    for (index = 0u; index < sizeof(cases) / sizeof(cases[0]); ++index) {
      salts_unicode_scalar scalar = {0xfeedu, 0xbeefu, 41u, 43u};
      salts_unicode_scalar before = scalar;
      size_t cursor = 0u;
      check_equal(salts_unicode_utf8_next(
                      vstr_from_buf((const char *)cases[index].bytes, cases[index].length),
                      &cursor, &scalar),
                  SALTS_UNICODE_ERR_INVALID_UTF8);
      check_equal(cursor, 0u);
      check_equal(memcmp(&scalar, &before, sizeof(scalar)), 0);
    }
  }

  it("rejects invalid arguments without mutating outputs") {
    static const unsigned char ascii[] = {'A'};
    vstr view = vstr_from_buf((const char *)ascii, sizeof(ascii));
    vstr invalid_view = {NULL, 1u};
    salts_unicode_scalar scalar = {0xfeedu, 0xbeefu, 41u, 43u};
    salts_unicode_scalar before = scalar;
    size_t cursor = 0u;
    size_t past_end = 2u;

    check_equal(salts_unicode_utf8_next(view, NULL, &scalar),
                SALTS_UNICODE_ERR_INVALID_ARGUMENT);
    check_equal(salts_unicode_utf8_next(view, &cursor, NULL),
                SALTS_UNICODE_ERR_INVALID_ARGUMENT);
    check_equal(salts_unicode_utf8_next(invalid_view, &cursor, &scalar),
                SALTS_UNICODE_ERR_INVALID_ARGUMENT);
    check_equal(salts_unicode_utf8_next(view, &past_end, &scalar),
                SALTS_UNICODE_ERR_INVALID_ARGUMENT);
    check_equal(cursor, 0u);
    check_equal(past_end, 2u);
    check_equal(memcmp(&scalar, &before, sizeof(scalar)), 0);
  }

  it("queries scalar properties and rejects non-scalars") {
    uint32_t properties = 0xfeedu;

    check_equal(salts_unicode_scalar_properties(0x03b1u, &properties), SALTS_UNICODE_OK);
    check_equal(properties,
                SALTS_UNICODE_PROPERTY_XID_START | SALTS_UNICODE_PROPERTY_XID_CONTINUE);
    check_equal(salts_unicode_scalar_properties(0x2003u, &properties), SALTS_UNICODE_OK);
    check_equal(properties, SALTS_UNICODE_PROPERTY_WHITE_SPACE);
    properties = 0xfeedu;
    check_equal(salts_unicode_scalar_properties(0xd800u, &properties),
                SALTS_UNICODE_ERR_INVALID_ARGUMENT);
    check_equal(properties, 0xfeedu);
    check_equal(salts_unicode_scalar_properties(0x110000u, &properties),
                SALTS_UNICODE_ERR_INVALID_ARGUMENT);
    check_equal(salts_unicode_scalar_properties('A', NULL),
                SALTS_UNICODE_ERR_INVALID_ARGUMENT);
  }

  it("queries Unicode 17 General_Category punctuation and symbols") {
    static const struct { uint32_t scalar; int expected; } cases[] = {
      {0x0024u, 1}, {0x00a3u, 1}, {0x20acu, 1}, /* Sc currency */
      {0x002eu, 1}, {0x2014u, 1}, {0xff1au, 1}, /* Po, Pd, Po */
      {0x005fu, 1}, {0x003du, 1}, {0x2603u, 1}, /* Pc, Sm, So */
      {0x1f600u, 1},                              /* So emoji */
      {0x0041u, 0}, {0x03b1u, 0}, {0x00a0u, 0},
      {0x0301u, 0}, {0x2003u, 0}, {0x10ffffu, 0}
    };
    for (size_t i = 0u; i < sizeof(cases) / sizeof(cases[0]); ++i) {
      int actual = -1;
      check_equal(salts_unicode_is_punctuation_or_symbol(cases[i].scalar, &actual),
                  SALTS_UNICODE_OK);
      check_equal(actual, cases[i].expected);
    }
    int unchanged = 0x5a5a;
    check_equal(salts_unicode_is_punctuation_or_symbol(0xd800u, &unchanged),
                SALTS_UNICODE_ERR_INVALID_ARGUMENT);
    check_equal(unchanged, 0x5a5a);
    check_equal(salts_unicode_is_punctuation_or_symbol(0xdfffu, &unchanged),
                SALTS_UNICODE_ERR_INVALID_ARGUMENT);
    check_equal(unchanged, 0x5a5a);
    check_equal(salts_unicode_is_punctuation_or_symbol(0x110000u, &unchanged),
                SALTS_UNICODE_ERR_INVALID_ARGUMENT);
    check_equal(unchanged, 0x5a5a);
    check_equal(salts_unicode_is_punctuation_or_symbol(UINT32_MAX, &unchanged),
                SALTS_UNICODE_ERR_INVALID_ARGUMENT);
    check_equal(unchanged, 0x5a5a);
    check_equal(salts_unicode_is_punctuation_or_symbol(0x20acu, NULL),
                SALTS_UNICODE_ERR_INVALID_ARGUMENT);
  }

  it("trims Unicode whitespace as a borrowed view") {
    static const unsigned char ascii[] = {'\t', 'x', '\r', '\n'};
    static const unsigned char padded[] = {0xc2, 0xa0, 'a', 0x00, 'b',
                                           0xe2, 0x80, 0x83};
    static const unsigned char internal[] = {'a', 0xc2, 0xa0, 'b'};
    static const unsigned char only_space[] = {0xc2, 0xa0, 0xe2, 0x80, 0x83};
    static const unsigned char malformed[] = {' ', 0x80, ' '};
    vstr output = {(const char *)0x1, 41u};
    vstr before = output;

    check_equal(salts_unicode_trim_whitespace(
                    vstr_from_buf((const char *)ascii, sizeof(ascii)), &output),
                SALTS_UNICODE_OK);
    check_equal((const void *)output.data, (const void *)(ascii + 1u));
    check_equal(output.len, 1u);

    check_equal(salts_unicode_trim_whitespace(
                    vstr_from_buf((const char *)padded, sizeof(padded)), &output),
                SALTS_UNICODE_OK);
    check_equal((const void *)output.data, (const void *)((const char *)padded + 2u));
    check_equal(output.len, 3u);
    check_equal(output.data, "a\0b", output.len);

    check_equal(salts_unicode_trim_whitespace(
                    vstr_from_buf((const char *)internal, sizeof(internal)), &output),
                SALTS_UNICODE_OK);
    check_equal((const void *)output.data, (const void *)internal);
    check_equal(output.len, sizeof(internal));

    check_equal(salts_unicode_trim_whitespace(
                    vstr_from_buf((const char *)only_space, sizeof(only_space)), &output),
                SALTS_UNICODE_OK);
    check_equal(output.len, 0u);

    output = before;
    check_equal(salts_unicode_trim_whitespace(
                    vstr_from_buf((const char *)malformed, sizeof(malformed)), &output),
                SALTS_UNICODE_ERR_INVALID_UTF8);
    check_equal((const void *)output.data, (const void *)before.data);
    check_equal(output.len, before.len);
    check_equal(salts_unicode_trim_whitespace(vstr_from_cstr("x"), NULL),
                SALTS_UNICODE_ERR_INVALID_ARGUMENT);
  }

  it("finds an XID span without consuming language punctuation") {
    static const unsigned char identifier[] = {0xce, 0xb1, 0xcc, 0x81, '7', '-'};
    static const unsigned char non_bmp[] = {0xf0, 0x90, 0x90, 0x80, '2'};
    static const unsigned char malformed[] = {'A', 0x80};
    size_t end = 41u;

    check_equal(salts_unicode_xid_span(
                    vstr_from_buf((const char *)identifier, sizeof(identifier)), 0u, &end),
                SALTS_UNICODE_OK);
    check_equal(end, 5u);
    check_equal(salts_unicode_xid_span(
                    vstr_from_buf((const char *)non_bmp, sizeof(non_bmp)), 0u, &end),
                SALTS_UNICODE_OK);
    check_equal(end, sizeof(non_bmp));

    end = 41u;
    check_equal(salts_unicode_xid_span(vstr_from_cstr("_name"), 0u, &end),
                SALTS_UNICODE_NO_MATCH);
    check_equal(end, 41u);
    check_equal(salts_unicode_xid_span(vstr_from_cstr("name"), 4u, &end),
                SALTS_UNICODE_END);
    check_equal(end, 41u);
    check_equal(salts_unicode_xid_span(
                    vstr_from_buf((const char *)malformed, sizeof(malformed)), 0u, &end),
                SALTS_UNICODE_ERR_INVALID_UTF8);
    check_equal(end, 41u);
    check_equal(salts_unicode_xid_span(
                    vstr_from_buf((const char *)identifier, sizeof(identifier)), 1u, &end),
                SALTS_UNICODE_ERR_INVALID_UTF8);
    check_equal(end, 41u);
    check_equal(salts_unicode_xid_span(vstr_from_cstr("name"), 5u, &end),
                SALTS_UNICODE_ERR_INVALID_ARGUMENT);
    check_equal(salts_unicode_xid_span(vstr_from_cstr("name"), 0u, NULL),
                SALTS_UNICODE_ERR_INVALID_ARGUMENT);
  }

  it("validates a complete XID string") {
    static const unsigned char identifier[] = {0xe7, 0x94, 0xa8, 0xcc, 0x81, '7'};
    static const unsigned char malformed[] = {'A', '!', 0x80};
    int result = 41;

    check_equal(salts_unicode_is_xid(
                    vstr_from_buf((const char *)identifier, sizeof(identifier)), &result),
                SALTS_UNICODE_OK);
    check_equal(result, 1);
    check_equal(salts_unicode_is_xid(vstr_from_cstr("_name"), &result),
                SALTS_UNICODE_OK);
    check_equal(result, 0);
    check_equal(salts_unicode_is_xid(vstr_from_cstr("alpha-beta"), &result),
                SALTS_UNICODE_OK);
    check_equal(result, 0);
    check_equal(salts_unicode_is_xid(vstr_from_buf(NULL, 0u), &result),
                SALTS_UNICODE_OK);
    check_equal(result, 0);

    result = 41;
    check_equal(salts_unicode_is_xid(
                    vstr_from_buf((const char *)malformed, sizeof(malformed)), &result),
                SALTS_UNICODE_ERR_INVALID_UTF8);
    check_equal(result, 41);
    check_equal(salts_unicode_is_xid(vstr_from_cstr("name"), NULL),
                SALTS_UNICODE_ERR_INVALID_ARGUMENT);
  }
}
