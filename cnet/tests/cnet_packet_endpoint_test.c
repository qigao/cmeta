#include "tinytest.h"
#include <cnet/cnet.h>

#include <string.h>

enum { CNET_PACKET_TEST_TIMEOUT_MS = 5000 };

typedef struct cnet_packet_test_probe {
  unsigned char received[128];
  size_t received_size;
  size_t reply_size;
  cnet_packet_session session;
  int admit_count;
  int connecting_count;
  int open_count;
  int close_count;
  int receive_count;
  int error_count;
  int last_error;
  cnet_packet_session terminal_session;
  size_t terminal_size;
  int terminal_status;
  uint64_t terminal_tag;
  uint64_t terminal_tags[8];
  uint32_t terminal_generations[8];
  int terminal_statuses[8];
  int terminal_count;
  int terminal_count_at_close;
  int receive_action_status;
  int terminal_action_status;
  int open_send_status;
  int open_close_status;
  bool send_reply_on_receive;
  bool close_on_receive;
  bool send_again_on_terminal;
  bool send_and_close_on_open;
} cnet_packet_test_probe;

static native_io_backend_kind cnet_packet_test_backend(void) {
#if defined(_WIN32)
  return NATIVE_IO_BACKEND_IOCP;
#elif defined(__linux__)
  return NATIVE_IO_BACKEND_EPOLL;
#elif defined(__APPLE__) || defined(__FreeBSD__)
  return NATIVE_IO_BACKEND_KQUEUE;
#else
  return (native_io_backend_kind)0;
#endif
}

static cnet_datagram_peer cnet_packet_test_peer(uint16_t port) {
  cnet_datagram_peer peer = {0};
  peer.family = CNET_DATAGRAM_ADDRESS_IPV4;
  peer.port = port;
  peer.address[0] = 127u;
  peer.address[3] = 1u;
  return peer;
}

/* Match the packet index's field-wise hash to intentionally generate
 * colliding normalized peer keys without depending on host struct padding. */
static size_t cnet_packet_test_peer_bucket(const cnet_datagram_peer *peer,
                                           uint32_t conversation, size_t mask) {
  const unsigned char *fields[] = {(const unsigned char *)&peer->family,
                                   (const unsigned char *)&peer->port,
                                   (const unsigned char *)&peer->scope_id,
                                   peer->address,
                                   (const unsigned char *)&conversation};
  const size_t sizes[] = {sizeof(peer->family), sizeof(peer->port),
                          sizeof(peer->scope_id), sizeof(peer->address),
                          sizeof(conversation)};
  size_t hash = (size_t)UINT32_C(2166136261);
  for (size_t field = 0u; field < sizeof(fields)/sizeof(fields[0]); ++field) {
    for (size_t index = 0u; index < sizes[field]; ++index) {
      hash ^= fields[field][index];
      hash *= (size_t)UINT32_C(16777619);
    }
  }
  return hash & mask;
}

static int cnet_packet_test_admit(void *user, cnet_packet_endpoint *endpoint,
                                  cnet_packet_protocol protocol,
                                  const cnet_datagram_peer *peer, uint32_t conversation) {
  cnet_packet_test_probe *probe = (cnet_packet_test_probe *)user;
  (void)endpoint;
  (void)protocol;
  (void)peer;
  (void)conversation;
  ++probe->admit_count;
  return SALTS_OK;
}

static void cnet_packet_test_state(void *user, cnet_packet_endpoint *endpoint,
                                   cnet_packet_session session, cnet_packet_session_state state,
                                   const cnet_datagram_peer *peer, uint32_t conversation) {
  cnet_packet_test_probe *probe = (cnet_packet_test_probe *)user;
  (void)endpoint;
  (void)peer;
  (void)conversation;
  probe->session = session;
  if (state == CNET_PACKET_SESSION_CONNECTING) ++probe->connecting_count;
  if (state == CNET_PACKET_SESSION_OPEN) {
    ++probe->open_count;
    if (probe->send_and_close_on_open) {
      probe->send_and_close_on_open = false;
      probe->open_send_status =
          cnet_packet_send_tagged(endpoint, session, "open", 4u, UINT64_C(77));
      probe->open_close_status = cnet_packet_session_close(endpoint, session);
    }
  }
  if (state == CNET_PACKET_SESSION_CLOSED) {
    probe->terminal_count_at_close = probe->terminal_count;
    ++probe->close_count;
  }
}

static void cnet_packet_test_receive(void *user, cnet_packet_endpoint *endpoint,
                                     cnet_packet_session session,
                                     const cnet_receive_view *view) {
  static const unsigned char large_reply[700] = {0x5au};
  cnet_packet_test_probe *probe = (cnet_packet_test_probe *)user;
  (void)endpoint;
  if (view == NULL || view->size > sizeof(probe->received)) return;
  probe->session = session;
  memcpy(probe->received, view->data, view->size);
  probe->received_size = view->size;
  ++probe->receive_count;
  if (probe->send_reply_on_receive) {
    const size_t reply_size = probe->reply_size == 0u ? 5u : probe->reply_size;
    probe->send_reply_on_receive = false;
    probe->receive_action_status =
        reply_size == 5u ? cnet_packet_send(endpoint, session, "reply", reply_size)
                         : cnet_packet_send(endpoint, session, large_reply, reply_size);
  }
  if (probe->close_on_receive) {
    probe->close_on_receive = false;
    probe->receive_action_status = cnet_packet_session_close(endpoint, session);
  }
}

static void cnet_packet_test_error(void *user, cnet_packet_endpoint *endpoint,
                                   cnet_packet_session session, int status) {
  cnet_packet_test_probe *probe = (cnet_packet_test_probe *)user;
  (void)endpoint;
  probe->session = session;
  probe->last_error = status;
  ++probe->error_count;
}

static void cnet_packet_test_send_terminal(void *user, cnet_packet_endpoint *endpoint,
                                           cnet_packet_session session, size_t size, int status,
                                           uint64_t tag) {
  cnet_packet_test_probe *probe = (cnet_packet_test_probe *)user;
  (void)endpoint;
  probe->terminal_session = session;
  probe->terminal_size = size;
  probe->terminal_status = status;
  probe->terminal_tag = tag;
  if (probe->terminal_count < 8) {
    probe->terminal_tags[probe->terminal_count] = tag;
    probe->terminal_generations[probe->terminal_count] = session.generation;
    probe->terminal_statuses[probe->terminal_count] = status;
  }
  ++probe->terminal_count;
  if (probe->send_again_on_terminal) {
    probe->send_again_on_terminal = false;
    probe->terminal_action_status =
        cnet_packet_send_tagged(endpoint, session, "two", 3u, UINT64_C(2));
  }
}

static cnet_packet_endpoint_config cnet_packet_test_config(cnet_packet_protocol protocol,
                                                           cnet_packet_test_probe *probe) {
  cnet_packet_endpoint_config config = CNET_PACKET_ENDPOINT_CONFIG_INIT;
  config.protocol = protocol;
  config.session_capacity = 4u;
  config.datagram.backend = cnet_packet_test_backend();
  config.datagram.host = "127.0.0.1";
  config.datagram.port = 0u;
  config.datagram.send_capacity = 32u;
  config.datagram.request_capacity = 33u;
  config.datagram.completion_batch_capacity = 16u;
  config.datagram.max_datagram_bytes = 1500u;
  config.datagram.receive_buffer_bytes = 1500u;
  config.kcp.mtu = 512u;
  config.kcp.send_window = 32u;
  config.kcp.receive_window = 32u;
  config.kcp.send_segment_capacity = 64u;
  config.kcp.max_message_bytes = sizeof(probe->received);
  config.observer.on_admit = cnet_packet_test_admit;
  config.observer.on_state = cnet_packet_test_state;
  config.observer.on_receive = cnet_packet_test_receive;
  config.observer.on_error = cnet_packet_test_error;
  config.observer.user = probe;
  return config;
}

