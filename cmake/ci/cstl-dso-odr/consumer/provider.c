#include <cstl/detail/instance_meta.h>
#include <cmeta/type_identity.h>

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

/* A ContainerDesc is not a semantic type identity. The stable generic
 * constructor ID is public CMeta's cross-DSO equality contract.
 * Every caller must retain the foreign descriptor owner's DSO until after
 * this function returns; borrowed metadata is never copied or persisted.
 */
CSTL_PROBE_EXPORT const cmeta_generic_desc *
salts_test_cstl_bplus_generic(void) {
  return &stl_bplus_tree_generic_desc;
}

CSTL_PROBE_EXPORT int salts_test_cstl_bplus_semantic_equal(
    const cmeta_generic_desc *other) {
  return cmeta_generic_desc_equal(&stl_bplus_tree_generic_desc, other) ? 1 : 0;
}
