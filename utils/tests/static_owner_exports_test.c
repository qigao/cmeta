/* Guard the native public SDK from a regression to preemptible static
 * CSTL/CFlow reflection globals (#1167 / #1180). This is a *real dynamic
 * loader* test, not a compile flag assertion or a second ownership runtime.
 *
 * For static CSTL/CFlow, libsalts and libcnet must not export address-bearing
 * native descriptors. Public Salts/CNet functions remain exported. Explicit
 * BUILD_SHARED_LIBS=ON has a different, independently gated ABI policy.
 */
#include <stdio.h>

#if defined(_WIN32)
#include <windows.h>
typedef HMODULE image_handle;
static image_handle open_image(const char *path) {
  return LoadLibraryA(path);
}
static int has_symbol(image_handle image, const char *name) {
  return GetProcAddress(image, name) != NULL;
}
static int close_image(image_handle image) {
  return FreeLibrary(image) != 0;
}
#else
#include <dlfcn.h>
typedef void *image_handle;
static image_handle open_image(const char *path) {
  return dlopen(path, RTLD_NOW | RTLD_LOCAL);
}
static int has_symbol(image_handle image, const char *name) {
  return dlsym(image, name) != NULL;
}
static int close_image(image_handle image) {
  return dlclose(image) == 0;
}
#endif

static int audit(const char *path, const char *required_symbol) {
  static const char *const forbidden[] = {
      "stl_bplus_tree_container_desc",
      "stl_bplus_tree_generic_desc",
      "cflow_type_executor_protocol_stats_ptr",
      "crypto_ed25519_sign",
      "crypto_sha512",
      "crypto_x25519",
      "salts_core_private_crypto_ed25519_sign",
      "salts_core_private_crypto_sha512",
      "salts_core_private_crypto_x25519",
  };
  image_handle image = open_image(path);
  int failed = 0;
  unsigned i;
  if (image == NULL) {
    fprintf(stderr, "OWNER_PRIVATE_DSO_LOAD_FAILED: %s\n", path);
    return 1;
  }
  if (!has_symbol(image, required_symbol)) {
    fprintf(stderr, "OWNER_PRIVATE_PUBLIC_ABI_MISSING: %s in %s\n",
            required_symbol, path);
    failed = 1;
  }
  for (i = 0; i < sizeof(forbidden) / sizeof(forbidden[0]); ++i) {
    if (has_symbol(image, forbidden[i])) {
      fprintf(stderr, "OWNER_PRIVATE_REFLECTION_EXPORTED: %s in %s\n",
              forbidden[i], path);
      failed = 1;
    }
  }
  if (!close_image(image)) {
    fprintf(stderr, "OWNER_PRIVATE_DSO_CLOSE_FAILED: %s\n", path);
    failed = 1;
  }
  return failed;
}

int main(int argc, char **argv) {
  int failed;
  if (argc != 3 || argv[1] == NULL || argv[2] == NULL) {
    fputs("usage: owner_exports_test <libsalts> <libcnet>\n", stderr);
    return 2;
  }
  failed = audit(argv[1], "cmeta_log_str");
  failed |= audit(argv[2], "cnet_client_init");
  if (failed == 0) {
    puts("OWNER_PRIVATE_CSTL_CFLOW_DSO_EXPORTS_PASS");
  }
  return failed ? 1 : 0;
}
