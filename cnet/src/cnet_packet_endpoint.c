#include <cnet/cnet.h>

#include "cnet_kcp_fec_internal.h"
#include "cnet_kcp_internal.h"
#include "cnet_kcp_secure_internal.h"
#include "cnet_secure_kcp_internal.h"
#if defined(CNET_INTERNAL_TESTING)
  #include "cnet_test_internal.h"
#endif

#include <cstl/hash_map.h>
#include "cnet_monocypher_namespace.h"
#include <monocypher.h>
#include <salts/clock.h>

#include <limits.h>
#include <stdlib.h>
#include <string.h>

enum {
  CNET_PACKET_KCP_WIRE_HEADER_BYTES = 24,
  CNET_PACKET_KCP_MIN_MTU = 50,
  CNET_PACKET_KCP_MIN_INTERVAL_MS = 10,
  CNET_PACKET_KCP_MAX_INTERVAL_MS = 5000,
  CNET_PACKET_KCP_COMMAND_PUSH = 81,
  CNET_PACKET_KCP_COMMAND_ACK = 82,
  CNET_PACKET_KCP_COMMAND_WINDOW_PROBE = 83,
  CNET_PACKET_KCP_COMMAND_WINDOW_REPORT = 84
};

typedef struct cnet_packet_key {
  cnet_datagram_peer peer;
  uint32_t conversation;
} cnet_packet_key;

typedef struct cnet_packet_endpoint_impl cnet_packet_endpoint_impl;

typedef struct cnet_packet_send_operation {
  cnet_packet_session session;
  cnet_kcp_send_marker marker;
  size_t size;
  uint64_t tag;
  uint32_t generation;
  uint32_t next;
  int terminal_status;
  bool active;
  bool notify;
} cnet_packet_send_operation;

typedef struct cnet_packet_record {
  cnet_packet_endpoint_impl *endpoint;
  cnet_packet_key key;
  cnet_kcp kcp;
  cnet_secure_kcp secure_kcp;
  size_t active_datagram_sends;
  size_t pending_terminal_callbacks;
  uint32_t kcp_send_head;
  uint32_t kcp_send_tail;
  uint32_t derived_conversation;
  uint32_t generation;
  bool occupied;
  bool closing;
  bool kcp_initialized;
  bool secure_kcp_initialized;
} cnet_packet_record;

struct cnet_packet_endpoint_impl {
  cnet_packet_endpoint *owner;
  cnet_datagram datagram;
  cnet_packet_record *records;
  uint32_t *free_records;
  cnet_packet_send_operation *send_operations;
  uint32_t *free_send_operations;
  hash_map_t peer_index;
  size_t session_capacity;
  size_t free_record_count;
  size_t send_operation_capacity;
  size_t free_send_operation_count;
  size_t tagged_send_count;
  size_t max_datagram_bytes;
  uint32_t ready_send_head;
  uint32_t ready_send_tail;
  cnet_packet_protocol protocol;
  cnet_kcp_config kcp_template;
  cnet_kcp_security_config security_template;
  cnet_packet_observer observer;
  cnet_packet_terminal_config terminal;
  size_t callback_depth;
  int pending_status;
  int stop_status;
  bool polling;
  bool stopping;
  bool stopped;
};

static cnet_packet_endpoint_impl *cnet_packet_get(const cnet_packet_endpoint *endpoint) {
  return endpoint != NULL ? (cnet_packet_endpoint_impl *)endpoint->impl : NULL;
}

#if defined(CNET_INTERNAL_TESTING)
int cnet_test_packet_endpoint_fail_next_datagram_drive(cnet_packet_endpoint *endpoint,
                                                       int status) {
  cnet_packet_endpoint_impl *impl = cnet_packet_get(endpoint);
  if (impl == NULL) return SALTS_EINVAL;
  return cnet_test_datagram_fail_next_drive(&impl->datagram, status);
}
#endif

static uint32_t cnet_packet_decode_u32_le(const unsigned char *input) {
  return (uint32_t)input[0] | ((uint32_t)input[1] << 8u) | ((uint32_t)input[2] << 16u) |
         ((uint32_t)input[3] << 24u);
}

static bool cnet_packet_kcp_wire_valid(const void *data, size_t size, uint32_t conversation) {
  const unsigned char *cursor = (const unsigned char *)data;
  size_t remaining = size;
  while (remaining != 0u) {
    uint32_t payload_size;
    unsigned char command;
    if (remaining < CNET_PACKET_KCP_WIRE_HEADER_BYTES ||
        cnet_packet_decode_u32_le(cursor) != conversation)
      return false;
    command = cursor[4];
    if (command != CNET_PACKET_KCP_COMMAND_PUSH && command != CNET_PACKET_KCP_COMMAND_ACK &&
        command != CNET_PACKET_KCP_COMMAND_WINDOW_PROBE &&
        command != CNET_PACKET_KCP_COMMAND_WINDOW_REPORT)
      return false;
    payload_size = cnet_packet_decode_u32_le(cursor + 20u);
    if ((size_t)payload_size > remaining - CNET_PACKET_KCP_WIRE_HEADER_BYTES) return false;
    cursor += CNET_PACKET_KCP_WIRE_HEADER_BYTES + (size_t)payload_size;
    remaining -= CNET_PACKET_KCP_WIRE_HEADER_BYTES + (size_t)payload_size;
  }
  return true;
}

static bool cnet_packet_peer_normalize(const cnet_datagram_peer *peer,
                                       cnet_datagram_peer *output) {
  size_t address_size;
  if (peer == NULL || output == NULL || peer->port == 0u) return false;
  if (peer->family == CNET_DATAGRAM_ADDRESS_IPV4)
    address_size = 4u;
  else if (peer->family == CNET_DATAGRAM_ADDRESS_IPV6)
    address_size = 16u;
  else
    return false;
  memset(output, 0, sizeof(*output));
  output->family = peer->family;
  output->port = peer->port;
  output->scope_id = peer->family == CNET_DATAGRAM_ADDRESS_IPV6 ? peer->scope_id : 0u;
  memcpy(output->address, peer->address, address_size);
  return true;
}

static bool cnet_packet_key_make(cnet_packet_protocol protocol, bool secure_kcp,
                                 const cnet_datagram_peer *peer, uint32_t conversation,
                                 cnet_packet_key *output) {
  if (output == NULL ||
      (protocol == CNET_PACKET_UDP && conversation != 0u) ||
      (protocol == CNET_PACKET_KCP && secure_kcp && conversation != 0u) ||
      (protocol == CNET_PACKET_KCP && !secure_kcp && conversation == 0u))
    return false;
  memset(output, 0, sizeof(*output));
  output->conversation = conversation;
  return cnet_packet_peer_normalize(peer, &output->peer);
}

