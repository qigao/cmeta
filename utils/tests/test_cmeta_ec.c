#include "cmeta_crypto.h"
#include "tinytest.h"

#include <stdint.h>
#include <string.h>

static uint8_t hex_nibble(char value) {
  if (value >= '0' && value <= '9') return (uint8_t)(value - '0');
  return (uint8_t)(value - 'a' + 10);
}

static void decode_hex(const char *hex, uint8_t *output, size_t size) {
  size_t i;
  for (i = 0u; i < size; ++i)
    output[i] = (uint8_t)((hex_nibble(hex[i * 2u]) << 4) |
                          hex_nibble(hex[i * 2u + 1u]));
}

spec("Core elliptic-curve crypto") {
  it("matches RFC 8032 Ed25519 test vector 1") {
    uint8_t seed[SALTS_ED25519_SEED_BYTES];
    uint8_t expected_public[SALTS_ED25519_PUBLIC_KEY_BYTES];
    uint8_t expected_signature[SALTS_ED25519_SIGNATURE_BYTES];
    uint8_t public_key[SALTS_ED25519_PUBLIC_KEY_BYTES];
    uint8_t signature[SALTS_ED25519_SIGNATURE_BYTES];

    decode_hex("9d61b19deffd5a60ba844af492ec2cc4"
               "4449c5697b326919703bac031cae7f60", seed, sizeof(seed));
    decode_hex("d75a980182b10ab7d54bfed3c964073a"
               "0ee172f3daa62325af021a68f707511a",
               expected_public, sizeof(expected_public));
    decode_hex("e5564300c360ac729086e2cc806e828a"
               "84877f1eb8e5d974d873e06522490155"
               "5fb8821590a33bacc61e39701cf9b46b"
               "d25bf5f0595bbe24655141438e7a100b",
               expected_signature, sizeof(expected_signature));

    check_equal(cmeta_ed25519_public_key(seed, public_key), SALTS_OK);
    check_equal(public_key, expected_public, sizeof(public_key));
    check_equal(cmeta_ed25519_sign(seed, NULL, 0u, signature), SALTS_OK);
    check_equal(signature, expected_signature, sizeof(signature));
    check_equal(cmeta_ed25519_verify(public_key, NULL, 0u, signature), SALTS_OK);
    signature[0] ^= 1u;
    check_equal(cmeta_ed25519_verify(public_key, NULL, 0u, signature),
                SALTS_EAUTH);
  }

  it("matches RFC 7748 X25519 key agreement vectors") {
    uint8_t alice_private[32], alice_public[32], expected_alice_public[32];
    uint8_t bob_private[32], bob_public[32], expected_bob_public[32];
    uint8_t shared[32], expected_shared[32], rejected[32];
    uint8_t low_order[32] = {0};

    decode_hex("77076d0a7318a57d3c16c17251b26645"
               "df4c2f87ebc0992ab177fba51db92c2a",
               alice_private, sizeof(alice_private));
    decode_hex("8520f0098930a754748b7ddcb43ef75a"
               "0dbf3a0d26381af4eba4a98eaa9b4e6a",
               expected_alice_public, sizeof(expected_alice_public));
    decode_hex("5dab087e624a8a4b79e17f8b83800ee6"
               "6f3bb1292618b6fd1c2f8b27ff88e0eb",
               bob_private, sizeof(bob_private));
    decode_hex("de9edb7d7b7dc1b4d35b61c2ece43537"
               "3f8343c85b78674dadfc7e146f882b4f",
               expected_bob_public, sizeof(expected_bob_public));
    decode_hex("4a5d9d5ba4ce2de1728e3bf480350f25"
               "e07e21c947d19e3376f09b3c1e161742",
               expected_shared, sizeof(expected_shared));

    check_equal(cmeta_x25519_public_key(alice_private, alice_public), SALTS_OK);
    check_equal(alice_public, expected_alice_public, sizeof(alice_public));
    check_equal(cmeta_x25519_public_key(bob_private, bob_public), SALTS_OK);
    check_equal(bob_public, expected_bob_public, sizeof(bob_public));
    check_equal(cmeta_x25519_derive(alice_private, bob_public, shared), SALTS_OK);
    check_equal(shared, expected_shared, sizeof(shared));
    memset(rejected, 0xa5, sizeof(rejected));
    check_equal(cmeta_x25519_derive(alice_private, low_order, rejected),
                SALTS_EAUTH);
    check_equal(rejected[0], 0xa5u);
  }

  it("signs and verifies canonical DER ECDSA P-256 signatures") {
    static const uint8_t message[] = "Salts ECDSA P-256";
    uint8_t private_key[32] = {0};
    uint8_t public_key[65], expected_public[65];
    uint8_t signature[SALTS_ECDSA_P256_SIGNATURE_DER_MAX_BYTES];
    uint8_t unchanged[65];
    size_t signature_size = 0u;

    private_key[31] = 1u;
    decode_hex("046b17d1f2e12c4247f8bce6e563a440"
               "f277037d812deb33a0f4a13945d898c296"
               "4fe342e2fe1a7f9b8ee7eb4a7c0f9e1"
               "62bce33576b315ececbb6406837bf51f5",
               expected_public, sizeof(expected_public));
    check_equal(cmeta_ecdsa_p256_public_key(private_key, public_key), SALTS_OK);
    check_equal(public_key, expected_public, sizeof(public_key));
    check_equal(cmeta_ecdsa_p256_sign_sha256(
                    private_key, message, sizeof(message) - 1u,
                    signature, sizeof(signature), &signature_size),
                SALTS_OK);
    check_true(signature_size > 0u && signature_size <= sizeof(signature));
    check_equal(cmeta_ecdsa_p256_verify_sha256(
                    public_key, message, sizeof(message) - 1u,
                    signature, signature_size),
                SALTS_OK);
    signature[signature_size - 1u] ^= 1u;
    check_equal(cmeta_ecdsa_p256_verify_sha256(
                    public_key, message, sizeof(message) - 1u,
                    signature, signature_size),
                SALTS_EAUTH);

    memset(private_key, 0, sizeof(private_key));
    memset(unchanged, 0xa5, sizeof(unchanged));
    check_equal(cmeta_ecdsa_p256_public_key(private_key, unchanged),
                SALTS_EINVAL);
    check_equal(unchanged[0], 0xa5u);
  }

  it("reports ECDSA signature capacity without producing a signature") {
    uint8_t private_key[32] = {0};
    uint8_t signature[8];
    size_t required = 0u;
    private_key[31] = 1u;
    memset(signature, 0xa5, sizeof(signature));
    check_equal(cmeta_ecdsa_p256_sign_sha256(
                    private_key, NULL, 0u, signature, sizeof(signature),
                    &required),
                SALTS_ERANGE);
    check_equal(required,
                (size_t)SALTS_ECDSA_P256_SIGNATURE_DER_MAX_BYTES);
    check_equal(signature[0], 0xa5u);
  }
}
