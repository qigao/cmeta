#include "cnet_tls.h"

#if defined(CNET_INTERNAL_TESTING)
  #include "cnet_test_internal.h"
#endif

#include <salts/error_codes.h>

#include <gmssl/tls.h>
#include <gmssl/digest.h>
#include <gmssl/oid.h>
#include <gmssl/x509_cer.h>
#include <gmssl/x509_ext.h>
#include <gmssl/asn1.h>
#include <gmssl/pem.h>

#include <limits.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
  #include <winsock2.h>
  #include <windows.h>
  #include <wincrypt.h>
  #include <ws2tcpip.h>
#elif defined(__APPLE__)
  #include <arpa/inet.h>
  #include <dirent.h>
  #include <TargetConditionals.h>
  #include <CoreFoundation/CoreFoundation.h>
  #include <Security/Security.h>
#else
  #include <arpa/inet.h>
  #include <dirent.h>
#endif

enum {
  CNET_TLS_PATH_MAX_BYTES = 4095,
  CNET_TLS_PASSWORD_MAX_BYTES = 1023,
  CNET_TLS_TRUST_MAX_BYTES = 16 * 1024 * 1024,
  CNET_TLS_CERT_MAX_BYTES = 64 * 1024,
  CNET_TLS_PATH_BUFFER_BYTES = CNET_TLS_PATH_MAX_BYTES + 512
};

typedef enum cnet_tls_variant {
  CNET_TLS_VARIANT_RANGE = 0,
  CNET_TLS_VARIANT_1_2 = 1,
  CNET_TLS_VARIANT_1_3 = 2,
  CNET_TLS_VARIANT_SHANGMI_TLS13 = 3,
  CNET_TLS_VARIANT_COUNT = 4
} cnet_tls_variant;

struct cnet_tls_context {
  TLS_CTX tls[CNET_TLS_VARIANT_COUNT];
  bool tls_ready[CNET_TLS_VARIANT_COUNT];
  char **alpn_protocols;
  size_t alpn_protocol_count;
  char client_server_name[CNET_TLS_SERVER_NAME_CAPACITY];
  atomic_size_t references;
  bool server;
  bool verify_peer;
  bool native_system_trust;
  cnet_tls_security_profile security_profile;
};

typedef struct cnet_tls_gmssl_state {
  TLS_CONNECT connection;
  TLS_IO io;
  unsigned char *cipher_input;
  unsigned char *cipher_output;
  unsigned char *plaintext;
  size_t capacity;
  size_t input_head;
  size_t input_size;
  size_t output_head;
  size_t output_size;
  size_t diagnostic_send_calls;
  size_t diagnostic_send_requested;
  size_t diagnostic_send_accepted;
  size_t plaintext_pending_offset;
  size_t plaintext_pending_size;
  const unsigned char *write_source;
  size_t write_size;
  size_t write_offset;
  cnet_tls_variant variant;
  bool connection_ready;
  char server_name[CNET_TLS_SERVER_NAME_CAPACITY];
} cnet_tls_gmssl_state;

typedef struct cnet_tls_der_bundle {
  unsigned char *data;
  size_t size;
  size_t capacity;
} cnet_tls_der_bundle;

#define CNET_TLS_ENGINE(state) ((cnet_tls_gmssl_state *)((state)->engine))
#define CNET_TLS_CONNECTION(state)   (CNET_TLS_ENGINE(state) != NULL ? &CNET_TLS_ENGINE(state)->connection : NULL)

static bool cnet_tls_bounded_string(const char *value, size_t max_bytes, size_t *out_size) {
  size_t size;
  if (value == NULL) {
    if (out_size != NULL) *out_size = 0u;
    return true;
  }
  for (size = 0u; size <= max_bytes; ++size) {
    if (value[size] == '\0') {
      if (out_size != NULL) *out_size = size;
      return true;
    }
  }
  return false;
}

static bool cnet_tls_optional_path_valid(const char *value) {
  size_t size = 0u;
  return cnet_tls_bounded_string(value, CNET_TLS_PATH_MAX_BYTES, &size) &&
         (value == NULL || size != 0u);
}

static int cnet_tls_der_bundle_required_size(size_t current, size_t appended,
                                             size_t *out_required) {
  if (out_required == NULL) return SALTS_EINVAL;
  *out_required = 0u;
  if (appended == 0u) return SALTS_EINVAL;
  if (appended > CNET_TLS_TRUST_MAX_BYTES || current > CNET_TLS_TRUST_MAX_BYTES - appended)
    return SALTS_ERANGE;
  *out_required = current + appended;
  return SALTS_OK;
}

#if defined(CNET_INTERNAL_TESTING)
int cnet_test_tls_der_bundle_required_size(size_t current, size_t appended, size_t *out_required) {
  return cnet_tls_der_bundle_required_size(current, appended, out_required);
}
#endif

#if !defined(_WIN32) && !defined(__APPLE__)
static bool cnet_tls_file_readable(const char *path) {
  FILE *file;
  if (path == NULL || path[0] == '\0') return false;
  file = fopen(path, "rb");
  if (file == NULL) return false;
  (void)fclose(file);
  return true;
}
#endif

static int cnet_tls_der_bundle_append(cnet_tls_der_bundle *bundle,
                                      const unsigned char *data, size_t size) {
  unsigned char *next;
  size_t capacity;
  size_t required;
  int status;
  if (bundle == NULL || data == NULL || size == 0u) return SALTS_EINVAL;
  status = cnet_tls_der_bundle_required_size(bundle->size, size, &required);
  if (status != SALTS_OK) return status;
  if (required <= bundle->capacity) {
    memcpy(bundle->data + bundle->size, data, size);
    bundle->size = required;
    return SALTS_OK;
  }

  capacity = bundle->capacity != 0u ? bundle->capacity : 4096u;
  while (capacity < required) {
    if (capacity > CNET_TLS_TRUST_MAX_BYTES / 2u) {
      capacity = CNET_TLS_TRUST_MAX_BYTES;
      break;
    }
    capacity *= 2u;
  }
  if (capacity < required) return SALTS_ERANGE;
  next = (unsigned char *)realloc(bundle->data, capacity);
  if (next == NULL) return SALTS_ENOMEM;
  bundle->data = next;
  bundle->capacity = capacity;
  memcpy(bundle->data + bundle->size, data, size);
  bundle->size = required;
  return SALTS_OK;
}

static void cnet_tls_der_bundle_dispose(cnet_tls_der_bundle *bundle) {
  if (bundle == NULL) return;
  free(bundle->data);
  memset(bundle, 0, sizeof(*bundle));
}

static bool cnet_tls_ca_hash_name(const char *name) {
  size_t index;
  if (name == NULL) return false;
  for (index = 0u; index < 8u; ++index) {
    const char ch = name[index];
    if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') ||
          (ch >= 'A' && ch <= 'F')))
      return false;
  }
  if (name[8] != '.') return false;
  index = 9u;
  if (name[index] < '0' || name[index] > '9') return false;
  while (name[index] >= '0' && name[index] <= '9') ++index;
  return name[index] == '\0';
}

static int cnet_tls_pem_seek_entry(FILE *file) {
  int ch;
  if (file == NULL) return SALTS_EINVAL;
  for (;;) {
    ch = fgetc(file);
    if (ch == EOF) return feof(file) ? SALTS_EOF : SALTS_EIO;
    if (ch == '\0' || ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n')
      continue;
    if (ungetc(ch, file) == EOF) return SALTS_EIO;
    return SALTS_OK;
  }
}

static int cnet_tls_append_pem_cert_file(cnet_tls_der_bundle *bundle,
                                         const char *path,
                                         size_t *out_count) {
  unsigned char certificate[CNET_TLS_CERT_MAX_BYTES];
  FILE *file;
  size_t count = 0u;
  int status = SALTS_OK;

  if (bundle == NULL || path == NULL) return SALTS_EINVAL;
  file = fopen(path, "rb");
  if (file == NULL) return SALTS_EIO;
  for (;;) {
    size_t size = 0u;
    int result;

    status = cnet_tls_pem_seek_entry(file);
    if (status == SALTS_EOF) {
      status = SALTS_OK;
      break;
    }
    if (status != SALTS_OK) break;

    result = pem_read(file, "CERTIFICATE", certificate, &size,
                      sizeof(certificate));
    if (result <= 0 || size == 0u) {
      status = SALTS_EIO;
      break;
    }
    status = cnet_tls_der_bundle_append(bundle, certificate, size);
    if (status != SALTS_OK) break;
    ++count;
  }
  (void)fclose(file);
  memset(certificate, 0, sizeof(certificate));
  if (status == SALTS_OK && count == 0u) status = SALTS_EIO;
  if (out_count != NULL) *out_count = count;
  return status;
}

static int cnet_tls_append_ca_path(cnet_tls_der_bundle *bundle,
                                   const char *directory,
                                   size_t *out_count) {
  size_t count = 0u;
  int status = SALTS_OK;
  if (bundle == NULL || directory == NULL || directory[0] == '\0')
    return SALTS_EINVAL;
#if defined(_WIN32)
  {
    WIN32_FIND_DATAA data;
    HANDLE search;
    char pattern[CNET_TLS_PATH_BUFFER_BYTES];
    int length = snprintf(pattern, sizeof(pattern), "%s\\*", directory);
    if (length < 0 || (size_t)length >= sizeof(pattern)) return SALTS_ERANGE;
    search = FindFirstFileA(pattern, &data);
    if (search == INVALID_HANDLE_VALUE) return SALTS_EIO;
    do {
      char path[CNET_TLS_PATH_BUFFER_BYTES];
      size_t file_count = 0u;
      if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ||
          !cnet_tls_ca_hash_name(data.cFileName))
        continue;
      length = snprintf(path, sizeof(path), "%s\\%s", directory, data.cFileName);
      if (length < 0 || (size_t)length >= sizeof(path)) {
        status = SALTS_ERANGE;
        break;
      }
      status = cnet_tls_append_pem_cert_file(bundle, path, &file_count);
      if (status != SALTS_OK) break;
      count += file_count;
    } while (FindNextFileA(search, &data) != 0);
    (void)FindClose(search);
  }
#else
  {
    DIR *dir = opendir(directory);
    struct dirent *entry;
    if (dir == NULL) return SALTS_EIO;
    while ((entry = readdir(dir)) != NULL) {
      char path[CNET_TLS_PATH_BUFFER_BYTES];
      size_t file_count = 0u;
      int length;
      if (!cnet_tls_ca_hash_name(entry->d_name)) continue;
      length = snprintf(path, sizeof(path), "%s/%s", directory, entry->d_name);
      if (length < 0 || (size_t)length >= sizeof(path)) {
        status = SALTS_ERANGE;
        break;
      }
      status = cnet_tls_append_pem_cert_file(bundle, path, &file_count);
      if (status != SALTS_OK) break;
      count += file_count;
    }
    (void)closedir(dir);
  }
#endif
  if (status == SALTS_OK && count == 0u) status = SALTS_EIO;
  if (out_count != NULL) *out_count = count;
  return status;
}

