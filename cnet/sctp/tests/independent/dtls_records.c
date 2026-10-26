/* Test-only, REAL OpenSSL DTLS 1.2 application-record consumer of the
 * INSTALLED, opaque C11 CNetSctpPrototype::Session ABI.
 *
 * Both DTLS endpoints and both SCTP endpoints belong to this test process.
 * Each SCTP input is admitted ONLY after SSL_read authenticates/decrypts its
 * complete DTLS record, with both peer certificate SHA-256 fingerprints
 * checked. This is not independent browser/libwebrtc interoperability,
 * actual ICE/network peer testing, or a production DTLS Owner lease.
 * No private SCTP headers, production usrsctp or fallback are used. */
#include <cnet/sctp_session.h>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <openssl/crypto.h>
#include <openssl/ec.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

enum { DATAGRAM_CAPACITY = CNET_SCTP_SESSION_MAX_PACKET_BYTES };
typedef struct test_owner {
  unsigned char cookie_key[32];
  uint64_t generation;
} test_owner;

static int is_owner(void *p) { return p != NULL; }
static int entropy(void *p, uint8_t *output, size_t n) {
  (void)p;
  return output && n <= (size_t)INT_MAX &&
      RAND_bytes(output, (int)n) == 1 ? 0 : -1;
}
static int mac(void *p, const uint8_t *msg, size_t n,
    uint8_t output[CNET_SCTP_SESSION_MAC_BYTES]) {
  test_owner *owner = (test_owner *)p;
  unsigned int count = 0u;
  if (!owner || (!msg && n != 0u) || !output ||
      n > (size_t)INT_MAX)
    return -1;
  if (!HMAC(EVP_sha256(), owner->cookie_key, (int)sizeof(owner->cookie_key),
          msg, n, output, &count))
    return -1;
  return count == CNET_SCTP_SESSION_MAC_BYTES ? 0 : -1;
}
static int verify_mac(void *p, const uint8_t *msg, size_t n,
    const uint8_t claimed[CNET_SCTP_SESSION_MAC_BYTES]) {
  uint8_t digest[CNET_SCTP_SESSION_MAC_BYTES];
  int result;
  if (!claimed || mac(p, msg, n, digest) != 0) return -1;
  result = CRYPTO_memcmp(digest, claimed, sizeof(digest));
  OPENSSL_cleanse(digest, sizeof(digest));
  return result == 0 ? 0 : -1;
}

/* This callback admits test-generated self-signed chains so the fixture can
 * independently enforce the RFC 8827-style SHA-256 fingerprint, rather than
 * trusting an arbitrary self-signed peer certificate as an authority. */
