#ifndef CNET_TLS_H
#define CNET_TLS_H

#include <cnet/cnet.h>

#include <stdbool.h>
#include <stddef.h>

enum {
  CNET_TLS_SERVER_NAME_CAPACITY = 254,
  CNET_TLS_PEER_CHAIN_MAX_CERTIFICATES = 5,
  CNET_TLS_PEER_CHAIN_MAX_BYTES = 4096
};

typedef struct cnet_tls_peer_certificate {
  const uint8_t *data;
  size_t size;
} cnet_tls_peer_certificate;

typedef struct cnet_tls_peer_certificate_chain {
  cnet_tls_peer_certificate certificates[CNET_TLS_PEER_CHAIN_MAX_CERTIFICATES];
  size_t count;
  size_t total_bytes;
} cnet_tls_peer_certificate_chain;

typedef struct cnet_tls_context cnet_tls_context;

typedef enum cnet_tls_protocol_version {
  CNET_TLS_PROTOCOL_VERSION_DEFAULT = 0,
  CNET_TLS_PROTOCOL_VERSION_1_2 = 12,
  CNET_TLS_PROTOCOL_VERSION_1_3 = 13
} cnet_tls_protocol_version;

typedef struct cnet_tls_state {
  cnet_tls_context *context;
  void *engine;
  /* Reserved for transport receive through terminal completion, never plaintext. */
  unsigned char *read_buffer;
  unsigned char *write_buffer;
  size_t io_buffer_bytes;
  size_t negotiated_alpn_size;
  unsigned char negotiated_alpn[CNET_TLS_ALPN_NAME_MAX_BYTES];
  bool server;
  bool handshake_complete;
  bool peer_close_notify;
  bool close_notify_started;
} cnet_tls_state;

int cnet_tls_client_context_create(const cnet_tls_client_config *config,
                                   cnet_tls_context **out_context);
cnet_tls_context *cnet_tls_client_context(const cnet_tls_client *client);
const char *cnet_tls_client_server_name(const cnet_tls_client *client);
cnet_tls_context *cnet_tls_server_context(const cnet_tls_server *server);
void cnet_tls_context_retain(cnet_tls_context *context);
void cnet_tls_context_release(cnet_tls_context *context);

/** Takes ownership of `context` only on success. */
int cnet_tls_state_init(cnet_tls_state *state, cnet_tls_context *context, bool server,
                        const char *server_name, size_t io_buffer_bytes);
void cnet_tls_state_destroy(cnet_tls_state *state);

int cnet_tls_handshake(cnet_tls_state *state, bool *out_complete);
bool cnet_tls_state_handshake_complete(const cnet_tls_state *state);
int cnet_tls_state_set_protocol_range(cnet_tls_state *state,
                                      cnet_tls_protocol_version minimum,
                                      cnet_tls_protocol_version maximum);
void *cnet_tls_state_read_buffer(cnet_tls_state *state);
void *cnet_tls_state_write_buffer(cnet_tls_state *state);
size_t cnet_tls_state_io_buffer_bytes(const cnet_tls_state *state);
size_t cnet_tls_cipher_input_capacity(const cnet_tls_state *state);
int cnet_tls_feed_cipher(cnet_tls_state *state, const void *data, size_t size);
#if defined(CNET_INTERNAL_TESTING)
/* Internal diagnostics only; compiled only into the test-profile archive. */
int cnet_tls_diagnostic_send_stats(const cnet_tls_state *state,
                                   size_t *calls, size_t *requested,
                                   size_t *accepted, size_t *queued);
#endif
int cnet_tls_take_cipher(cnet_tls_state *state, void *buffer, size_t capacity, size_t *out_size);
int cnet_tls_write(cnet_tls_state *state, const void *data, size_t size, bool *out_complete);
int cnet_tls_read(cnet_tls_state *state, void *buffer, size_t capacity, size_t *out_size,
                  bool *out_peer_closed);
/**
 * Advances TLS control state without consuming application plaintext.
 * out_plaintext_pending is true when application data is retained in the
 * engine's plaintext buffer until receive demand exists.
 */
int cnet_tls_probe_peer_close(cnet_tls_state *state, bool *out_peer_closed,
                              bool *out_plaintext_pending);
int cnet_tls_shutdown(cnet_tls_state *state, bool *out_notify_generated);
int cnet_tls_get_negotiated_alpn(const cnet_tls_state *state, const unsigned char **out_data,
                                 size_t *out_size);
int cnet_tls_state_negotiated_version(const cnet_tls_state *state, char *buffer, size_t capacity,
                                      size_t *out_size);
int cnet_tls_state_negotiated_cipher(const cnet_tls_state *state, char *buffer, size_t capacity,
                                     size_t *out_size);
int cnet_tls_state_peer_certificate_sha256(
    const cnet_tls_state *state,
    char buffer[CNET_TLS_PEER_CERTIFICATE_SHA256_CAPACITY]);
int cnet_tls_peer_certificate_chain_parse(
    const uint8_t *data, size_t size,
    cnet_tls_peer_certificate_chain *out_chain);
int cnet_tls_state_peer_certificate_chain(
    const cnet_tls_state *state,
    cnet_tls_peer_certificate_chain *out_chain);
int cnet_tls_server_end_point_binding_from_certificate(
    const uint8_t *certificate, size_t certificate_size,
    uint8_t *output, size_t capacity, size_t *out_size);
int cnet_tls_state_server_end_point_binding(
    const cnet_tls_state *state, uint8_t *output, size_t capacity,
    size_t *out_size);
int cnet_tls_state_export_channel_binding(
    const cnet_tls_state *state, uint8_t output[CNET_TLS_CHANNEL_BINDING_BYTES]);

#endif /* CNET_TLS_H */