static size_t cnet_packet_key_hash(const void *key_value, size_t key_size, void *context) {
  const cnet_packet_key *key = (const cnet_packet_key *)key_value;
  const unsigned char *fields[] = {(const unsigned char *)&key->peer.family,
                                   (const unsigned char *)&key->peer.port,
                                   (const unsigned char *)&key->peer.scope_id,
                                   key->peer.address,
                                   (const unsigned char *)&key->conversation};
  const size_t sizes[] = {sizeof(key->peer.family), sizeof(key->peer.port),
                          sizeof(key->peer.scope_id), sizeof(key->peer.address),
                          sizeof(key->conversation)};
  size_t hash = (size_t)UINT32_C(2166136261);
  size_t field;
  (void)key_size;
  (void)context;
  for (field = 0u; field < sizeof(fields) / sizeof(fields[0]); ++field) {
    size_t index;
    for (index = 0u; index < sizes[field]; ++index) {
      hash ^= fields[field][index];
      hash *= (size_t)UINT32_C(16777619);
    }
  }
  return hash;
}

static bool cnet_packet_key_equal(const void *left_value, const void *right_value, size_t key_size,
                                  void *context) {
  const cnet_packet_key *left = (const cnet_packet_key *)left_value;
  const cnet_packet_key *right = (const cnet_packet_key *)right_value;
  (void)key_size;
  (void)context;
  return left->conversation == right->conversation && left->peer.family == right->peer.family &&
         left->peer.port == right->peer.port && left->peer.scope_id == right->peer.scope_id &&
         memcmp(left->peer.address, right->peer.address, sizeof(left->peer.address)) == 0;
}

static int cnet_packet_stl_status(stl_status status) {
  if (status == STL_OK) return SALTS_OK;
  if (status == STL_OUT_OF_MEMORY) return SALTS_ENOMEM;
  if (status == STL_CAPACITY_EXCEEDED) return SALTS_ENOBUFS;
  if (status == STL_NOT_FOUND) return SALTS_ENOENT;
  return SALTS_EINVAL;
}

bool cnet_packet_session_valid(cnet_packet_session session) {
  return session.slot != 0u && session.generation != 0u;
}

static cnet_packet_session cnet_packet_record_handle(const cnet_packet_endpoint_impl *impl,
                                                      const cnet_packet_record *record) {
  cnet_packet_session result = {0};
  if (impl == NULL || record == NULL) return result;
  result.slot = (uint32_t)((size_t)(record - impl->records) + 1u);
  result.generation = record->generation;
  return result;
}

static uint64_t cnet_packet_handle_tag(cnet_packet_session session) {
  return ((uint64_t)session.generation << 32u) | session.slot;
}

static cnet_packet_session cnet_packet_tag_handle(uint64_t tag) {
  return (cnet_packet_session){(uint32_t)tag, (uint32_t)(tag >> 32u)};
}

static cnet_packet_record *cnet_packet_record_from_handle(cnet_packet_endpoint_impl *impl,
                                                           cnet_packet_session session) {
  cnet_packet_record *record;
  if (impl == NULL || !cnet_packet_session_valid(session) ||
      (size_t)session.slot > impl->session_capacity)
    return NULL;
  record = &impl->records[session.slot - 1u];
  return record->occupied && record->generation == session.generation ? record : NULL;
}

static uint64_t cnet_packet_send_operation_tag(const cnet_packet_send_operation *operation,
                                               const cnet_packet_endpoint_impl *impl) {
  const uint32_t slot = (uint32_t)((size_t)(operation - impl->send_operations) + 1u);
  return ((uint64_t)operation->generation << 32u) | slot;
}

static cnet_packet_send_operation *
cnet_packet_send_operation_from_tag(cnet_packet_endpoint_impl *impl, uint64_t tag) {
  const uint32_t slot = (uint32_t)tag;
  const uint32_t generation = (uint32_t)(tag >> 32u);
  cnet_packet_send_operation *operation;
  if (impl == NULL || slot == 0u || generation == 0u ||
      (size_t)slot > impl->send_operation_capacity)
    return NULL;
  operation = &impl->send_operations[slot - 1u];
  return operation->active && operation->generation == generation ? operation : NULL;
}

static int cnet_packet_send_operation_acquire(cnet_packet_endpoint_impl *impl,
                                              cnet_packet_session session, size_t size,
                                              uint64_t tag, bool notify,
                                              cnet_packet_send_operation **out_operation) {
  cnet_packet_send_operation *operation;
  size_t index;
  if (out_operation == NULL) return SALTS_EINVAL;
  *out_operation = NULL;
  if (impl->send_operations == NULL) return SALTS_ENOTSUP;
  if (notify && impl->tagged_send_count >= impl->terminal.send_capacity) return SALTS_ENOBUFS;
  if (impl->free_send_operation_count == 0u) return SALTS_ENOBUFS;
  index = impl->free_send_operations[impl->free_send_operation_count - 1u];
  operation = &impl->send_operations[index];
  --impl->free_send_operation_count;
  if (++operation->generation == 0u) ++operation->generation;
  operation->session = session;
  operation->marker = (cnet_kcp_send_marker){0};
  operation->size = size;
  operation->tag = tag;
  operation->next = 0u;
  operation->terminal_status = SALTS_OK;
  operation->notify = notify;
  operation->active = true;
  if (notify) ++impl->tagged_send_count;
  *out_operation = operation;
  return SALTS_OK;
}

static void cnet_packet_send_operation_release(cnet_packet_endpoint_impl *impl,
                                               cnet_packet_send_operation *operation) {
  const size_t index = (size_t)(operation - impl->send_operations);
  if (!operation->active) return;
  if (operation->notify && impl->tagged_send_count != 0u) --impl->tagged_send_count;
  operation->session = (cnet_packet_session){0};
  operation->marker = (cnet_kcp_send_marker){0};
  operation->size = 0u;
  operation->tag = 0u;
  operation->next = 0u;
  operation->terminal_status = SALTS_OK;
  operation->notify = false;
  operation->active = false;
  impl->free_send_operations[impl->free_send_operation_count++] = (uint32_t)index;
}

static void cnet_packet_send_operation_ready(cnet_packet_endpoint_impl *impl,
                                             cnet_packet_record *record,
                                             cnet_packet_send_operation *operation, int status) {
  const uint32_t slot = (uint32_t)((size_t)(operation - impl->send_operations) + 1u);
  operation->terminal_status = status < SALTS_OK ? status : SALTS_OK;
  operation->next = 0u;
  if (impl->ready_send_tail == 0u) impl->ready_send_head = slot;
  else impl->send_operations[impl->ready_send_tail - 1u].next = slot;
  impl->ready_send_tail = slot;
  ++record->pending_terminal_callbacks;
}