static void cnet_packet_test_round_trip(cnet_packet_protocol protocol, uint32_t conversation) {
  static const unsigned char message[] = "unified-packet";
  cnet_packet_endpoint left = {0};
  cnet_packet_endpoint right = {0};
  cnet_packet_test_probe left_probe = {0};
  cnet_packet_test_probe right_probe = {0};
  cnet_packet_endpoint_config left_config = cnet_packet_test_config(protocol, &left_probe);
  cnet_packet_endpoint_config right_config = cnet_packet_test_config(protocol, &right_probe);
  cnet_packet_session left_session = {0};
  cnet_datagram_peer right_peer;
  uint16_t right_port = 0u;
  size_t events = 0u;
  size_t attempts;

  check_equal(cnet_packet_endpoint_init(&left, &left_config), SALTS_OK);
  check_equal(cnet_packet_endpoint_init(&right, &right_config), SALTS_OK);
  check_equal(cnet_packet_endpoint_port(&right, &right_port), SALTS_OK);
  right_peer = cnet_packet_test_peer(right_port);
  check_equal(cnet_packet_session_open(&left, &right_peer, conversation, &left_session), SALTS_OK);
  check_true(cnet_packet_session_valid(left_session));
  check_equal(cnet_packet_send(&left, left_session, message, sizeof(message)), SALTS_OK);

  for (attempts = 0u; attempts < 1000u && right_probe.receive_count == 0; ++attempts) {
    check_equal(cnet_packet_poll(&left, 1u, &events), SALTS_OK);
    check_equal(cnet_packet_poll(&right, 1u, &events), SALTS_OK);
  }
  check_equal(right_probe.admit_count, 1);
  check_equal(right_probe.open_count, 1);
  check_equal(right_probe.receive_count, 1);
  check_equal(right_probe.received_size, sizeof(message));
  check_equal(right_probe.received, message, sizeof(message));
  check_equal(right_probe.error_count, 0);

  check_equal(cnet_packet_endpoint_stop(&right, CNET_PACKET_TEST_TIMEOUT_MS), SALTS_OK);
  check_equal(cnet_packet_endpoint_stop(&left, CNET_PACKET_TEST_TIMEOUT_MS), SALTS_OK);
  check_equal(cnet_packet_endpoint_destroy(&right), SALTS_OK);
  check_equal(cnet_packet_endpoint_destroy(&left), SALTS_OK);
}

static void cnet_packet_test_udp_tagged_terminal(void) {
  static const unsigned char message[] = "tagged-udp";
  static const uint64_t tag = UINT64_C(0x1020304050607080);
  cnet_packet_endpoint left = {0};
  cnet_packet_endpoint right = {0};
  cnet_packet_test_probe left_probe = {0};
  cnet_packet_test_probe right_probe = {0};
  cnet_packet_endpoint_config left_config = cnet_packet_test_config(CNET_PACKET_UDP, &left_probe);
  cnet_packet_endpoint_config right_config = cnet_packet_test_config(CNET_PACKET_UDP, &right_probe);
  cnet_packet_terminal_config terminal_config = CNET_PACKET_TERMINAL_CONFIG_INIT;
  cnet_packet_session left_session = {0};
  cnet_datagram_peer right_peer;
  uint16_t right_port = 0u;
  size_t events = 0u;
  size_t attempts;

  terminal_config.send_capacity = 2u;
  terminal_config.on_send = cnet_packet_test_send_terminal;
  terminal_config.user = &left_probe;
  check_equal(cnet_packet_endpoint_init_ex(&left, &left_config, &terminal_config), SALTS_OK);
  check_equal(cnet_packet_endpoint_init(&right, &right_config), SALTS_OK);
  check_equal(cnet_packet_endpoint_port(&right, &right_port), SALTS_OK);
  right_peer = cnet_packet_test_peer(right_port);
  check_equal(cnet_packet_session_open(&left, &right_peer, 0u, &left_session), SALTS_OK);

  check_equal(cnet_packet_send_tagged(&left, left_session, message, sizeof(message), tag),
              SALTS_OK);
  check_equal(left_probe.terminal_count, 0);
  for (attempts = 0u; attempts < 1000u && left_probe.terminal_count == 0; ++attempts) {
    check_equal(cnet_packet_poll(&left, 1u, &events), SALTS_OK);
    check_equal(cnet_packet_poll(&right, 1u, &events), SALTS_OK);
  }
  check_equal(left_probe.terminal_count, 1);
  check_equal(left_probe.terminal_session.slot, left_session.slot);
  check_equal(left_probe.terminal_session.generation, left_session.generation);
  check_equal(left_probe.terminal_size, sizeof(message));
  check_equal(left_probe.terminal_status, SALTS_OK);
  check_equal(left_probe.terminal_tag, tag);

  check_equal(cnet_packet_endpoint_stop(&right, CNET_PACKET_TEST_TIMEOUT_MS), SALTS_OK);
  check_equal(cnet_packet_endpoint_stop(&left, CNET_PACKET_TEST_TIMEOUT_MS), SALTS_OK);
  check_equal(cnet_packet_endpoint_destroy(&right), SALTS_OK);
  check_equal(cnet_packet_endpoint_destroy(&left), SALTS_OK);
}

static void cnet_packet_test_kcp_tagged_terminals(void) {
  unsigned char first_message[5000] = {0x31u};
  static const unsigned char second_message[] = "second-kcp-message";
  static const uint64_t first_tag = UINT64_C(0x1111222233334444);
  static const uint64_t second_tag = UINT64_C(0x5555666677778888);
  cnet_packet_endpoint left = {0};
  cnet_packet_endpoint right = {0};
  cnet_packet_test_probe left_probe = {0};
  cnet_packet_test_probe right_probe = {0};
  cnet_packet_endpoint_config left_config = cnet_packet_test_config(CNET_PACKET_KCP, &left_probe);
  cnet_packet_endpoint_config right_config = cnet_packet_test_config(CNET_PACKET_KCP, &right_probe);
  cnet_packet_terminal_config terminal_config = CNET_PACKET_TERMINAL_CONFIG_INIT;
  cnet_packet_session left_session = {0};
  cnet_datagram_peer right_peer;
  uint16_t right_port = 0u;
  size_t events = 0u;
  size_t attempts;
  int left_status;
  bool saw_backpressure = false;

  left_config.kcp.max_message_bytes = sizeof(first_message);
  right_config.kcp.max_message_bytes = sizeof(first_message);
  left_config.datagram.send_capacity = 1u;
  left_config.datagram.request_capacity = 2u;
  left_config.datagram.completion_batch_capacity = 2u;
  terminal_config.send_capacity = 4u;
  terminal_config.on_send = cnet_packet_test_send_terminal;
  terminal_config.user = &left_probe;
  check_equal(cnet_packet_endpoint_init_ex(&left, &left_config, &terminal_config), SALTS_OK);
  check_equal(cnet_packet_endpoint_init(&right, &right_config), SALTS_OK);
  check_equal(cnet_packet_endpoint_port(&right, &right_port), SALTS_OK);
  right_peer = cnet_packet_test_peer(right_port);
  check_equal(cnet_packet_session_open(&left, &right_peer, UINT32_C(0x01020304), &left_session),
              SALTS_OK);

  check_equal(
      cnet_packet_send_tagged(&left, left_session, first_message, sizeof(first_message), first_tag),
      SALTS_OK);
  check_equal(cnet_packet_send_tagged(&left, left_session, second_message, sizeof(second_message),
                                      second_tag),
              SALTS_OK);
  for (attempts = 0u; attempts < 20u; ++attempts) {
    left_status = cnet_packet_poll(&left, 1u, &events);
    check_true(left_status == SALTS_OK || left_status == SALTS_ENOBUFS);
    if (left_status == SALTS_ENOBUFS) saw_backpressure = true;
  }
  check_equal(left_probe.terminal_count, 0);

  for (attempts = 0u; attempts < 2000u && left_probe.terminal_count != 2; ++attempts) {
    check_equal(cnet_packet_poll(&right, 1u, &events), SALTS_OK);
    left_status = cnet_packet_poll(&left, 1u, &events);
    check_true(left_status == SALTS_OK || left_status == SALTS_ENOBUFS);
    if (left_status == SALTS_ENOBUFS) saw_backpressure = true;
  }
  check_true(saw_backpressure);
  check_equal(left_probe.terminal_count, 2);
  check_equal(left_probe.terminal_tags[0], first_tag);
  check_equal(left_probe.terminal_statuses[0], SALTS_OK);
  check_equal(left_probe.terminal_tags[1], second_tag);
  check_equal(left_probe.terminal_statuses[1], SALTS_OK);

  check_equal(cnet_packet_endpoint_stop(&right, CNET_PACKET_TEST_TIMEOUT_MS), SALTS_OK);
  check_equal(cnet_packet_endpoint_stop(&left, CNET_PACKET_TEST_TIMEOUT_MS), SALTS_OK);
  check_equal(cnet_packet_endpoint_destroy(&right), SALTS_OK);
  check_equal(cnet_packet_endpoint_destroy(&left), SALTS_OK);
}

