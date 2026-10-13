#include "tinytest.h"
#include <gmssl/tls.h>
#include <stdlib.h>
#include <string.h>

/* Exercise the actual provider with authenticated records and controllable I/O. */
typedef struct framing_fixture {
  TLS_CONNECT conn;
  TLS_CTX ctx;
  uint8_t wire[4096];
  size_t size, available, offset;
  uint8_t write_sequence[8];
} framing_fixture;

static const uint8_t extensions[] = {8, 0, 0, 2, 0, 0};

static tls_ret_t framing_recv(void *user, void *data, size_t size, int flags) {
  framing_fixture *f = user;
  size_t count = f->available - f->offset;
  (void)flags;
  if (!count) return TLS_ERROR_RECV_AGAIN;
  if (count > size) count = size;
  memcpy(data, f->wire + f->offset, count);
  f->offset += count;
  return (tls_ret_t)count;
}

static tls_ret_t framing_send(void *user, const void *data, size_t size, int flags) {
  (void)user; (void)data; (void)flags;
  return (tls_ret_t)size;
}

static int framing_init(framing_fixture *f) {
  const uint8_t key[16] = {0}; /* Deterministic test-only traffic key. */
  TLS_IO io = {f, framing_send, framing_recv};
  memset(f, 0, sizeof(*f));
  f->conn.ctx = &f->ctx;
  /* This fixture consumes the peer's server traffic as a TLS client.
   * Fatal Alerts use our client-write AEAD epoch, never the inbound
   * server read epoch. Keep the fail-closed authentication tag test. */
  f->ctx.is_client = TLS_client_mode;
  f->conn.protocol = TLS_protocol_tls13;
  f->conn.cipher_suite = TLS_cipher_aes_128_gcm_sha256;
  f->conn.digest = DIGEST_sha256();
  f->conn.cipher = BLOCK_CIPHER_aes128();
  if (block_cipher_set_encrypt_key(&f->conn.server_write_key, BLOCK_CIPHER_aes128(), key) != 1 ||
      block_cipher_set_encrypt_key(&f->conn.client_write_key, BLOCK_CIPHER_aes128(), key) != 1 ||
      digest_init(&f->conn.dgst_ctx, DIGEST_sha256()) != 1) return -1;
  return tls_set_io(&f->conn, &io);
}

static int framing_append(framing_fixture *f, int type, const uint8_t *data, size_t size) {
  uint8_t plain[TLS_MAX_RECORD_SIZE] = {0};
  uint8_t encrypted[TLS_MAX_RECORD_SIZE];
  size_t encrypted_size;
  if (size > TLS_MAX_PLAINTEXT_SIZE) return -1;
  plain[0] = (uint8_t)type; plain[1] = 3; plain[2] = 3;
  plain[3] = (uint8_t)(size >> 8); plain[4] = (uint8_t)size;
  if (size) memcpy(plain + 5, data, size);
  if (tls13_record_encrypt(f->conn.cipher_suite, &f->conn.server_write_key,
      f->conn.server_write_iv, f->write_sequence, plain, size + 5, 0,
      encrypted, &encrypted_size) != 1) return -1;
  if (encrypted_size > sizeof(f->wire) - f->size) return -1;
  memcpy(f->wire + f->size, encrypted, encrypted_size);
  f->size += encrypted_size;
  tls_seq_num_incr(f->write_sequence);
  return 1;
}

static int framing_digest_matches(framing_fixture *f, const uint8_t *data, size_t size) {
  DIGEST_CTX actual = f->conn.dgst_ctx;
  DIGEST_CTX expected;
  uint8_t a[64], b[64];
  size_t an, bn;
  return digest_init(&expected, DIGEST_sha256()) == 1 &&
      digest_update(&expected, data, size) == 1 &&
      digest_finish(&actual, a, &an) == 1 && digest_finish(&expected, b, &bn) == 1 &&
      an == bn && memcmp(a, b, an) == 0;
}

