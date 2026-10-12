/*
 * Issue #1123: pinned GmSSL ShangMi provider-only contract.
 *
 * This binary deliberately links GmSSL::GmSSL, not Salts::CNet/Core. It is
 * evidence about the pinned *private provider*; it does not claim that CNet
 * exposes SM2/SM3/SM4, or that a real RFC 8998/TLCP handshake has passed.
 */
#include <gmssl/hkdf.h>
#include <gmssl/mem.h>
#include <gmssl/sm2.h>
#include <gmssl/sm3.h>
#include <gmssl/sm4.h>
#include <gmssl/tls.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* RFC 8998 exact registered identifiers; this is a pinned-provider ABI gate. */
_Static_assert(TLS_cipher_sm4_gcm_sm3 == 0x00c6, "wrong RFC 8998 SM4-GCM suite");
_Static_assert(TLS_sig_sm2sig_sm3 == 0x0708, "wrong RFC 8998 SM2 scheme");
_Static_assert(TLS_curve_sm2p256v1 == 41, "wrong RFC 8998 SM2 curve");

static int hex_digit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static int check_hex(const char *label, const uint8_t *value, size_t size,
                     const char *expected) {
  size_t i;
  for (i = 0; i < size; ++i) {
    int hi = hex_digit(expected[i * 2u]);
    int lo = hex_digit(expected[i * 2u + 1u]);
    if (hi < 0 || lo < 0 || value[i] != (uint8_t)((hi << 4) | lo)) {
      fprintf(stderr, "GmSSL ShangMi provider KAT failed: %s\n", label);
      return 0;
    }
  }
  return 1;
}

static int probe_sm3_hmac_hkdf(void) {
  static const uint8_t msg[] = {'a', 'b', 'c'};
  static const uint8_t hm_msg[] = "Hi There";
  static const uint8_t hkdf_salt[] = "salt";
  static const uint8_t hkdf_ikm[] = "input key material";
  static const uint8_t hkdf_info[] = "context";
  uint8_t key[20];
  uint8_t digest[SM3_DIGEST_SIZE];
  uint8_t mac[SM3_HMAC_SIZE];
  uint8_t prk[32];
  uint8_t okm[32];
  SM3_CTX sm3;
  SM3_HMAC_CTX hmac;
  int ok = 0;

  memset(key, 0x0bu, sizeof(key));
  sm3_init(&sm3);
  sm3_update(&sm3, msg, 1u);
  sm3_update(&sm3, msg + 1u, 2u);
  sm3_finish(&sm3, digest);
  if (!check_hex("SM3 abc", digest, sizeof(digest),
                 "66c7f0f462eeedd9d1f2d46bdc10e4e"
                 "24167c4875cf2f7a2297da02b8f4ba8e0"))
    goto done;

  sm3_hmac_init(&hmac, key, sizeof(key));
  sm3_hmac_update(&hmac, hm_msg, sizeof(hm_msg) - 1u);
  sm3_hmac_finish(&hmac, mac);
  if (!check_hex("HMAC-SM3", mac, sizeof(mac),
                 "51b00d1fb49832bfb01c3ce27848e59"
                 "f871d9ba938dc563b338ca964755cce70"))
    goto done;

  if (sm3_hkdf_extract(hkdf_salt, sizeof(hkdf_salt) - 1u,
                       hkdf_ikm, sizeof(hkdf_ikm) - 1u, prk) != 1)
    goto done;
  if (!check_hex("HKDF-SM3 extract", prk, sizeof(prk),
                 "8f09fbbd810b19aefe363b296b686568"
                 "6a3e2c9b2edaa6d310df72d029b5077b"))
    goto done;
  if (sm3_hkdf_expand(prk, hkdf_info, sizeof(hkdf_info) - 1u,
                      sizeof(okm), okm) != 1)
    goto done;
  if (!check_hex("HKDF-SM3 expand", okm, sizeof(okm),
                 "181ad115657b29aa306ec15487f83d95"
                 "f6a9e123b78de5fa2c2a4d6a299ec653"))
    goto done;
  ok = 1;
done:
  gmssl_secure_clear(&sm3, sizeof(sm3));
  gmssl_secure_clear(&hmac, sizeof(hmac));
  gmssl_secure_clear(key, sizeof(key));
  gmssl_secure_clear(prk, sizeof(prk));
  gmssl_secure_clear(okm, sizeof(okm));
  if (!ok) fprintf(stderr, "SM3/HMAC/HKDF provider check failed\n");
  return ok;
}