static void cnet_packet_test_kcp_open_callback_drains_close(void) {
  cnet_packet_endpoint endpoint = {0};
  cnet_packet_test_probe probe = {0};
  cnet_packet_endpoint_config config = cnet_packet_test_config(CNET_PACKET_KCP, &probe);
  cnet_packet_terminal_config terminal_config = CNET_PACKET_TERMINAL_CONFIG_INIT;
  cnet_packet_session session = {0};
  cnet_packet_session_info info = {0};
  cnet_datagram_peer peer = cnet_packet_test_peer(10001u);

  config.session_capacity = 1u;
  terminal_config.send_capacity = 1u;
  terminal_config.on_send = cnet_packet_test_send_terminal;
  terminal_config.user = &probe;
  probe.send_and_close_on_open = true;
  check_equal(cnet_packet_endpoint_init_ex(&endpoint, &config, &terminal_config), SALTS_OK);
  check_equal(cnet_packet_session_open(&endpoint, &peer, UINT32_C(0x10203040), &session),
              SALTS_OK);
  check_equal(probe.open_count, 1);
  check_equal(probe.open_send_status, SALTS_OK);
  check_equal(probe.open_close_status, SALTS_OK);
  check_equal(probe.terminal_count, 1);
  check_equal(probe.terminal_tag, UINT64_C(77));
  check_equal(probe.terminal_status, SALTS_ECANCELED);
  check_equal(probe.close_count, 1);
  check_equal(probe.terminal_count_at_close, 1);
  check_equal(cnet_packet_session_get_info(&endpoint, session, &info), SALTS_ENOENT);

  check_equal(cnet_packet_endpoint_stop(&endpoint, CNET_PACKET_TEST_TIMEOUT_MS), SALTS_OK);
  check_equal(cnet_packet_endpoint_destroy(&endpoint), SALTS_OK);
}

static void cnet_packet_test_kcp_capacity_and_close(void) {
  cnet_packet_endpoint endpoint = {0};
  cnet_packet_test_probe probe = {0};
  cnet_packet_endpoint_config config = cnet_packet_test_config(CNET_PACKET_KCP, &probe);
  cnet_packet_terminal_config terminal_config = CNET_PACKET_TERMINAL_CONFIG_INIT;
  cnet_packet_session session = {0};
  cnet_packet_session reused_session = {0};
  cnet_datagram_peer peer = cnet_packet_test_peer(10001u);
  cnet_datagram_peer reused_peer = cnet_packet_test_peer(10002u);

  config.session_capacity = 1u;
  terminal_config.send_capacity = 1u;
  terminal_config.on_send = cnet_packet_test_send_terminal;
  terminal_config.user = &probe;
  check_equal(cnet_packet_endpoint_init_ex(&endpoint, &config, &terminal_config), SALTS_OK);
  check_equal(cnet_packet_session_open(&endpoint, &peer, UINT32_C(0x11223344), &session), SALTS_OK);
  check_equal(cnet_packet_send_tagged(&endpoint, session, "first", 5u, UINT64_C(11)), SALTS_OK);
  check_equal(cnet_packet_send_tagged(&endpoint, session, "second", 6u, UINT64_C(22)),
              SALTS_ENOBUFS);
  check_equal(probe.terminal_count, 0);

  check_equal(cnet_packet_session_close(&endpoint, session), SALTS_OK);
  check_equal(probe.terminal_count, 1);
  check_equal(probe.terminal_tag, UINT64_C(11));
  check_equal(probe.terminal_status, SALTS_ECANCELED);
  check_equal(probe.close_count, 1);
  check_equal(probe.terminal_count_at_close, 1);
  check_equal(cnet_packet_send_tagged(&endpoint, session, "stale", 5u, UINT64_C(33)), SALTS_ENOENT);
  check_equal(probe.terminal_count, 1);
  check_equal(cnet_packet_session_open(&endpoint, &reused_peer, UINT32_C(0x55667788),
                                       &reused_session),
              SALTS_OK);
  check_equal(reused_session.slot, session.slot);
  check_true(reused_session.generation != session.generation);
  check_equal(cnet_packet_send_tagged(&endpoint, reused_session, "reused", 6u, UINT64_C(44)),
              SALTS_OK);
  check_equal(cnet_packet_endpoint_stop(&endpoint, CNET_PACKET_TEST_TIMEOUT_MS), SALTS_OK);
  check_equal(probe.terminal_count, 2);
  check_equal(probe.terminal_tags[1], UINT64_C(44));
  check_equal(probe.terminal_statuses[1], SALTS_ECANCELED);
  check_equal(probe.terminal_generations[0], session.generation);
  check_equal(probe.terminal_generations[1], reused_session.generation);
  check_equal(probe.close_count, 2);
  check_equal(probe.terminal_count_at_close, 2);

  check_equal(cnet_packet_endpoint_destroy(&endpoint), SALTS_OK);
}

static void cnet_packet_test_terminal_config_validation(void) {
  cnet_packet_endpoint endpoint = {0};
  cnet_packet_test_probe probe = {0};
  cnet_packet_endpoint_config config = cnet_packet_test_config(CNET_PACKET_KCP, &probe);
  cnet_packet_terminal_config terminal_config = CNET_PACKET_TERMINAL_CONFIG_INIT;

  terminal_config.send_capacity = 1u;
  terminal_config.on_send = cnet_packet_test_send_terminal;
  terminal_config.user = &probe;
  terminal_config.version = CNET_PACKET_TERMINAL_API_VERSION + 1u;
  check_equal(cnet_packet_endpoint_init_ex(&endpoint, &config, &terminal_config), SALTS_EINVAL);
  check_null(endpoint.impl);

  terminal_config.version = CNET_PACKET_TERMINAL_API_VERSION;
  config.kcp.stream_mode = true;
  check_equal(cnet_packet_endpoint_init_ex(&endpoint, &config, &terminal_config), SALTS_EINVAL);
  check_null(endpoint.impl);
  check_equal(cnet_packet_endpoint_init(&endpoint, &config), SALTS_OK);
  check_equal(cnet_packet_endpoint_stop(&endpoint, CNET_PACKET_TEST_TIMEOUT_MS), SALTS_OK);
  check_equal(cnet_packet_endpoint_destroy(&endpoint), SALTS_OK);
}

static void cnet_packet_test_udp_stop_drains_terminal(void) {
  cnet_packet_endpoint endpoint = {0};
  cnet_packet_endpoint receiver = {0};
  cnet_packet_test_probe probe = {0};
  cnet_packet_test_probe receiver_probe = {0};
  cnet_packet_endpoint_config config = cnet_packet_test_config(CNET_PACKET_UDP, &probe);
  cnet_packet_endpoint_config receiver_config =
      cnet_packet_test_config(CNET_PACKET_UDP, &receiver_probe);
  cnet_packet_terminal_config terminal_config = CNET_PACKET_TERMINAL_CONFIG_INIT;
  cnet_packet_session session = {0};
  cnet_datagram_peer peer;
  uint16_t receiver_port = 0u;

  terminal_config.send_capacity = 1u;
  terminal_config.on_send = cnet_packet_test_send_terminal;
  terminal_config.user = &probe;
  check_equal(cnet_packet_endpoint_init_ex(&endpoint, &config, &terminal_config), SALTS_OK);
  check_equal(cnet_packet_endpoint_init(&receiver, &receiver_config), SALTS_OK);
  check_equal(cnet_packet_endpoint_port(&receiver, &receiver_port), SALTS_OK);
  peer = cnet_packet_test_peer(receiver_port);
  check_equal(cnet_packet_session_open(&endpoint, &peer, 0u, &session), SALTS_OK);
  check_equal(cnet_packet_send_tagged(&endpoint, session, "stop", 4u, UINT64_C(44)), SALTS_OK);
  check_equal(probe.terminal_count, 0);

  check_equal(cnet_packet_endpoint_stop(&endpoint, CNET_PACKET_TEST_TIMEOUT_MS), SALTS_OK);
  check_equal(probe.terminal_count, 1);
  check_equal(probe.terminal_tag, UINT64_C(44));
  check_true(probe.terminal_status == SALTS_OK || probe.terminal_status == SALTS_ECANCELED);
  check_equal(probe.close_count, 1);
  check_equal(probe.terminal_count_at_close, 1);
  check_equal(cnet_packet_endpoint_stop(&receiver, CNET_PACKET_TEST_TIMEOUT_MS), SALTS_OK);
  check_equal(cnet_packet_endpoint_destroy(&receiver), SALTS_OK);
  check_equal(cnet_packet_endpoint_destroy(&endpoint), SALTS_OK);
}

