#include <cmeta/type_identity.h>
#include <cstl/detail/instance_meta.h>
#include "shared_cstl_data_imports.h"
#include <type_traits>
#include <cstdint>

/* Independent installed-SDK C++17 consumer, not an in-tree header check.
 * The C ABI descriptor layout and public semantic identity stay unchanged.
 */
static_assert(std::is_standard_layout<cmeta_generic_desc>::value,
              "CMeta GenericDesc must retain a standard-layout public C ABI");
static_assert(sizeof(std::uint8_t) == 1, "8-bit CMeta arity ABI required");

int main() {
  const cmeta_generic_desc *provider = &stl_bplus_tree_generic_desc;
  return cmeta_generic_desc_valid(provider) &&
                 cmeta_generic_desc_equal(provider, provider)
             ? 0 : 1;
}