static int cnet_tls_load_explicit_trust(TLS_CTX *tls,
                                        const char *ca_file,
                                        const char *ca_path) {
  cnet_tls_der_bundle bundle = {0};
  size_t count = 0u;
  int status = SALTS_OK;

  if (tls == NULL || (ca_file == NULL && ca_path == NULL)) return SALTS_EINVAL;
  if (ca_file != NULL) {
    size_t file_count = 0u;
    status = cnet_tls_append_pem_cert_file(&bundle, ca_file, &file_count);
    if (status == SALTS_OK) count += file_count;
  }
  if (status == SALTS_OK && ca_path != NULL) {
    size_t path_count = 0u;
    status = cnet_tls_append_ca_path(&bundle, ca_path, &path_count);
    if (status == SALTS_OK) count += path_count;
  }
  if (status == SALTS_OK && count == 0u) status = SALTS_EIO;
  if (status == SALTS_OK &&
      tls_ctx_set_ca_certificates_der(tls, bundle.data, bundle.size,
                                      TLS_DEFAULT_VERIFY_DEPTH) != 1)
    status = SALTS_EIO;
  cnet_tls_der_bundle_dispose(&bundle);
  return status;
}

#if defined(_WIN32)
static int cnet_tls_append_windows_store(cnet_tls_der_bundle *bundle, const char *name) {
  HCERTSTORE store;
  PCCERT_CONTEXT certificate = NULL;
  int status = SALTS_OK;

  if (bundle == NULL || name == NULL) return SALTS_EINVAL;
  store = CertOpenSystemStoreA(0u, name);
  if (store == NULL) return SALTS_EIO;
  while ((certificate = CertEnumCertificatesInStore(store, certificate)) != NULL) {
    status = cnet_tls_der_bundle_append(bundle, certificate->pbCertEncoded,
                                        (size_t)certificate->cbCertEncoded);
    if (status != SALTS_OK) break;
  }
  (void)CertCloseStore(store, 0u);
  return status;
}
#endif

#if defined(__APPLE__) && TARGET_OS_OSX
static int cnet_tls_load_apple_anchors(cnet_tls_der_bundle *bundle) {
  CFArrayRef anchors = NULL;
  CFIndex index;
  CFIndex count;
  OSStatus result;
  int status = SALTS_OK;

  if (bundle == NULL) return SALTS_EINVAL;
  result = SecTrustCopyAnchorCertificates(&anchors);
  if (result != errSecSuccess || anchors == NULL) return SALTS_EIO;
  count = CFArrayGetCount(anchors);
  for (index = 0; index < count; ++index) {
    SecCertificateRef certificate =
        (SecCertificateRef)CFArrayGetValueAtIndex(anchors, index);
    CFDataRef der;
    if (certificate == NULL) continue;
    der = SecCertificateCopyData(certificate);
    if (der == NULL) {
      status = SALTS_EIO;
      break;
    }
    status = cnet_tls_der_bundle_append(
        bundle, (const unsigned char *)CFDataGetBytePtr(der),
        (size_t)CFDataGetLength(der));
    CFRelease(der);
    if (status != SALTS_OK) break;
  }
  CFRelease(anchors);
  return status;
}
#endif

static int cnet_tls_load_system_trust(TLS_CTX *tls) {
#if defined(_WIN32)
  cnet_tls_der_bundle bundle = {0};
  int status = cnet_tls_append_windows_store(&bundle, "ROOT");
  if (status == SALTS_OK) status = cnet_tls_append_windows_store(&bundle, "CA");
  if (status == SALTS_OK && bundle.size == 0u) status = SALTS_EIO;
  if (status == SALTS_OK &&
      tls_ctx_set_ca_certificates_der(tls, bundle.data, bundle.size,
                                      TLS_DEFAULT_VERIFY_DEPTH) != 1)
    status = SALTS_EIO;
  cnet_tls_der_bundle_dispose(&bundle);
  return status;
#elif defined(__APPLE__)
  #if TARGET_OS_OSX
    cnet_tls_der_bundle bundle = {0};
    int status = cnet_tls_load_apple_anchors(&bundle);
    if (status == SALTS_OK && bundle.size == 0u) status = SALTS_EIO;
    if (status == SALTS_OK &&
        tls_ctx_set_ca_certificates_der(tls, bundle.data, bundle.size,
                                        TLS_DEFAULT_VERIFY_DEPTH) != 1)
      status = SALTS_EIO;
    cnet_tls_der_bundle_dispose(&bundle);
    return status;
  #else
    /*
     * iOS does not expose a public system-root enumeration API equivalent to
     * macOS SecTrustCopyAnchorCertificates(). Keep GmSSL's certificate/identity
     * checks active, but delegate only trust-anchor/path validation to the
     * Security.framework bridge after the provider handshake.
     */
    return tls_ctx_enable_external_trust(tls, 1) == 1
               ? SALTS_OK
               : SALTS_EIO;
  #endif
#elif defined(__ANDROID__)
  return cnet_tls_load_explicit_trust(
      tls, NULL, "/system/etc/security/cacerts");
#else
  static const char *const paths[] = {
      "/etc/ssl/certs/ca-certificates.crt",
      "/etc/pki/tls/certs/ca-bundle.crt",
      "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem",
      "/etc/ssl/ca-bundle.pem"};
  const char *environment = getenv("SSL_CERT_FILE");
  size_t index;

  if (environment != NULL && environment[0] != '\0' &&
      cnet_tls_file_readable(environment))
    return cnet_tls_load_explicit_trust(tls, environment, NULL);
  for (index = 0u; index < sizeof(paths) / sizeof(paths[0]); ++index) {
    if (cnet_tls_file_readable(paths[index]))
      return cnet_tls_load_explicit_trust(tls, paths[index], NULL);
  }
  return SALTS_EIO;
#endif
}

static void cnet_tls_alpn_dispose(cnet_tls_context *context) {
  size_t index;
  if (context == NULL || context->alpn_protocols == NULL) return;
  for (index = 0u; index < context->alpn_protocol_count; ++index)
    free(context->alpn_protocols[index]);
  free(context->alpn_protocols);
  context->alpn_protocols = NULL;
  context->alpn_protocol_count = 0u;
}

static int cnet_tls_alpn_copy(cnet_tls_context *context,
                              const char *const *protocols, size_t count) {
  size_t index;
  size_t wire_size = 0u;

  if (context == NULL) return SALTS_EINVAL;
  if (count == 0u) return protocols == NULL ? SALTS_OK : SALTS_EINVAL;
  if (protocols == NULL) return SALTS_EINVAL;

  for (index = 0u; index < count; ++index) {
    size_t length = 0u;
    if (protocols[index] == NULL ||
        !cnet_tls_bounded_string(protocols[index],
                                 CNET_TLS_ALPN_NAME_MAX_BYTES, &length) ||
        length == 0u)
      return SALTS_EINVAL;
    if (wire_size > UINT16_MAX - 1u - length) return SALTS_ERANGE;
    wire_size += 1u + length;
  }

  if (count > SIZE_MAX / sizeof(char *)) return SALTS_ERANGE;
  context->alpn_protocols = (char **)calloc(count, sizeof(char *));
  if (context->alpn_protocols == NULL) return SALTS_ENOMEM;
  context->alpn_protocol_count = count;

  for (index = 0u; index < count; ++index) {
    size_t length = strlen(protocols[index]);
    context->alpn_protocols[index] = (char *)malloc(length + 1u);
    if (context->alpn_protocols[index] == NULL) {
      cnet_tls_alpn_dispose(context);
      return SALTS_ENOMEM;
    }
    memcpy(context->alpn_protocols[index], protocols[index], length + 1u);
  }
  return SALTS_OK;
}

static int cnet_tls_variant_protocol(cnet_tls_variant variant) {
  return variant == CNET_TLS_VARIANT_1_2 ? TLS_protocol_tls12
                                         : TLS_protocol_tls13;
}