/* RFC 8998 Appendix A.1: independent on-wire SM4-GCM known-answer vector.
 * These bytes are not derived from the provider under test.
 */
static int probe_sm4_rfc8998_kat(void) {
  static const uint8_t raw_key[16] = {
      0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef,
      0xfe, 0xdc, 0xba, 0x98, 0x76, 0x54, 0x32, 0x10};
  static const uint8_t iv[12] = {
      0x00, 0x00, 0x12, 0x34, 0x56, 0x78,
      0x00, 0x00, 0x00, 0x00, 0xab, 0xcd};
  static const uint8_t aad[20] = {
      0xfe, 0xed, 0xfa, 0xce, 0xde, 0xad, 0xbe, 0xef,
      0xfe, 0xed, 0xfa, 0xce, 0xde, 0xad, 0xbe, 0xef,
      0xab, 0xad, 0xda, 0xd2};
  static const uint8_t plain[64] = {
      0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa,
      0xbb, 0xbb, 0xbb, 0xbb, 0xbb, 0xbb, 0xbb, 0xbb,
      0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc,
      0xdd, 0xdd, 0xdd, 0xdd, 0xdd, 0xdd, 0xdd, 0xdd,
      0xee, 0xee, 0xee, 0xee, 0xee, 0xee, 0xee, 0xee,
      0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
      0xee, 0xee, 0xee, 0xee, 0xee, 0xee, 0xee, 0xee,
      0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa};
  uint8_t ciphertext[sizeof(plain)];
  uint8_t decrypted[sizeof(plain)];
  uint8_t tag[16];
  SM4_KEY key;
  int ok = 0;

  sm4_set_encrypt_key(&key, raw_key);
  if (sm4_gcm_encrypt(&key, iv, sizeof(iv), aad, sizeof(aad),
                      plain, sizeof(plain), ciphertext, sizeof(tag), tag) != 1)
    goto done;
  if (!check_hex("RFC 8998 A.1 SM4-GCM ciphertext", ciphertext,
                 sizeof(ciphertext),
                 "17f399f08c67d5ee19d0dc9969c4bb7d"
                 "5fd46fd3756489069157b282bb200735"
                 "d82710ca5c22f0ccfa7cbf93d496ac15"
                 "a56834cbcf98c397b4024a2691233b8d"))
    goto done;
  if (!check_hex("RFC 8998 A.1 SM4-GCM tag", tag, sizeof(tag),
                 "83de3541e4c2b58177e065a9bf7b62ec"))
    goto done;
  if (sm4_gcm_decrypt(&key, iv, sizeof(iv), aad, sizeof(aad),
                      ciphertext, sizeof(ciphertext), tag, sizeof(tag),
                      decrypted) != 1 ||
      memcmp(decrypted, plain, sizeof(plain)) != 0)
    goto done;
  ok = 1;
done:
  gmssl_secure_clear(&key, sizeof(key));
  gmssl_secure_clear(decrypted, sizeof(decrypted));
  if (!ok) fprintf(stderr, "RFC 8998 A.1 SM4-GCM provider vector failed\n");
  return ok;
}