static void cnet_packet_dispatch_ready_sends(cnet_packet_endpoint_impl *impl) {
  while (impl->callback_depth == 0u && impl->ready_send_head != 0u) {
    const uint32_t slot = impl->ready_send_head;
    cnet_packet_send_operation *operation = &impl->send_operations[slot - 1u];
    cnet_packet_record *record = cnet_packet_record_from_handle(impl, operation->session);
    const cnet_packet_session session = operation->session;
    const size_t size = operation->size;
    const int status = operation->terminal_status;
    const uint64_t tag = operation->tag;
    impl->ready_send_head = operation->next;
    if (impl->ready_send_head == 0u) impl->ready_send_tail = 0u;
    cnet_packet_send_operation_release(impl, operation);
    ++impl->callback_depth;
    impl->terminal.on_send(impl->terminal.user, impl->owner, session, size, status, tag);
    --impl->callback_depth;
    if (record != NULL && record->pending_terminal_callbacks != 0u)
      --record->pending_terminal_callbacks;
  }
}

static void cnet_packet_sweep_closed(cnet_packet_endpoint_impl *impl);

static void cnet_packet_callback_leave(cnet_packet_endpoint_impl *impl) {
  --impl->callback_depth;
  if (impl->callback_depth != 0u) return;
  cnet_packet_dispatch_ready_sends(impl);
}

static cnet_packet_record *cnet_packet_record_from_key(cnet_packet_endpoint_impl *impl,
                                                        const cnet_packet_key *key) {
  const uint32_t *slot = (const uint32_t *)hash_map_get_const(&impl->peer_index, key);
  if (slot == NULL || *slot == 0u || (size_t)*slot > impl->session_capacity) return NULL;
  return impl->records[*slot - 1u].occupied ? &impl->records[*slot - 1u] : NULL;
}

static uint32_t cnet_packet_record_conversation(const cnet_packet_record *record) {
  return record->derived_conversation != 0u ? record->derived_conversation
                                            : record->key.conversation;
}

static void cnet_packet_user_state(cnet_packet_endpoint_impl *impl, cnet_packet_record *record,
                                   cnet_packet_session_state state) {
  if (impl->observer.on_state == NULL) return;
  ++impl->callback_depth;
  impl->observer.on_state(impl->observer.user, impl->owner,
                          cnet_packet_record_handle(impl, record), state, &record->key.peer,
                          cnet_packet_record_conversation(record));
  cnet_packet_callback_leave(impl);
}

static void cnet_packet_user_error(cnet_packet_endpoint_impl *impl, cnet_packet_session session,
                                   int status) {
  if (status >= SALTS_OK) status = SALTS_EIO;
  if (impl->pending_status == SALTS_OK) impl->pending_status = status;
  if (impl->observer.on_error == NULL) return;
  ++impl->callback_depth;
  impl->observer.on_error(impl->observer.user, impl->owner, session, status);
  cnet_packet_callback_leave(impl);
}

static void cnet_packet_record_append_kcp_send(cnet_packet_endpoint_impl *impl,
                                               cnet_packet_record *record,
                                               cnet_packet_send_operation *operation) {
  const uint32_t slot = (uint32_t)((size_t)(operation - impl->send_operations) + 1u);
  operation->next = 0u;
  if (record->kcp_send_tail == 0u) record->kcp_send_head = slot;
  else impl->send_operations[record->kcp_send_tail - 1u].next = slot;
  record->kcp_send_tail = slot;
}

static bool cnet_packet_record_kcp_marker_complete(const cnet_packet_record *record,
                                                   cnet_kcp_send_marker marker) {
  if (record->secure_kcp_initialized)
    return cnet_secure_kcp_send_marker_complete(&record->secure_kcp, marker);
  return record->kcp_initialized && cnet_kcp_send_marker_complete(&record->kcp, marker);
}

static void cnet_packet_record_complete_kcp_sends(cnet_packet_endpoint_impl *impl,
                                                  cnet_packet_record *record) {
  while (record->kcp_send_head != 0u) {
    cnet_packet_send_operation *operation = &impl->send_operations[record->kcp_send_head - 1u];
    if (!cnet_packet_record_kcp_marker_complete(record, operation->marker)) break;
    record->kcp_send_head = operation->next;
    if (record->kcp_send_head == 0u) record->kcp_send_tail = 0u;
    cnet_packet_send_operation_ready(impl, record, operation, SALTS_OK);
  }
  cnet_packet_dispatch_ready_sends(impl);
}

static void cnet_packet_record_cancel_kcp_sends(cnet_packet_endpoint_impl *impl,
                                                cnet_packet_record *record, int status) {
  while (record->kcp_send_head != 0u) {
    cnet_packet_send_operation *operation = &impl->send_operations[record->kcp_send_head - 1u];
    record->kcp_send_head = operation->next;
    cnet_packet_send_operation_ready(impl, record, operation, status);
  }
  record->kcp_send_tail = 0u;
}

static void cnet_packet_record_finalize(cnet_packet_endpoint_impl *impl,
                                        cnet_packet_record *record) {
  const size_t index = (size_t)(record - impl->records);
  if (!record->occupied || !record->closing || record->active_datagram_sends != 0u ||
      record->kcp_send_head != 0u || record->pending_terminal_callbacks != 0u)
    return;
  if (record->kcp_initialized) {
    (void)cnet_kcp_destroy(&record->kcp);
    record->kcp_initialized = false;
  }
  if (record->secure_kcp_initialized) {
    (void)cnet_secure_kcp_destroy(&record->secure_kcp);
    record->secure_kcp_initialized = false;
  }
  if (hash_map_remove(&impl->peer_index, &record->key, NULL) != STL_OK) {
    cnet_packet_user_error(impl, cnet_packet_record_handle(impl, record), SALTS_EPROTO);
  }
  cnet_packet_user_state(impl, record, CNET_PACKET_SESSION_CLOSED);
  memset(&record->key, 0, sizeof(record->key));
  record->derived_conversation = 0u;
  record->occupied = false;
  record->closing = false;
  impl->free_records[impl->free_record_count++] = (uint32_t)index;
}

static void cnet_packet_sweep_closed(cnet_packet_endpoint_impl *impl) {
  size_t index;
  if (impl->callback_depth != 0u) return;
  for (index = 0u; index < impl->session_capacity; ++index)
    cnet_packet_record_finalize(impl, &impl->records[index]);
}

static void cnet_packet_record_begin_close(cnet_packet_endpoint_impl *impl,
                                           cnet_packet_record *record, int terminal_status) {
  if (record->closing) return;
  record->closing = true;
  cnet_packet_record_complete_kcp_sends(impl, record);
  cnet_packet_record_cancel_kcp_sends(impl, record, terminal_status);
  if (impl->callback_depth == 0u && record->kcp_initialized) {
    (void)cnet_kcp_destroy(&record->kcp);
    record->kcp_initialized = false;
  }
  if (impl->callback_depth == 0u && record->secure_kcp_initialized) {
    (void)cnet_secure_kcp_destroy(&record->secure_kcp);
    record->secure_kcp_initialized = false;
  }
  cnet_packet_dispatch_ready_sends(impl);
  cnet_packet_sweep_closed(impl);
}