static int cnet_tls_configure_variant(cnet_tls_context *context,
                                      cnet_tls_variant variant,
                                      const char *ca_file,
                                      const char *ca_path,
                                      const char *cert_file,
                                      const char *key_file,
                                      const char *key_password,
                                      cnet_tls_client_auth client_auth) {
  static const int range_ciphers[] = {
      TLS_cipher_aes_128_gcm_sha256,
      TLS_cipher_ecdhe_rsa_with_aes_128_gcm_sha256,
      TLS_cipher_ecdhe_ecdsa_with_aes_128_gcm_sha256};
  static const int tls13_ciphers[] = {TLS_cipher_aes_128_gcm_sha256};
  static const int shangmi_ciphers[] = {TLS_cipher_sm4_gcm_sm3};
  static const int shangmi_signatures[] = {TLS_sig_sm2sig_sm3};
  static const int shangmi_groups[] = {TLS_curve_sm2p256v1};
  static const int tls12_ciphers[] = {
      TLS_cipher_ecdhe_rsa_with_aes_128_gcm_sha256,
      TLS_cipher_ecdhe_ecdsa_with_aes_128_gcm_sha256};
  static const int range_signatures[] = {
      TLS_sig_rsa_pss_rsae_sha256,
      TLS_sig_rsa_pkcs1_sha256,
      TLS_sig_ecdsa_secp256r1_sha256};
  static const int tls13_signatures[] = {
      TLS_sig_rsa_pss_rsae_sha256,
      TLS_sig_rsa_pkcs1_sha256,
      TLS_sig_ecdsa_secp256r1_sha256};
  static const int tls12_signatures[] = {
      TLS_sig_rsa_pkcs1_sha256,
      TLS_sig_ecdsa_secp256r1_sha256};
  static const int groups[] = {TLS_curve_secp256r1};
  const int *selected_groups = groups;
  size_t group_count = sizeof(groups) / sizeof(groups[0]);
  TLS_CTX *tls;
  const int *ciphers;
  const int *signatures;
  size_t cipher_count;
  size_t signature_count;
  int mode;
  int status = SALTS_OK;

  if (context == NULL || variant < 0 || variant >= CNET_TLS_VARIANT_COUNT)
    return SALTS_EINVAL;
  tls = &context->tls[variant];
  mode = context->server ? TLS_server_mode : TLS_client_mode;
  if (tls_ctx_init(tls, cnet_tls_variant_protocol(variant), mode) != 1)
    return SALTS_EIO;
  context->tls_ready[variant] = true;

  if (variant == CNET_TLS_VARIANT_SHANGMI_TLS13) {
    /* Exact provider admission must happen before any other TLS_CTX policy.
     * Never retry or fall back to the standard TLS1.2/1.3 configuration. */
    if (tls_ctx_set_strict_protocol(tls, TLS_protocol_tls13) != 1)
      return SALTS_EIO;
    ciphers = shangmi_ciphers;
    cipher_count = sizeof(shangmi_ciphers) / sizeof(shangmi_ciphers[0]);
    signatures = shangmi_signatures;
    signature_count = sizeof(shangmi_signatures) / sizeof(shangmi_signatures[0]);
    selected_groups = shangmi_groups;
    group_count = sizeof(shangmi_groups) / sizeof(shangmi_groups[0]);
  } else if (variant == CNET_TLS_VARIANT_RANGE) {
    if (tls_ctx_set_protocol_range(tls, TLS_protocol_tls12,
                                   TLS_protocol_tls13) != 1)
      return SALTS_EIO;
    ciphers = range_ciphers;
    cipher_count = sizeof(range_ciphers) / sizeof(range_ciphers[0]);
    signatures = range_signatures;
    signature_count = sizeof(range_signatures) / sizeof(range_signatures[0]);
  } else if (variant == CNET_TLS_VARIANT_1_2) {
    ciphers = tls12_ciphers;
    cipher_count = sizeof(tls12_ciphers) / sizeof(tls12_ciphers[0]);
    signatures = tls12_signatures;
    signature_count = sizeof(tls12_signatures) / sizeof(tls12_signatures[0]);
  } else {
    ciphers = tls13_ciphers;
    cipher_count = sizeof(tls13_ciphers) / sizeof(tls13_ciphers[0]);
    signatures = tls13_signatures;
    signature_count = sizeof(tls13_signatures) / sizeof(tls13_signatures[0]);
  }

  if (tls_ctx_set_cipher_suites(tls, ciphers, cipher_count) != 1 ||
      tls_ctx_set_supported_groups(tls, selected_groups, group_count) != 1 ||
      tls_ctx_set_signature_algorithms(tls, signatures, signature_count) != 1)
    return SALTS_EIO;

  if (variant == CNET_TLS_VARIANT_1_2) {
    if (context->server) {
      if (tls12_ctx_set_renegotiation_info(tls, 1) != 1)
        return SALTS_EIO;
    } else if (tls12_ctx_set_empty_renegotiation_info_scsv(tls, 1) != 1) {
      return SALTS_EIO;
    }
  }

  if (context->alpn_protocol_count != 0u &&
      tls_ctx_set_application_layer_protocol_negotiation(
          tls, context->alpn_protocols, context->alpn_protocol_count) != 1)
    return SALTS_EIO;

  if (context->verify_peer) {
    status = (ca_file != NULL || ca_path != NULL)
                 ? cnet_tls_load_explicit_trust(tls, ca_file, ca_path)
                 : cnet_tls_load_system_trust(tls);
    if (status != SALTS_OK) return status;
  }

  if (cert_file != NULL) {
    if (tls_ctx_set_certificate_and_key(
            tls, cert_file, key_file,
            key_password != NULL ? key_password : "") != 1)
      return SALTS_EIO;
  }

  if (context->server && client_auth == CNET_TLS_CLIENT_AUTH_REQUIRED &&
      tls_ctx_enable_certificate_request(tls, 1) != 1)
    return SALTS_EIO;

  return SALTS_OK;
}

static void cnet_tls_context_dispose(cnet_tls_context *context) {
  size_t index;
  if (context == NULL) return;
  for (index = 0u; index < CNET_TLS_VARIANT_COUNT; ++index) {
    if (context->tls_ready[index]) tls_ctx_cleanup(&context->tls[index]);
  }
  cnet_tls_alpn_dispose(context);
  free(context);
}

void cnet_tls_context_retain(cnet_tls_context *context) {
  if (context != NULL)
    (void)atomic_fetch_add_explicit(&context->references, 1u,
                                    memory_order_relaxed);
}

void cnet_tls_context_release(cnet_tls_context *context) {
  if (context != NULL &&
      atomic_fetch_sub_explicit(&context->references, 1u,
                                memory_order_acq_rel) == 1u)
    cnet_tls_context_dispose(context);
}

static int cnet_tls_context_create_common(
    bool server, bool verify_peer, const char *ca_file, const char *ca_path,
    const char *cert_file, const char *key_file, const char *key_password,
    cnet_tls_client_auth client_auth, const char *const *alpn_protocols,
    size_t alpn_protocol_count, cnet_tls_security_profile security_profile,
    cnet_tls_context **out_context) {
  cnet_tls_context *context;
  size_t index;
  int status;

  if (out_context == NULL) return SALTS_EINVAL;
  *out_context = NULL;

  context = (cnet_tls_context *)calloc(1u, sizeof(*context));
  if (context == NULL) return SALTS_ENOMEM;
  context->server = server;
  context->verify_peer = verify_peer;
  context->security_profile = security_profile;
#if defined(__APPLE__) && !TARGET_OS_OSX
  context->native_system_trust =
      !server && verify_peer && ca_file == NULL && ca_path == NULL;
#else
  context->native_system_trust = false;
#endif
  atomic_init(&context->references, 1u);

  status = cnet_tls_alpn_copy(context, alpn_protocols, alpn_protocol_count);
  if (status != SALTS_OK) {
    cnet_tls_context_dispose(context);
    return status;
  }

  for (index = 0u; index < CNET_TLS_VARIANT_COUNT; ++index) {
    if (security_profile == CNET_TLS_SECURITY_PROFILE_SHANGMI_TLS13 &&
        index != CNET_TLS_VARIANT_SHANGMI_TLS13) continue;
    if (security_profile == CNET_TLS_SECURITY_PROFILE_STANDARD &&
        index == CNET_TLS_VARIANT_SHANGMI_TLS13) continue;
    status = cnet_tls_configure_variant(
        context, (cnet_tls_variant)index, ca_file, ca_path, cert_file, key_file,
        key_password, client_auth);
    if (status != SALTS_OK) {
      cnet_tls_context_dispose(context);
      return status;
    }
  }

  *out_context = context;
  return SALTS_OK;
}

int cnet_tls_client_context_create(const cnet_tls_client_config *config,
                                   cnet_tls_context **out_context) {
  cnet_tls_client_config defaults = {sizeof(defaults)};
  cnet_tls_context *context = NULL;
  int status;

  if (out_context == NULL) return SALTS_EINVAL;
  *out_context = NULL;
  if (config == NULL) config = &defaults;
  if (config->size != sizeof(*config) ||
      !cnet_tls_optional_path_valid(config->ca_file) ||
      !cnet_tls_optional_path_valid(config->ca_path) ||
      !cnet_tls_optional_path_valid(config->cert_file) ||
      !cnet_tls_optional_path_valid(config->key_file) ||
      !cnet_tls_bounded_string(config->key_password,
                               CNET_TLS_PASSWORD_MAX_BYTES, NULL) ||
      !cnet_tls_bounded_string(config->server_name,
                               CNET_TLS_SERVER_NAME_CAPACITY - 1u, NULL) ||
      (config->server_name != NULL && config->server_name[0] == '\0') ||
      ((config->cert_file == NULL) != (config->key_file == NULL)) ||
      (config->cert_file == NULL && config->key_password != NULL) ||
      (config->security_profile != CNET_TLS_SECURITY_PROFILE_STANDARD &&
       config->security_profile != CNET_TLS_SECURITY_PROFILE_SHANGMI_TLS13))
    return SALTS_EINVAL;

  status = cnet_tls_context_create_common(
      false, true, config->ca_file, config->ca_path, config->cert_file,
      config->key_file, config->key_password, CNET_TLS_CLIENT_AUTH_NONE,
      config->alpn_protocols, config->alpn_protocol_count,
      config->security_profile, &context);
  if (status != SALTS_OK) return status;
  if (config->server_name != NULL)
    memcpy(context->client_server_name, config->server_name,
           strlen(config->server_name) + 1u);
  *out_context = context;
  return SALTS_OK;
}