static int probe_sm4_gcm(void) {
  static const uint8_t raw_key[16] = {
      0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef,
      0xfe, 0xdc, 0xba, 0x98, 0x76, 0x54, 0x32, 0x10};
  static const uint8_t nonce[12] = {
      0x00, 0x01, 0x02, 0x03, 0x04, 0x05,
      0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b};
  static const uint8_t aad[] = {'C', 'N', 'e', 't', '-', 'P', '0'};
  static const uint8_t plain[] = {
      0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
      0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};
  uint8_t cipher[sizeof(plain)];
  uint8_t recovered[sizeof(plain)];
  uint8_t tag[16];
  uint8_t bad_tag[sizeof(tag)];
  uint8_t bad_aad[sizeof(aad)];
  SM4_KEY key;
  int ok = 0;

  sm4_set_encrypt_key(&key, raw_key);
  if (sm4_gcm_encrypt(&key, nonce, sizeof(nonce), aad, sizeof(aad),
                      plain, sizeof(plain), cipher, sizeof(tag), tag) != 1)
    goto done;
  memset(recovered, 0xa5, sizeof(recovered));
  if (sm4_gcm_decrypt(&key, nonce, sizeof(nonce), aad, sizeof(aad),
                      cipher, sizeof(cipher), tag, sizeof(tag), recovered) != 1 ||
      memcmp(recovered, plain, sizeof(plain)) != 0)
    goto done;

  memcpy(bad_tag, tag, sizeof(tag));
  bad_tag[0] ^= 0x01u;
  memset(recovered, 0xa5, sizeof(recovered));
  if (sm4_gcm_decrypt(&key, nonce, sizeof(nonce), aad, sizeof(aad),
                      cipher, sizeof(cipher), bad_tag, sizeof(bad_tag),
                      recovered) == 1)
    goto done;
  { size_t i; for (i = 0; i < sizeof(recovered); ++i)
      if (recovered[i] != 0xa5u) goto done; }

  memcpy(bad_aad, aad, sizeof(aad));
  bad_aad[0] ^= 0x01u;
  memset(recovered, 0xa5, sizeof(recovered));
  if (sm4_gcm_decrypt(&key, nonce, sizeof(nonce), bad_aad, sizeof(bad_aad),
                      cipher, sizeof(cipher), tag, sizeof(tag), recovered) == 1)
    goto done;
  { size_t i; for (i = 0; i < sizeof(recovered); ++i)
      if (recovered[i] != 0xa5u) goto done; }

  ok = 1;
done:
  gmssl_secure_clear(&key, sizeof(key));
  gmssl_secure_clear(recovered, sizeof(recovered));
  if (!ok) fprintf(stderr, "SM4-GCM authenticated decrypt check failed\n");
  return ok;
}


/* Fixed public-key and DER signature fixture created with independent
 * OpenSSL 3.5.5 SM2/SM3 using distid=1234567812345678.
 * The private key is NOT embedded. This is a verifier interoperability KAT,
 * independent from the GmSSL-generated-key roundtrip smoke below.
 */
static int decode_hex(const char *hex, uint8_t *output, size_t capacity,
                      size_t *out_size) {
  size_t len;
  size_t i;
  if (hex == NULL || output == NULL || out_size == NULL) return 0;
  len = strlen(hex);
  if ((len & 1u) != 0u || len / 2u > capacity) return 0;
  for (i = 0u; i < len / 2u; ++i) {
    int hi = hex_digit(hex[2u * i]);
    int lo = hex_digit(hex[2u * i + 1u]);
    if (hi < 0 || lo < 0) return 0;
    output[i] = (uint8_t)((hi << 4) | lo);
  }
  *out_size = len / 2u;
  return 1;
}