static void cnet_packet_record_fail(cnet_packet_endpoint_impl *impl,
                                    cnet_packet_record *record, int status) {
  const cnet_packet_session session = cnet_packet_record_handle(impl, record);
  if (impl->terminal.on_send == NULL) {
    cnet_packet_user_error(impl, session, status);
    if (record->occupied && !record->closing)
      cnet_packet_record_begin_close(impl, record, status);
    return;
  }
  ++impl->callback_depth;
  cnet_packet_record_begin_close(impl, record, status);
  cnet_packet_user_error(impl, session, status);
  cnet_packet_callback_leave(impl);
  cnet_packet_sweep_closed(impl);
}

static int cnet_packet_record_output(cnet_packet_record *record, const void *data, size_t size) {
  cnet_packet_endpoint_impl *impl = record->endpoint;
  cnet_packet_session handle;
  int status;
  if (!record->occupied || record->closing || impl->stopping) return SALTS_ESHUTDOWN;
  handle = cnet_packet_record_handle(impl, record);
  status = cnet_datagram_send(&impl->datagram, &record->key.peer, data, size,
                              cnet_packet_handle_tag(handle));
  if (status == SALTS_OK) ++record->active_datagram_sends;
  return status;
}

static int cnet_packet_kcp_output(void *user, cnet_kcp *session, const void *data, size_t size) {
  (void)session;
  return cnet_packet_record_output((cnet_packet_record *)user, data, size);
}

static int cnet_packet_secure_kcp_output(void *user, cnet_secure_kcp *session,
                                         const void *data, size_t size) {
  (void)session;
  return cnet_packet_record_output((cnet_packet_record *)user, data, size);
}

static void cnet_packet_record_receive(cnet_packet_record *record,
                                       const cnet_receive_view *view) {
  cnet_packet_endpoint_impl *impl = record->endpoint;
  if (record->closing || impl->observer.on_receive == NULL) return;
  ++impl->callback_depth;
  impl->observer.on_receive(impl->observer.user, impl->owner,
                            cnet_packet_record_handle(impl, record), view);
  cnet_packet_callback_leave(impl);
}

static void cnet_packet_kcp_receive(void *user, cnet_kcp *session,
                                    const cnet_receive_view *view) {
  (void)session;
  cnet_packet_record_receive((cnet_packet_record *)user, view);
}

static void cnet_packet_secure_kcp_receive(void *user, cnet_secure_kcp *session,
                                           const cnet_receive_view *view) {
  (void)session;
  cnet_packet_record_receive((cnet_packet_record *)user, view);
}

static void cnet_packet_secure_kcp_established(void *user, cnet_secure_kcp *session) {
  cnet_packet_record *record = (cnet_packet_record *)user;
  cnet_packet_endpoint_impl *impl = record->endpoint;
  uint32_t conversation = 0u;
  int status;
  if (!record->occupied || record->closing) return;
  status = cnet_secure_kcp_conversation(session, &conversation);
  if (status != SALTS_OK) {
    cnet_packet_user_error(impl, cnet_packet_record_handle(impl, record), status);
    return;
  }
  record->derived_conversation = conversation;
  cnet_packet_user_state(impl, record, CNET_PACKET_SESSION_OPEN);
}

static int cnet_packet_record_open(cnet_packet_endpoint_impl *impl, const cnet_packet_key *key,
                                   cnet_secure_kcp_role secure_role,
                                   cnet_packet_session *out_session) {
  cnet_packet_record *record;
  uint32_t map_slot;
  size_t index;
  int status;
  if (out_session == NULL) return SALTS_EINVAL;
  *out_session = (cnet_packet_session){0};
  if (impl->stopping) return SALTS_ESHUTDOWN;
  if (cnet_packet_record_from_key(impl, key) != NULL) return SALTS_EALREADY;
  if (impl->free_record_count == 0u) return SALTS_ENOBUFS;
  index = impl->free_records[impl->free_record_count - 1u];
  record = &impl->records[index];
  record->endpoint = impl;
  record->key = *key;
  record->active_datagram_sends = 0u;
  record->pending_terminal_callbacks = 0u;
  record->kcp_send_head = 0u;
  record->kcp_send_tail = 0u;
  record->derived_conversation = 0u;
  record->closing = false;
  if (++record->generation == 0u) ++record->generation;
  if (impl->protocol == CNET_PACKET_KCP) {
    if (impl->security_template.mode == CNET_KCP_SECURITY_PSK_V1) {
      cnet_secure_kcp_config config = CNET_SECURE_KCP_CONFIG_INIT;
      config.role = secure_role;
      config.kcp = impl->kcp_template;
      config.security = impl->security_template;
      config.observer.output = cnet_packet_secure_kcp_output;
      config.observer.on_receive = cnet_packet_secure_kcp_receive;
      config.observer.on_established = cnet_packet_secure_kcp_established;
      config.observer.user = record;
      status = cnet_secure_kcp_init(&record->secure_kcp, &config);
      if (status == SALTS_OK) record->secure_kcp_initialized = true;
    } else {
      cnet_kcp_config config = impl->kcp_template;
      config.conversation = key->conversation;
      config.observer.output = cnet_packet_kcp_output;
      config.observer.on_receive = cnet_packet_kcp_receive;
      config.observer.user = record;
      status = cnet_kcp_init(&record->kcp, &config);
      if (status == SALTS_OK) record->kcp_initialized = true;
    }
    if (status != SALTS_OK) {
      memset(&record->key, 0, sizeof(record->key));
      return status;
    }
  }
  map_slot = (uint32_t)(index + 1u);
  status = cnet_packet_stl_status(hash_map_put(&impl->peer_index, key, &map_slot));
  if (status != SALTS_OK) {
    if (record->kcp_initialized) {
      (void)cnet_kcp_destroy(&record->kcp);
      record->kcp_initialized = false;
    }
    if (record->secure_kcp_initialized) {
      (void)cnet_secure_kcp_destroy(&record->secure_kcp);
      record->secure_kcp_initialized = false;
    }
    memset(&record->key, 0, sizeof(record->key));
    return status;
  }
  --impl->free_record_count;
  record->occupied = true;
  *out_session = cnet_packet_record_handle(impl, record);
  if (record->secure_kcp_initialized) {
    status = cnet_secure_kcp_start(&record->secure_kcp, (uint32_t)cmeta_monotonic_ms());
    if (status != SALTS_OK) {
      (void)hash_map_remove(&impl->peer_index, &record->key, NULL);
      (void)cnet_secure_kcp_destroy(&record->secure_kcp);
      record->secure_kcp_initialized = false;
      record->occupied = false;
      memset(&record->key, 0, sizeof(record->key));
      ++impl->free_record_count;
      *out_session = (cnet_packet_session){0};
      return status;
    }
    cnet_packet_user_state(impl, record, CNET_PACKET_SESSION_CONNECTING);
  } else {
    cnet_packet_user_state(impl, record, CNET_PACKET_SESSION_OPEN);
  }
  cnet_packet_sweep_closed(impl);
  return SALTS_OK;
}

