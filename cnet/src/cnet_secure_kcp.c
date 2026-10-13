#include <cnet/cnet.h>

#include "cnet_kcp_fec_internal.h"
#include "cnet_kcp_secure_internal.h"
#include "cnet_secure_kcp_internal.h"

#include "cnet_monocypher_namespace.h"
#include <monocypher.h>

#include <limits.h>
#include <stdlib.h>
#include <string.h>

enum {
  CNET_SECURE_KCP_MIN_MTU = 576,
  CNET_SECURE_KCP_MIN_INTERVAL_MS = 10,
  CNET_SECURE_KCP_MAX_INTERVAL_MS = 5000
};

typedef struct cnet_secure_kcp_impl {
  cnet_secure_kcp *owner;
  cnet_secure_kcp_role role;
  cnet_kcp_config kcp_template;
  cnet_kcp_security_config security;
  cnet_secure_kcp_observer observer;
  cnet_kcp_secure_state secure;
  cnet_kcp_fec_state *fec;
  cnet_kcp kcp;
  uint8_t *secure_send_buffer;
  uint8_t *secure_receive_buffer;
  size_t secure_buffer_size;
  uint32_t last_handshake_send_ms;
  bool started;
  bool kcp_initialized;
  bool established_notified;
  bool last_record_authenticated;
} cnet_secure_kcp_impl;

static cnet_secure_kcp_impl *cnet_secure_kcp_get(const cnet_secure_kcp *session) {
  return session != NULL ? (cnet_secure_kcp_impl *)session->impl : NULL;
}

static bool cnet_secure_kcp_key_valid(const uint8_t key[CNET_KCP_PSK_BYTES]) {
  uint8_t combined = 0u;
  size_t index;
  for (index = 0u; index < CNET_KCP_PSK_BYTES; ++index) combined |= key[index];
  return combined != 0u;
}

bool cnet_secure_kcp_transport_config_valid(const cnet_kcp_config *kcp,
                                            const cnet_kcp_security_config *security) {
  if (kcp == NULL || security == NULL) return false;
  if (kcp->size != sizeof(*kcp) || kcp->conversation != 0u ||
      kcp->mtu < CNET_SECURE_KCP_MIN_MTU || kcp->mtu > CNET_DATAGRAM_MAX_PAYLOAD_BYTES ||
      kcp->send_window == 0u || kcp->send_window > (uint32_t)INT_MAX ||
      kcp->receive_window == 0u || kcp->receive_window > (uint32_t)INT_MAX ||
      kcp->interval_ms < CNET_SECURE_KCP_MIN_INTERVAL_MS ||
      kcp->interval_ms > CNET_SECURE_KCP_MAX_INTERVAL_MS ||
      kcp->fast_resend > (uint32_t)INT_MAX || kcp->stream_mode ||
      kcp->send_segment_capacity == 0u || kcp->send_segment_capacity > (size_t)INT_MAX ||
      kcp->max_message_bytes == 0u || kcp->max_message_bytes > (size_t)INT_MAX ||
      kcp->observer.output != NULL || kcp->observer.on_receive != NULL ||
      kcp->observer.user != NULL)
    return false;
  if (security->size != sizeof(*security) || security->mode != CNET_KCP_SECURITY_PSK_V1 ||
      !cnet_secure_kcp_key_valid(security->pre_shared_key) ||
      security->handshake_retry_ms == 0u ||
      security->fec.max_payload_bytes < kcp->mtu + CNET_KCP_SECURE_RECORD_OVERHEAD)
    return false;
  return cnet_kcp_fec_config_validate(&security->fec) == SALTS_OK;
}

static bool cnet_secure_kcp_config_valid(const cnet_secure_kcp_config *config) {
  return config != NULL && config->size == sizeof(*config) &&
         (config->role == CNET_SECURE_KCP_CLIENT || config->role == CNET_SECURE_KCP_SERVER) &&
         config->observer.output != NULL && config->observer.on_receive != NULL &&
         cnet_secure_kcp_transport_config_valid(&config->kcp, &config->security);
}

static uint32_t cnet_secure_kcp_derive_conversation(uint64_t epoch) {
  uint32_t conversation = (uint32_t)epoch ^ (uint32_t)(epoch >> 32u);
  return conversation != 0u ? conversation : 1u;
}