static int probe_sm2_fixed_verify(void) {
  static const char spki_hex[] =
      "3059301306072a8648ce3d020106082a811ccf5501822d03420004"
      "47d809769c27d8ceae286851d11d422463f02b8488edaab4cbe764f2b0f67ed4"
      "fbc7ef9ad7292ccc256c583b228581a45e173417eea77a22bdb866b0b664ae19";
  static const char signature_hex[] =
      "3045022079c5cffc33a5d8346a9ab0d7e3dadbcb7964ee0e4d6dfdf6bc532a58b6a9d1b9"
      "022100b4d528e48e3a5296d0b2e72fa9e1d1556f94c6f0d1ae7a5e422d28b61dfa6e79";
  static const char id[] = "1234567812345678";
  static const char wrong_id[] = "1234567812345679";
  static const uint8_t message[] = "CNet-ShangMi-SM2-KAT-v1";
  uint8_t spki[128] = {0};
  uint8_t signature[80] = {0};
  uint8_t altered_message[sizeof(message) - 1u];
  size_t spki_len = 0u;
  size_t sig_len = 0u;
  size_t remaining;
  const uint8_t *cursor;
  SM2_KEY key;
  SM2_VERIFY_CTX verify;
  int ok = 0;

  memset(&key, 0, sizeof(key));
  memset(&verify, 0, sizeof(verify));
  if (!decode_hex(spki_hex, spki, sizeof(spki), &spki_len) ||
      !decode_hex(signature_hex, signature, sizeof(signature), &sig_len))
    goto done;

  cursor = spki;
  remaining = spki_len;
  if (sm2_public_key_info_from_der(&key, &cursor, &remaining) != 1 ||
      remaining != 0u || cursor != spki + spki_len)
    goto done;

  if (sm2_verify_init(&verify, &key, id, sizeof(id) - 1u) != 1 ||
      sm2_verify_update(&verify, message, sizeof(message) - 1u) != 1 ||
      sm2_verify_finish(&verify, signature, sig_len) != 1)
    goto done;

  gmssl_secure_clear(&verify, sizeof(verify));
  if (sm2_verify_init(&verify, &key, wrong_id, sizeof(wrong_id) - 1u) != 1 ||
      sm2_verify_update(&verify, message, sizeof(message) - 1u) != 1 ||
      sm2_verify_finish(&verify, signature, sig_len) == 1)
    goto done;

  memcpy(altered_message, message, sizeof(altered_message));
  altered_message[0] ^= 1u;
  gmssl_secure_clear(&verify, sizeof(verify));
  if (sm2_verify_init(&verify, &key, id, sizeof(id) - 1u) != 1 ||
      sm2_verify_update(&verify, altered_message, sizeof(altered_message)) != 1 ||
      sm2_verify_finish(&verify, signature, sig_len) == 1)
    goto done;

  /* Mutation in the DER r integer must invalidate the signature. */
  signature[8] ^= 1u;
  gmssl_secure_clear(&verify, sizeof(verify));
  if (sm2_verify_init(&verify, &key, id, sizeof(id) - 1u) != 1 ||
      sm2_verify_update(&verify, message, sizeof(message) - 1u) != 1 ||
      sm2_verify_finish(&verify, signature, sig_len) == 1)
    goto done;
  ok = 1;
done:
  gmssl_secure_clear(&verify, sizeof(verify));
  gmssl_secure_clear(&key, sizeof(key));
  gmssl_secure_clear(spki, sizeof(spki));
  gmssl_secure_clear(signature, sizeof(signature));
  gmssl_secure_clear(altered_message, sizeof(altered_message));
  if (!ok) fprintf(stderr, "fixed OpenSSL SM2 verification probe failed\n");
  return ok;
}

static int probe_sm2_identity(void) {
  static const char user_id[] = "1234567812345678";
  static const char wrong_id[] = "1234567812345679";
  static const uint8_t message[] = "CNet-ShangMi-SM2-provider";
  SM2_KEY key;
  SM2_SIGN_CTX sign_ctx;
  SM2_VERIFY_CTX verify_ctx;
  uint8_t signature[SM2_MAX_SIGNATURE_SIZE];
  size_t signature_size = 0u;
  int ok = 0;

  memset(&key, 0, sizeof(key));
  memset(&sign_ctx, 0, sizeof(sign_ctx));
  memset(&verify_ctx, 0, sizeof(verify_ctx));
  if (sm2_key_generate(&key) != 1) goto done;
  if (sm2_sign_init(&sign_ctx, &key, user_id, sizeof(user_id) - 1u) != 1 ||
      sm2_sign_update(&sign_ctx, message, sizeof(message) - 1u) != 1 ||
      sm2_sign_finish(&sign_ctx, signature, &signature_size) != 1 ||
      signature_size == 0u || signature_size > sizeof(signature))
    goto done;

  if (sm2_verify_init(&verify_ctx, &key, user_id, sizeof(user_id) - 1u) != 1 ||
      sm2_verify_update(&verify_ctx, message, sizeof(message) - 1u) != 1 ||
      sm2_verify_finish(&verify_ctx, signature, signature_size) != 1)
    goto done;

  gmssl_secure_clear(&verify_ctx, sizeof(verify_ctx));
  if (sm2_verify_init(&verify_ctx, &key, wrong_id, sizeof(wrong_id) - 1u) != 1 ||
      sm2_verify_update(&verify_ctx, message, sizeof(message) - 1u) != 1)
    goto done;
  if (sm2_verify_finish(&verify_ctx, signature, signature_size) == 1)
    goto done;
  ok = 1;
done:
  gmssl_secure_clear(&sign_ctx, sizeof(sign_ctx));
  gmssl_secure_clear(&verify_ctx, sizeof(verify_ctx));
  gmssl_secure_clear(&key, sizeof(key));
  gmssl_secure_clear(signature, sizeof(signature));
  if (!ok) fprintf(stderr, "SM2 identity/signature provider check failed\n");
  return ok;
}

