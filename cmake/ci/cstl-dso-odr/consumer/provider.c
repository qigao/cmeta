#include <cstl/detail/instance_meta.h>

/* This is a REAL CSTL public descriptor symbol, not a mock for ASan.
 * In static mode the linked archive must own a definition in this DSO;
 * in shared mode the DSO must borrow its one shared provider's definition.
 */
#if defined(__GNUC__)
#  define CSTL_PROBE_EXPORT __attribute__((visibility("default")))
#else
#  define CSTL_PROBE_EXPORT
#endif

CSTL_PROBE_EXPORT const cmeta_container_desc *
salts_test_cstl_bplus_descriptor(void) {
  return &stl_bplus_tree_container_desc;
}