int cnet_tls_client_init(cnet_tls_client *client,
                         const cnet_tls_client_config *config) {
  cnet_tls_context *context = NULL;
  int status;
  if (client == NULL || config == NULL) return SALTS_EINVAL;
  if (client->impl != NULL) return SALTS_EALREADY;
  status = cnet_tls_client_context_create(config, &context);
  if (status != SALTS_OK) return status;
  client->impl = context;
  return SALTS_OK;
}

int cnet_tls_client_destroy(cnet_tls_client *client) {
  cnet_tls_context *context;
  if (client == NULL) return SALTS_EINVAL;
  context = (cnet_tls_context *)client->impl;
  if (context == NULL) return SALTS_OK;
  client->impl = NULL;
  cnet_tls_context_release(context);
  return SALTS_OK;
}

cnet_tls_context *cnet_tls_client_context(const cnet_tls_client *client) {
  cnet_tls_context *context =
      client != NULL ? (cnet_tls_context *)client->impl : NULL;
  return context != NULL && !context->server ? context : NULL;
}

const char *cnet_tls_client_server_name(const cnet_tls_client *client) {
  const cnet_tls_context *context = cnet_tls_client_context(client);
  return context != NULL && context->client_server_name[0] != '\0'
             ? context->client_server_name
             : NULL;
}

int cnet_tls_server_init(cnet_tls_server *server,
                         const cnet_tls_server_config *config) {
  cnet_tls_context *context = NULL;
  int status;

  if (server == NULL || config == NULL) return SALTS_EINVAL;
  if (server->impl != NULL) return SALTS_EALREADY;
  if (config->size != sizeof(*config) ||
      !cnet_tls_optional_path_valid(config->cert_file) ||
      !cnet_tls_optional_path_valid(config->key_file) ||
      config->cert_file == NULL || config->key_file == NULL ||
      !cnet_tls_optional_path_valid(config->ca_file) ||
      !cnet_tls_optional_path_valid(config->ca_path) ||
      !cnet_tls_bounded_string(config->key_password,
                               CNET_TLS_PASSWORD_MAX_BYTES, NULL) ||
      (config->client_auth != CNET_TLS_CLIENT_AUTH_NONE &&
       config->client_auth != CNET_TLS_CLIENT_AUTH_REQUIRED) ||
      (config->client_auth == CNET_TLS_CLIENT_AUTH_REQUIRED &&
       config->ca_file == NULL && config->ca_path == NULL) ||
      (config->client_auth == CNET_TLS_CLIENT_AUTH_NONE &&
       (config->ca_file != NULL || config->ca_path != NULL)) ||
      (config->security_profile != CNET_TLS_SECURITY_PROFILE_STANDARD &&
       config->security_profile != CNET_TLS_SECURITY_PROFILE_SHANGMI_TLS13))
    return SALTS_EINVAL;

  status = cnet_tls_context_create_common(
      true, config->client_auth == CNET_TLS_CLIENT_AUTH_REQUIRED,
      config->ca_file, config->ca_path, config->cert_file, config->key_file,
      config->key_password, config->client_auth, config->alpn_protocols,
      config->alpn_protocol_count, config->security_profile, &context);
  if (status != SALTS_OK) return status;
  server->impl = context;
  return SALTS_OK;
}

int cnet_tls_server_destroy(cnet_tls_server *server) {
  cnet_tls_context *context;
  if (server == NULL) return SALTS_EINVAL;
  context = (cnet_tls_context *)server->impl;
  if (context == NULL) return SALTS_OK;
  server->impl = NULL;
  cnet_tls_context_release(context);
  return SALTS_OK;
}

cnet_tls_context *cnet_tls_server_context(const cnet_tls_server *server) {
  cnet_tls_context *context =
      server != NULL ? (cnet_tls_context *)server->impl : NULL;
  return context != NULL && context->server ? context : NULL;
}

static size_t cnet_tls_ring_write(unsigned char *buffer, size_t capacity,
                                  size_t head, size_t size,
                                  const unsigned char *data, size_t length) {
  size_t tail;
  size_t first;
  if (buffer == NULL || data == NULL || capacity == 0u ||
      size >= capacity || length == 0u)
    return 0u;
  if (length > capacity - size) length = capacity - size;
  tail = (head + size) % capacity;
  first = capacity - tail;
  if (first > length) first = length;
  memcpy(buffer + tail, data, first);
  if (length > first) memcpy(buffer, data + first, length - first);
  return length;
}

static size_t cnet_tls_ring_read(unsigned char *buffer, size_t capacity,
                                 size_t *head, size_t *size,
                                 unsigned char *data, size_t length) {
  size_t first;
  if (buffer == NULL || head == NULL || size == NULL || data == NULL ||
      capacity == 0u || *size == 0u || length == 0u)
    return 0u;
  if (length > *size) length = *size;
  first = capacity - *head;
  if (first > length) first = length;
  memcpy(data, buffer + *head, first);
  if (length > first) memcpy(data + first, buffer, length - first);
  *head = (*head + length) % capacity;
  *size -= length;
  return length;
}

static size_t cnet_tls_ring_peek(const unsigned char *buffer, size_t capacity,
                                 size_t head, size_t size,
                                 unsigned char *data, size_t length) {
  size_t first;
  if (buffer == NULL || data == NULL || capacity == 0u ||
      size == 0u || length == 0u)
    return 0u;
  if (length > size) length = size;
  first = capacity - head;
  if (first > length) first = length;
  memcpy(data, buffer + head, first);
  if (length > first) memcpy(data + first, buffer, length - first);
  return length;
}

static int cnet_tls_cipher_record_ready(const cnet_tls_gmssl_state *engine,
                                        bool *out_ready) {
  unsigned char header[TLS_RECORD_HEADER_SIZE];
  size_t record_size;
  if (engine == NULL || out_ready == NULL) return SALTS_EINVAL;
  *out_ready = false;
  if (engine->input_size < TLS_RECORD_HEADER_SIZE) return SALTS_OK;
  if (cnet_tls_ring_peek(engine->cipher_input, engine->capacity,
                         engine->input_head, engine->input_size,
                         header, sizeof(header)) != sizeof(header))
    return SALTS_EIO;
  record_size = TLS_RECORD_HEADER_SIZE +
                (((size_t)header[3] << 8u) | (size_t)header[4]);
  if (record_size < TLS_RECORD_HEADER_SIZE ||
      record_size > TLS_MAX_RECORD_SIZE ||
      record_size > engine->capacity)
    return SALTS_EPROTO;
  *out_ready = engine->input_size >= record_size;
  return SALTS_OK;
}

static tls_ret_t cnet_tls_io_send(void *user, const void *buffer, size_t length,
                                  int flags) {
  cnet_tls_gmssl_state *engine = (cnet_tls_gmssl_state *)user;
  size_t written;
  (void)flags;
  if (engine == NULL || buffer == NULL || length == 0u)
    return TLS_ERROR_SYSCALL;
  ++engine->diagnostic_send_calls;
  engine->diagnostic_send_requested += length;
  written = cnet_tls_ring_write(
      engine->cipher_output, engine->capacity, engine->output_head,
      engine->output_size, (const unsigned char *)buffer, length);
  if (written == 0u) return TLS_ERROR_SEND_AGAIN;
  engine->output_size += written;
  engine->diagnostic_send_accepted += written;
  return (tls_ret_t)written;
}

static tls_ret_t cnet_tls_io_recv(void *user, void *buffer, size_t length,
                                  int flags) {
  cnet_tls_gmssl_state *engine = (cnet_tls_gmssl_state *)user;
  size_t read_size;
  (void)flags;
  if (engine == NULL || buffer == NULL || length == 0u)
    return TLS_ERROR_SYSCALL;
  read_size = cnet_tls_ring_read(
      engine->cipher_input, engine->capacity, &engine->input_head,
      &engine->input_size, (unsigned char *)buffer, length);
  if (read_size == 0u) return TLS_ERROR_RECV_AGAIN;
  return (tls_ret_t)read_size;
}

static int cnet_tls_ip_literal_bytes(const char *name,
                                     unsigned char output[16],
                                     size_t *out_size) {
  if (name == NULL || output == NULL || out_size == NULL) return 0;
  if (inet_pton(AF_INET, name, output) == 1) {
    *out_size = 4u;
    return 1;
  }
  if (inet_pton(AF_INET6, name, output) == 1) {
    *out_size = 16u;
    return 1;
  }
  *out_size = 0u;
  return 0;
}

static bool cnet_tls_ip_literal(const char *name) {
  unsigned char bytes[16];
  size_t size = 0u;
  return cnet_tls_ip_literal_bytes(name, bytes, &size) == 1;
}

