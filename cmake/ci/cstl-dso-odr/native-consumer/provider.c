#include <cstl/detail/instance_meta.h>
#include "shared_cstl_data_imports.h"
#include <cmeta/type_identity.h>
#include <cmeta/range.h>
#include <cmeta/cmeta.h>
#include <cstl/vec.h>
#include <stdlib.h>
#include <string.h>

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

/* A concrete callback-bearing ContainerDesc. Its range factory, size,
 * version and next functions are owned by the Provider's CSTL image (or the
 * single shared CSTL image), not by the Loader.
 *
 * The Host only borrows these callbacks while this DSO remains loaded.
 * The Provider owns the Vec handle and releases it before native unload.
 */
static unsigned cstl_native_live_vectors = 0;

CSTL_NATIVE_EXPORT const cmeta_container_desc *
salts_test_cstl_vec_container(void) {
  return &stl_vec_container_desc;
}

CSTL_NATIVE_EXPORT void *salts_test_cstl_vec_create(void) {
  static const int values[3] = {4, 7, 9};
  vec_t *value = (vec_t *)calloc(1, sizeof(*value));
  if (!value) return NULL;
  if (vec_raw_from_array(value, values, 3, &cmeta_type_int, 8) != STL_OK) {
    free(value);
    return NULL;
  }
  ++cstl_native_live_vectors;
  return value;
}

CSTL_NATIVE_EXPORT int salts_test_cstl_vec_destroy(void *opaque) {
  vec_t *value = (vec_t *)opaque;
  if (!value || cstl_native_live_vectors == 0) return 0;
  vec_raw_destroy_storage(value);
  free(value);
  --cstl_native_live_vectors;
  return 1;
}

CSTL_NATIVE_EXPORT unsigned salts_test_cstl_vec_live_count(void) {
  return cstl_native_live_vectors;
}