static int cnet_secure_kcp_wire_output(void *user, const void *data, size_t size) {
  cnet_secure_kcp_impl *impl = (cnet_secure_kcp_impl *)user;
  int status;
  if (impl == NULL || data == NULL || size == 0u) return SALTS_EINVAL;
  status = impl->observer.output(impl->observer.user, impl->owner, data, size);
  return status <= SALTS_OK ? status : SALTS_EIO;
}

static void cnet_secure_kcp_receive(void *user, cnet_kcp *kcp,
                                    const cnet_receive_view *view) {
  cnet_secure_kcp_impl *impl = (cnet_secure_kcp_impl *)user;
  (void)kcp;
  impl->observer.on_receive(impl->observer.user, impl->owner, view);
}

static int cnet_secure_kcp_kcp_output(void *user, cnet_kcp *kcp, const void *data,
                                      size_t size) {
  cnet_secure_kcp_impl *impl = (cnet_secure_kcp_impl *)user;
  size_t record_size = 0u;
  int status;
  (void)kcp;
  status = cnet_kcp_secure_seal(&impl->secure, data, size, impl->secure_send_buffer,
                                impl->secure_buffer_size, &record_size);
  if (status != SALTS_OK) return status;
  return cnet_kcp_fec_send(impl->fec, impl->secure_send_buffer, record_size);
}

static int cnet_secure_kcp_activate(cnet_secure_kcp_impl *impl) {
  cnet_kcp_config config;
  int status;
  if (impl->kcp_initialized) return SALTS_OK;
  if (!impl->secure.established || impl->secure.session_epoch == 0u) return SALTS_EBUSY;
  status = cnet_kcp_fec_set_session(impl->fec, impl->secure.session_epoch, impl->secure.fec_key);
  if (status != SALTS_OK) return status;
  config = impl->kcp_template;
  config.conversation = cnet_secure_kcp_derive_conversation(impl->secure.session_epoch);
  config.observer.output = cnet_secure_kcp_kcp_output;
  config.observer.on_receive = cnet_secure_kcp_receive;
  config.observer.user = impl;
  status = cnet_kcp_init(&impl->kcp, &config);
  if (status == SALTS_OK) impl->kcp_initialized = true;
  return status;
}

static void cnet_secure_kcp_notify_established(cnet_secure_kcp_impl *impl) {
  if (impl->established_notified) return;
  impl->established_notified = true;
  if (impl->observer.on_established != NULL)
    impl->observer.on_established(impl->observer.user, impl->owner);
}

static int cnet_secure_kcp_send_client_hello(cnet_secure_kcp_impl *impl, uint32_t now_ms) {
  uint8_t hello[CNET_KCP_SECURE_HANDSHAKE_BYTES];
  int status = cnet_kcp_secure_build_client_hello(&impl->secure, hello);
  if (status == SALTS_OK) status = cnet_secure_kcp_wire_output(impl, hello, sizeof(hello));
  if (status == SALTS_OK) impl->last_handshake_send_ms = now_ms;
  crypto_wipe(hello, sizeof(hello));
  return status;
}

static int cnet_secure_kcp_deliver_record(void *user, const void *data, size_t size) {
  cnet_secure_kcp_impl *impl = (cnet_secure_kcp_impl *)user;
  size_t plain_size = 0u;
  int status;
  impl->last_record_authenticated = false;
  status = cnet_kcp_secure_open(&impl->secure, data, size, impl->secure_receive_buffer,
                                impl->secure_buffer_size, &plain_size);
  if (status != SALTS_OK) return status;
  impl->last_record_authenticated = true;
  return cnet_kcp_input(&impl->kcp, impl->secure_receive_buffer, plain_size);
}

