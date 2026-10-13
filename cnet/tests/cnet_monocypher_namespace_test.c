/* First-party ABI regression: the installed CNet DSO is public, but its
 * vendored Monocypher implementation is not a public CNet crypto provider.
 * Check real loader exports rather than relying only on compiler flags.
 */
#include <stdio.h>

#if defined(_WIN32)
#include <windows.h>
typedef HMODULE cnet_module_handle;
static cnet_module_handle cnet_open(const char *path) {
  return LoadLibraryA(path);
}
static int cnet_has_symbol(cnet_module_handle module, const char *name) {
  return GetProcAddress(module, name) != NULL;
}
static int cnet_close(cnet_module_handle module) {
  return FreeLibrary(module) != 0;
}
#else
#include <dlfcn.h>
typedef void *cnet_module_handle;
static cnet_module_handle cnet_open(const char *path) {
  return dlopen(path, RTLD_NOW | RTLD_LOCAL);
}
static int cnet_has_symbol(cnet_module_handle module, const char *name) {
  return dlsym(module, name) != NULL;
}
static int cnet_close(cnet_module_handle module) {
  return dlclose(module) == 0;
}
#endif

int main(int argc, char **argv) {
  static const char *const private_symbols[] = {
      "crypto_argon2_no_extras",
      "crypto_blake2b_keyed",
      "crypto_wipe",
      "salts_cnet_private_crypto_argon2_no_extras",
      "salts_cnet_private_crypto_blake2b_keyed",
      "salts_cnet_private_crypto_wipe",
  };
  cnet_module_handle module;
  unsigned i;
  int failed = 0;
  if (argc != 2 || argv[1] == NULL) {
    fprintf(stderr, "Expected an exact built CNet shared-library path\n");
    return 2;
  }
  module = cnet_open(argv[1]);
  if (module == NULL) {
    fprintf(stderr, "Could not load CNet DSO: %s\n", argv[1]);
    return 3;
  }
  if (!cnet_has_symbol(module, "cnet_client_init")) {
    fprintf(stderr, "Expected public CNet ABI export missing: cnet_client_init\n");
    failed = 1;
  }
  for (i = 0; i < sizeof(private_symbols) / sizeof(private_symbols[0]); ++i) {
    if (cnet_has_symbol(module, private_symbols[i])) {
      fprintf(stderr, "CNet private Monocypher export leaked: %s\n",
              private_symbols[i]);
      failed = 1;
    }
  }
  if (!cnet_close(module)) {
    fprintf(stderr, "Could not unload CNet DSO\n");
    failed = 1;
  }
  return failed;
}