static int cnet_tls_verify_ip_subject_alt_name(const TLS_CONNECT *connection,
                                                const char *name) {
  unsigned char expected[16];
  size_t expected_size = 0u;
  const unsigned char *cert = NULL;
  size_t cert_size = 0u;
  const unsigned char *exts = NULL;
  size_t exts_size = 0u;
  const unsigned char *encoded_names = NULL;
  size_t encoded_names_size = 0u;
  const unsigned char *names = NULL;
  size_t names_size = 0u;
  const unsigned char *cursor;
  const unsigned char *value = NULL;
  size_t value_size = 0u;
  int critical = 0;
  int result;

  if (connection == NULL ||
      cnet_tls_ip_literal_bytes(name, expected, &expected_size) != 1)
    return SALTS_EINVAL;

  result = tls_get_peer_certificate(connection, &cert, &cert_size);
  if (result < 0) return SALTS_EIO;
  if (result == 0 || cert == NULL || cert_size == 0u)
    return SALTS_ECONNABORTED;

  result = x509_cert_get_exts(cert, cert_size, &exts, &exts_size);
  if (result < 0) return SALTS_EIO;
  if (result == 0) return SALTS_ECONNABORTED;

  result = x509_exts_get_ext_by_oid(
      exts, exts_size, OID_ce_subject_alt_name, &critical,
      &encoded_names, &encoded_names_size);
  if (result < 0) return SALTS_EIO;
  if (result == 0) return SALTS_ECONNABORTED;

  if (asn1_sequence_from_der(&names, &names_size,
                             &encoded_names, &encoded_names_size) != 1 ||
      encoded_names_size != 0u)
    return SALTS_EPROTO;

  cursor = names;
  for (;;) {
    result = x509_general_names_get_next(
        names, names_size, &cursor, X509_gn_ip_address, &value, &value_size);
    if (result < 0) return SALTS_EPROTO;
    if (result == 0) return SALTS_ECONNABORTED;
    if (value_size == expected_size &&
        memcmp(value, expected, expected_size) == 0)
      return SALTS_OK;
  }
}

static int cnet_tls_engine_init_connection(cnet_tls_state *state,
                                           cnet_tls_variant variant) {
  cnet_tls_gmssl_state *engine;
  TLS_CTX *tls;
  if (state == NULL || state->context == NULL ||
      (engine = CNET_TLS_ENGINE(state)) == NULL)
    return SALTS_EINVAL;
  if (variant < 0 || variant >= CNET_TLS_VARIANT_COUNT ||
      !state->context->tls_ready[variant])
    return SALTS_EINVAL;
  if (engine->input_size != 0u || engine->output_size != 0u ||
      engine->plaintext_pending_size != 0u)
    return SALTS_EBUSY;

  if (engine->connection_ready) {
    tls_cleanup(&engine->connection);
    memset(&engine->connection, 0, sizeof(engine->connection));
    engine->connection_ready = false;
  }

  tls = &state->context->tls[variant];
  if (tls_init(&engine->connection, tls) != 1) return SALTS_EIO;
  engine->connection_ready = true;
  engine->variant = variant;
  engine->io.user = engine;
  engine->io.send = cnet_tls_io_send;
  engine->io.recv = cnet_tls_io_recv;

  if (!state->server) {
    if (engine->server_name[0] == '\0') return SALTS_EINVAL;
    if (!cnet_tls_ip_literal(engine->server_name) &&
        (tls_set_hostname(&engine->connection, engine->server_name) != 1 ||
         tls_set_server_name(&engine->connection) != 1))
      return SALTS_EIO;
  }
  if (tls_set_io(&engine->connection, &engine->io) != 1)
    return SALTS_EIO;
  return SALTS_OK;
}

int cnet_tls_state_init(cnet_tls_state *state, cnet_tls_context *context,
                        bool server, const char *server_name,
                        size_t io_buffer_bytes) {
  cnet_tls_gmssl_state *engine = NULL;
  unsigned char *read_buffer = NULL;
  unsigned char *write_buffer = NULL;
  size_t ring_capacity;
  size_t allocation_size;
  int status;

  if (state == NULL || context == NULL || context->server != server ||
      io_buffer_bytes < CNET_TLS_MIN_IO_BUFFER_BYTES ||
      (!server &&
       (server_name == NULL || server_name[0] == '\0' ||
        !cnet_tls_bounded_string(server_name,
                                 CNET_TLS_SERVER_NAME_CAPACITY - 1u, NULL))))
    return SALTS_EINVAL;
  if (state->engine != NULL || state->context != NULL) return SALTS_EALREADY;

  ring_capacity = io_buffer_bytes < (size_t)TLS_MAX_RECORD_SIZE
                      ? (size_t)TLS_MAX_RECORD_SIZE
                      : io_buffer_bytes;
  if (ring_capacity >
      (SIZE_MAX - sizeof(cnet_tls_gmssl_state)) / 2u)
    return SALTS_ERANGE;

  allocation_size = sizeof(cnet_tls_gmssl_state) + ring_capacity * 2u;
  if (io_buffer_bytes > SIZE_MAX - allocation_size) return SALTS_ERANGE;
  allocation_size += io_buffer_bytes;
  engine = (cnet_tls_gmssl_state *)calloc(1u, allocation_size);
  read_buffer = (unsigned char *)malloc(io_buffer_bytes);
  write_buffer = (unsigned char *)malloc(io_buffer_bytes);
  if (engine == NULL || read_buffer == NULL || write_buffer == NULL) {
    free(write_buffer);
    free(read_buffer);
    free(engine);
    return SALTS_ENOMEM;
  }

  engine->capacity = ring_capacity;
  engine->cipher_input = (unsigned char *)(engine + 1);
  engine->cipher_output = engine->cipher_input + ring_capacity;
  /* NativeIO owns state->read_buffer until the receive completion is consumed.
   * Provider reads/probes and pending plaintext must not alias that storage. */
  engine->plaintext = engine->cipher_output + ring_capacity;
  if (!server)
    memcpy(engine->server_name, server_name, strlen(server_name) + 1u);

  state->context = context;
  state->engine = engine;
  state->read_buffer = read_buffer;
  state->write_buffer = write_buffer;
  state->io_buffer_bytes = io_buffer_bytes;
  state->server = server;

  status = cnet_tls_engine_init_connection(
      state, context->security_profile == CNET_TLS_SECURITY_PROFILE_SHANGMI_TLS13
                 ? CNET_TLS_VARIANT_SHANGMI_TLS13
                 : CNET_TLS_VARIANT_RANGE);
  if (status != SALTS_OK) {
    state->context = NULL;
    state->engine = NULL;
    state->read_buffer = NULL;
    state->write_buffer = NULL;
    state->io_buffer_bytes = 0u;
    state->server = false;
    if (engine->connection_ready) tls_cleanup(&engine->connection);
    free(write_buffer);
    free(read_buffer);
    free(engine);
    return status;
  }
  return SALTS_OK;
}

void cnet_tls_state_destroy(cnet_tls_state *state) {
  cnet_tls_context *context;
  cnet_tls_gmssl_state *engine;
  if (state == NULL) return;
  context = state->context;
  engine = CNET_TLS_ENGINE(state);
  if (engine != NULL && engine->connection_ready)
    tls_cleanup(&engine->connection);
  free(engine);
  free(state->write_buffer);
  free(state->read_buffer);
  memset(state, 0, sizeof(*state));
  cnet_tls_context_release(context);
}

static bool cnet_tls_retryable(int result) {
  return result == TLS_ERROR_RECV_AGAIN || result == TLS_ERROR_SEND_AGAIN;
}

#if defined(__APPLE__) && !TARGET_OS_OSX
static int cnet_tls_verify_apple_system_trust(
    const cnet_tls_gmssl_state *engine) {
  cnet_tls_peer_certificate_chain chain = {0};
  CFMutableArrayRef certificates = NULL;
  SecPolicyRef policy = NULL;
  SecTrustRef trust = NULL;
  CFErrorRef error = NULL;
  size_t index;
  int status;

  if (engine == NULL) return SALTS_EINVAL;
  status = cnet_tls_peer_certificate_chain_parse(
      engine->connection.peer_cert_chain,
      engine->connection.peer_cert_chain_len,
      &chain);
  if (status != SALTS_OK) return status;

  certificates = CFArrayCreateMutable(
      kCFAllocatorDefault, (CFIndex)chain.count,
      &kCFTypeArrayCallBacks);
  if (certificates == NULL) return SALTS_ENOMEM;

  for (index = 0u; index < chain.count; ++index) {
    CFDataRef data = CFDataCreate(
        kCFAllocatorDefault,
        chain.certificates[index].data,
        (CFIndex)chain.certificates[index].size);
    SecCertificateRef certificate;
    if (data == NULL) {
      status = SALTS_ENOMEM;
      goto cleanup;
    }
    certificate = SecCertificateCreateWithData(
        kCFAllocatorDefault, data);
    CFRelease(data);
    if (certificate == NULL) {
      status = SALTS_EPROTO;
      goto cleanup;
    }
    CFArrayAppendValue(certificates, certificate);
    CFRelease(certificate);
  }

  /*
   * CNet/GmSSL already owns peer identity validation, including the explicit
   * IP-SAN path. Security.framework is used here only for native system
   * trust-anchor/path validation so platform policy cannot change CNet's
   * hostname/IP semantics.
   */
  policy = SecPolicyCreateBasicX509();
  if (policy == NULL) {
    status = SALTS_EIO;
    goto cleanup;
  }
  if (SecTrustCreateWithCertificates(
          certificates, policy, &trust) != errSecSuccess ||
      trust == NULL) {
    status = SALTS_EIO;
    goto cleanup;
  }
  if (!SecTrustEvaluateWithError(trust, &error)) {
    status = SALTS_ECONNABORTED;
    goto cleanup;
  }
  status = SALTS_OK;

cleanup:
  if (error != NULL) CFRelease(error);
  if (trust != NULL) CFRelease(trust);
  if (policy != NULL) CFRelease(policy);
  if (certificates != NULL) CFRelease(certificates);
  return status;
}
#endif

