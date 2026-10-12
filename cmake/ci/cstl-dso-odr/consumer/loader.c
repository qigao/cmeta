/* Compare two *real* Provider DSO CMeta generic identities.
 *
 * The borrowed pointers are never dereferenced after their supplying
 * Provider image is closed. This does not change or fake the public ABI.
 */
#define _POSIX_C_SOURCE 200809L
#include <dlfcn.h>
#include <stdio.h>

typedef const void *(*cstl_descriptor_getter)(void);
typedef int (*cstl_generic_equal)(const void *);

static void *load_checked(const char *path, int flags, const char *label) {
  void *library = dlopen(path, flags);
  if (!library)
    fprintf(stderr, "%s_LOAD_FAILED: %s\n", label, dlerror());
  return library;
}

static void *symbol_checked(void *library, const char *name) {
  const char *error;
  void *symbol;
  dlerror();
  symbol = dlsym(library, name);
  error = dlerror();
  if (error || !symbol)
    fprintf(stderr, "CSTL_SYMBOL_MISSING=%s: %s\n",
            name, error ? error : "null");
  return symbol;
}

int main(int argc, char **argv) {
  void *core, *one, *two;
  cstl_descriptor_getter container_one, generic_one, generic_two;
  cstl_generic_equal equal_one, equal_two;
  const void *container, *first, *second;
  int same_address;

  if (argc != 4) {
    fputs("usage: cstl_loader <libsalts.so> <provider-one.so> "
          "<provider-two.so>\n", stderr);
    return 2;
  }
  core = load_checked(argv[1], RTLD_NOW | RTLD_GLOBAL, "CSTL_CORE");
  if (!core) return 3;
  one = load_checked(argv[2], RTLD_NOW | RTLD_LOCAL, "CSTL_ONE");
  if (!one) return 4;
  two = load_checked(argv[3], RTLD_NOW | RTLD_LOCAL, "CSTL_TWO");
  if (!two) return 5;

  *(void **)(&container_one) =
      symbol_checked(one, "salts_test_cstl_bplus_descriptor");
  *(void **)(&generic_one) =
      symbol_checked(one, "salts_test_cstl_bplus_generic");
  *(void **)(&generic_two) =
      symbol_checked(two, "salts_test_cstl_bplus_generic");
  *(void **)(&equal_one) =
      symbol_checked(one, "salts_test_cstl_bplus_semantic_equal");
  *(void **)(&equal_two) =
      symbol_checked(two, "salts_test_cstl_bplus_semantic_equal");
  if (!container_one || !generic_one || !generic_two ||
      !equal_one || !equal_two)
    return 6;
  container = container_one();
  first = generic_one();
  second = generic_two();
  if (!container || !first || !second)
    return 7;
  same_address = first == second;

  /* The public CMeta stable-ID comparison must accept two descriptors
   * from independent image-local providers, not require pointer identity.
   * Both image leases are live for both directions of the comparison. */
  if (equal_one(second) != 1 || equal_two(first) != 1)
    return 8;
  printf("CSTL_GENERIC_SEMANTIC_EQUAL=1\n");
  printf("CSTL_GENERIC_POINTER_EQUAL=%d\n", same_address);
  fflush(stdout);

  /* End Provider TWO's lifetime first, without touching its old borrowed
   * descriptor bytes. Provider ONE still owns its metadata and code. */
  if (dlclose(two) != 0) {
    fputs("CSTL_TWO_CLOSE_FAILED\n", stderr);
    return 9;
  }
  if (equal_one(first) != 1 || container_one() != container) {
    fputs("CSTL_LIVE_OWNER_BROKEN_AFTER_OTHER_DSO_CLOSE\n", stderr);
    return 10;
  }
  puts("CSTL_LIVE_PROVIDER_SURVIVED_OTHER_UNLOAD");
  if (dlclose(one) != 0 || dlclose(core) != 0) {
    fputs("CSTL_FINAL_CLOSE_FAILED\n", stderr);
    return 11;
  }
  puts("CSTL_DSO_LOAD_UNLOAD_OK");
  return 0;
}