int cnet_secure_kcp_init(cnet_secure_kcp *session, const cnet_secure_kcp_config *config) {
  cnet_secure_kcp_impl *impl;
  int status;
  if (session == NULL) return SALTS_EINVAL;
  if (session->impl != NULL) return SALTS_EALREADY;
  if (!cnet_secure_kcp_config_valid(config)) return SALTS_EINVAL;
  impl = (cnet_secure_kcp_impl *)calloc(1u, sizeof(*impl));
  if (impl == NULL) return SALTS_ENOMEM;
  impl->secure_buffer_size = config->security.fec.max_payload_bytes;
  impl->secure_send_buffer = (uint8_t *)malloc(impl->secure_buffer_size);
  impl->secure_receive_buffer = (uint8_t *)malloc(impl->secure_buffer_size);
  if (impl->secure_send_buffer == NULL || impl->secure_receive_buffer == NULL) {
    free(impl->secure_receive_buffer);
    free(impl->secure_send_buffer);
    free(impl);
    return SALTS_ENOMEM;
  }
  impl->owner = session;
  impl->role = config->role;
  impl->kcp_template = config->kcp;
  impl->security = config->security;
  impl->observer = config->observer;
  status = cnet_kcp_secure_state_init(&impl->secure, config->role,
                                      config->security.pre_shared_key);
  if (status == SALTS_OK)
    status = cnet_kcp_fec_init(&config->security.fec, cnet_secure_kcp_wire_output, impl,
                               &impl->fec);
  if (status != SALTS_OK) {
    cnet_kcp_secure_state_wipe(&impl->secure);
    crypto_wipe(&impl->security, sizeof(impl->security));
    free(impl->secure_receive_buffer);
    free(impl->secure_send_buffer);
    free(impl);
    return status;
  }
  session->impl = impl;
  return SALTS_OK;
}

int cnet_secure_kcp_start(cnet_secure_kcp *session, uint32_t now_ms) {
  cnet_secure_kcp_impl *impl = cnet_secure_kcp_get(session);
  int status;
  if (impl == NULL) return SALTS_EINVAL;
  if (impl->started) return SALTS_EALREADY;
  if (impl->role == CNET_SECURE_KCP_CLIENT) {
    status = cnet_secure_kcp_send_client_hello(impl, now_ms);
    if (status != SALTS_OK) return status;
  }
  impl->started = true;
  return SALTS_OK;
}

bool cnet_secure_kcp_established(const cnet_secure_kcp *session) {
  cnet_secure_kcp_impl *impl = cnet_secure_kcp_get(session);
  return impl != NULL && impl->kcp_initialized;
}

int cnet_secure_kcp_conversation(const cnet_secure_kcp *session, uint32_t *out_conversation) {
  cnet_secure_kcp_impl *impl = cnet_secure_kcp_get(session);
  if (out_conversation == NULL) return SALTS_EINVAL;
  *out_conversation = 0u;
  if (impl == NULL) return SALTS_EINVAL;
  if (!impl->kcp_initialized) return SALTS_EBUSY;
  *out_conversation = cnet_secure_kcp_derive_conversation(impl->secure.session_epoch);
  return SALTS_OK;
}

int cnet_secure_kcp_send(cnet_secure_kcp *session, const void *data, size_t size) {
  cnet_secure_kcp_impl *impl = cnet_secure_kcp_get(session);
  if (impl == NULL || data == NULL || size == 0u) return SALTS_EINVAL;
  if (!impl->started || !impl->kcp_initialized) return SALTS_EBUSY;
  return cnet_kcp_send(&impl->kcp, data, size);
}

int cnet_secure_kcp_send_validate(const cnet_secure_kcp *session, const void *data, size_t size,
                                  bool marked) {
  cnet_secure_kcp_impl *impl = cnet_secure_kcp_get(session);
  if (impl == NULL || data == NULL || size == 0u) return SALTS_EINVAL;
  if (!impl->started || !impl->kcp_initialized) return SALTS_EBUSY;
  return cnet_kcp_send_validate(&impl->kcp, data, size, marked);
}

int cnet_secure_kcp_send_marked(cnet_secure_kcp *session, const void *data, size_t size,
                                cnet_kcp_send_marker *out_marker) {
  cnet_secure_kcp_impl *impl = cnet_secure_kcp_get(session);
  if (out_marker != NULL) *out_marker = (cnet_kcp_send_marker){0};
  if (impl == NULL || out_marker == NULL || data == NULL || size == 0u) return SALTS_EINVAL;
  if (!impl->started || !impl->kcp_initialized) return SALTS_EBUSY;
  return cnet_kcp_send_marked(&impl->kcp, data, size, out_marker);
}

bool cnet_secure_kcp_send_marker_complete(const cnet_secure_kcp *session,
                                          cnet_kcp_send_marker marker) {
  cnet_secure_kcp_impl *impl = cnet_secure_kcp_get(session);
  return impl != NULL && impl->kcp_initialized && cnet_kcp_send_marker_complete(&impl->kcp, marker);
}

