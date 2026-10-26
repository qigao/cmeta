/* Private POSIX/OpenSSL DTLS 1.2 adapter for independent Pion SCTP tests.
 * Never installed or referenced by Salts::CNet or production TurboMedia. */
#ifndef CNET_SCTP_TEST_PION_OPENSSL_DTLS_H
#define CNET_SCTP_TEST_PION_OPENSSL_DTLS_H

#include <stddef.h>
#include <stdint.h>
#include <netinet/in.h>
#include <openssl/ssl.h>

typedef struct pion_test_dtls {
  SSL_CTX *context;
  SSL *ssl;
  uint8_t expected_client_fingerprint[32];
  uint8_t accepted;
} pion_test_dtls;

/* Prints the SHA256 of the freshly created, self-signed server identity.
 * The test's Go launcher must pin that digest before Client handshake. */
int pion_test_dtls_init(pion_test_dtls *transport, const char *client_fp_hex);

/* Receive one complete encrypted DTLS UDP datagram. Returns 1 only for
 * authenticated application plaintext after the expected peer fingerprint
 * is verified; 0 for a handshake/empty record; -1 fail-closed. */
int pion_test_dtls_input(pion_test_dtls *transport, int socket_fd,
    const struct sockaddr_in *peer, const uint8_t *ciphertext,
    size_t ciphertext_size, uint8_t *plaintext, size_t capacity,
    size_t *plaintext_size);

/* Encrypt one whole SCTP datagram into OpenSSL DTLS application records. */
int pion_test_dtls_send(pion_test_dtls *transport, int socket_fd,
    const struct sockaddr_in *peer, const uint8_t *plaintext, size_t size);
void pion_test_dtls_stop(pion_test_dtls *transport);
#endif