static int cnet_tls_finish_handshake(cnet_tls_state *state,
                                     bool *out_complete) {
  cnet_tls_gmssl_state *engine = CNET_TLS_ENGINE(state);
  int complete = 0;
  int verify_result = X509_verify_ok;
  const char *alpn = NULL;
  size_t alpn_size = 0u;
  int result;

  if (tls_get_handshake_complete(&engine->connection, &complete) != 1)
    return SALTS_EIO;
  if (!complete) return SALTS_OK;
  if (state->context->security_profile == CNET_TLS_SECURITY_PROFILE_SHANGMI_TLS13 &&
      (engine->connection.protocol != TLS_protocol_tls13 ||
       engine->connection.cipher_suite != TLS_cipher_sm4_gcm_sm3))
    return SALTS_EPROTO;

  if (state->context->verify_peer) {
    result = tls_get_verify_result(&engine->connection, &verify_result);
    if (result != 1 || verify_result != X509_verify_ok)
      return SALTS_ECONNABORTED;
    if (!state->server && cnet_tls_ip_literal(engine->server_name)) {
      result = cnet_tls_verify_ip_subject_alt_name(
          &engine->connection, engine->server_name);
      if (result != SALTS_OK) return result;
    }
#if defined(__APPLE__) && !TARGET_OS_OSX
    if (state->context->native_system_trust) {
      result = cnet_tls_verify_apple_system_trust(engine);
      if (result != SALTS_OK) return result;
    }
#endif
  }

  result = tls_get_selected_alpn(&engine->connection, &alpn, &alpn_size);
  if (result < 0) return SALTS_EPROTO;
  if (result == 1) {
    if (alpn_size == 0u || alpn_size > CNET_TLS_ALPN_NAME_MAX_BYTES)
      return SALTS_EPROTO;
    memcpy(state->negotiated_alpn, alpn, alpn_size);
    state->negotiated_alpn_size = alpn_size;
  }

  state->handshake_complete = true;
  *out_complete = true;
  return SALTS_OK;
}

int cnet_tls_handshake(cnet_tls_state *state, bool *out_complete) {
  cnet_tls_gmssl_state *engine;
  int result;
  int status;
  if (state == NULL || out_complete == NULL ||
      (engine = CNET_TLS_ENGINE(state)) == NULL ||
      !engine->connection_ready)
    return SALTS_EINVAL;
  *out_complete = state->handshake_complete;
  if (state->handshake_complete) return SALTS_OK;

  result = tls_do_handshake(&engine->connection);
  if (result != 1 && !cnet_tls_retryable(result))
    return SALTS_ECONNABORTED;
  status = cnet_tls_finish_handshake(state, out_complete);
  return status;
}

bool cnet_tls_state_handshake_complete(const cnet_tls_state *state) {
  return state != NULL && state->handshake_complete;
}

static int cnet_tls_select_variant(cnet_tls_protocol_version minimum,
                                   cnet_tls_protocol_version maximum,
                                   cnet_tls_variant *out_variant) {
  cnet_tls_protocol_version effective_min = minimum;
  cnet_tls_protocol_version effective_max = maximum;
  if (out_variant == NULL) return SALTS_EINVAL;
  if (minimum != CNET_TLS_PROTOCOL_VERSION_DEFAULT &&
      minimum != CNET_TLS_PROTOCOL_VERSION_1_2 &&
      minimum != CNET_TLS_PROTOCOL_VERSION_1_3)
    return SALTS_EINVAL;
  if (maximum != CNET_TLS_PROTOCOL_VERSION_DEFAULT &&
      maximum != CNET_TLS_PROTOCOL_VERSION_1_2 &&
      maximum != CNET_TLS_PROTOCOL_VERSION_1_3)
    return SALTS_EINVAL;

  if (effective_min == CNET_TLS_PROTOCOL_VERSION_DEFAULT)
    effective_min = CNET_TLS_PROTOCOL_VERSION_1_2;
  if (effective_max == CNET_TLS_PROTOCOL_VERSION_DEFAULT)
    effective_max = CNET_TLS_PROTOCOL_VERSION_1_3;
  if (effective_min > effective_max) return SALTS_EINVAL;

  if (effective_min == CNET_TLS_PROTOCOL_VERSION_1_2 &&
      effective_max == CNET_TLS_PROTOCOL_VERSION_1_2)
    *out_variant = CNET_TLS_VARIANT_1_2;
  else if (effective_min == CNET_TLS_PROTOCOL_VERSION_1_3 &&
           effective_max == CNET_TLS_PROTOCOL_VERSION_1_3)
    *out_variant = CNET_TLS_VARIANT_1_3;
  else
    *out_variant = CNET_TLS_VARIANT_RANGE;
  return SALTS_OK;
}

int cnet_tls_state_set_protocol_range(cnet_tls_state *state,
                                      cnet_tls_protocol_version minimum,
                                      cnet_tls_protocol_version maximum) {
  cnet_tls_variant variant;
  int status;
  if (state == NULL || CNET_TLS_ENGINE(state) == NULL ||
      state->handshake_complete)
    return SALTS_EINVAL;
  if (state->context->security_profile == CNET_TLS_SECURITY_PROFILE_SHANGMI_TLS13) {
    /* A configured SM-only TLS 1.3 connection can never switch to range/TLS1.2. */
    return ((minimum == CNET_TLS_PROTOCOL_VERSION_DEFAULT &&
             maximum == CNET_TLS_PROTOCOL_VERSION_DEFAULT) ||
            (minimum == CNET_TLS_PROTOCOL_VERSION_1_3 &&
             maximum == CNET_TLS_PROTOCOL_VERSION_1_3))
               ? SALTS_OK : SALTS_EINVAL;
  }
  status = cnet_tls_select_variant(minimum, maximum, &variant);
  if (status != SALTS_OK) return status;
  if (CNET_TLS_ENGINE(state)->variant == variant) return SALTS_OK;
  state->negotiated_alpn_size = 0u;
  memset(state->negotiated_alpn, 0, sizeof(state->negotiated_alpn));
  return cnet_tls_engine_init_connection(state, variant);
}

void *cnet_tls_state_read_buffer(cnet_tls_state *state) {
  return state != NULL ? state->read_buffer : NULL;
}

void *cnet_tls_state_write_buffer(cnet_tls_state *state) {
  return state != NULL ? state->write_buffer : NULL;
}

size_t cnet_tls_state_io_buffer_bytes(const cnet_tls_state *state) {
  return state != NULL ? state->io_buffer_bytes : 0u;
}

size_t cnet_tls_cipher_input_capacity(const cnet_tls_state *state) {
  const cnet_tls_gmssl_state *engine =
      state != NULL ? CNET_TLS_ENGINE(state) : NULL;
  return engine != NULL && engine->input_size <= engine->capacity
             ? engine->capacity - engine->input_size
             : 0u;
}

int cnet_tls_feed_cipher(cnet_tls_state *state, const void *data, size_t size) {
  cnet_tls_gmssl_state *engine;
  size_t written;
  if (state == NULL || data == NULL || size == 0u ||
      (engine = CNET_TLS_ENGINE(state)) == NULL)
    return SALTS_EINVAL;
  if (size > engine->capacity - engine->input_size) return SALTS_ENOBUFS;
  written = cnet_tls_ring_write(
      engine->cipher_input, engine->capacity, engine->input_head,
      engine->input_size, (const unsigned char *)data, size);
  if (written != size) return SALTS_EIO;
  engine->input_size += written;
  return SALTS_OK;
}

int cnet_tls_diagnostic_send_stats(const cnet_tls_state *state,
                                   size_t *calls, size_t *requested,
                                   size_t *accepted, size_t *queued) {
  const cnet_tls_gmssl_state *engine =
      state != NULL ? CNET_TLS_ENGINE(state) : NULL;
  if (engine == NULL || calls == NULL || requested == NULL ||
      accepted == NULL || queued == NULL) return SALTS_EINVAL;
  *calls = engine->diagnostic_send_calls;
  *requested = engine->diagnostic_send_requested;
  *accepted = engine->diagnostic_send_accepted;
  *queued = engine->output_size;
  return SALTS_OK;
}

int cnet_tls_take_cipher(cnet_tls_state *state, void *buffer,
                         size_t capacity, size_t *out_size) {
  cnet_tls_gmssl_state *engine;
  size_t read_size;
  if (out_size == NULL) return SALTS_EINVAL;
  *out_size = 0u;
  if (state == NULL || buffer == NULL || capacity == 0u ||
      (engine = CNET_TLS_ENGINE(state)) == NULL)
    return SALTS_EINVAL;
  if (engine->output_size == 0u) return SALTS_ENOENT;
  read_size = cnet_tls_ring_read(
      engine->cipher_output, engine->capacity, &engine->output_head,
      &engine->output_size, (unsigned char *)buffer, capacity);
  if (read_size == 0u) return SALTS_EIO;
  *out_size = read_size;
  return SALTS_OK;
}