static void cnet_packet_test_tagged_send_requires_terminal_policy(void) {
  cnet_packet_endpoint endpoint = {0};
  cnet_packet_test_probe probe = {0};
  cnet_packet_endpoint_config config = cnet_packet_test_config(CNET_PACKET_UDP, &probe);
  cnet_packet_session session = {0};
  cnet_datagram_peer peer = cnet_packet_test_peer(10001u);

  check_equal(cnet_packet_endpoint_init(&endpoint, &config), SALTS_OK);
  check_equal(cnet_packet_session_open(&endpoint, &peer, 0u, &session), SALTS_OK);
  check_equal(cnet_packet_send_tagged(&endpoint, session, "x", 1u, UINT64_C(55)), SALTS_ENOTSUP);
  check_equal(probe.terminal_count, 0);
  check_equal(cnet_packet_endpoint_stop(&endpoint, CNET_PACKET_TEST_TIMEOUT_MS), SALTS_OK);
  check_equal(cnet_packet_endpoint_destroy(&endpoint), SALTS_OK);
}

static void cnet_packet_test_udp_tagged_capacity_reuse(void) {
  cnet_packet_endpoint left = {0};
  cnet_packet_endpoint right = {0};
  cnet_packet_test_probe left_probe = {0};
  cnet_packet_test_probe right_probe = {0};
  cnet_packet_endpoint_config left_config = cnet_packet_test_config(CNET_PACKET_UDP, &left_probe);
  cnet_packet_endpoint_config right_config = cnet_packet_test_config(CNET_PACKET_UDP, &right_probe);
  cnet_packet_terminal_config terminal_config = CNET_PACKET_TERMINAL_CONFIG_INIT;
  cnet_packet_session session = {0};
  cnet_datagram_peer peer;
  uint16_t right_port = 0u;
  size_t events = 0u;
  size_t attempts;

  terminal_config.send_capacity = 1u;
  terminal_config.on_send = cnet_packet_test_send_terminal;
  terminal_config.user = &left_probe;
  left_probe.send_again_on_terminal = true;
  check_equal(cnet_packet_endpoint_init_ex(&left, &left_config, &terminal_config), SALTS_OK);
  check_equal(cnet_packet_endpoint_init(&right, &right_config), SALTS_OK);
  check_equal(cnet_packet_endpoint_port(&right, &right_port), SALTS_OK);
  peer = cnet_packet_test_peer(right_port);
  check_equal(cnet_packet_session_open(&left, &peer, 0u, &session), SALTS_OK);
  check_equal(cnet_packet_send_tagged(&left, session, "one", 3u, UINT64_C(1)), SALTS_OK);
  check_equal(cnet_packet_send_tagged(&left, session, "full", 4u, UINT64_C(2)), SALTS_ENOBUFS);

  for (attempts = 0u; attempts < 1000u && left_probe.terminal_count == 0; ++attempts) {
    check_equal(cnet_packet_poll(&left, 1u, &events), SALTS_OK);
    check_equal(cnet_packet_poll(&right, 1u, &events), SALTS_OK);
  }
  check_equal(left_probe.terminal_count, 1);
  check_equal(left_probe.terminal_tag, UINT64_C(1));
  check_equal(left_probe.terminal_action_status, SALTS_OK);
  for (attempts = 0u; attempts < 1000u && left_probe.terminal_count != 2; ++attempts) {
    check_equal(cnet_packet_poll(&left, 1u, &events), SALTS_OK);
    check_equal(cnet_packet_poll(&right, 1u, &events), SALTS_OK);
  }
  check_equal(left_probe.terminal_count, 2);
  check_equal(left_probe.terminal_tags[1], UINT64_C(2));

  check_equal(cnet_packet_endpoint_stop(&right, CNET_PACKET_TEST_TIMEOUT_MS), SALTS_OK);
  check_equal(cnet_packet_endpoint_stop(&left, CNET_PACKET_TEST_TIMEOUT_MS), SALTS_OK);
  check_equal(cnet_packet_endpoint_destroy(&right), SALTS_OK);
  check_equal(cnet_packet_endpoint_destroy(&left), SALTS_OK);
}

static void cnet_packet_test_full_capacity_preserves_validation_errors(void) {
  unsigned char oversized[129] = {0};
  cnet_packet_endpoint udp = {0};
  cnet_packet_endpoint receiver = {0};
  cnet_packet_test_probe udp_probe = {0};
  cnet_packet_test_probe receiver_probe = {0};
  cnet_packet_endpoint_config udp_config = cnet_packet_test_config(CNET_PACKET_UDP, &udp_probe);
  cnet_packet_endpoint_config receiver_config =
      cnet_packet_test_config(CNET_PACKET_UDP, &receiver_probe);
  cnet_packet_terminal_config terminal_config = CNET_PACKET_TERMINAL_CONFIG_INIT;
  cnet_packet_session udp_session = {0};
  cnet_datagram_peer peer;
  uint16_t receiver_port = 0u;

  udp_config.datagram.max_datagram_bytes = sizeof(oversized) - 1u;
  udp_config.datagram.receive_buffer_bytes = sizeof(oversized) - 1u;
  terminal_config.send_capacity = 1u;
  terminal_config.on_send = cnet_packet_test_send_terminal;
  terminal_config.user = &udp_probe;
  check_equal(cnet_packet_endpoint_init_ex(&udp, &udp_config, &terminal_config), SALTS_OK);
  check_equal(cnet_packet_endpoint_init(&receiver, &receiver_config), SALTS_OK);
  check_equal(cnet_packet_endpoint_port(&receiver, &receiver_port), SALTS_OK);
  peer = cnet_packet_test_peer(receiver_port);
  check_equal(cnet_packet_session_open(&udp, &peer, 0u, &udp_session), SALTS_OK);
  check_equal(cnet_packet_send_tagged(&udp, udp_session, "held", 4u, UINT64_C(1)), SALTS_OK);
  check_equal(cnet_packet_send_tagged(&udp, udp_session, oversized, sizeof(oversized), UINT64_C(2)),
              SALTS_EMSGSIZE);
  check_equal(cnet_packet_endpoint_stop(&udp, CNET_PACKET_TEST_TIMEOUT_MS), SALTS_OK);
  check_equal(cnet_packet_endpoint_stop(&receiver, CNET_PACKET_TEST_TIMEOUT_MS), SALTS_OK);
  check_equal(cnet_packet_endpoint_destroy(&receiver), SALTS_OK);
  check_equal(cnet_packet_endpoint_destroy(&udp), SALTS_OK);
}

