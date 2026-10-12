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
  if (!ok) fprintf(stderr, "RFC 8998 A.1 SM4-GCM provider vector failed\\n");
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
  TLS_CTX ctx;
  int ok = 0;
  if (tls_ctx_init(&ctx, protocol, TLS_client_mode) != 1) {
    fprintf(stderr, "GmSSL protocol context init failed: 0x%x\n", protocol);
    return 0;
  }
  if (tls_ctx_set_cipher_suites(&ctx, &cipher, 1u) == 1 &&
      tls_ctx_set_supported_groups(&ctx, groups, 1u) == 1 &&
      tls_ctx_set_signature_algorithms(&ctx, sigs, 1u) == 1)
    ok = 1;
  tls_ctx_cleanup(&ctx);
  if (!ok)
    fprintf(stderr, "GmSSL cannot configure ShangMi protocol 0x%x\n", protocol);
  return ok;
}

int main(void) {
  if (!probe_sm3_hmac_hkdf()) return 1;
  if (!probe_sm4_rfc8998_kat()) return 1;
  if (!probe_sm4_gcm()) return 1;
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
