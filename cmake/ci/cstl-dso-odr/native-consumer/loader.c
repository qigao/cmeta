/* C11 load/unload consumer of two *separate* installed-SDK Provider images.
 * Do not dereference a borrowed descriptor after its owning DSO is closed.
 */
#include <cmeta/type_identity.h>
#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#  include <windows.h>
typedef HMODULE cstl_native_library;
static cstl_native_library cstl_open(const char *path, int global_scope) {
  (void)global_scope;
  return LoadLibraryA(path);
}
static void *cstl_lookup(cstl_native_library lib, const char *name) {
  return (void *)(uintptr_t)GetProcAddress(lib, name);
}
static int cstl_close(cstl_native_library lib) {
  return FreeLibrary(lib) != 0;
}
static unsigned long cstl_error(void) { return GetLastError(); }
#else
#  include <dlfcn.h>
typedef void *cstl_native_library;
static cstl_native_library cstl_open(const char *path, int global_scope) {
  return dlopen(path, RTLD_NOW | (global_scope ? RTLD_GLOBAL : RTLD_LOCAL));
}
static void *cstl_lookup(cstl_native_library lib, const char *name) {
  return dlsym(lib, name);
}
static int cstl_close(cstl_native_library lib) {
  return dlclose(lib) == 0;
}
static unsigned long cstl_error(void) { return 0UL; }
#endif

typedef const cmeta_generic_desc *(*cstl_generic_fn)(void);
typedef int (*cstl_semantic_fn)(const cmeta_generic_desc *);

int main(int argc, char **argv) {
  cstl_native_library core = NULL, first_lib = NULL, second_lib = NULL;
  cstl_generic_fn first_desc = NULL, second_desc = NULL;
  cstl_semantic_fn first_equal = NULL, second_equal = NULL;
  const cmeta_generic_desc *first, *second;
  int shared_mode, pointers_equal;

  if (argc != 5) {
    fputs("usage: cstl_native_loader <core> <provider-a> <provider-b> "
          "<hidden|shared>\n", stderr);
    return 2;
  }
  shared_mode = strcmp(argv[4], "shared") == 0;
  if (!shared_mode && strcmp(argv[4], "hidden") != 0) return 3;
  core = cstl_open(argv[1], 1);
  if (!core) {
    fprintf(stderr, "CSTL_CORE_LOAD_FAILED %lu\n", cstl_error());
    return 4;
  }
  first_lib = cstl_open(argv[2], 0);
  if (!first_lib) {
    fprintf(stderr, "CSTL_PROVIDER_A_LOAD_FAILED %lu\n", cstl_error());
    return 5;
  }
  second_lib = cstl_open(argv[3], 0);
  if (!second_lib) {
    fprintf(stderr, "CSTL_PROVIDER_B_LOAD_FAILED %lu\n", cstl_error());
    return 6;
  }
  /* These are *exported runtime functions*, never public DLL data imported
   * into static initializers (important for the Windows C11 consumer). */
#if defined(_WIN32)
  first_desc = (cstl_generic_fn)cstl_lookup(
      first_lib, "salts_test_cstl_bplus_generic");
  second_desc = (cstl_generic_fn)cstl_lookup(
      second_lib, "salts_test_cstl_bplus_generic");
  first_equal = (cstl_semantic_fn)cstl_lookup(
      first_lib, "salts_test_cstl_bplus_semantic_equal");
  second_equal = (cstl_semantic_fn)cstl_lookup(
      second_lib, "salts_test_cstl_bplus_semantic_equal");
#else
  *(void **)(&first_desc) = cstl_lookup(
      first_lib, "salts_test_cstl_bplus_generic");
  *(void **)(&second_desc) = cstl_lookup(
      second_lib, "salts_test_cstl_bplus_generic");
  *(void **)(&first_equal) = cstl_lookup(
      first_lib, "salts_test_cstl_bplus_semantic_equal");
  *(void **)(&second_equal) = cstl_lookup(
      second_lib, "salts_test_cstl_bplus_semantic_equal");
#endif
  if (!first_desc || !second_desc || !first_equal || !second_equal) {
    fputs("CSTL_PROVIDER_RUNTIME_EXPORT_MISSING\n", stderr);
    return 7;
  }
  first = first_desc();
  second = second_desc();
  if (!first || !second) return 8;
  if (first_equal(second) != 1 || second_equal(first) != 1) {
    fputs("CSTL_CROSS_PROVIDER_SEMANTIC_ID_MISMATCH\n", stderr);
    return 9;
  }
  pointers_equal = first == second;
  if (pointers_equal != shared_mode) {
    fprintf(stderr, "CSTL_OWNER_ALIAS_MISMATCH actual=%d expected=%d\n",
            pointers_equal, shared_mode);
    return 10;
  }
  printf("CSTL_GENERIC_SEMANTIC_EQUAL=1\n");
  printf("CSTL_GENERIC_POINTER_EQUAL=%d\n", pointers_equal);
  fflush(stdout);

  if (!cstl_close(second_lib)) {
    fputs("CSTL_PROVIDER_B_UNLOAD_FAILED\n", stderr);
    return 11;
  }
  /* We NEVER read 'second' again. The first Provider remains loaded and its
   * borrowed descriptor can still be used through its owning module. */
  if (first_desc() != first || first_equal(first) != 1) {
    fputs("CSTL_LIVE_PROVIDER_A_BORROW_FAILED\n", stderr);
    return 12;
  }
  puts("CSTL_LIVE_PROVIDER_SURVIVED_OTHER_UNLOAD");
  if (!cstl_close(first_lib) || !cstl_close(core)) {
    fputs("CSTL_FINAL_NATIVE_UNLOAD_FAILED\n", stderr);
    return 13;
  }
  puts("CSTL_NATIVE_DSO_LOAD_UNLOAD_OK");
  return 0;
}