int cnet_tls_write(cnet_tls_state *state, const void *data, size_t size,
                   bool *out_complete) {
  cnet_tls_gmssl_state *engine;
  const unsigned char *bytes = (const unsigned char *)data;
  if (state == NULL || data == NULL || size == 0u ||
      out_complete == NULL ||
      (engine = CNET_TLS_ENGINE(state)) == NULL)
    return SALTS_EINVAL;
  *out_complete = false;
  if (!state->handshake_complete || state->close_notify_started)
    return SALTS_ENOTCONN;

  if (engine->write_size == 0u) {
    engine->write_source = bytes;
    engine->write_size = size;
    engine->write_offset = 0u;
  } else if (engine->write_source != bytes || engine->write_size != size ||
             engine->write_offset >= engine->write_size) {
    return SALTS_EBUSY;
  }

  while (engine->write_offset < engine->write_size) {
    size_t sent_size = 0u;
    size_t remaining = engine->write_size - engine->write_offset;
    int result = tls_send(&engine->connection,
                          engine->write_source + engine->write_offset,
                          remaining, &sent_size);
    if (result == 1) {
      if (sent_size == 0u || sent_size > remaining) return SALTS_EPROTO;
      engine->write_offset += sent_size;
      continue;
    }
    if (cnet_tls_retryable(result)) return SALTS_OK;
    engine->write_source = NULL;
    engine->write_size = 0u;
    engine->write_offset = 0u;
    return SALTS_EPROTO;
  }

  engine->write_source = NULL;
  engine->write_size = 0u;
  engine->write_offset = 0u;
  *out_complete = true;
  return SALTS_OK;
}

static int cnet_tls_read_pending(cnet_tls_state *state, void *buffer,
                                 size_t capacity, size_t *out_size) {
  cnet_tls_gmssl_state *engine = CNET_TLS_ENGINE(state);
  size_t available;
  size_t size;
  if (engine == NULL || engine->plaintext_pending_size == 0u)
    return SALTS_ENOENT;
  available =
      engine->plaintext_pending_size - engine->plaintext_pending_offset;
  size = capacity < available ? capacity : available;
  memcpy(buffer, engine->plaintext + engine->plaintext_pending_offset, size);
  engine->plaintext_pending_offset += size;
  if (engine->plaintext_pending_offset == engine->plaintext_pending_size) {
    engine->plaintext_pending_offset = 0u;
    engine->plaintext_pending_size = 0u;
  }
  *out_size = size;
  return SALTS_OK;
}

int cnet_tls_read(cnet_tls_state *state, void *buffer, size_t capacity,
                  size_t *out_size, bool *out_peer_closed) {
  cnet_tls_gmssl_state *engine;
  size_t received_size = 0u;
  size_t copy_size;
  bool record_ready = false;
  int result;
  int status;
  if (out_size == NULL || out_peer_closed == NULL) return SALTS_EINVAL;
  *out_size = 0u;
  *out_peer_closed = false;
  if (state == NULL || buffer == NULL || capacity == 0u ||
      (engine = CNET_TLS_ENGINE(state)) == NULL)
    return SALTS_EINVAL;
  if (!state->handshake_complete) return SALTS_ENOTCONN;
  if (state->peer_close_notify) {
    *out_peer_closed = true;
    return SALTS_OK;
  }

  status = cnet_tls_read_pending(state, buffer, capacity, out_size);
  if (status == SALTS_OK) return SALTS_OK;
  if (status != SALTS_ENOENT) return status;

  /*
   * GmSSL uses one TLS_CONNECT record scratch for both tls_send() and
   * tls_recv(). A retryable application write can leave a partially emitted
   * provider record in that scratch after cnet_tls_write() returns
   * incomplete. Do not enter provider receive until that logical write has
   * been completely accepted; encrypted peer records remain bounded in the
   * CNet input ring and are consumed after the write finishes.
   */
  if (engine->write_size != 0u) return SALTS_OK;

  status = cnet_tls_cipher_record_ready(engine, &record_ready);
  if (status != SALTS_OK) return status;
  if (!record_ready) return SALTS_OK;

  result = tls_recv(&engine->connection, engine->plaintext,
                    state->io_buffer_bytes, &received_size);
  if (result == 1) {
    if (received_size > state->io_buffer_bytes) return SALTS_EPROTO;
    copy_size = capacity < received_size ? capacity : received_size;
    if (copy_size != 0u) memcpy(buffer, engine->plaintext, copy_size);
    *out_size = copy_size;
    if (copy_size < received_size) {
      engine->plaintext_pending_offset = copy_size;
      engine->plaintext_pending_size = received_size;
    }
    return SALTS_OK;
  }
  if (result == 0 || result == TLS_ERROR_TCP_CLOSED) {
    state->peer_close_notify = true;
    *out_peer_closed = true;
    return SALTS_OK;
  }
  if (cnet_tls_retryable(result)) return SALTS_OK;
  return SALTS_EPROTO;
}

int cnet_tls_probe_peer_close(cnet_tls_state *state, bool *out_peer_closed,
                              bool *out_plaintext_pending) {
  cnet_tls_gmssl_state *engine;
  size_t received_size = 0u;
  int result;
  int received = 0;

  if (out_peer_closed == NULL || out_plaintext_pending == NULL)
    return SALTS_EINVAL;
  *out_peer_closed = false;
  *out_plaintext_pending = false;
  if (state == NULL || (engine = CNET_TLS_ENGINE(state)) == NULL)
    return SALTS_EINVAL;
  if (!state->handshake_complete) return SALTS_ENOTCONN;
  if (state->peer_close_notify) {
    *out_peer_closed = true;
    return SALTS_OK;
  }
  if (engine->plaintext_pending_size != 0u) {
    *out_plaintext_pending = true;
    return SALTS_OK;
  }
  if (engine->write_size != 0u) return SALTS_OK;

  {
    bool record_ready = false;
    int status = cnet_tls_cipher_record_ready(engine, &record_ready);
    if (status != SALTS_OK) return status;
    if (!record_ready) return SALTS_OK;
  }

  result = tls_recv(&engine->connection, engine->plaintext,
                    state->io_buffer_bytes, &received_size);
  if (result == 1 && received_size != 0u) {
    engine->plaintext_pending_offset = 0u;
    engine->plaintext_pending_size = received_size;
    *out_plaintext_pending = true;
    return SALTS_OK;
  }
  if (result == 0 || result == TLS_ERROR_TCP_CLOSED) {
    state->peer_close_notify = true;
    *out_peer_closed = true;
    return SALTS_OK;
  }
  if (!cnet_tls_retryable(result) && result != 1) return SALTS_EPROTO;

  if (tls_get_peer_close_notify(&engine->connection, &received) != 1)
    return SALTS_EIO;
  if (received) {
    state->peer_close_notify = true;
    *out_peer_closed = true;
  }
  return SALTS_OK;
}

int cnet_tls_shutdown(cnet_tls_state *state, bool *out_notify_generated) {
  cnet_tls_gmssl_state *engine;
  size_t output_before;
  int result;
  if (state == NULL || out_notify_generated == NULL ||
      (engine = CNET_TLS_ENGINE(state)) == NULL)
    return SALTS_EINVAL;
  *out_notify_generated = state->close_notify_started;
  if (!state->handshake_complete) return SALTS_ENOTCONN;

  output_before = engine->output_size;
  result = tls_shutdown(&engine->connection);
  if (engine->output_size > output_before) state->close_notify_started = true;
  *out_notify_generated = state->close_notify_started;

  if (result == 1 || result == TLS_ERROR_TCP_CLOSED ||
      cnet_tls_retryable(result))
    return SALTS_OK;
  return SALTS_EPROTO;
}

int cnet_tls_get_negotiated_alpn(const cnet_tls_state *state,
                                 const unsigned char **out_data,
                                 size_t *out_size) {
  if (out_data == NULL || out_size == NULL) return SALTS_EINVAL;
  *out_data = NULL;
  *out_size = 0u;
  if (state == NULL || CNET_TLS_ENGINE(state) == NULL) return SALTS_EINVAL;
  if (!state->handshake_complete) return SALTS_ENOTCONN;
  if (state->negotiated_alpn_size == 0u) return SALTS_ENOENT;
  *out_data = state->negotiated_alpn;
  *out_size = state->negotiated_alpn_size;
  return SALTS_OK;
}

static int cnet_tls_state_copy_session_string(const cnet_tls_state *state,
                                              const char *value,
                                              char *buffer, size_t capacity,
                                              size_t *out_size) {
  size_t size;
  if (out_size == NULL) return SALTS_EINVAL;
  *out_size = 0u;
  if (state == NULL || buffer == NULL || capacity == 0u)
    return SALTS_EINVAL;
  buffer[0] = '\0';
  if (CNET_TLS_ENGINE(state) == NULL || !state->handshake_complete)
    return SALTS_ENOTCONN;
  if (value == NULL || value[0] == '\0') return SALTS_ENOENT;
  size = strlen(value);
  if (capacity <= size) return SALTS_EMSGSIZE;
  memcpy(buffer, value, size + 1u);
  *out_size = size;
  return SALTS_OK;
}