static void cnet_packet_test_kcp_session_failure_terminal(void) {
  static const uint32_t conversation = UINT32_C(0x12345678);
  unsigned char malformed_kcp[24] = {0};
  cnet_packet_endpoint endpoint = {0};
  cnet_packet_endpoint peer_endpoint = {0};
  cnet_packet_test_probe probe = {0};
  cnet_packet_test_probe peer_probe = {0};
  cnet_packet_endpoint_config config = cnet_packet_test_config(CNET_PACKET_KCP, &probe);
  cnet_packet_endpoint_config peer_config = cnet_packet_test_config(CNET_PACKET_UDP, &peer_probe);
  cnet_packet_terminal_config terminal_config = CNET_PACKET_TERMINAL_CONFIG_INIT;
  cnet_packet_session session = {0};
  cnet_packet_session peer_session = {0};
  cnet_packet_session_info info = {0};
  cnet_datagram_peer endpoint_peer;
  cnet_datagram_peer peer;
  uint16_t endpoint_port = 0u;
  uint16_t peer_port = 0u;
  size_t events = 0u;
  int status = SALTS_OK;

  terminal_config.send_capacity = 2u;
  terminal_config.on_send = cnet_packet_test_send_terminal;
  terminal_config.user = &probe;
  check_equal(cnet_packet_endpoint_init_ex(&endpoint, &config, &terminal_config), SALTS_OK);
  check_equal(cnet_packet_endpoint_init(&peer_endpoint, &peer_config), SALTS_OK);
  check_equal(cnet_packet_endpoint_port(&endpoint, &endpoint_port), SALTS_OK);
  check_equal(cnet_packet_endpoint_port(&peer_endpoint, &peer_port), SALTS_OK);
  endpoint_peer = cnet_packet_test_peer(endpoint_port);
  peer = cnet_packet_test_peer(peer_port);
  check_equal(cnet_packet_session_open(&endpoint, &peer, conversation, &session), SALTS_OK);
  check_equal(cnet_packet_session_open(&peer_endpoint, &endpoint_peer, 0u, &peer_session), SALTS_OK);
  check_equal(cnet_packet_send_tagged(&endpoint, session, "pending-a", 9u, UINT64_C(66)),
              SALTS_OK);
  check_equal(cnet_packet_send_tagged(&endpoint, session, "pending-b", 9u, UINT64_C(67)),
              SALTS_OK);

  malformed_kcp[0] = (unsigned char)conversation;
  malformed_kcp[1] = (unsigned char)(conversation >> 8u);
  malformed_kcp[2] = (unsigned char)(conversation >> 16u);
  malformed_kcp[3] = (unsigned char)(conversation >> 24u);
  malformed_kcp[4] = 0xffu;
  check_equal(cnet_packet_send(&peer_endpoint, peer_session, malformed_kcp, sizeof(malformed_kcp)),
              SALTS_OK);
  check_equal(cnet_packet_poll(&peer_endpoint, 1u, &events), SALTS_OK);
  status = cnet_packet_poll(&endpoint, 1u, &events);
  check_equal(status, SALTS_EPROTO);
  check_equal(probe.error_count, 1);
  check_equal(probe.last_error, SALTS_EPROTO);
  check_equal(probe.terminal_count, 2);
  check_equal(probe.terminal_tags[0], UINT64_C(66));
  check_equal(probe.terminal_statuses[0], SALTS_EPROTO);
  check_equal(probe.terminal_tags[1], UINT64_C(67));
  check_equal(probe.terminal_statuses[1], SALTS_EPROTO);
  check_equal(probe.close_count, 1);
  check_equal(probe.terminal_count_at_close, 2);

  check_equal(cnet_packet_endpoint_stop(&peer_endpoint, CNET_PACKET_TEST_TIMEOUT_MS), SALTS_OK);
  check_equal(cnet_packet_endpoint_stop(&endpoint, CNET_PACKET_TEST_TIMEOUT_MS), SALTS_OK);
  check_equal(cnet_packet_endpoint_destroy(&peer_endpoint), SALTS_OK);
  check_equal(cnet_packet_endpoint_destroy(&endpoint), SALTS_OK);

  probe = (cnet_packet_test_probe){0};
  peer_probe = (cnet_packet_test_probe){0};
  config = cnet_packet_test_config(CNET_PACKET_KCP, &probe);
  peer_config = cnet_packet_test_config(CNET_PACKET_UDP, &peer_probe);
  check_equal(cnet_packet_endpoint_init(&endpoint, &config), SALTS_OK);
  check_equal(cnet_packet_endpoint_init(&peer_endpoint, &peer_config), SALTS_OK);
  check_equal(cnet_packet_endpoint_port(&endpoint, &endpoint_port), SALTS_OK);
  check_equal(cnet_packet_endpoint_port(&peer_endpoint, &peer_port), SALTS_OK);
  endpoint_peer = cnet_packet_test_peer(endpoint_port);
  peer = cnet_packet_test_peer(peer_port);
  check_equal(cnet_packet_session_open(&endpoint, &peer, conversation, &session), SALTS_OK);
  check_equal(cnet_packet_session_open(&peer_endpoint, &endpoint_peer, 0u, &peer_session), SALTS_OK);
  check_equal(cnet_packet_send(&peer_endpoint, peer_session, malformed_kcp, sizeof(malformed_kcp)),
              SALTS_OK);
  check_equal(cnet_packet_poll(&peer_endpoint, 1u, &events), SALTS_OK);
  check_equal(cnet_packet_poll(&endpoint, 1u, &events), SALTS_EPROTO);
  check_equal(probe.close_count, 0);
  check_equal(cnet_packet_session_get_info(&endpoint, session, &info), SALTS_OK);

  check_equal(cnet_packet_endpoint_stop(&peer_endpoint, CNET_PACKET_TEST_TIMEOUT_MS), SALTS_OK);
  check_equal(cnet_packet_endpoint_stop(&endpoint, CNET_PACKET_TEST_TIMEOUT_MS), SALTS_OK);
  check_equal(cnet_packet_endpoint_destroy(&peer_endpoint), SALTS_OK);
  check_equal(cnet_packet_endpoint_destroy(&endpoint), SALTS_OK);
}

static void cnet_packet_test_kcp_ack_close_race(void) {
  cnet_packet_endpoint left = {0};
  cnet_packet_endpoint right = {0};
  cnet_packet_test_probe left_probe = {0};
  cnet_packet_test_probe right_probe = {0};
  cnet_packet_endpoint_config left_config = cnet_packet_test_config(CNET_PACKET_KCP, &left_probe);
  cnet_packet_endpoint_config right_config = cnet_packet_test_config(CNET_PACKET_KCP, &right_probe);
  cnet_packet_terminal_config terminal_config = CNET_PACKET_TERMINAL_CONFIG_INIT;
  cnet_packet_session session = {0};
  cnet_datagram_peer peer;
  uint16_t right_port = 0u;
  size_t events = 0u;
  size_t attempts;

  terminal_config.send_capacity = 1u;
  terminal_config.on_send = cnet_packet_test_send_terminal;
  terminal_config.user = &left_probe;
  right_probe.send_reply_on_receive = true;
  left_probe.close_on_receive = true;
  check_equal(cnet_packet_endpoint_init_ex(&left, &left_config, &terminal_config), SALTS_OK);
  check_equal(cnet_packet_endpoint_init(&right, &right_config), SALTS_OK);
  check_equal(cnet_packet_endpoint_port(&right, &right_port), SALTS_OK);
  peer = cnet_packet_test_peer(right_port);
  check_equal(cnet_packet_session_open(&left, &peer, UINT32_C(0x87654321), &session), SALTS_OK);
  check_equal(cnet_packet_send_tagged(&left, session, "request", 7u, UINT64_C(77)), SALTS_OK);

  for (attempts = 0u; attempts < 2000u && left_probe.close_count == 0; ++attempts) {
    check_equal(cnet_packet_poll(&left, 1u, &events), SALTS_OK);
    check_equal(cnet_packet_poll(&right, 1u, &events), SALTS_OK);
  }
  check_equal(right_probe.receive_action_status, SALTS_OK);
  check_equal(left_probe.receive_action_status, SALTS_OK);
  check_equal(left_probe.terminal_count, 1);
  check_equal(left_probe.terminal_tag, UINT64_C(77));
  check_equal(left_probe.terminal_status, SALTS_OK);
  check_equal(left_probe.close_count, 1);
  check_equal(left_probe.terminal_count_at_close, 1);

  check_equal(cnet_packet_endpoint_stop(&right, CNET_PACKET_TEST_TIMEOUT_MS), SALTS_OK);
  check_equal(cnet_packet_endpoint_stop(&left, CNET_PACKET_TEST_TIMEOUT_MS), SALTS_OK);
  check_equal(cnet_packet_endpoint_destroy(&right), SALTS_OK);
  check_equal(cnet_packet_endpoint_destroy(&left), SALTS_OK);
}