static bool cnet_packet_secure_input_rejected(int status) {
  return status == SALTS_EPROTO || status == SALTS_EPERM || status == SALTS_EALREADY ||
         status == SALTS_EMSGSIZE;
}

static void cnet_packet_datagram_receive(void *user, cnet_datagram *datagram,
                                         const cnet_datagram_peer *peer,
                                         const cnet_receive_view *view) {
  cnet_packet_endpoint_impl *impl = (cnet_packet_endpoint_impl *)user;
  cnet_packet_key key = {0};
  cnet_packet_record *record = NULL;
  cnet_packet_session handle = {0};
  uint32_t conversation = 0u;
  bool key_ready = false;
  const bool secure_kcp = impl->protocol == CNET_PACKET_KCP &&
                          impl->security_template.mode == CNET_KCP_SECURITY_PSK_V1;
  int status;
  (void)datagram;
  if (impl->protocol == CNET_PACKET_KCP) {
    if (view == NULL || view->data == NULL) {
      if (!secure_kcp) cnet_packet_user_error(impl, handle, SALTS_EPROTO);
      return;
    }
    if (!secure_kcp && view->size < CNET_PACKET_KCP_WIRE_HEADER_BYTES) {
      cnet_packet_user_error(impl, handle, SALTS_EPROTO);
      return;
    }
    if (!secure_kcp) {
      conversation = cnet_packet_decode_u32_le((const unsigned char *)view->data);
      if (conversation == 0u ||
          !cnet_packet_key_make(impl->protocol, false, peer, conversation, &key)) {
        cnet_packet_user_error(impl, handle, SALTS_EPROTO);
        return;
      }
      key_ready = true;
      record = cnet_packet_record_from_key(impl, &key);
      if (!cnet_packet_kcp_wire_valid(view->data, view->size, conversation)) {
        if (record != NULL) handle = cnet_packet_record_handle(impl, record);
        if (record != NULL && !record->closing && impl->terminal.on_send != NULL)
          cnet_packet_record_fail(impl, record, SALTS_EPROTO);
        else
          cnet_packet_user_error(impl, handle, SALTS_EPROTO);
        return;
      }
    }
  }
  if (!key_ready && !cnet_packet_key_make(impl->protocol, secure_kcp, peer, conversation, &key)) {
    cnet_packet_user_error(impl, handle, SALTS_EPROTO);
    return;
  }
  if (record == NULL) record = cnet_packet_record_from_key(impl, &key);
  if (record == NULL) {
    if (secure_kcp && cnet_kcp_secure_client_hello_authenticate(
                          &impl->security_template, view->data, view->size) != SALTS_OK)
      return;
    if (impl->observer.on_admit == NULL) return;
    ++impl->callback_depth;
    status = impl->observer.on_admit(impl->observer.user, impl->owner, impl->protocol, &key.peer,
                                     key.conversation);
    cnet_packet_callback_leave(impl);
    cnet_packet_sweep_closed(impl);
    if (status != SALTS_OK) return;
    status = cnet_packet_record_open(impl, &key, CNET_SECURE_KCP_SERVER, &handle);
    if (status != SALTS_OK) {
      cnet_packet_user_error(impl, handle, status);
      return;
    }
    record = cnet_packet_record_from_handle(impl, handle);
  }
  if (record == NULL || record->closing) return;
  handle = cnet_packet_record_handle(impl, record);
  if (secure_kcp) {
    bool authenticated = false;
    status = cnet_secure_kcp_input_classified(&record->secure_kcp, view->data, view->size,
                                              &authenticated);
    cnet_packet_record_complete_kcp_sends(impl, record);
    if (status != SALTS_OK &&
        (!cnet_packet_secure_input_rejected(status) ||
         (authenticated && impl->terminal.on_send != NULL))) {
      cnet_packet_record_fail(impl, record, status);
    }
  } else if (impl->protocol == CNET_PACKET_KCP) {
    status = cnet_kcp_input(&record->kcp, view->data, view->size);
    cnet_packet_record_complete_kcp_sends(impl, record);
    if (status != SALTS_OK) {
      if (!record->closing && (status == SALTS_EPROTO || status == SALTS_EMSGSIZE))
        cnet_packet_record_fail(impl, record, status);
      else
        cnet_packet_user_error(impl, handle, status);
    }
  } else if (impl->observer.on_receive != NULL) {
    ++impl->callback_depth;
    impl->observer.on_receive(impl->observer.user, impl->owner, handle, view);
    cnet_packet_callback_leave(impl);
  }
  cnet_packet_sweep_closed(impl);
}

static void cnet_packet_datagram_send(void *user, cnet_datagram *datagram,
                                      const cnet_datagram_peer *peer, size_t size, int status,
                                      uint64_t tag) {
  cnet_packet_endpoint_impl *impl = (cnet_packet_endpoint_impl *)user;
  cnet_packet_session handle;
  cnet_packet_record *record;
  (void)datagram;
  (void)peer;
  if (impl->protocol == CNET_PACKET_UDP && impl->terminal.on_send != NULL) {
    cnet_packet_send_operation *operation = cnet_packet_send_operation_from_tag(impl, tag);
    if (operation == NULL) {
      cnet_packet_user_error(impl, (cnet_packet_session){0}, SALTS_EPROTO);
      return;
    }
    handle = operation->session;
    record = cnet_packet_record_from_handle(impl, handle);
    if (record == NULL || record->active_datagram_sends == 0u || size != operation->size) {
      cnet_packet_send_operation_release(impl, operation);
      cnet_packet_user_error(impl, handle, SALTS_EPROTO);
      return;
    }
    --record->active_datagram_sends;
    if (operation->notify) cnet_packet_send_operation_ready(impl, record, operation, status);
    else cnet_packet_send_operation_release(impl, operation);
    if (status != SALTS_OK && !(impl->stopping && status == SALTS_ECANCELED))
      cnet_packet_user_error(impl, handle, status);
    cnet_packet_dispatch_ready_sends(impl);
    cnet_packet_sweep_closed(impl);
    return;
  }
  handle = cnet_packet_tag_handle(tag);
  record = cnet_packet_record_from_handle(impl, handle);
  if (record == NULL || record->active_datagram_sends == 0u) {
    cnet_packet_user_error(impl, handle, SALTS_EPROTO);
    return;
  }
  --record->active_datagram_sends;
  if (status != SALTS_OK && !(impl->stopping && status == SALTS_ECANCELED))
    cnet_packet_user_error(impl, handle, status);
  cnet_packet_sweep_closed(impl);
}

