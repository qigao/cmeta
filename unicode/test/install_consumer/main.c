#include <salts_unicode.h>
#include <salts_idna.h>
#include <string.h>

int main(void) {
  const char input_bytes[] = "\xf0\x9f\x99\x82";
  const vstr input = vstr_from_buf(input_bytes, sizeof(input_bytes) - 1u);
  salts_unicode_scalar scalar = {0};
  size_t cursor = 0u;
  if (strcmp(salts_unicode_version(), "17.0.0") != 0) return 1;
  if (salts_unicode_utf8_next(input, &cursor, &scalar) != SALTS_UNICODE_OK) return 2;
  if (scalar.value != 0x1f642u || cursor != sizeof(input_bytes) - 1u) return 3;
  if (salts_unicode_utf8_next(input, &cursor, &scalar) != SALTS_UNICODE_END) return 4;
  char mapped[128], normalized[128], output[254]; uint32_t work[128]; size_t size = 0;
  salts_idna_workspace workspace = {mapped, sizeof mapped, normalized, sizeof normalized, work, 128};
  if (salts_idna_to_ascii(vstr_from_cstr("B\xc3\xbc" "cher.de"),
      SALTS_IDNA_UNICODE17_UTS46_35_STRICT, &workspace, output, sizeof output, &size) != SALTS_IDNA_OK) return 5;
  if (size != 16 || strcmp(output, "xn--bcher-kva.de") != 0) return 6;
  if (salts_unicode_nfc(vstr_from_cstr("e\xcc\x81"), SALTS_UNICODE_NFC_17_0_0,
      work, 128, output, sizeof output, &size) != SALTS_UNICODE_NFC_OK) return 7;
  if (size != 2 || memcmp(output, "\xc3\xa9", 3) != 0) return 8;
  size_t fold_bytes = 0u;
  char folded[8] = {0};
  if (salts_unicode_case_fold(vstr_from_cstr("\xc3\x9f"), folded, sizeof folded, &fold_bytes) != SALTS_UNICODE_OK) return 9;
  if (fold_bytes != 2u || memcmp(folded, "ss", 2u) != 0) return 10;
  return 0;
}
