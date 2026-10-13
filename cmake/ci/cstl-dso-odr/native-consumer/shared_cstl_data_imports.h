#ifndef SALTS_CSTL_TEST_SHARED_DATA_IMPORTS_H
#define SALTS_CSTL_TEST_SHARED_DATA_IMPORTS_H

/*
 * Deliberately TEST-ONLY Windows import contract. The exact rc.15 installed
 * headers describe exported CSTL data with plain 'extern const' declarations,
 * which are not sufficient for MSVC to import data from a separate DLL.
 *
 * This file proves the necessary explicit data-import boundary without
 * silently changing the public SDK, ABI layout or released CMake targets.
 * A production shared-CSTL decision MUST migrate/qualify the official header
 * annotation rather than rely on this fixture header.
 */
#if defined(_WIN32) && defined(CSTL_NATIVE_SHARED_IMPORT)
#ifdef __cplusplus
extern "C" {
#endif
extern __declspec(dllimport) const cmeta_generic_desc
    stl_bplus_tree_generic_desc;
extern __declspec(dllimport) const cmeta_container_desc
    stl_bplus_tree_container_desc;
extern __declspec(dllimport) const cmeta_container_desc
    stl_vec_container_desc;
#ifdef __cplusplus
}
#endif
#endif

#endif /* SALTS_CSTL_TEST_SHARED_DATA_IMPORTS_H */
