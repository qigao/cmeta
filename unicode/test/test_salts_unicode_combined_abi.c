#include "salts_unicode.h"
#include "tinytest.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* The two additive Unicode APIs coexist in one C11 Salts::Unicode SDK. */
spec("Unicode 17 combined P/S and C/F ABI proof") {
  it("links and exercises both providers without changing legacy modes") {
    int ps = -1;
    char output[8] = {0};
    size_t bytes = SIZE_MAX;
    check_equal(salts_unicode_is_punctuation_or_symbol(0x00a3u, &ps), SALTS_UNICODE_OK);
    check_equal(ps, 1);
    check_equal(salts_unicode_is_punctuation_or_symbol(0x00a0u, &ps), SALTS_UNICODE_OK);
    check_equal(ps, 0);
    check_equal(salts_unicode_case_fold(vstr_from_cstr("\xe1\xba\x9e"),
                                       NULL, 0u, &bytes), SALTS_UNICODE_OK);
    check_equal(bytes, (size_t)2);
    check_equal(salts_unicode_case_fold(vstr_from_cstr("\xe1\xba\x9e"),
                                       output, sizeof(output), &bytes), SALTS_UNICODE_OK);
    check_equal(bytes, (size_t)2);
    check_equal(memcmp(output, "ss", 2u), 0);
    check_equal((int)SALTS_UNICODE_ERR_CALLBACK, -3);
    check_equal((int)SALTS_UNICODE_ERR_CAPACITY, -4);
    uint32_t white_space = 0u;
    check_equal(salts_unicode_scalar_properties(0x00a0u, &white_space), SALTS_UNICODE_OK);
    check_true((white_space & SALTS_UNICODE_PROPERTY_WHITE_SPACE) != 0u);
  }
}
