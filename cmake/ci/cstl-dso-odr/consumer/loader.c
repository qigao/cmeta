/* Load the *installed* Salts Core and an independent CSTL-linked DSO.
 * ASan ODR detection is deliberately enabled; do not suppress it.
 */
#define _POSIX_C_SOURCE 200809L
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>

typedef const void *(*cstl_descriptor_getter)(void);

int main(int argc, char **argv) {
  void *core = NULL;
  void *provider = NULL;
  cstl_descriptor_getter descriptor = NULL;
  const void *value = NULL;
  const char *error = NULL;
  if (argc != 3) {
    fputs("usage: cstl_loader <exact-libsalts.so> <provider.so>\n", stderr);
    return 2;
  }
  core = dlopen(argv[1], RTLD_NOW | RTLD_GLOBAL);
  if (core == NULL) {
    fprintf(stderr, "CSTL_CORE_LOAD_FAILED: %s\n", dlerror());
    return 3;
  }
  provider = dlopen(argv[2], RTLD_NOW | RTLD_LOCAL);
  if (provider == NULL) {
    fprintf(stderr, "CSTL_PROVIDER_LOAD_FAILED: %s\n", dlerror());
    (void)dlclose(core);
    return 4;
  }
  dlerror();
  *(void **)(&descriptor) = dlsym(provider, "salts_test_cstl_bplus_descriptor");
  error = dlerror();
  if (error != NULL || descriptor == NULL) {
    fprintf(stderr, "CSTL_GETTER_MISSING: %s\n",
            error == NULL ? "null result" : error);
    return 5;
  }
  value = descriptor();
  if (value == NULL) {
    fputs("CSTL_DESCRIPTOR_NULL\n", stderr);
    return 6;
  }
  printf("CSTL_DESCRIPTOR_AVAILABLE=%p\n", value);
  fflush(stdout);
  if (dlclose(provider) != 0 || dlclose(core) != 0) {
    fprintf(stderr, "CSTL_CLOSE_FAILED: %s\n", dlerror());
    return 7;
  }
  puts("CSTL_DSO_LOAD_UNLOAD_OK");
  return 0;
}
