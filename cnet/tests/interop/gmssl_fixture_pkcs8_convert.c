/*
 * Interop fixture helper only: translate a GmSSL-encrypted test SM2 PKCS#8
 * into standard unencrypted PKCS#8 for independently built Tongsuo.
 *
 * Tongsuo does not support the GmSSL-specific PKCS#8 PBKDF2 SM3 PRF, so the
 * PEM wrapper must be normalized; certificate/key algorithms stay unchanged.
 * Never ship or install this program in Salts SDK and never log private keys.
 */
#include <gmssl/sm2.h>
#include <stdio.h>
#include <string.h>

static void wipe(void *ptr, size_t size) {
  volatile unsigned char *p = (volatile unsigned char *)ptr;
  while (size--) *p++ = 0;
}

int main(int argc, char **argv) {
  SM2_KEY key;
  FILE *source = NULL;
  FILE *dest = NULL;
  int ok = 0;

  if (argc != 4) {
    fputs("usage: gmssl_fixture_pkcs8_convert input password output\n", stderr);
    return 2;
  }

  memset(&key, 0, sizeof(key));
  source = fopen(argv[1], "rb");
  if (source == NULL) goto cleanup;
  if (sm2_private_key_info_decrypt_from_pem(&key, argv[2], source) != 1)
    goto cleanup;
  fclose(source);
  source = NULL;

  dest = fopen(argv[3], "wb");
  if (dest == NULL) goto cleanup;
  if (sm2_private_key_info_to_pem(&key, dest) != 1) goto cleanup;
  if (fflush(dest) != 0 || ferror(dest)) goto cleanup;
  ok = 1;

cleanup:
  if (source != NULL) fclose(source);
  if (dest != NULL) fclose(dest);
  wipe(&key, sizeof(key));
  if (!ok) {
    remove(argv[3]);
    fputs("interop fixture SM2 PKCS8 conversion failed\n", stderr);
  }
  return ok ? 0 : 1;
}
