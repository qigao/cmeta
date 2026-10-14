#include <salts_unicode.h>
#include <salts_idna.h>
#include <cstdint>
#include <cstring>

static_assert(SALTS_UNICODE_VERSION_MAJOR == 17, "Expected Unicode 17 SDK");

int main() {
  std::uint32_t properties = 0;
  if (salts_unicode_scalar_properties(0x41u, &properties) != SALTS_UNICODE_OK)
    return 1;
  if ((properties & SALTS_UNICODE_PROPERTY_XID_START) == 0u)
    return 2;
  salts_unicode_grapheme_break kind = SALTS_UNICODE_GRAPHEME_OTHER;
  if (salts_unicode_grapheme_break_class(0x41u, &kind) != SALTS_UNICODE_OK)
    return 3;
  char mapped[128], normalized[128], output[254]; std::uint32_t work[128]; std::size_t size = 0;
  salts_idna_workspace workspace{mapped, sizeof mapped, normalized, sizeof normalized, work, 128};
  if (salts_idna_to_ascii(vstr_from_cstr("fa\xc3\x9f.de"), SALTS_IDNA_UNICODE17_UTS46_35_STRICT,
      &workspace, output, sizeof output, &size) != SALTS_IDNA_OK) return 4;
  if (size != 13 || std::strcmp(output, "xn--fa-hia.de") != 0) return 5;
  char folded[8] = {0}; std::size_t fold_bytes = 0u;
  if (salts_unicode_case_fold(vstr_from_cstr("\xe1\xba\x9e"), folded, sizeof folded, &fold_bytes) != SALTS_UNICODE_OK) return 6;
  if (fold_bytes != 2u || std::memcmp(folded, "ss", 2u) != 0) return 7;
  return 0;
}
