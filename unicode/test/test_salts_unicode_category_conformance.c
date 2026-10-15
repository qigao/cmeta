#include "salts_unicode.h"
#include "tinytest.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef SALTS_UNICODE_CATEGORY_TEST_DATA
#error SALTS_UNICODE_CATEGORY_TEST_DATA must name pinned Unicode 17 UnicodeData.txt
#endif

#define CATEGORY_SCALAR_LIMIT 0x110000u
#define CATEGORY_LINE_BYTES 2048u

/* UnicodeData.txt fields: codepoint;name;General_Category;...
 * First/Last ranges are interpreted explicitly. P and S category mappings
 * are normally individual entries. All unlisted scalars have Cn and are 0.
 */
typedef struct parsed_category {
  uint32_t scalar;
  int punctuation_or_symbol;
  int range_first;
  int range_last;
} parsed_category;

static int parse_unicode_data(char *line, parsed_category *out) {
  char *first = strchr(line, ';');
  char *second;
  char *third;
  char *end = NULL;
  unsigned long scalar;
  if (first == NULL) return 0;
  second = strchr(first + 1, ';');
  if (second == NULL) return 0;
  third = strchr(second + 1, ';');
  if (third == NULL || third - second != 3) return 0;
  scalar = strtoul(line, &end, 16);
  if (end != first || scalar > 0x10fffful) return 0;
  out->scalar = (uint32_t)scalar;
  out->punctuation_or_symbol = second[1] == 'P' || second[1] == 'S';
  out->range_first = strstr(first + 1, ", First>") != NULL;
  out->range_last = strstr(first + 1, ", Last>") != NULL;
  return 1;
}

spec("salts_unicode Unicode 17 General_Category P and S conformance") {
  it("agrees with UnicodeData.txt for every valid scalar") {
    FILE *input = fopen(SALTS_UNICODE_CATEGORY_TEST_DATA, "rb");
    uint8_t *oracle = (uint8_t *)calloc(CATEGORY_SCALAR_LIMIT, 1u);
    char line[CATEGORY_LINE_BYTES];
    size_t lines_seen = 0u;
    size_t marked = 0u;
    uint32_t range_first = 0u;
    int range_ps = 0;
    int in_range = 0;
    int valid = input != NULL && oracle != NULL;
    if (valid) {
      while (fgets(line, sizeof(line), input) != NULL) {
        parsed_category current = {0};
        const size_t bytes = strlen(line);
        if (bytes == 0u || (line[bytes - 1u] != '\n' && !feof(input)) ||
            !parse_unicode_data(line, &current)) {
          valid = 0;
          break;
        }
        ++lines_seen;
        if (current.range_first) {
          if (in_range) { valid = 0; break; }
          in_range = 1;
          range_first = current.scalar;
          range_ps = current.punctuation_or_symbol;
        } else if (current.range_last) {
          if (!in_range || current.scalar < range_first ||
              current.punctuation_or_symbol != range_ps) {
            valid = 0;
            break;
          }
          if (range_ps) {
            memset(oracle + range_first, 1u,
                   (size_t)(current.scalar - range_first) + 1u);
            marked += (size_t)(current.scalar - range_first) + 1u;
          }
          in_range = 0;
        } else {
          if (in_range) { valid = 0; break; }
          oracle[current.scalar] = (uint8_t)current.punctuation_or_symbol;
          marked += (size_t)current.punctuation_or_symbol;
        }
      }
      if (ferror(input) || in_range) valid = 0;
    }
    if (input != NULL) fclose(input);
    check_true(valid);
    check_true(lines_seen > 30000u);
    check_true(marked > 6000u);

    if (valid) {
      for (uint32_t scalar = 0u; scalar < CATEGORY_SCALAR_LIMIT; ++scalar) {
        int result = -1;
        salts_unicode_status status;
        if (scalar >= 0xd800u && scalar <= 0xdfffu) continue;
        status = salts_unicode_is_punctuation_or_symbol(scalar, &result);
        if (status != SALTS_UNICODE_OK || result != oracle[scalar]) {
          fprintf(stderr, "Unicode 17 P/S discrepancy at U+%06X: %d vs %d (status %d)\n",
                  scalar, result, oracle[scalar], (int)status);
          valid = 0;
          break;
        }
      }
      check_true(valid);
    }
    free(oracle);
  }
}