spec("TLS 1.3 provider handshake framing") {
  static framing_fixture *f;
  before_each() {
    f = calloc(1, sizeof(*f));
    check(f != NULL);
    check_equal(framing_init(f), 1);
  }
  after_each() { free(f); f = NULL; }

  it("consumes coalesced messages separately and hashes only the current message") {
    uint8_t flight[2 * sizeof(extensions)];
    memcpy(flight, extensions, sizeof(extensions));
    memcpy(flight + sizeof(extensions), extensions, sizeof(extensions));
    check_equal(framing_append(f, TLS_record_handshake, flight, sizeof(flight)), 1);
    f->available = f->size;
    check_equal(tls13_recv_encrypted_extensions(&f->conn), 1);
    check(framing_digest_matches(f, extensions, sizeof(extensions)));
    check_equal(f->conn.server_seq_num[7], 1);
    /* Optional CertificateRequest must leave the following message for its owner. */
    check_equal(tls13_recv_certificate_request(&f->conn), 0);
    check(framing_digest_matches(f, extensions, sizeof(extensions)));
    check_equal(tls13_recv_encrypted_extensions(&f->conn), 1);
    check(framing_digest_matches(f, flight, sizeof(flight)));
    check_equal(f->conn.server_seq_num[7], 1);
    check_equal(tls13_recv_encrypted_extensions(&f->conn), TLS_ERROR_RECV_AGAIN);
  }

  it("reassembles every header and body split while retrying at each wire byte") {
    for (size_t split = 1; split < sizeof(extensions); ++split) {
      check_equal(framing_init(f), 1);
      check_equal(framing_append(f, TLS_record_handshake, extensions, split), 1);
      check_equal(framing_append(f, TLS_record_handshake, extensions + split,
                                sizeof(extensions) - split), 1);
      for (f->available = 0; f->available < f->size; ++f->available)
        check_equal(tls13_recv_encrypted_extensions(&f->conn), TLS_ERROR_RECV_AGAIN);
      check_equal(tls13_recv_encrypted_extensions(&f->conn), 1);
      check(framing_digest_matches(f, extensions, sizeof(extensions)));
      check_equal(f->conn.server_seq_num[7], 2);
    }
  }

  it("rejects an oversized handshake length before waiting for its body") {
    const uint8_t oversized[] = {8, 0xff, 0xff, 0xff};
    check_equal(framing_append(f, TLS_record_handshake, oversized, sizeof(oversized)), 1);
    f->available = f->size;
    check_equal(tls13_recv_encrypted_extensions(&f->conn), -1);
    check(framing_digest_matches(f, extensions, 0));
  }

  it("accepts coalesced client Certificate and Finished with the correct transcript") {
    const uint8_t certificate[] = {11, 0, 0, 4, 0, 0, 0, 0};
    uint8_t flight[sizeof(certificate) + 4 + 64];
    uint8_t verify_data[64];
    size_t verify_size;
    DIGEST_CTX transcript = f->conn.dgst_ctx;
    f->ctx.client_certificate_optional = 1;
    check_equal(digest_update(&transcript, certificate, sizeof(certificate)), 1);
    check_equal(tls13_compute_verify_data(f->conn.client_handshake_traffic_secret,
                                         &transcript, verify_data, &verify_size), 1);
    check_equal(verify_size, 32u);
    memcpy(flight, certificate, sizeof(certificate));
    flight[8] = TLS_handshake_finished;
    flight[9] = 0; flight[10] = 0; flight[11] = (uint8_t)verify_size;
    memcpy(flight + 12, verify_data, verify_size);
    check_equal(framing_append(f, TLS_record_handshake, flight, 12 + verify_size), 1);
    f->available = f->size;
    check_equal(tls13_recv_client_certificate(&f->conn), 0);
    check(framing_digest_matches(f, certificate, sizeof(certificate)));
    check_equal(f->conn.client_seq_num[7], 1);
    check_equal(tls13_recv_client_finished(&f->conn), 1);
    check(framing_digest_matches(f, flight, 12 + verify_size));
    check_equal(f->conn.client_seq_num[7], 0); /* Application keys reset the epoch. */
  }

  it("rejects empty handshake fragments") {
    check_equal(framing_append(f, TLS_record_handshake, extensions, 0), 1);
    f->available = f->size;
    check_equal(tls13_recv_encrypted_extensions(&f->conn), -1);
    check(framing_digest_matches(f, extensions, 0));
  }

  it("rejects application data interleaved with an incomplete handshake") {
    check_equal(framing_append(f, TLS_record_handshake, extensions, 2), 1);
    check_equal(framing_append(f, TLS_record_application_data, extensions + 2, 4), 1);
    f->available = f->size;
    check_equal(tls13_recv_encrypted_extensions(&f->conn), -1);
    check(framing_digest_matches(f, extensions, 0));
  }

  it("rejects trailing bytes after Finished before changing keys or the transcript") {
    uint8_t flight[4 + 32 + sizeof(extensions)] = {20, 0, 0, 32};
    memcpy(flight + 36, extensions, sizeof(extensions));
    check_equal(framing_append(f, TLS_record_handshake, flight, sizeof(flight)), 1);
    f->available = f->size;
    check_equal(tls13_recv_encrypted_extensions(&f->conn), -1);
    check(framing_digest_matches(f, extensions, 0));
  }

  it("rejects corrupt authentication tags without accepting handshake bytes") {
    check_equal(framing_append(f, TLS_record_handshake, extensions, sizeof(extensions)), 1);
    f->wire[f->size - 1] ^= 1;
    f->available = f->size;
    check_equal(tls13_recv_encrypted_extensions(&f->conn), -1);
    /* The rejected peer ciphertext must not spend its server nonce.
     * The locally emitted authenticated fatal Alert spends one client nonce. */
    check_equal(f->conn.server_seq_num[7], 0);
    check_equal(f->conn.client_seq_num[7], 1);
    check(framing_digest_matches(f, extensions, 0));
  }
}