static int probe_protocol_configuration(int protocol, int cipher) {
  static const int groups[] = {TLS_curve_sm2p256v1};
  static const int sigs[] = {TLS_sig_sm2sig_sm3};
  const int other_protocol =
      protocol == TLS_protocol_tls13 ? TLS_protocol_tlcp : TLS_protocol_tls13;
  const int wrong_cipher =
      protocol == TLS_protocol_tls13 ? TLS_cipher_ecc_sm4_gcm_sm3
                                     : TLS_cipher_sm4_gcm_sm3;
  TLS_CTX ctx;
  size_t original_versions;
  int original_first;
  int ok = 0;

  if (tls_ctx_init(&ctx, protocol, TLS_client_mode) != 1) {
    fprintf(stderr, "GmSSL protocol context init failed: 0x%x\n", protocol);
    return 0;
  }
  original_versions = ctx.supported_versions_cnt;
  original_first = ctx.supported_versions[0];

  /* The new pinned provider 3.2.0#10 exposes a validated strict-version
   * setter; production CNet must not mutate TLS_CTX fields directly.
   * Reject the wrong profile without changing the existing version offer. */
  if (tls_ctx_set_strict_protocol(&ctx, other_protocol) == 1 ||
      ctx.supported_versions_cnt != original_versions ||
      ctx.supported_versions[0] != original_first)
    goto done;
  if (tls_ctx_set_strict_protocol(&ctx, protocol) != 1 ||
      ctx.protocol != protocol ||
      ctx.min_protocol != protocol ||
      ctx.max_protocol != protocol ||
      ctx.supported_versions_cnt != 1u ||
      ctx.supported_versions[0] != protocol)
    goto done;
  if (tls_ctx_set_cipher_suites(&ctx, &cipher, 1u) != 1 ||
      tls_ctx_set_supported_groups(&ctx, groups, 1u) != 1 ||
      tls_ctx_set_signature_algorithms(&ctx, sigs, 1u) != 1)
    goto done;

  /* A mismatched cipher must not replace the accepted suite; after policy
   * configuration has started, reapplying the setter is forbidden. */
  if (tls_ctx_set_cipher_suites(&ctx, &wrong_cipher, 1u) == 1 ||
      tls_ctx_set_strict_protocol(&ctx, protocol) == 1 ||
      ctx.protocol != protocol ||
      ctx.supported_versions_cnt != 1u ||
      ctx.supported_versions[0] != protocol ||
      ctx.cipher_suites_cnt != 1u || ctx.cipher_suites[0] != cipher ||
      ctx.supported_groups_cnt != 1u || ctx.supported_groups[0] != groups[0] ||
      ctx.signature_algorithms_cnt != 1u ||
      ctx.signature_algorithms[0] != sigs[0])
    goto done;

  ok = 1;
done:
  tls_ctx_cleanup(&ctx);
  if (!ok)
    fprintf(stderr, "GmSSL strict ShangMi protocol 0x%x check failed\n", protocol);
  return ok;
}

int main(void) {
  if (strcmp(TLS13_SM2_ID, "TLSv1.3+GM+Cipher+Suite") != 0 ||
      TLS13_SM2_ID_LENGTH != sizeof("TLSv1.3+GM+Cipher+Suite") - 1u) {
    fprintf(stderr, "RFC 8998 TLS 1.3 SM2 CertificateVerify identity mismatch\n");
    return 1;
  }
  if (!probe_sm3_hmac_hkdf()) return 1;
  if (!probe_sm4_rfc8998_kat()) return 1;
  if (!probe_sm4_gcm()) return 1;
  if (!probe_sm2_fixed_verify()) return 1;
  if (!probe_sm2_identity()) return 1;
  if (!probe_protocol_configuration(TLS_protocol_tls13,
                                    TLS_cipher_sm4_gcm_sm3))
    return 1;
  if (!probe_protocol_configuration(TLS_protocol_tlcp,
                                    TLS_cipher_ecc_sm4_gcm_sm3))
    return 1;
  puts("GmSSL ShangMi private provider: KAT, AEAD, SM2 and config probes passed");
  puts("NOTE: real RFC 8998 and TLCP handshakes remain separate open gates.");
  return 0;
}