static bool cnet_packet_config_valid(const cnet_packet_endpoint_config *config) {
  const cnet_datagram_observer *datagram_observer;
  const cnet_kcp_observer *kcp_observer;
  if (config == NULL || config->size != sizeof(*config) || config->session_capacity == 0u ||
      config->session_capacity > UINT32_MAX || config->observer.on_receive == NULL ||
      config->session_capacity > SIZE_MAX / sizeof(cnet_packet_record) ||
      config->session_capacity > SIZE_MAX / sizeof(uint32_t) ||
      (config->protocol != CNET_PACKET_UDP && config->protocol != CNET_PACKET_KCP) ||
      config->datagram.size != sizeof(config->datagram) ||
      config->security.size != sizeof(config->security))
    return false;
  datagram_observer = &config->datagram.observer;
  if (datagram_observer->on_receive != NULL || datagram_observer->on_send != NULL ||
      datagram_observer->user != NULL)
    return false;
  if (config->protocol == CNET_PACKET_UDP)
    return config->security.mode == CNET_KCP_SECURITY_NONE;
  kcp_observer = &config->kcp.observer;
  if (kcp_observer->output != NULL || kcp_observer->on_receive != NULL ||
      kcp_observer->user != NULL)
    return false;
  if (config->security.mode == CNET_KCP_SECURITY_PSK_V1) {
    const size_t fec_datagram_bytes = (size_t)config->security.fec.max_payload_bytes +
                                      CNET_KCP_FEC_MAX_WIRE_OVERHEAD_BYTES;
    return cnet_secure_kcp_transport_config_valid(&config->kcp, &config->security) &&
           fec_datagram_bytes <= config->datagram.max_datagram_bytes &&
           fec_datagram_bytes <= config->datagram.receive_buffer_bytes;
  }
  return config->security.mode == CNET_KCP_SECURITY_NONE &&
         config->kcp.size == sizeof(config->kcp) && config->kcp.conversation == 0u &&
         config->kcp.mtu >= CNET_PACKET_KCP_MIN_MTU &&
         config->kcp.mtu <= config->datagram.max_datagram_bytes &&
         config->kcp.send_window != 0u && config->kcp.send_window <= (uint32_t)INT_MAX &&
         config->kcp.receive_window != 0u && config->kcp.receive_window <= (uint32_t)INT_MAX &&
         config->kcp.interval_ms >= CNET_PACKET_KCP_MIN_INTERVAL_MS &&
         config->kcp.interval_ms <= CNET_PACKET_KCP_MAX_INTERVAL_MS &&
         config->kcp.fast_resend <= (uint32_t)INT_MAX &&
         config->kcp.send_segment_capacity != 0u &&
         config->kcp.send_segment_capacity <= (size_t)INT_MAX &&
         config->kcp.max_message_bytes != 0u && config->kcp.max_message_bytes <= (size_t)INT_MAX;
}

static bool cnet_packet_terminal_config_valid(const cnet_packet_endpoint_config *config,
                                              const cnet_packet_terminal_config *terminal) {
  size_t operation_capacity;
  if (terminal == NULL) return true;
  if (terminal->size != sizeof(*terminal) ||
      terminal->version != CNET_PACKET_TERMINAL_API_VERSION || terminal->send_capacity == 0u ||
      terminal->on_send == NULL)
    return false;
  if (config->protocol == CNET_PACKET_UDP &&
      terminal->send_capacity > config->datagram.send_capacity)
    return false;
  if (config->protocol == CNET_PACKET_KCP && config->kcp.stream_mode) return false;
  operation_capacity = config->protocol == CNET_PACKET_UDP ? config->datagram.send_capacity
                                                            : terminal->send_capacity;
  return operation_capacity <= UINT32_MAX &&
         operation_capacity <= SIZE_MAX / sizeof(cnet_packet_send_operation) &&
         operation_capacity <= SIZE_MAX / sizeof(uint32_t);
}

int cnet_packet_endpoint_init(cnet_packet_endpoint *endpoint,
                              const cnet_packet_endpoint_config *config) {
  return cnet_packet_endpoint_init_ex(endpoint, config, NULL);
}

int cnet_packet_endpoint_init_ex(cnet_packet_endpoint *endpoint,
                                 const cnet_packet_endpoint_config *config,
                                 const cnet_packet_terminal_config *terminal) {
  cnet_packet_endpoint_impl *impl;
  cnet_datagram_config datagram_config;
  size_t send_operation_capacity = 0u;
  stl_status map_status;
  size_t index;
  int status;
  if (endpoint == NULL) return SALTS_EINVAL;
  if (endpoint->impl != NULL) return SALTS_EALREADY;
  if (!cnet_packet_config_valid(config) || !cnet_packet_terminal_config_valid(config, terminal))
    return SALTS_EINVAL;
  if (terminal != NULL)
    send_operation_capacity = config->protocol == CNET_PACKET_UDP ? config->datagram.send_capacity
                                                                  : terminal->send_capacity;
  impl = (cnet_packet_endpoint_impl *)calloc(1u, sizeof(*impl));
  if (impl == NULL) return SALTS_ENOMEM;
  impl->records = (cnet_packet_record *)calloc(config->session_capacity, sizeof(*impl->records));
  impl->free_records = (uint32_t *)malloc(config->session_capacity * sizeof(*impl->free_records));
  if (send_operation_capacity != 0u) {
    impl->send_operations = (cnet_packet_send_operation *)calloc(send_operation_capacity,
                                                                 sizeof(*impl->send_operations));
    impl->free_send_operations =
        (uint32_t *)malloc(send_operation_capacity * sizeof(*impl->free_send_operations));
  }
  if (impl->records == NULL || impl->free_records == NULL ||
      (send_operation_capacity != 0u &&
       (impl->send_operations == NULL || impl->free_send_operations == NULL))) {
    free(impl->free_send_operations);
    free(impl->send_operations);
    free(impl->free_records);
    free(impl->records);
    free(impl);
    return SALTS_ENOMEM;
  }
  map_status = hash_map_init_bytes(&impl->peer_index, sizeof(cnet_packet_key),
                                   _Alignof(cnet_packet_key), sizeof(uint32_t), _Alignof(uint32_t),
                                   config->session_capacity, cnet_packet_key_hash,
                                   cnet_packet_key_equal, NULL);
  if (map_status == STL_OK) map_status = hash_map_reserve(&impl->peer_index, config->session_capacity);
  if (map_status != STL_OK) {
    hash_map_raw_destroy_storage(&impl->peer_index);
    free(impl->free_send_operations);
    free(impl->send_operations);
    free(impl->free_records);
    free(impl->records);
    free(impl);
    return cnet_packet_stl_status(map_status);
  }
  impl->owner = endpoint;
  impl->session_capacity = config->session_capacity;
  impl->free_record_count = config->session_capacity;
  impl->send_operation_capacity = send_operation_capacity;
  impl->free_send_operation_count = send_operation_capacity;
  impl->max_datagram_bytes = config->datagram.max_datagram_bytes;
  impl->protocol = config->protocol;
  impl->kcp_template = config->kcp;
  impl->security_template = config->security;
  impl->observer = config->observer;
  if (terminal != NULL) impl->terminal = *terminal;
  impl->pending_status = SALTS_OK;
  for (index = 0u; index < config->session_capacity; ++index)
    impl->free_records[index] = (uint32_t)(config->session_capacity - index - 1u);
  for (index = 0u; index < send_operation_capacity; ++index)
    impl->free_send_operations[index] = (uint32_t)(send_operation_capacity - index - 1u);
  datagram_config = config->datagram;
  datagram_config.observer.on_receive = cnet_packet_datagram_receive;
  datagram_config.observer.on_send = cnet_packet_datagram_send;
  datagram_config.observer.user = impl;
  status = cnet_datagram_init(&impl->datagram, &datagram_config);
  if (status != SALTS_OK) {
    hash_map_raw_destroy_storage(&impl->peer_index);
    free(impl->free_send_operations);
    free(impl->send_operations);
    free(impl->free_records);
    free(impl->records);
    crypto_wipe(&impl->security_template, sizeof(impl->security_template));
    free(impl);
    return status;
  }
  status = cnet_datagram_receive(&impl->datagram, SIZE_MAX);
  if (status != SALTS_OK) {
    (void)cnet_datagram_stop(&impl->datagram, 1000u);
    (void)cnet_datagram_destroy(&impl->datagram);
    hash_map_raw_destroy_storage(&impl->peer_index);
    free(impl->free_send_operations);
    free(impl->send_operations);
    free(impl->free_records);
    free(impl->records);
    crypto_wipe(&impl->security_template, sizeof(impl->security_template));
    free(impl);
    return status;
  }
  endpoint->impl = impl;
  return SALTS_OK;
}

