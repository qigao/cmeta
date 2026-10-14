#include "salts_unicode.h"
#include "tinytest.hpp"

spec("salts_unicode_header_cpp") {
  it("exposes a C-compatible public header") {
    salts_unicode_scalar scalar{};
    size_t cursor = 0u;

    check_equal(salts_unicode_utf8_next(vstr_from_cstr("A"), &cursor, &scalar),
                SALTS_UNICODE_OK);
    check_equal(scalar.value, static_cast<uint32_t>('A'));
    check_equal(salts_unicode_version(), SALTS_UNICODE_VERSION_STRING);
    uint32_t digit = UINT32_MAX;
    check_equal(salts_unicode_decimal_value(0x0662u, &digit), SALTS_UNICODE_OK);
    check_equal(digit, static_cast<uint32_t>(2));
    check_equal(salts_unicode_name_lookup(vstr_from_cstr("LF"), &digit), SALTS_UNICODE_OK);
    check_equal(digit, static_cast<uint32_t>(10));
    int punctuation_or_symbol = -1;
    check_equal(salts_unicode_is_punctuation_or_symbol(0x20acu, &punctuation_or_symbol),
                SALTS_UNICODE_OK);
    check_equal(punctuation_or_symbol, 1);
    check_equal(salts_unicode_is_punctuation_or_symbol(0x03b1u, &punctuation_or_symbol),
                SALTS_UNICODE_OK);
    check_equal(punctuation_or_symbol, 0);
    std::size_t folded_bytes = static_cast<std::size_t>(-1);
    check_equal(salts_unicode_case_fold(vstr_from_cstr("\xc3\x9f"), nullptr,
                                        0u, &folded_bytes), SALTS_UNICODE_OK);
    check_equal(folded_bytes, static_cast<std::size_t>(2));
    check_equal(static_cast<int>(SALTS_UNICODE_CASE_UPPER), 0);
    check_equal(static_cast<int>(SALTS_UNICODE_CASE_TITLE), 2);
  }
}