int cnet_secure_kcp_input_classified(cnet_secure_kcp *session, const void *data, size_t size,
                                     bool *out_authenticated) {
  cnet_secure_kcp_impl *impl = cnet_secure_kcp_get(session);
  int status;
  if (out_authenticated == NULL) return SALTS_EINVAL;
  *out_authenticated = false;
  if (impl == NULL || data == NULL || size == 0u) return SALTS_EINVAL;
  if (!impl->started) return SALTS_EBUSY;
  impl->last_record_authenticated = false;
  if (cnet_kcp_secure_is_handshake(data, size)) {
    if (impl->role == CNET_SECURE_KCP_CLIENT) {
      status = cnet_kcp_secure_accept_server_hello(&impl->secure, data, size);
      if (status != SALTS_OK) return status;
      status = cnet_secure_kcp_activate(impl);
      if (status == SALTS_OK) cnet_secure_kcp_notify_established(impl);
      return status;
    }
    {
      uint8_t response[CNET_KCP_SECURE_HANDSHAKE_BYTES];
      status = cnet_kcp_secure_accept_client_hello(&impl->secure, data, size, response);
      if (status == SALTS_OK) status = cnet_secure_kcp_activate(impl);
      if (status == SALTS_OK)
        status = cnet_secure_kcp_wire_output(impl, response, sizeof(response));
      if (status == SALTS_OK) cnet_secure_kcp_notify_established(impl);
      crypto_wipe(response, sizeof(response));
      return status;
    }
  }
  if (!impl->kcp_initialized) return SALTS_EBUSY;
  status = cnet_kcp_fec_input(impl->fec, data, size, cnet_secure_kcp_deliver_record, impl);
  *out_authenticated = impl->last_record_authenticated;
  return status;
}

int cnet_secure_kcp_input(cnet_secure_kcp *session, const void *data, size_t size) {
  bool authenticated;
  return cnet_secure_kcp_input_classified(session, data, size, &authenticated);
}

int cnet_secure_kcp_update(cnet_secure_kcp *session, uint32_t now_ms) {
  cnet_secure_kcp_impl *impl = cnet_secure_kcp_get(session);
  if (impl == NULL) return SALTS_EINVAL;
  if (!impl->started) return SALTS_EBUSY;
  if (impl->kcp_initialized) return cnet_kcp_update(&impl->kcp, now_ms);
  if (impl->role == CNET_SECURE_KCP_CLIENT &&
      (uint32_t)(now_ms - impl->last_handshake_send_ms) >= impl->security.handshake_retry_ms)
    return cnet_secure_kcp_send_client_hello(impl, now_ms);
  return SALTS_OK;
}

int cnet_secure_kcp_terminal_status(const cnet_secure_kcp *session) {
  cnet_secure_kcp_impl *impl = cnet_secure_kcp_get(session);
  if (impl == NULL) return SALTS_EINVAL;
  if (!impl->started || !impl->kcp_initialized) return SALTS_EBUSY;
  return cnet_kcp_terminal_status(&impl->kcp);
}

int cnet_secure_kcp_check(const cnet_secure_kcp *session, uint32_t now_ms,
                          uint32_t *out_next_ms) {
  cnet_secure_kcp_impl *impl = cnet_secure_kcp_get(session);
  if (out_next_ms == NULL) return SALTS_EINVAL;
  *out_next_ms = 0u;
  if (impl == NULL) return SALTS_EINVAL;
  if (!impl->started) return SALTS_EBUSY;
  if (impl->kcp_initialized) return cnet_kcp_check(&impl->kcp, now_ms, out_next_ms);
  *out_next_ms = impl->role == CNET_SECURE_KCP_CLIENT
                     ? impl->last_handshake_send_ms + impl->security.handshake_retry_ms
                     : UINT32_MAX;
  return SALTS_OK;
}

int cnet_secure_kcp_destroy(cnet_secure_kcp *session) {
  cnet_secure_kcp_impl *impl;
  if (session == NULL) return SALTS_EINVAL;
  impl = cnet_secure_kcp_get(session);
  if (impl == NULL) return SALTS_OK;
  if (impl->kcp_initialized) (void)cnet_kcp_destroy(&impl->kcp);
  cnet_kcp_fec_destroy(impl->fec);
  cnet_kcp_secure_state_wipe(&impl->secure);
  crypto_wipe(&impl->security, sizeof(impl->security));
  crypto_wipe(impl->secure_send_buffer, impl->secure_buffer_size);
  crypto_wipe(impl->secure_receive_buffer, impl->secure_buffer_size);
  free(impl->secure_receive_buffer);
  free(impl->secure_send_buffer);
  free(impl);
  session->impl = NULL;
  return SALTS_OK;
}
