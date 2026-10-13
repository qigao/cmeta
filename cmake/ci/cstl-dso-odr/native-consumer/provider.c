#include <cstl/detail/instance_meta.h>
#include <cmeta/type_identity.h>

/* A public runtime query, never a DLL data pointer in a static initializer.
 * Both Provider DSO instances must remain loaded for foreign borrows.
 */
#if defined(_WIN32)
#  define CSTL_NATIVE_EXPORT __declspec(dllexport)
#elif defined(__GNUC__) || defined(__clang__)
#  define CSTL_NATIVE_EXPORT __attribute__((visibility("default")))
#else
#  define CSTL_NATIVE_EXPORT
#endif

CSTL_NATIVE_EXPORT const cmeta_container_desc *
salts_test_cstl_bplus_descriptor(void) {
  return &stl_bplus_tree_container_desc;
}

CSTL_NATIVE_EXPORT const cmeta_generic_desc *
salts_test_cstl_bplus_generic(void) {
  return &stl_bplus_tree_generic_desc;
}

CSTL_NATIVE_EXPORT int
salts_test_cstl_bplus_semantic_equal(const cmeta_generic_desc *foreign) {
  return cmeta_generic_desc_equal(&stl_bplus_tree_generic_desc, foreign) ? 1 : 0;
}