static void cnet_packet_test_secure_config(cnet_packet_endpoint_config *config,
                                           unsigned char psk_byte) {
  memset(config->security.pre_shared_key, psk_byte,
         sizeof(config->security.pre_shared_key));
  config->security.mode = CNET_KCP_SECURITY_PSK_V1;
  config->security.handshake_retry_ms = 10u;
  config->security.fec.backend = CNET_KCP_FEC_REED_SOLOMON;
  config->security.fec.data_shards = 2u;
  config->security.fec.parity_shards = 1u;
  config->security.fec.max_payload_bytes = 624u;
  config->security.fec.receive_group_count = 4u;
  config->kcp.mtu = 576u;
}

static void cnet_packet_test_secure_round_trip(size_t max_datagram_bytes) {
  static const unsigned char message[] = "authenticated-kcp";
  static const uint64_t tag = UINT64_C(0xabcdef0123456789);
  cnet_packet_endpoint left = {0};
  cnet_packet_endpoint right = {0};
  cnet_packet_test_probe left_probe = {0};
  cnet_packet_test_probe right_probe = {0};
  cnet_packet_endpoint_config left_config = cnet_packet_test_config(CNET_PACKET_KCP, &left_probe);
  cnet_packet_endpoint_config right_config = cnet_packet_test_config(CNET_PACKET_KCP, &right_probe);
  cnet_packet_terminal_config terminal_config = CNET_PACKET_TERMINAL_CONFIG_INIT;
  cnet_packet_session left_session = {0};
  cnet_datagram_peer right_peer;
  cnet_packet_session_info info = {0};
  uint16_t right_port = 0u;
  size_t events = 0u;
  size_t attempts;

  cnet_packet_test_secure_config(&left_config, 0x5au);
  cnet_packet_test_secure_config(&right_config, 0x5au);
  left_config.datagram.max_datagram_bytes = max_datagram_bytes;
  left_config.datagram.receive_buffer_bytes = max_datagram_bytes;
  right_config.datagram.max_datagram_bytes = max_datagram_bytes;
  right_config.datagram.receive_buffer_bytes = max_datagram_bytes;
  terminal_config.send_capacity = 2u;
  terminal_config.on_send = cnet_packet_test_send_terminal;
  terminal_config.user = &left_probe;
  check_equal(cnet_packet_endpoint_init_ex(&left, &left_config, &terminal_config), SALTS_OK);
  check_equal(cnet_packet_endpoint_init(&right, &right_config), SALTS_OK);
  check_equal(cnet_packet_endpoint_port(&right, &right_port), SALTS_OK);
  right_peer = cnet_packet_test_peer(right_port);
  check_equal(cnet_packet_session_open(&left, &right_peer, 0u, &left_session), SALTS_OK);
  check_equal(left_probe.connecting_count, 1);
  check_equal(left_probe.open_count, 0);
  check_equal(cnet_packet_send(&left, left_session, message, sizeof(message)), SALTS_EBUSY);

  for (attempts = 0u; attempts < 1000u && left_probe.open_count == 0; ++attempts) {
    check_equal(cnet_packet_poll(&left, 1u, &events), SALTS_OK);
    check_equal(cnet_packet_poll(&right, 1u, &events), SALTS_OK);
  }
  check_equal(left_probe.open_count, 1);
  check_equal(right_probe.admit_count, 1);
  check_equal(right_probe.open_count, 1);
  check_equal(cnet_packet_session_get_info(&left, left_session, &info), SALTS_OK);
  check_true(info.conversation != 0u);
  check_equal(cnet_packet_send_tagged(&left, left_session, message, sizeof(message), tag),
              SALTS_OK);
  check_equal(left_probe.terminal_count, 0);

  for (attempts = 0u; attempts < 1000u && right_probe.receive_count == 0; ++attempts) {
    check_equal(cnet_packet_poll(&left, 1u, &events), SALTS_OK);
    check_equal(cnet_packet_poll(&right, 1u, &events), SALTS_OK);
  }
  check_equal(right_probe.receive_count, 1);
  check_equal(right_probe.received_size, sizeof(message));
  check_equal(right_probe.received, message, sizeof(message));
  for (attempts = 0u; attempts < 1000u && left_probe.terminal_count == 0; ++attempts) {
    check_equal(cnet_packet_poll(&right, 1u, &events), SALTS_OK);
    check_equal(cnet_packet_poll(&left, 1u, &events), SALTS_OK);
  }
  check_equal(left_probe.terminal_count, 1);
  check_equal(left_probe.terminal_tag, tag);
  check_equal(left_probe.terminal_status, SALTS_OK);
  check_equal(left_probe.error_count, 0);
  check_equal(right_probe.error_count, 0);

  check_equal(cnet_packet_endpoint_stop(&right, CNET_PACKET_TEST_TIMEOUT_MS), SALTS_OK);
  check_equal(cnet_packet_endpoint_stop(&left, CNET_PACKET_TEST_TIMEOUT_MS), SALTS_OK);
  check_equal(cnet_packet_endpoint_destroy(&right), SALTS_OK);
  check_equal(cnet_packet_endpoint_destroy(&left), SALTS_OK);
}

static void cnet_packet_test_secure_full_capacity_preserves_handshake_error(void) {
  cnet_packet_endpoint left = {0};
  cnet_packet_endpoint right = {0};
  cnet_packet_endpoint waiting_right = {0};
  cnet_packet_test_probe left_probe = {0};
  cnet_packet_test_probe right_probe = {0};
  cnet_packet_test_probe waiting_probe = {0};
  cnet_packet_endpoint_config left_config = cnet_packet_test_config(CNET_PACKET_KCP, &left_probe);
  cnet_packet_endpoint_config right_config = cnet_packet_test_config(CNET_PACKET_KCP, &right_probe);
  cnet_packet_endpoint_config waiting_config =
      cnet_packet_test_config(CNET_PACKET_KCP, &waiting_probe);
  cnet_packet_terminal_config terminal_config = CNET_PACKET_TERMINAL_CONFIG_INIT;
  cnet_packet_session established = {0};
  cnet_packet_session handshaking = {0};
  cnet_datagram_peer right_peer;
  cnet_datagram_peer waiting_peer;
  uint16_t right_port = 0u;
  uint16_t waiting_port = 0u;
  size_t events = 0u;
  size_t attempts;

  cnet_packet_test_secure_config(&left_config, 0x3cu);
  cnet_packet_test_secure_config(&right_config, 0x3cu);
  cnet_packet_test_secure_config(&waiting_config, 0x3cu);
  terminal_config.send_capacity = 1u;
  terminal_config.on_send = cnet_packet_test_send_terminal;
  terminal_config.user = &left_probe;
  check_equal(cnet_packet_endpoint_init_ex(&left, &left_config, &terminal_config), SALTS_OK);
  check_equal(cnet_packet_endpoint_init(&right, &right_config), SALTS_OK);
  check_equal(cnet_packet_endpoint_init(&waiting_right, &waiting_config), SALTS_OK);
  check_equal(cnet_packet_endpoint_port(&right, &right_port), SALTS_OK);
  check_equal(cnet_packet_endpoint_port(&waiting_right, &waiting_port), SALTS_OK);
  right_peer = cnet_packet_test_peer(right_port);
  check_equal(cnet_packet_session_open(&left, &right_peer, 0u, &established), SALTS_OK);
  for (attempts = 0u; attempts < 1000u && left_probe.open_count == 0; ++attempts) {
    check_equal(cnet_packet_poll(&left, 1u, &events), SALTS_OK);
    check_equal(cnet_packet_poll(&right, 1u, &events), SALTS_OK);
  }
  check_equal(left_probe.open_count, 1);
  check_equal(cnet_packet_send_tagged(&left, established, "held", 4u, UINT64_C(1)), SALTS_OK);
  waiting_peer = cnet_packet_test_peer(waiting_port);
  check_equal(cnet_packet_session_open(&left, &waiting_peer, 0u, &handshaking), SALTS_OK);
  check_equal(cnet_packet_send_tagged(&left, handshaking, "busy", 4u, UINT64_C(2)), SALTS_EBUSY);

  check_equal(cnet_packet_endpoint_stop(&left, CNET_PACKET_TEST_TIMEOUT_MS), SALTS_OK);
  check_equal(cnet_packet_endpoint_stop(&waiting_right, CNET_PACKET_TEST_TIMEOUT_MS), SALTS_OK);
  check_equal(cnet_packet_endpoint_stop(&right, CNET_PACKET_TEST_TIMEOUT_MS), SALTS_OK);
  check_equal(cnet_packet_endpoint_destroy(&waiting_right), SALTS_OK);
  check_equal(cnet_packet_endpoint_destroy(&right), SALTS_OK);
  check_equal(cnet_packet_endpoint_destroy(&left), SALTS_OK);
}