int cnet_packet_endpoint_port(const cnet_packet_endpoint *endpoint, uint16_t *out_port) {
  cnet_packet_endpoint_impl *impl = cnet_packet_get(endpoint);
  return impl != NULL ? cnet_datagram_port(&impl->datagram, out_port) : SALTS_EINVAL;
}

int cnet_packet_session_get_info(const cnet_packet_endpoint *endpoint,
                                 cnet_packet_session session,
                                 cnet_packet_session_info *out_info) {
  cnet_packet_endpoint_impl *impl = cnet_packet_get(endpoint);
  cnet_packet_record *record;
  if (out_info == NULL) return SALTS_EINVAL;
  memset(out_info, 0, sizeof(*out_info));
  if (impl == NULL) return SALTS_EINVAL;
  record = cnet_packet_record_from_handle(impl, session);
  if (record == NULL || record->closing) return SALTS_ENOENT;
  out_info->protocol = impl->protocol;
  out_info->peer = record->key.peer;
  out_info->conversation = cnet_packet_record_conversation(record);
  return SALTS_OK;
}

int cnet_packet_session_open(cnet_packet_endpoint *endpoint, const cnet_datagram_peer *peer,
                             uint32_t conversation, cnet_packet_session *out_session) {
  cnet_packet_endpoint_impl *impl = cnet_packet_get(endpoint);
  cnet_packet_key key;
  bool secure_kcp;
  if (out_session != NULL) *out_session = (cnet_packet_session){0};
  if (impl == NULL || out_session == NULL) return SALTS_EINVAL;
  secure_kcp = impl->protocol == CNET_PACKET_KCP &&
               impl->security_template.mode == CNET_KCP_SECURITY_PSK_V1;
  if (!cnet_packet_key_make(impl->protocol, secure_kcp, peer, conversation, &key))
    return SALTS_EINVAL;
  return cnet_packet_record_open(impl, &key, CNET_SECURE_KCP_CLIENT, out_session);
}

int cnet_packet_session_close(cnet_packet_endpoint *endpoint, cnet_packet_session session) {
  cnet_packet_endpoint_impl *impl = cnet_packet_get(endpoint);
  cnet_packet_record *record = cnet_packet_record_from_handle(impl, session);
  if (impl == NULL) return SALTS_EINVAL;
  if (record == NULL) return SALTS_ENOENT;
  if (record->closing) return SALTS_EALREADY;
  cnet_packet_record_begin_close(impl, record, SALTS_ECANCELED);
  return SALTS_OK;
}

static int cnet_packet_send_impl(cnet_packet_endpoint *endpoint, cnet_packet_session session,
                                 const void *data, size_t size, uint64_t tag, bool notify) {
  cnet_packet_endpoint_impl *impl = cnet_packet_get(endpoint);
  cnet_packet_record *record = cnet_packet_record_from_handle(impl, session);
  cnet_packet_send_operation *operation = NULL;
  uint64_t native_tag;
  int status;
  if (impl == NULL || data == NULL || size == 0u) return SALTS_EINVAL;
  if (record == NULL) return SALTS_ENOENT;
  if (impl->stopping || record->closing) return SALTS_ESHUTDOWN;
  if (notify && impl->terminal.on_send == NULL) return SALTS_ENOTSUP;
  if (impl->protocol == CNET_PACKET_KCP) {
    if (!notify)
      return record->secure_kcp_initialized ? cnet_secure_kcp_send(&record->secure_kcp, data, size)
                                            : cnet_kcp_send(&record->kcp, data, size);
    status = record->secure_kcp_initialized
                 ? cnet_secure_kcp_send_validate(&record->secure_kcp, data, size, true)
                 : cnet_kcp_send_validate(&record->kcp, data, size, true);
    if (status != SALTS_OK) return status;
    status = cnet_packet_send_operation_acquire(impl, session, size, tag, true, &operation);
    if (status != SALTS_OK) return status;
    status = record->secure_kcp_initialized
                 ? cnet_secure_kcp_send_marked(&record->secure_kcp, data, size, &operation->marker)
                 : cnet_kcp_send_marked(&record->kcp, data, size, &operation->marker);
    if (status != SALTS_OK) {
      cnet_packet_send_operation_release(impl, operation);
      return status;
    }
    cnet_packet_record_append_kcp_send(impl, record, operation);
    return SALTS_OK;
  }
  if (size > impl->max_datagram_bytes) return SALTS_EMSGSIZE;
  if (impl->terminal.on_send != NULL) {
    status = cnet_packet_send_operation_acquire(impl, session, size, tag, notify, &operation);
    if (status != SALTS_OK) return status;
    native_tag = cnet_packet_send_operation_tag(operation, impl);
  } else {
    native_tag = cnet_packet_handle_tag(session);
  }
  status = cnet_datagram_send(&impl->datagram, &record->key.peer, data, size, native_tag);
  if (status != SALTS_OK && operation != NULL) cnet_packet_send_operation_release(impl, operation);
  if (status == SALTS_OK) ++record->active_datagram_sends;
  return status;
}

