/* Original public CNet DLL/DSO export contract gate: the embedded
 * Monocypher implementation is private, including all globals and functions.
 * Test both raw upstream and CNet-prefixed names against the real native
 * dynamic loader. This does not replace full-source ASan/UBSan cross-DSO tests.
 */
#include <stdio.h>
#include <string.h>

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
  /* Inventory is deliberately checked against the private namespace header.
   * Audit the 53 entries whenever the vendored Monocypher version changes. */
  static const char *const private_symbols[] = {
      "crypto_aead_ctx",
      "crypto_aead_init_djb",
      "crypto_aead_init_ietf",
      "crypto_aead_init_x",
      "crypto_aead_lock",
      "crypto_aead_read",
      "crypto_aead_unlock",
      "crypto_aead_write",
      "crypto_argon2",
      "crypto_argon2_config",
      "crypto_argon2_extras",
      "crypto_argon2_inputs",
      "crypto_argon2_no_extras",
      "crypto_blake2b",
      "crypto_blake2b_ctx",
      "crypto_blake2b_final",
      "crypto_blake2b_init",
      "crypto_blake2b_keyed",
      "crypto_blake2b_keyed_init",
      "crypto_blake2b_update",
      "crypto_chacha20_djb",
      "crypto_chacha20_h",
      "crypto_chacha20_ietf",
      "crypto_chacha20_x",
      "crypto_curve_to_hidden",
      "crypto_eddsa_check",
      "crypto_eddsa_check_equation",
      "crypto_eddsa_key_pair",
      "crypto_eddsa_mul_add",
      "crypto_eddsa_reduce",
      "crypto_eddsa_scalarbase",
      "crypto_eddsa_sign",
      "crypto_eddsa_to_x25519",
      "crypto_eddsa_trim_scalar",
      "crypto_elligator_key_pair",
      "crypto_elligator_map",
      "crypto_elligator_rev",
      "crypto_lock",
      "crypto_poly1305",
      "crypto_poly1305_ctx",
      "crypto_poly1305_final",
      "crypto_poly1305_init",
      "crypto_poly1305_update",
      "crypto_verify16",
      "crypto_verify32",
      "crypto_verify64",
      "crypto_wipe",
      "crypto_x25519",
      "crypto_x25519_dirty_fast",
      "crypto_x25519_dirty_small",
      "crypto_x25519_inverse",
      "crypto_x25519_public_key",
      "crypto_x25519_to_eddsa",
  };
  const size_t count = sizeof(private_symbols) / sizeof(private_symbols[0]);
  cnet_module_handle module;
  size_t i;
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
  if (cnet_has_symbol(module, "cnet_tls_diagnostic_send_stats")) {
    fprintf(stderr, "Test-only TLS diagnostic export leaked into production CNet\n");
    failed = 1;
  }
  for (i = 0; i < count; ++i) {
    char internal_name[128];
    int n;
    if (cnet_has_symbol(module, private_symbols[i])) {
      fprintf(stderr, "Upstream Monocypher export leaked: %s\n",
              private_symbols[i]);
      failed = 1;
    }
    n = snprintf(internal_name, sizeof(internal_name), "salts_cnet_private_%s",
                 private_symbols[i]);
    if (n < 0 || (size_t)n >= sizeof(internal_name)) {
      fprintf(stderr, "Private symbol name exceeded bound\n");
      failed = 1;
      continue;
    }
    if (cnet_has_symbol(module, internal_name)) {
      fprintf(stderr, "Namespaced private Monocypher export leaked: %s\n",
              internal_name);
      failed = 1;
    }
  }
  if (!cnet_close(module)) {
    fprintf(stderr, "Could not unload CNet DSO\n");
    failed = 1;
  }
  if (!failed) fprintf(stdout, "CNET_MONOCYPHER_PRIVATE_SYMBOLS=%zu\n", count);
  return failed;
}