static void cnet_packet_test_secure_authenticated_failure_settles_ack(void) {
  cnet_packet_endpoint left = {0};
  cnet_packet_endpoint right = {0};
  cnet_packet_test_probe left_probe = {0};
  cnet_packet_test_probe right_probe = {0};
  cnet_packet_endpoint_config left_config = cnet_packet_test_config(CNET_PACKET_KCP, &left_probe);
  cnet_packet_endpoint_config right_config = cnet_packet_test_config(CNET_PACKET_KCP, &right_probe);
  cnet_packet_terminal_config terminal_config = CNET_PACKET_TERMINAL_CONFIG_INIT;
  cnet_packet_session session = {0};
  cnet_datagram_peer peer;
  uint16_t right_port = 0u;
  size_t events = 0u;
  size_t attempts;
  int left_status = SALTS_OK;

  cnet_packet_test_secure_config(&left_config, 0x6du);
  cnet_packet_test_secure_config(&right_config, 0x6du);
  left_config.kcp.max_message_bytes = sizeof(left_probe.received);
  right_config.kcp.max_message_bytes = 700u;
  right_probe.send_reply_on_receive = true;
  right_probe.reply_size = 700u;
  terminal_config.send_capacity = 1u;
  terminal_config.on_send = cnet_packet_test_send_terminal;
  terminal_config.user = &left_probe;
  check_equal(cnet_packet_endpoint_init_ex(&left, &left_config, &terminal_config), SALTS_OK);
  check_equal(cnet_packet_endpoint_init(&right, &right_config), SALTS_OK);
  check_equal(cnet_packet_endpoint_port(&right, &right_port), SALTS_OK);
  peer = cnet_packet_test_peer(right_port);
  check_equal(cnet_packet_session_open(&left, &peer, 0u, &session), SALTS_OK);
  for (attempts = 0u; attempts < 1000u && left_probe.open_count == 0; ++attempts) {
    check_equal(cnet_packet_poll(&left, 1u, &events), SALTS_OK);
    check_equal(cnet_packet_poll(&right, 1u, &events), SALTS_OK);
  }
  check_equal(cnet_packet_send_tagged(&left, session, "request", 7u, UINT64_C(99)), SALTS_OK);
  for (attempts = 0u; attempts < 2000u && left_probe.close_count == 0; ++attempts) {
    if (left_status == SALTS_OK) left_status = cnet_packet_poll(&left, 1u, &events);
    check_equal(cnet_packet_poll(&right, 1u, &events), SALTS_OK);
  }
  check_equal(right_probe.receive_action_status, SALTS_OK);
  check_equal(left_status, SALTS_EMSGSIZE);
  check_equal(left_probe.last_error, SALTS_EMSGSIZE);
  check_equal(left_probe.terminal_count, 1);
  check_equal(left_probe.terminal_tag, UINT64_C(99));
  check_equal(left_probe.terminal_status, SALTS_OK);
  check_equal(left_probe.close_count, 1);
  check_equal(left_probe.terminal_count_at_close, 1);

  check_equal(cnet_packet_endpoint_stop(&right, CNET_PACKET_TEST_TIMEOUT_MS), SALTS_OK);
  check_equal(cnet_packet_endpoint_stop(&left, CNET_PACKET_TEST_TIMEOUT_MS), SALTS_OK);
  check_equal(cnet_packet_endpoint_destroy(&right), SALTS_OK);
  check_equal(cnet_packet_endpoint_destroy(&left), SALTS_OK);
}