int cnet_packet_send(cnet_packet_endpoint *endpoint, cnet_packet_session session, const void *data,
                     size_t size) {
  return cnet_packet_send_impl(endpoint, session, data, size, 0u, false);
}

int cnet_packet_send_tagged(cnet_packet_endpoint *endpoint, cnet_packet_session session,
                            const void *data, size_t size, uint64_t tag) {
  return cnet_packet_send_impl(endpoint, session, data, size, tag, true);
}

static void cnet_packet_update_kcp(cnet_packet_endpoint_impl *impl, uint32_t now_ms) {
  size_t index;
  if (impl->protocol != CNET_PACKET_KCP) return;
  for (index = 0u; index < impl->session_capacity; ++index) {
    cnet_packet_record *record = &impl->records[index];
    int status;
    if (!record->occupied || record->closing) continue;
    status = record->secure_kcp_initialized
                 ? cnet_secure_kcp_update(&record->secure_kcp, now_ms)
                 : cnet_kcp_update(&record->kcp, now_ms);
    cnet_packet_record_complete_kcp_sends(impl, record);
    if (impl->terminal.on_send != NULL && !record->closing) {
      const int terminal_status =
          record->secure_kcp_initialized
              ? cnet_secure_kcp_terminal_status(&record->secure_kcp)
              : cnet_kcp_terminal_status(&record->kcp);
      if (terminal_status == SALTS_ETIMEDOUT) {
        cnet_packet_record_fail(impl, record, terminal_status);
        continue;
      }
    }
    if (status != SALTS_OK)
      cnet_packet_user_error(impl, cnet_packet_record_handle(impl, record), status);
  }
  cnet_packet_sweep_closed(impl);
}

static uint32_t cnet_packet_wait_ms(cnet_packet_endpoint_impl *impl, uint32_t now_ms,
                                    uint32_t requested_ms) {
  size_t index;
  uint32_t result = requested_ms;
  if (impl->protocol != CNET_PACKET_KCP) return result;
  for (index = 0u; index < impl->session_capacity; ++index) {
    cnet_packet_record *record = &impl->records[index];
    uint32_t next_ms;
    int32_t difference;
    if (!record->occupied || record->closing) continue;
    if ((record->secure_kcp_initialized
             ? cnet_secure_kcp_check(&record->secure_kcp, now_ms, &next_ms)
             : cnet_kcp_check(&record->kcp, now_ms, &next_ms)) != SALTS_OK)
      continue;
    difference = (int32_t)(next_ms - now_ms);
    if (difference <= 0) return 0u;
    if ((uint32_t)difference < result) result = (uint32_t)difference;
  }
  return result;
}

int cnet_packet_poll(cnet_packet_endpoint *endpoint, uint32_t timeout_ms, size_t *out_events) {
  cnet_packet_endpoint_impl *impl = cnet_packet_get(endpoint);
  uint32_t now_ms;
  uint32_t wait_ms;
  int status;
  if (out_events == NULL) return SALTS_EINVAL;
  *out_events = 0u;
  if (impl == NULL) return SALTS_EINVAL;
  if (impl->stopping) return SALTS_ESHUTDOWN;
  if (impl->polling || impl->callback_depth != 0u) return SALTS_EBUSY;
  impl->polling = true;
  impl->pending_status = SALTS_OK;
  now_ms = (uint32_t)cmeta_monotonic_ms();
  cnet_packet_update_kcp(impl, now_ms);
  wait_ms = cnet_packet_wait_ms(impl, now_ms, timeout_ms);
  status = cnet_datagram_poll(&impl->datagram, wait_ms, out_events);
  now_ms = (uint32_t)cmeta_monotonic_ms();
  cnet_packet_update_kcp(impl, now_ms);
  if (status == SALTS_OK && impl->pending_status != SALTS_OK) status = impl->pending_status;
  impl->polling = false;
  return status;
}

int cnet_packet_wake(cnet_packet_endpoint *endpoint) {
  cnet_packet_endpoint_impl *impl = cnet_packet_get(endpoint);
  if (impl == NULL) return SALTS_EINVAL;
  return cnet_datagram_wake(&impl->datagram);
}

int cnet_packet_endpoint_stop(cnet_packet_endpoint *endpoint, uint32_t timeout_ms) {
  cnet_packet_endpoint_impl *impl = cnet_packet_get(endpoint);
  size_t index;
  int status;
  if (impl == NULL) return SALTS_EINVAL;
  if (impl->polling || impl->callback_depth != 0u) return SALTS_EBUSY;
  if (impl->stopped) return SALTS_OK;
  impl->stopping = true;
  for (index = 0u; index < impl->session_capacity; ++index) {
    cnet_packet_record *record = &impl->records[index];
    if (record->occupied) cnet_packet_record_begin_close(impl, record, SALTS_ECANCELED);
  }
  status = cnet_datagram_stop(&impl->datagram, timeout_ms);
  if (status == SALTS_ETIMEDOUT) return status;
  if (status != SALTS_OK && impl->stop_status == SALTS_OK) impl->stop_status = status;
  cnet_packet_dispatch_ready_sends(impl);
  cnet_packet_sweep_closed(impl);
  if (impl->free_record_count != impl->session_capacity || impl->ready_send_head != 0u ||
      impl->tagged_send_count != 0u ||
      impl->free_send_operation_count != impl->send_operation_capacity)
    return SALTS_EBUSY;
  impl->stopped = true;
  return impl->stop_status;
}

int cnet_packet_endpoint_destroy(cnet_packet_endpoint *endpoint) {
  cnet_packet_endpoint_impl *impl;
  int status;
  if (endpoint == NULL) return SALTS_EINVAL;
  impl = cnet_packet_get(endpoint);
  if (impl == NULL) return SALTS_OK;
  if (!impl->stopped) return SALTS_EBUSY;
  status = cnet_datagram_destroy(&impl->datagram);
  if (status != SALTS_OK) return status;
  hash_map_raw_destroy_storage(&impl->peer_index);
  free(impl->free_send_operations);
  free(impl->send_operations);
  free(impl->free_records);
  free(impl->records);
  crypto_wipe(&impl->security_template, sizeof(impl->security_template));
  free(impl);
  endpoint->impl = NULL;
  return SALTS_OK;
}