static int allow_self_signed_for_fingerprint_test(int ok, X509_STORE_CTX *ctx) {
  (void)ok; (void)ctx;
  return 1;
}
static SSL_CTX *new_identity(uint8_t fingerprint[32]) {
  EVP_PKEY_CTX *key_context = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, NULL);
  EVP_PKEY *key = NULL;
  X509 *cert = NULL;
  X509_NAME *name;
  SSL_CTX *context;
  unsigned int digest_length = 0u;
  assert(key_context != NULL);
  assert(EVP_PKEY_keygen_init(key_context) > 0);
  assert(EVP_PKEY_CTX_set_ec_paramgen_curve_nid(key_context,
      NID_X9_62_prime256v1) > 0);
  assert(EVP_PKEY_keygen(key_context, &key) > 0);
  EVP_PKEY_CTX_free(key_context);

  cert = X509_new();
  assert(cert != NULL);
  assert(X509_set_version(cert, 2L) == 1);
  assert(ASN1_INTEGER_set(X509_get_serialNumber(cert), 1L) == 1);
  assert(X509_gmtime_adj(X509_get_notBefore(cert), -60L) != NULL);
  assert(X509_gmtime_adj(X509_get_notAfter(cert), 86400L) != NULL);
  assert(X509_set_pubkey(cert, key) == 1);
  name = X509_get_subject_name(cert);
  assert(name != NULL);
  assert(X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
      (const unsigned char *)"SCTP opaque DTLS test", -1, -1, 0) == 1);
  assert(X509_set_issuer_name(cert, name) == 1);
  assert(X509_sign(cert, key, EVP_sha256()) > 0);
  assert(X509_digest(cert, EVP_sha256(), fingerprint,
      &digest_length) == 1 && digest_length == 32u);

  context = SSL_CTX_new(DTLS_method());
  assert(context != NULL);
  assert(SSL_CTX_set_min_proto_version(context, DTLS1_2_VERSION) == 1);
  assert(SSL_CTX_set_max_proto_version(context, DTLS1_2_VERSION) == 1);
  SSL_CTX_set_verify(context,
      SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT,
      allow_self_signed_for_fingerprint_test);
  assert(SSL_CTX_use_certificate(context, cert) == 1);
  assert(SSL_CTX_use_PrivateKey(context, key) == 1);
  assert(SSL_CTX_check_private_key(context) == 1);
  X509_free(cert);
  EVP_PKEY_free(key);
  return context;
}
static SSL *new_dtls_peer(SSL_CTX *context, int is_server) {
  SSL *session = SSL_new(context);
  BIO *input = BIO_new(BIO_s_mem()), *output = BIO_new(BIO_s_mem());
  assert(session && input && output);
  BIO_set_mem_eof_return(input, -1);
  BIO_set_mem_eof_return(output, -1);
  SSL_set_bio(session, input, output);
  assert(SSL_set_mtu(session, 1200) == 1);
  if (is_server) SSL_set_accept_state(session);
  else SSL_set_connect_state(session);
  return session;
}
static size_t transfer_ciphertext(SSL *from, SSL *to) {
  uint8_t ciphertext[4096];
  int length;
  size_t total = 0u;
  while ((length = BIO_read(SSL_get_wbio(from),
          ciphertext, sizeof(ciphertext))) > 0) {
    assert(BIO_write(SSL_get_rbio(to), ciphertext, length) == length);
    total += (size_t)length;
  }
  assert(length <= 0);
  return total;
}
static void handshake(SSL *client, SSL *server) {
  unsigned int retry;
  for (retry = 0u; retry < 128u; ++retry) {
    int state, error;
    if (!SSL_is_init_finished(client)) {
      state = SSL_do_handshake(client);
      if (state != 1) {
        error = SSL_get_error(client, state);
        assert(error == SSL_ERROR_WANT_READ ||
            error == SSL_ERROR_WANT_WRITE);
      }
    }
    (void)transfer_ciphertext(client, server);
    if (!SSL_is_init_finished(server)) {
      state = SSL_do_handshake(server);
      if (state != 1) {
        error = SSL_get_error(server, state);
        assert(error == SSL_ERROR_WANT_READ ||
            error == SSL_ERROR_WANT_WRITE);
      }
    }
    (void)transfer_ciphertext(server, client);
    if (SSL_is_init_finished(client) && SSL_is_init_finished(server)) {
      assert(SSL_version(client) == DTLS1_2_VERSION);
      assert(SSL_version(server) == DTLS1_2_VERSION);
      return;
    }
  }
  assert(!"real DTLS 1.2 handshake failed before 128 bounded steps");
}
static int peer_matches(SSL *peer, const uint8_t expected[32]) {
  X509 *cert = SSL_get1_peer_certificate(peer);
  uint8_t actual[32];
  unsigned int len = 0u;
  int same;
  if (!cert) return 0;
  same = X509_digest(cert, EVP_sha256(), actual, &len) == 1 &&
      len == 32u && CRYPTO_memcmp(actual, expected, 32u) == 0;
  OPENSSL_cleanse(actual, sizeof(actual));
  X509_free(cert);
  return same;
}
static uint64_t bound_peer_generation(const uint8_t client[32],
    const uint8_t server[32]) {
  /* The immutable Owner generation ID derives from the two verified DTLS
   * certificate fingerprints. It is not a security key or MAC provider. */
  uint64_t value = UINT64_C(0xcbf29ce484222325);
  size_t i;
  for (i = 0u; i < 32u; ++i) {
    value ^= client[i];
    value *= UINT64_C(0x100000001b3);
  }
  for (i = 0u; i < 32u; ++i) {
    value ^= server[i];
    value *= UINT64_C(0x100000001b3);
  }
  assert(value != 0u);
  return value;
}
static cnet_sctp_session_create_config config(uint16_t port,
    uint64_t generation, int client, test_owner *owner) {
  cnet_sctp_session_create_config cfg = {0};
  cfg.abi_version = CNET_SCTP_SESSION_ABI_VERSION;
  cfg.struct_size = sizeof(cfg);
  cfg.local_port = port;
  cfg.remote_port = port == 5000u ? 5001u : 5000u;
  cfg.receive_window = 32768u;
  cfg.inbound_streams = cfg.outbound_streams = 4u;
  cfg.t1_ms = 100u;
  cfg.t1_max_ms = 400u;
  cfg.cookie_lifetime_ms = 5000u;
  cfg.peer_binding = generation;
  cfg.max_retransmits = 3u;
  cfg.is_dtls_client = (uint8_t)client;
  cfg.is_owner = is_owner;
  cfg.entropy = entropy;
  cfg.mac = mac;
  cfg.verify_mac = verify_mac;
  cfg.provider_context = owner;
  return cfg;
}
static size_t decrypt_packet(SSL *send, SSL *receive,
    const uint8_t *packet, size_t packet_size,
    uint8_t plaintext[DATAGRAM_CAPACITY]) {
  int count, got;
  assert(packet && packet_size > 0u &&
      packet_size < DATAGRAM_CAPACITY &&
      packet_size <= (size_t)INT_MAX);
  count = SSL_write(send, packet, (int)packet_size);
  assert(count == (int)packet_size);
  count = BIO_read(SSL_get_wbio(send), plaintext, DATAGRAM_CAPACITY);
  /* Confirm a REAL encrypted DTLS application record, not an
   * "authenticated=true" shim or pass-through plaintext. */
  assert(count > (int)packet_size);
  assert(plaintext[0] == 23u); /* DTLS application_data record type */
  assert(memcmp(plaintext, packet, packet_size) != 0);
  assert(BIO_write(SSL_get_rbio(receive), plaintext, count) == count);
  got = SSL_read(receive, plaintext, DATAGRAM_CAPACITY);
  assert(got == (int)packet_size);
  assert(memcmp(plaintext, packet, packet_size) == 0);
  return (size_t)got;
}
static size_t input_over_verified_dtls(cnet_sctp_session *recipient,
    SSL *sending_dtls, SSL *receiving_dtls, uint64_t now,
    const uint8_t *packet, size_t packet_size,
    uint8_t output[DATAGRAM_CAPACITY]) {
  uint8_t authenticated_plaintext[DATAGRAM_CAPACITY];
  size_t n = 0u;
  cnet_sctp_session_status status;
  const size_t decrypted_size = decrypt_packet(sending_dtls,
      receiving_dtls, packet, packet_size, authenticated_plaintext);
  status = cnet_sctp_session_input(recipient, now,
      authenticated_plaintext, decrypted_size,
      output, DATAGRAM_CAPACITY, &n);
  assert(status == CNET_SCTP_SESSION_OK);
  return n;
}
static void test_actual_dtls_application_records(void) {
  uint8_t client_fp[32], server_fp[32], incorrect[32];
  SSL_CTX *client_context = new_identity(client_fp);
  SSL_CTX *server_context = new_identity(server_fp);
  SSL *client_dtls = new_dtls_peer(client_context, 0);
  SSL *server_dtls = new_dtls_peer(server_context, 1);
  uint64_t peer_generation;
  test_owner a = {{0}, 0u}, b = {{0}, 0u};
  cnet_sctp_session_create_config ca, cb;
  cnet_sctp_session *client = NULL, *server = NULL;
  cnet_sctp_session_event event = {0};
  uint8_t first[DATAGRAM_CAPACITY], second[DATAGRAM_CAPACITY];
  uint8_t third[DATAGRAM_CAPACITY], fourth[DATAGRAM_CAPACITY];
  uint8_t application[CNET_SCTP_SESSION_MAX_MESSAGE_BYTES];
  size_t n1=0u, n2=0u, n3=0u, n4=0u;
  uint64_t old_generation = 0u;
  const char message[] = "dtls-opaque-c11";

  handshake(client_dtls, server_dtls);
  assert(peer_matches(client_dtls, server_fp));
  assert(peer_matches(server_dtls, client_fp));
  memcpy(incorrect, server_fp, sizeof(incorrect));
  incorrect[0] ^= 1u;
  assert(!peer_matches(client_dtls, incorrect));
  peer_generation = bound_peer_generation(client_fp, server_fp);
  a.generation = b.generation = peer_generation;
  assert(RAND_bytes(a.cookie_key, sizeof(a.cookie_key)) == 1);
  assert(RAND_bytes(b.cookie_key, sizeof(b.cookie_key)) == 1);
  ca = config(5000u, peer_generation, 1, &a);
  cb = config(5001u, peer_generation, 0, &b);
  assert(cnet_sctp_session_create(&ca, 0u, &client) ==
      CNET_SCTP_SESSION_OK);
  assert(cnet_sctp_session_create(&cb, 0u, &server) ==
      CNET_SCTP_SESSION_OK);
  puts("REAL_DTLS_1_2_FINGERPRINTS_VERIFIED");

  /* All four SCTP association handshake legs pass through authenticated
   * DTLS application records, never as direct SCTP fixture calls. */
  assert(cnet_sctp_session_start(client, 1u, first,
      sizeof(first), &n1) == CNET_SCTP_SESSION_OK && n1 > 0u);
  n2 = input_over_verified_dtls(server, client_dtls, server_dtls,
      2u, first, n1, second);
  assert(n2 > 0u);
  n3 = input_over_verified_dtls(client, server_dtls, client_dtls,
      3u, second, n2, third);
  assert(n3 > 0u);
  n4 = input_over_verified_dtls(server, client_dtls, server_dtls,
      4u, third, n3, fourth);
  assert(n4 > 0u);
  n1 = input_over_verified_dtls(client, server_dtls, client_dtls,
      5u, fourth, n4, first);
  assert(n1 == 0u);
  puts("REAL_DTLS_SCTP_INIT_COOKIE_HANDSHAKE");

  assert(cnet_sctp_session_open_channel(client, 6u, 0u,
      CNET_SCTP_SESSION_CHANNEL_RELIABLE_UNORDERED, 0u, 0u,
      "dtls", 4u, NULL, 0u,
      first, sizeof(first), &n1) == CNET_SCTP_SESSION_OK && n1 > 0u);
  n2 = input_over_verified_dtls(server, client_dtls, server_dtls,
      7u, first, n1, second);
  if (n2) {
    n3 = input_over_verified_dtls(client, server_dtls, client_dtls,
        8u, second, n2, third);
    assert(n3 == 0u);
  }
  assert(cnet_sctp_session_poll(server, 9u, &event,
      application, sizeof(application),
      second, sizeof(second), &n2) == CNET_SCTP_SESSION_OK &&
      event.kind == CNET_SCTP_SESSION_EVENT_CHANNEL_OPEN &&
      event.stream_id == 0u && event.generation != 0u && n2 > 0u);
  old_generation = event.generation;
  n3 = input_over_verified_dtls(client, server_dtls, client_dtls,
      10u, second, n2, third);
  if (n3) {
    n4 = input_over_verified_dtls(server, client_dtls, server_dtls,
        11u, third, n3, fourth);
    assert(n4 == 0u);
  }
  assert(cnet_sctp_session_poll(client, 12u, &event,
      application, sizeof(application),
      first, sizeof(first), &n1) == CNET_SCTP_SESSION_OK &&
      event.kind == CNET_SCTP_SESSION_EVENT_CHANNEL_OPEN &&
      event.generation != 0u && event.stream_id == 0u);
  puts("REAL_DTLS_DCEP_OPEN_ACK");

  n1 = 0u;
  assert(cnet_sctp_session_send(client, 13u, 0u, 1,
      message, sizeof(message) - 1u, first, sizeof(first),
      &n1) == CNET_SCTP_SESSION_OK && n1 > 0u &&
      first[12u] == 64u); /* Negotiated I-DATA, not legacy DATA. */
  n2 = input_over_verified_dtls(server, client_dtls, server_dtls,
      14u, first, n1, second);
  assert(n2 > 0u);
  n3 = input_over_verified_dtls(client, server_dtls, client_dtls,
      15u, second, n2, third);
  assert(n3 == 0u);
  assert(cnet_sctp_session_poll(server, 16u, &event,
      application, sizeof(application),
      fourth, sizeof(fourth), &n4) == CNET_SCTP_SESSION_OK &&
      event.kind == CNET_SCTP_SESSION_EVENT_MESSAGE &&
      event.stream_id == 0u && event.generation == old_generation &&
      event.ppid == 53u && event.binary &&
      event.message_size == sizeof(message) - 1u &&
      memcmp(application, message, sizeof(message) - 1u) == 0);
  puts("REAL_DTLS_OPAQUE_I_DATA_PPID53_RECEIVED");

  /* Test a FRESH encrypted DTLS record carrying old SCTP DATA after the
   * C11 receiving Owner has performed explicit abortive stop. The record
   * still decrypts correctly, but cannot resurrect the old Session/DTLS
   * generation. It must be refused by the SCTP Owner, not fed to usrsctp. */
  assert(cnet_sctp_session_stop(server) == CNET_SCTP_SESSION_OK);
  {
    uint8_t authenticated[DATAGRAM_CAPACITY];
    const size_t bytes = decrypt_packet(client_dtls, server_dtls,
        first, n1, authenticated);
    size_t discarded = 123u;
    assert(bytes == n1);
    assert(cnet_sctp_session_input(server, 17u,
        authenticated, bytes, fourth, sizeof(fourth),
        &discarded) == CNET_SCTP_SESSION_CLOSED &&
        discarded == 0u);
  }
  puts("REAL_DTLS_LATE_OLD_GENERATION_REJECTED");
  assert(cnet_sctp_session_stop(client) == CNET_SCTP_SESSION_OK);
  assert(cnet_sctp_session_drained(client) != 0);
  assert(cnet_sctp_session_drained(server) != 0);
  assert(cnet_sctp_session_destroy(client) == CNET_SCTP_SESSION_OK);
  assert(cnet_sctp_session_destroy(server) == CNET_SCTP_SESSION_OK);
  OPENSSL_cleanse(a.cookie_key, sizeof(a.cookie_key));
  OPENSSL_cleanse(b.cookie_key, sizeof(b.cookie_key));
  SSL_free(client_dtls);
  SSL_free(server_dtls);
  SSL_CTX_free(client_context);
  SSL_CTX_free(server_context);
}
int main(void) {
  test_actual_dtls_application_records();
  puts("REAL_DTLS_OPAQUE_INSTALLED_SESSION_PASS");
  return 0;
}