spec("CNet unified UDP and KCP packet endpoint") {
  it("publishes one tagged UDP terminal only after native completion") {
    cnet_packet_test_udp_tagged_terminal();
  }

  it("waits for acknowledgements before publishing ordered KCP terminals") {
    cnet_packet_test_kcp_tagged_terminals();
  }

  it("rejects excess tagged KCP sends and cancels the admitted send on close") {
    cnet_packet_test_kcp_capacity_and_close();
  }

  it("drains a tagged KCP close initiated by the open callback") {
    cnet_packet_test_kcp_open_callback_drains_close();
  }

  it("validates terminal API versions without changing legacy KCP stream init") {
    cnet_packet_test_terminal_config_validation();
  }

  it("drains one tagged UDP terminal before stop completes") {
    cnet_packet_test_udp_stop_drains_terminal();
  }

  it("fails tagged admission when the endpoint has no terminal policy") {
    cnet_packet_test_tagged_send_requires_terminal_policy();
  }

  it("rejects full tagged UDP capacity and reuses it after terminal completion") {
    cnet_packet_test_udp_tagged_capacity_reuse();
  }

  it("reports size errors before full tagged-operation capacity") {
    cnet_packet_test_full_capacity_preserves_validation_errors();
  }

  it("settles pending KCP sends with the concrete remote session failure") {
    cnet_packet_test_kcp_session_failure_terminal();
  }

  it("keeps an acknowledged KCP terminal successful when receive closes the session") {
    cnet_packet_test_kcp_ack_close_race();
  }

  it("uses one endpoint contract for UDP") { cnet_packet_test_round_trip(CNET_PACKET_UDP, 0u); }

  it("uses one endpoint contract for KCP") {
    cnet_packet_test_round_trip(CNET_PACKET_KCP, UINT32_C(0x12345678));
  }

  it("uses the endpoint contract for authenticated KCP after handshake") {
    cnet_packet_test_secure_round_trip(1500u);
  }

  group("authenticated FEC datagram capacity") {
    enum {
      FEC_WIRE_OVERHEAD_BYTES = 2 + 30 + 16,
      FEC_MAX_WIRE_BYTES = 624 + FEC_WIRE_OVERHEAD_BYTES
    };
    static cnet_packet_endpoint endpoint;
    static cnet_packet_test_probe probe;
    static cnet_packet_endpoint_config config;

    before_each() {
      memset(&endpoint, 0, sizeof(endpoint));
      memset(&probe, 0, sizeof(probe));
      config = cnet_packet_test_config(CNET_PACKET_KCP, &probe);
      cnet_packet_test_secure_config(&config, 0x5au);
      config.datagram.receive_buffer_bytes = FEC_MAX_WIRE_BYTES;
    }

    after_each() {
      if (endpoint.impl != NULL) {
        check_equal(cnet_packet_endpoint_stop(&endpoint, CNET_PACKET_TEST_TIMEOUT_MS), SALTS_OK);
        check_equal(cnet_packet_endpoint_destroy(&endpoint), SALTS_OK);
      }
    }

    it("rejects a data-only wire budget before publishing an endpoint") {
      config.datagram.max_datagram_bytes = FEC_MAX_WIRE_BYTES - 2u;
      check_equal(cnet_packet_endpoint_init(&endpoint, &config), SALTS_EINVAL);
      check_null(endpoint.impl);
    }

    it("rejects a wire budget one byte below the parity frame size") {
      config.datagram.max_datagram_bytes = FEC_MAX_WIRE_BYTES - 1u;
      check_equal(cnet_packet_endpoint_init(&endpoint, &config), SALTS_EINVAL);
      check_null(endpoint.impl);
    }

    it("rejects parity exceeding the maximum supported UDP payload") {
      config.security.fec.max_payload_bytes =
          CNET_DATAGRAM_MAX_PAYLOAD_BYTES - FEC_WIRE_OVERHEAD_BYTES + 1u;
      config.datagram.max_datagram_bytes = CNET_DATAGRAM_MAX_PAYLOAD_BYTES;
      config.datagram.receive_buffer_bytes = CNET_DATAGRAM_MAX_PAYLOAD_BYTES;
      check_equal(cnet_packet_endpoint_init(&endpoint, &config), SALTS_EINVAL);
      check_null(endpoint.impl);
    }

    it("supports authenticated traffic at the exact parity frame budget") {
      cnet_packet_test_secure_round_trip(FEC_MAX_WIRE_BYTES);
    }
  }

  it("reports secure handshake readiness before full tagged-operation capacity") {
    cnet_packet_test_secure_full_capacity_preserves_handshake_error();
  }

  it("settles authenticated acknowledgements before a fatal receive error") {
    cnet_packet_test_secure_authenticated_failure_settles_ack();
  }

  it("does not admit an unauthenticated KCP client hello") {
    cnet_packet_endpoint left = {0};
    cnet_packet_endpoint right = {0};
    cnet_packet_test_probe left_probe = {0};
    cnet_packet_test_probe right_probe = {0};
    cnet_packet_endpoint_config left_config = cnet_packet_test_config(CNET_PACKET_KCP, &left_probe);
    cnet_packet_endpoint_config right_config = cnet_packet_test_config(CNET_PACKET_KCP, &right_probe);
    cnet_packet_session left_session = {0};
    cnet_datagram_peer right_peer;
    uint16_t right_port = 0u;
    size_t events = 0u;
    size_t attempts;

    cnet_packet_test_secure_config(&left_config, 0x11u);
    cnet_packet_test_secure_config(&right_config, 0x22u);
    check_equal(cnet_packet_endpoint_init(&left, &left_config), SALTS_OK);
    check_equal(cnet_packet_endpoint_init(&right, &right_config), SALTS_OK);
    check_equal(cnet_packet_endpoint_port(&right, &right_port), SALTS_OK);
    right_peer = cnet_packet_test_peer(right_port);
    check_equal(cnet_packet_session_open(&left, &right_peer, 0u, &left_session), SALTS_OK);
    for (attempts = 0u; attempts < 50u; ++attempts) {
      check_equal(cnet_packet_poll(&left, 1u, &events), SALTS_OK);
      check_equal(cnet_packet_poll(&right, 1u, &events), SALTS_OK);
    }
    check_equal(left_probe.connecting_count, 1);
    check_equal(left_probe.open_count, 0);
    check_equal(right_probe.admit_count, 0);
    check_equal(right_probe.open_count, 0);
    check_equal(left_probe.error_count, 0);
    check_equal(right_probe.error_count, 0);

    check_equal(cnet_packet_endpoint_stop(&right, CNET_PACKET_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(cnet_packet_endpoint_stop(&left, CNET_PACKET_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(cnet_packet_endpoint_destroy(&right), SALTS_OK);
    check_equal(cnet_packet_endpoint_destroy(&left), SALTS_OK);
  }

  it("rejects stale sessions after bounded slot reuse") {
    cnet_packet_endpoint endpoint = {0};
    cnet_packet_test_probe probe = {0};
    cnet_packet_endpoint_config config = cnet_packet_test_config(CNET_PACKET_UDP, &probe);
    cnet_datagram_peer first_peer = cnet_packet_test_peer(10001u);
    cnet_datagram_peer second_peer = cnet_packet_test_peer(10002u);
    cnet_packet_session first = {0};
    cnet_packet_session second = {0};
    cnet_packet_session rejected = {0};
    cnet_packet_session_info info = {0};

    config.session_capacity = 1u;
    check_equal(cnet_packet_endpoint_init(&endpoint, &config), SALTS_OK);
    check_equal(cnet_packet_endpoint_init(&endpoint, &config), SALTS_EALREADY);
    check_not_null(endpoint.impl);
    check_equal(cnet_packet_session_open(&endpoint, &first_peer, 0u, &first), SALTS_OK);
    check_equal(cnet_packet_session_get_info(&endpoint, first, &info), SALTS_OK);
    check_equal(info.protocol, CNET_PACKET_UDP);
    check_equal(info.peer.port, first_peer.port);
    check_equal(info.conversation, 0u);
    check_equal(cnet_packet_session_open(&endpoint, &first_peer, 0u, &rejected), SALTS_EALREADY);
    check_false(cnet_packet_session_valid(rejected));
    check_equal(cnet_packet_session_open(&endpoint, &second_peer, 0u, &rejected), SALTS_ENOBUFS);
    check_false(cnet_packet_session_valid(rejected));
    check_equal(cnet_packet_session_close(&endpoint, first), SALTS_OK);
    check_equal(cnet_packet_session_open(&endpoint, &second_peer, 0u, &second), SALTS_OK);
    check_equal(first.slot, second.slot);
    check_true(first.generation != second.generation);
    check_equal(cnet_packet_send(&endpoint, first, "x", 1u), SALTS_ENOENT);
    check_equal(cnet_packet_session_get_info(&endpoint, first, &info), SALTS_ENOENT);
    check_equal(cnet_packet_endpoint_stop(&endpoint, CNET_PACKET_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(cnet_packet_endpoint_destroy(&endpoint), SALTS_OK);
  }

  it("preserves colliding peer lookup through bounded backshift and slot reuse") {
    cnet_packet_endpoint endpoint = {0};
    cnet_packet_test_probe probe = {0};
    cnet_packet_endpoint_config config = cnet_packet_test_config(CNET_PACKET_UDP, &probe);
    cnet_datagram_peer peers[4] = {0};
    cnet_packet_session sessions[3] = {0};
    cnet_packet_session current = {0};
    cnet_packet_session rejected = {0};
    cnet_packet_session_info info = {0};
    size_t matched = 0u;
    size_t bucket = SIZE_MAX;

    config.session_capacity = 3u; /* The private index has 8 buckets. */
    for (uint32_t port = 10000u; port <= 65535u && matched < 4u; ++port) {
      cnet_datagram_peer peer = cnet_packet_test_peer((uint16_t)port);
      const size_t candidate = cnet_packet_test_peer_bucket(&peer, 0u, 7u);
      if (bucket == SIZE_MAX) bucket = candidate;
      if (candidate == bucket) peers[matched++] = peer;
    }
    check_equal(matched, (size_t)4u);
    if (matched != 4u) return;
    check_equal(cnet_packet_endpoint_init(&endpoint, &config), SALTS_OK);
    for (size_t i = 0u; i < 3u; ++i) {
      check_equal(cnet_packet_session_open(&endpoint, &peers[i], 0u, &sessions[i]), SALTS_OK);
      check_true(cnet_packet_session_valid(sessions[i]));
    }
    check_equal(cnet_packet_session_open(&endpoint, &peers[3], 0u, &rejected), SALTS_ENOBUFS);
    check_false(cnet_packet_session_valid(rejected));
    check_equal(cnet_packet_session_close(&endpoint, sessions[1]), SALTS_OK);
    check_equal(cnet_packet_session_open(&endpoint, &peers[3], 0u, &current), SALTS_OK);
    check_equal(cnet_packet_session_get_info(&endpoint, sessions[0], &info), SALTS_OK);
    check_equal(info.peer.port, peers[0].port);
    check_equal(cnet_packet_session_get_info(&endpoint, sessions[2], &info), SALTS_OK);
    check_equal(info.peer.port, peers[2].port);
    check_equal(cnet_packet_session_open(&endpoint, &peers[0], 0u, &rejected), SALTS_EALREADY);
    for (size_t iteration = 0u; iteration < 32u; ++iteration) {
      const cnet_datagram_peer *next = &peers[iteration & 1u ? 3u : 1u];
      const cnet_packet_session stale = current;
      check_equal(cnet_packet_session_close(&endpoint, current), SALTS_OK);
      check_equal(cnet_packet_session_open(&endpoint, next, 0u, &current), SALTS_OK);
      check_true(current.generation != stale.generation);
      check_equal(cnet_packet_session_get_info(&endpoint, stale, &info), SALTS_ENOENT);
      check_equal(cnet_packet_session_get_info(&endpoint, sessions[0], &info), SALTS_OK);
      check_equal(cnet_packet_session_get_info(&endpoint, sessions[2], &info), SALTS_OK);
    }
    check_equal(cnet_packet_endpoint_stop(&endpoint, CNET_PACKET_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(cnet_packet_endpoint_destroy(&endpoint), SALTS_OK);
  }
}