int cnet_tls_state_negotiated_version(const cnet_tls_state *state,
                                      char *buffer, size_t capacity,
                                      size_t *out_size) {
  int protocol = 0;
  int result;
  const char *name = NULL;
  if (state == NULL || CNET_TLS_ENGINE(state) == NULL)
    return cnet_tls_state_copy_session_string(state, NULL, buffer, capacity,
                                              out_size);
  if (!state->handshake_complete)
    return cnet_tls_state_copy_session_string(state, NULL, buffer, capacity,
                                              out_size);
  result = tls_get_negotiated_protocol(
      &CNET_TLS_ENGINE(state)->connection, &protocol);
  if (result < 0) return SALTS_EIO;
  if (result == 0) return SALTS_ENOENT;
  if (protocol == TLS_protocol_tls12)
    name = "TLSv1.2";
  else if (protocol == TLS_protocol_tls13)
    name = "TLSv1.3";
  else
    return SALTS_EPROTO;
  return cnet_tls_state_copy_session_string(state, name, buffer, capacity,
                                            out_size);
}

int cnet_tls_state_negotiated_cipher(const cnet_tls_state *state,
                                     char *buffer, size_t capacity,
                                     size_t *out_size) {
  const char *name = NULL;
  int result;
  if (state == NULL || CNET_TLS_ENGINE(state) == NULL)
    return cnet_tls_state_copy_session_string(state, NULL, buffer, capacity,
                                              out_size);
  if (!state->handshake_complete)
    return cnet_tls_state_copy_session_string(state, NULL, buffer, capacity,
                                              out_size);
  result = tls_get_negotiated_cipher_name(
      &CNET_TLS_ENGINE(state)->connection, &name);
  if (result < 0) return SALTS_EIO;
  if (result == 0) return SALTS_ENOENT;
  return cnet_tls_state_copy_session_string(state, name, buffer, capacity,
                                            out_size);
}

int cnet_tls_state_peer_certificate_sha256(
    const cnet_tls_state *state,
    char buffer[CNET_TLS_PEER_CERTIFICATE_SHA256_CAPACITY]) {
  static const char hex[] = "0123456789abcdef";
  uint8_t digest[32];
  size_t index;
  int result;

  if (state == NULL || buffer == NULL) return SALTS_EINVAL;
  buffer[0] = '\0';
  if (CNET_TLS_ENGINE(state) == NULL || !state->handshake_complete)
    return SALTS_ENOTCONN;
  result = tls_get_peer_certificate_sha256(
      &CNET_TLS_ENGINE(state)->connection, digest);
  if (result < 0) return SALTS_EIO;
  if (result == 0) return SALTS_ENOENT;
  for (index = 0u; index < sizeof(digest); ++index) {
    buffer[index * 2u] = hex[digest[index] >> 4u];
    buffer[index * 2u + 1u] = hex[digest[index] & 0x0fu];
  }
  buffer[sizeof(digest) * 2u] = '\0';
  return SALTS_OK;
}

static const DIGEST *cnet_tls_server_end_point_digest(int signature_algor) {
  switch (signature_algor) {
    case OID_rsasign_with_md5:
    case OID_rsasign_with_sha1:
    case OID_ecdsa_with_sha1:
      return DIGEST_sha256();
    case OID_rsasign_with_sha224:
    case OID_ecdsa_with_sha224:
      return DIGEST_sha224();
    case OID_rsasign_with_sha256:
    case OID_ecdsa_with_sha256:
      return DIGEST_sha256();
    case OID_rsasign_with_sha384:
    case OID_ecdsa_with_sha384:
      return DIGEST_sha384();
    case OID_rsasign_with_sha512:
    case OID_ecdsa_with_sha512:
      return DIGEST_sha512();
    case OID_rsasign_with_sm3:
    case OID_sm2sign_with_sm3:
      return DIGEST_sm3();
    default:
      return NULL;
  }
}

int cnet_tls_server_end_point_binding_from_certificate(
    const uint8_t *certificate, size_t certificate_size,
    uint8_t *output, size_t capacity, size_t *out_size) {
  uint8_t digest_bytes[DIGEST_MAX_SIZE];
  const DIGEST *digest_algor;
  size_t digest_size = 0u;
  int signature_algor = OID_undef;
  size_t clear_size;

  if (output == NULL || capacity == 0u || out_size == NULL ||
      certificate == NULL || certificate_size == 0u)
    return SALTS_EINVAL;
  clear_size = capacity < CNET_TLS_SERVER_END_POINT_MAX_BYTES
                   ? capacity
                   : CNET_TLS_SERVER_END_POINT_MAX_BYTES;
  memset(output, 0, clear_size);
  *out_size = 0u;

  if (x509_cert_get_signature_algor(certificate, certificate_size,
                                    &signature_algor) != 1)
    return SALTS_EPROTO;
  digest_algor = cnet_tls_server_end_point_digest(signature_algor);
  if (digest_algor == NULL ||
      digest_algor->digest_size > CNET_TLS_SERVER_END_POINT_MAX_BYTES)
    return SALTS_ENOTSUP;

  *out_size = digest_algor->digest_size;
  if (capacity < digest_algor->digest_size) return SALTS_EMSGSIZE;
  if (digest(digest_algor, certificate, certificate_size, digest_bytes,
             &digest_size) != 1 ||
      digest_size != digest_algor->digest_size) {
    memset(digest_bytes, 0, sizeof(digest_bytes));
    return SALTS_EIO;
  }
  memcpy(output, digest_bytes, digest_size);
  memset(digest_bytes, 0, sizeof(digest_bytes));
  return SALTS_OK;
}

int cnet_tls_state_server_end_point_binding(
    const cnet_tls_state *state, uint8_t *output, size_t capacity,
    size_t *out_size) {
  const uint8_t *certificate = NULL;
  size_t certificate_size = 0u;
  size_t clear_size;
  int result;

  if (output == NULL || capacity == 0u || out_size == NULL) return SALTS_EINVAL;
  clear_size = capacity < CNET_TLS_SERVER_END_POINT_MAX_BYTES
                   ? capacity
                   : CNET_TLS_SERVER_END_POINT_MAX_BYTES;
  memset(output, 0, clear_size);
  *out_size = 0u;
  if (state == NULL || CNET_TLS_ENGINE(state) == NULL ||
      !state->handshake_complete)
    return SALTS_ENOTCONN;

  result = tls_get_peer_certificate(&CNET_TLS_ENGINE(state)->connection,
                                    &certificate, &certificate_size);
  if (result < 0) return SALTS_EIO;
  if (result == 0) return SALTS_ENOENT;
  return cnet_tls_server_end_point_binding_from_certificate(
      certificate, certificate_size, output, capacity, out_size);
}

int cnet_tls_peer_certificate_chain_parse(
    const uint8_t *data, size_t size,
    cnet_tls_peer_certificate_chain *out_chain) {
  cnet_tls_peer_certificate_chain candidate = {0};
  size_t count = 0u;
  size_t index;
  size_t total = 0u;

  if (out_chain == NULL) return SALTS_EINVAL;
  memset(out_chain, 0, sizeof(*out_chain));
  if (data == NULL || size == 0u) return SALTS_ENOENT;
  if (size > CNET_TLS_PEER_CHAIN_MAX_BYTES) return SALTS_ERANGE;

  if (x509_certs_get_count(data, size, &count) != 1)
    return SALTS_EPROTO;
  if (count == 0u) return SALTS_EPROTO;
  if (count > CNET_TLS_PEER_CHAIN_MAX_CERTIFICATES)
    return SALTS_ERANGE;

  for (index = 0u; index < count; ++index) {
    const uint8_t *cert = NULL;
    size_t cert_size = 0u;
    size_t offset;
    if (x509_certs_get_cert_by_index(
            data, size, index, &cert, &cert_size) != 1 ||
        cert == NULL || cert_size == 0u ||
        cert < data || cert > data + size)
      return SALTS_EPROTO;
    offset = (size_t)(cert - data);
    if (cert_size > size - offset || total > size - cert_size)
      return SALTS_EPROTO;
    candidate.certificates[index].data = cert;
    candidate.certificates[index].size = cert_size;
    total += cert_size;
  }

  if (total != size) return SALTS_EPROTO;
  candidate.count = count;
  candidate.total_bytes = total;
  *out_chain = candidate;
  return SALTS_OK;
}

int cnet_tls_state_peer_certificate_chain(
    const cnet_tls_state *state,
    cnet_tls_peer_certificate_chain *out_chain) {
  const cnet_tls_gmssl_state *engine;
  if (out_chain == NULL) return SALTS_EINVAL;
  memset(out_chain, 0, sizeof(*out_chain));
  if (state == NULL || (engine = CNET_TLS_ENGINE(state)) == NULL)
    return SALTS_EINVAL;
  if (!state->handshake_complete) return SALTS_ENOTCONN;
  if (engine->connection.peer_cert_chain_len == 0u)
    return SALTS_ENOENT;
  if (engine->connection.peer_cert_chain_len >
      CNET_TLS_PEER_CHAIN_MAX_BYTES)
    return SALTS_ERANGE;
  return cnet_tls_peer_certificate_chain_parse(
      engine->connection.peer_cert_chain,
      engine->connection.peer_cert_chain_len,
      out_chain);
}

int cnet_tls_state_export_channel_binding(
    const cnet_tls_state *state,
    uint8_t output[CNET_TLS_CHANNEL_BINDING_BYTES]) {
  static const char exporter_label[] = "EXPORTER-Channel-Binding";
  if (output == NULL) return SALTS_EINVAL;
  memset(output, 0, CNET_TLS_CHANNEL_BINDING_BYTES);
  if (state == NULL || CNET_TLS_ENGINE(state) == NULL ||
      !state->handshake_complete)
    return SALTS_ENOTCONN;
  return tls_export_keying_material(
             &CNET_TLS_ENGINE(state)->connection, output,
             CNET_TLS_CHANNEL_BINDING_BYTES, exporter_label, NULL, 0u, 1) == 1
             ? SALTS_OK
             : SALTS_EIO;
}
