#include <cnet/client_pool.h>
#include <cnet/managed_dial.h>
#include <cnet/websocket_transport.h>
#include <fmt.h>
#include <salts/clock.h>
#include <string.h>
#include <tinytest.h>

/* Actual TCP writes, Manager recycle and Pool leases share one Owner. */
enum { WAIT_MS = 5000, SEND_BYTES = 64 };
static cnet_client client, peers;
static cnet_listener listener;
static cnet_manager manager;
static cnet_managed_dial dial;
static cnet_client_pool pool;
static cnet_pool_connection physical;
static cnet_pool_lease lease;
static cnet_pool_key key;
static cnet_websocket_transport ws_transport;
static cnet_websocket *ws;
static mem_buffer_t *output;
static cnet_connection outgoing, accepted[3];
static tstr uri;
static size_t connected, terminals, raw_sends, tags, received, accept_count, gate_calls;
static size_t client_received, expected_client_reply;
static int tag_status;
static uint64_t last_tag;
static unsigned char wire[256];
static bool stopping;
#if defined(CNET_COMPOSITION_TLS)
static cnet_tls_server tls_server;
static cnet_tls_client tls_client;
#endif

static native_io_backend_kind backend_kind(void) {
#if defined(_WIN32)
  return NATIVE_IO_BACKEND_IOCP;
#elif defined(__APPLE__)
  return NATIVE_IO_BACKEND_KQUEUE;
#else
  return NATIVE_IO_BACKEND_EPOLL;
#endif
}
static void state_cb(void *u, cnet_connection c, cnet_connection_state s, const cnet_error *e) {
  (void)u;
  (void)c;
  (void)e;
  if (s == CNET_CONNECTION_CONNECTED) ++connected;
  if (s == CNET_CONNECTION_CLOSED || s == CNET_CONNECTION_FAILED) ++terminals;
}
static void peer_state(void *u, cnet_connection c, cnet_connection_state s, const cnet_error *e) {
  (void)u;
  (void)e;
  if (!stopping && s == CNET_CONNECTION_CONNECTED)
    check_warn(cnet_receive(&peers, c, 1u) == SALTS_OK);
}
static void sent_cb(void *u, cnet_connection c, size_t n) {
  (void)u;
  (void)c;
  (void)n;
  ++raw_sends;
}
static void client_receive_cb(void *u, cnet_connection c, const cnet_receive_view *v) {
  (void)u;
  (void)c;
  client_received += v->size;
  /* A real application reply may be fragmented across transport receives. */
  if (expected_client_reply != 0u && client_received < expected_client_reply)
    check_warn(cnet_receive(&client, c, 1u) == SALTS_OK);
}
static void receive_cb(void *u, cnet_connection c, const cnet_receive_view *v) {
  (void)u;
  if (received + v->size <= sizeof(wire)) {
    memcpy(wire + received, v->data, v->size);
    received += v->size;
  } else check_warn(false);
  if (!stopping) check_warn(cnet_receive(&peers, c, 1u) == SALTS_OK);
}
static void event_cb(void *u, cnet_websocket *s, const cnet_websocket_event *e) {
  (void)u;
  (void)s;
  (void)e;
}
static void tag_cb(void *u, cnet_websocket *s, uint64_t tag, size_t n, int status) {
  (void)u;
  (void)s;
  (void)n;
  ++tags;
  last_tag = tag;
  tag_status = status;
}
static cnet_reconnect_failure_kind classify(void *u, cnet_connection_state s, const cnet_error *e) {
  (void)u;
  (void)s;
  (void)e;
  return CNET_RECONNECT_TRANSIENT; /* Explicit test-only recovery permission. */
}
static int admit(void *u) {
  uint64_t wait;
  (void)u;
  ++gate_calls;
  check_warn(cnet_managed_dial_advance(&dial, cmeta_monotonic_ms(), &wait) == SALTS_EBUSY);
  check_warn(cnet_managed_dial_seal(&dial) == SALTS_EBUSY);
  if (ws_transport.impl != NULL || lease.slot != 0u) return SALTS_EBUSY;
  if (physical.slot == 0u) return cnet_pool_reserve_connecting(&pool, &key, &physical);
  return SALTS_OK;
}
static void progress(void) {
  size_t events, work;
  int ready = 0;
  check_equal(cnet_client_poll(&client, 0u, &events), SALTS_OK);
  check_equal(cnet_client_poll(&peers, 0u, &events), SALTS_OK);
  check_equal(cnet_manager_advance(&manager, 2u, &work), SALTS_OK);
  if (!stopping && accept_count < 3u) {
    int rc = cnet_listener_wait(&listener, 0u, &ready);
    check(rc == SALTS_OK || rc == SALTS_ETIMEDOUT);
    if (ready) {
      const cnet_observer o = {.on_state = peer_state, .on_receive = receive_cb};
      cnet_accepted_stream stream = {0};
      check_equal(cnet_listener_accept_detached(&listener, &stream), SALTS_OK);
#if defined(CNET_COMPOSITION_TLS)
      check_equal(
          cnet_client_adopt_accepted_tls(&peers, &stream, &tls_server, &o, &accepted[accept_count]),
          SALTS_OK);
#else
      check_equal(cnet_client_adopt_accepted(&peers, &stream, &o, &accepted[accept_count]),
                  SALTS_OK);
#endif
      ++accept_count;
    }
  }
}
static void wait_connected(size_t count) {
  const uint64_t until = cmeta_monotonic_ms() + WAIT_MS;
  while ((connected < count || accept_count < count) && cmeta_monotonic_ms() < until)
    progress();
  check_equal(connected, count);
  check_equal(accept_count, count);
}
static cnet_websocket_config ws_config(void) {
  cnet_websocket_config c = {0};
  c.size = sizeof(c);
  c.role = CNET_WEBSOCKET_SERVER;
  c.max_frame_bytes = 8u;
  c.max_message_bytes = 32u;
  c.max_buffered_input_bytes = 64u;
  c.on_event = event_cb;
  c.output_buffer = output;
  return c;
}
static int bind_ws(void) {
  cnet_websocket_config c = ws_config();
  const cnet_websocket_tagged_policy p = {sizeof(p), CNET_WEBSOCKET_TAGGED_SEND_VERSION, 3u, tag_cb,
                                          NULL};
  int rc = cnet_websocket_transport_init(&ws_transport, &client, outgoing, &c, &p);
  if (rc == SALTS_OK) rc = cnet_websocket_transport_session(&ws_transport, &ws);
  return rc;
}
static mem_buffer_t *bytes(const void *data, size_t n) {
  mem_buffer_t *b = mem_get_buffer(mem_global(), n);
  check_not_null(b);
  memcpy(mem_buffer_data(b), data, n);
  mem_set_used(b, n);
  return b;
}
static void send_raw(cnet_connection c, const void *data, size_t n) {
  mem_buffer_t *b = bytes(data, n);
  int rc = cnet_send_buffer(&client, c, b);
  mem_buffer_release(b);
  check_equal(rc, SALTS_OK);
}
static void wait_tag(void) {
  const uint64_t until = cmeta_monotonic_ms() + WAIT_MS;
  size_t events;
  while (tags == 0u && cmeta_monotonic_ms() < until) {
    progress();
    check_equal(cnet_websocket_transport_advance(&ws_transport, 2u, &events), SALTS_OK);
  }
  check_equal(tags, 1u);
}

spec("CNet dedicated WS transport and pooled recovery composition") {
  before_each() {
    cnet_client_config c = {.backend = backend_kind(),
                            .connection_capacity = 2u,
                            .command_capacity = 2u,
                            .request_capacity = 16u,
                            .completion_batch_capacity = 8u,
                            .event_capacity = 16u,
                            .max_send_bytes = SEND_BYTES,
                            .receive_buffer_bytes = 64u,
                            .connect_timeout_ms = WAIT_MS,
                            .write_timeout_ms = WAIT_MS};
    const cnet_listener_config l = {backend_kind(), "127.0.0.1", 0u, 8u};
    const cnet_manager_config m = {sizeof(m), CNET_MANAGER_VERSION, &client, 2u, 2u};
    const cnet_pool_config p = {sizeof(p), CNET_CLIENT_POOL_VERSION, &manager, 1u, 1u, 1u, 1u};
    cnet_managed_dial_config d = {0};
    cnet_managed_dial_snapshot snapshot;
    uint16_t port;
    uint64_t wait;
    stopping = false;
    connected = terminals = raw_sends = tags = received = accept_count = gate_calls = 0u;
    client_received = expected_client_reply = 0u;
    tag_status = SALTS_OK;
    last_tag = 0u;
#if defined(CNET_COMPOSITION_TLS)
    c.tls_io_buffer_bytes = CNET_TLS_MIN_IO_BUFFER_BYTES;
    c.tls_handshake_timeout_ms = WAIT_MS;
    const cnet_tls_server_config server_config = {.size = sizeof(server_config),
                                                  .cert_file = CNET_COMPOSITION_CERT,
                                                  .key_file = CNET_COMPOSITION_KEY};
    const cnet_tls_client_config client_config = {
        .size = sizeof(client_config), .ca_file = CNET_COMPOSITION_CA, .server_name = "127.0.0.1"};
    check_equal(cnet_tls_server_init(&tls_server, &server_config), SALTS_OK);
    check_equal(cnet_tls_client_init(&tls_client, &client_config), SALTS_OK);
#endif
    memset(accepted, 0, sizeof(accepted));
    memset(wire, 0, sizeof(wire));
    physical = (cnet_pool_connection){0};
    lease = (cnet_pool_lease){0};
    ws = NULL;
    check_equal(cnet_client_init(&client, &c), SALTS_OK);
    c.connection_capacity = 4u;
    c.command_capacity = 8u;
    check_equal(cnet_client_init(&peers, &c), SALTS_OK);
    cnet_stream_socket_options socket_options = CNET_STREAM_SOCKET_OPTIONS_INIT;
    socket_options.linger = 1; /* Permit a deterministic real peer-reset case. */
    check_equal(cnet_client_set_stream_socket_options(&peers, &socket_options), SALTS_OK);
    check_equal(cnet_listener_init(&listener, &l), SALTS_OK);
    check_equal(cnet_listener_port(&listener, &port), SALTS_OK);
#if defined(CNET_COMPOSITION_TLS)
    uri = tstr_format("tls://127.0.0.1:{}", port);
    d.connection.tls_client = &tls_client;
#else
    uri = tstr_format("tcp://127.0.0.1:{}", port);
#endif
    check_not_null(uri);
    check_equal(cnet_manager_init(&manager, &m), SALTS_OK);
    check_equal(cnet_pool_init(&pool, &p), SALTS_OK);
    key = (cnet_pool_key){.size = sizeof(key),
                          .version = CNET_CLIENT_POOL_VERSION,
                          .runtime_id = 1u,
                          .owner_id = 1u,
                          .endpoint_id = 1u,
                          .authority_id = 1u,
                          .transport_id = 1u,
                          .protocol_id = 1u,
                          .session_id = 1u};
    d.size = sizeof(d);
    d.version = CNET_MANAGED_DIAL_VERSION;
    d.client = &client;
    d.manager = &manager;
    d.connection.uri = uri;
    d.connection.observer = (cnet_observer){
        .on_state = state_cb, .on_send = sent_cb, .on_receive = client_receive_cb};
    d.recovery = (cnet_reconnect_config){sizeof(d.recovery),
                                         CNET_RECOVERY_POLICY_VERSION,
                                         3u,
                                         cmeta_monotonic_ms() + WAIT_MS,
                                         10u,
                                         20u,
                                         17u};
    d.recovery_episode_ms = WAIT_MS;
    d.classify = classify;
    check_equal(cnet_managed_dial_init_admitted(&dial, &d, admit, NULL), SALTS_OK);
    check_equal(cnet_managed_dial_advance(&dial, cmeta_monotonic_ms(), &wait), SALTS_OK);
    check_equal(cnet_managed_dial_get_snapshot(&dial, &snapshot), SALTS_OK);
    outgoing = snapshot.connection;
    wait_connected(1u);
    output = mem_get_buffer(mem_global(), 32u);
    check_not_null(output);
  }
  after_each() {
    size_t events, work;
    stopping = true;
    if (dial.impl != NULL) check_warn(cnet_managed_dial_seal(&dial) == SALTS_OK);
    if (client.impl != NULL) check_warn(cnet_client_stop(&client, WAIT_MS) == SALTS_OK);
    if (peers.impl != NULL) check_warn(cnet_client_stop(&peers, WAIT_MS) == SALTS_OK);
    if (ws_transport.impl != NULL) {
      (void)cnet_websocket_transport_advance(&ws_transport, 8u, &events);
      check_warn(cnet_websocket_transport_destroy(&ws_transport) == SALTS_OK);
    }
    if (lease.slot != 0u) {
      check_warn(cnet_pool_release(&pool, lease) == SALTS_OK);
      lease = (cnet_pool_lease){0};
    }
    if (physical.slot != 0u) {
      check_warn(cnet_pool_terminal(&pool, physical) == SALTS_OK);
      physical = (cnet_pool_connection){0};
    }
    if (pool.impl != NULL) check_warn(cnet_pool_destroy(&pool) == SALTS_OK);
    if (manager.impl != NULL) check_warn(cnet_manager_advance(&manager, 2u, &work) == SALTS_OK);
    if (dial.impl != NULL) check_warn(cnet_managed_dial_destroy(&dial) == SALTS_OK);
    if (manager.impl != NULL) check_warn(cnet_manager_destroy(&manager) == SALTS_OK);
    if (client.impl != NULL) check_warn(cnet_client_destroy(&client) == SALTS_OK);
    if (peers.impl != NULL) check_warn(cnet_client_destroy(&peers) == SALTS_OK);
    if (listener.impl != NULL) {
      check_warn(cnet_listener_close(&listener) == SALTS_OK);
      check_warn(cnet_listener_destroy(&listener) == SALTS_OK);
    }
    if (output != NULL) {
      mem_buffer_release(output);
      output = NULL;
    }
#if defined(CNET_COMPOSITION_TLS)
    if (tls_client.impl != NULL) check_warn(cnet_tls_client_destroy(&tls_client) == SALTS_OK);
    if (tls_server.impl != NULL) check_warn(cnet_tls_server_destroy(&tls_server) == SALTS_OK);
#endif
    tstr_free(uri);
    uri = NULL;
  }
  it("requires a real handshake-write barrier and consumes only WS frame terminals") {
    size_t events;
    const uint64_t until = cmeta_monotonic_ms() + WAIT_MS;
    send_raw(outgoing, "HTTP!", 5u); /* Equal to one encoded WS frame's size. */
    check_equal(bind_ws(), SALTS_EBUSY);
    check_null(ws_transport.impl);
    while ((raw_sends != 1u || received != 5u) && cmeta_monotonic_ms() < until)
      progress();
    check_equal(raw_sends, 1u);
    check_equal(received, 5u);
    check_equal(bind_ws(), SALTS_OK);
    mem_buffer_t *b = bytes("other", 5u);
    check_equal(cnet_send_buffer(&client, outgoing, b), SALTS_EBUSY);
    mem_buffer_release(b);
    check_equal(cnet_websocket_send_tagged(ws, CNET_WEBSOCKET_MESSAGE_BINARY, "abc", 3u, 7u),
                SALTS_OK);
    check_equal(cnet_websocket_transport_advance(&ws_transport, 1u, &events), SALTS_OK);
    check_equal(tags, 0u);
    wait_tag();
    while (received != 10u && cmeta_monotonic_ms() < until)
      progress();
    const unsigned char expected[] = {'H', 'T', 'T', 'P', '!', 0x82, 3, 'a', 'b', 'c'};
    check_equal(received, sizeof(expected));
    check_equal(wire, expected, sizeof(expected));
    check_equal(raw_sends, 1u);
    check_equal(last_tag, UINT64_C(7));
    check_equal(tag_status, SALTS_OK);
  }
  it("retries actual shared queue pressure without duplicate WS frame admission") {
    cnet_connection neighbor;
    cnet_connect_options o = {.uri = uri, .observer = {.on_state = state_cb, .on_send = sent_cb}};
#if defined(CNET_COMPOSITION_TLS)
    o.tls_client = &tls_client;
#endif
    size_t events;
    check_equal(cnet_connect(&client, &o, &neighbor), SALTS_OK);
    wait_connected(2u);
    check_equal(bind_ws(), SALTS_OK);
    send_raw(neighbor, "n", 1u);
    send_raw(neighbor, "n", 1u);
    check_equal(cnet_websocket_send_tagged(ws, CNET_WEBSOCKET_MESSAGE_BINARY, "abc", 3u, 8u),
                SALTS_OK);
    check_equal(cnet_websocket_transport_advance(&ws_transport, 1u, &events), SALTS_OK);
    check_equal(events, 0u);
    check_equal(tags, 0u);
    cnet_websocket_state state;
    check_equal(cnet_websocket_state_get(ws, &state), SALTS_OK);
    check_equal(state, CNET_WEBSOCKET_OPEN);
    wait_tag();
    check_equal(tag_status, SALTS_OK);
    check_equal(last_tag, UINT64_C(8));
    const uint64_t until = cmeta_monotonic_ms() + WAIT_MS;
    while (received < 7u && cmeta_monotonic_ms() < until) {
      progress();
      check_equal(cnet_websocket_transport_advance(&ws_transport, 1u, &events), SALTS_OK);
    }
    check_equal(tags, 1u);
    check_equal(raw_sends, 2u);
    check_equal(received, 7u);
  }
  it("settles a queue-blocked WS message when the client stops") {
    cnet_connection neighbor;
    cnet_connect_options o = {.uri = uri, .observer = {.on_state = state_cb, .on_send = sent_cb}};
#if defined(CNET_COMPOSITION_TLS)
    o.tls_client = &tls_client;
#endif
    size_t events;
    check_equal(cnet_connect(&client, &o, &neighbor), SALTS_OK);
    wait_connected(2u);
    check_equal(bind_ws(), SALTS_OK);
    send_raw(neighbor, "n", 1u);
    send_raw(neighbor, "n", 1u);
    check_equal(cnet_websocket_send_tagged(ws, CNET_WEBSOCKET_MESSAGE_BINARY, "abc", 3u, 12u),
                SALTS_OK);
    check_equal(cnet_websocket_transport_advance(&ws_transport, 1u, &events), SALTS_OK);
    check_equal(events, 0u);
    check(cnet_websocket_has_pending_output(ws));
    check_equal(cnet_client_stop(&client, WAIT_MS), SALTS_OK);
    check_equal(tags, 0u);
    check_equal(cnet_websocket_transport_advance(&ws_transport, 1u, &events), SALTS_OK);
    check_equal(tags, 1u);
    check_equal(last_tag, UINT64_C(12));
    check_equal(tag_status, SALTS_ECANCELED);
    check_equal(cnet_websocket_transport_destroy(&ws_transport), SALTS_OK);
  }
  it("settles cancelled native writes before logical callbacks and prevents early reuse") {
    size_t events;
    check_equal(bind_ws(), SALTS_OK);
    check_equal(cnet_websocket_send_tagged(ws, CNET_WEBSOCKET_MESSAGE_BINARY, "abcdef", 6u, 9u),
                SALTS_OK);
    check_equal(cnet_websocket_transport_advance(&ws_transport, 1u, &events), SALTS_OK);
    check_equal(cnet_websocket_transport_destroy(&ws_transport), SALTS_EBUSY);
    check_equal(cnet_close(&client, outgoing), SALTS_OK);
    const uint64_t until = cmeta_monotonic_ms() + WAIT_MS;
    while (terminals == 0u && cmeta_monotonic_ms() < until)
      progress();
    check_equal(terminals, 1u);
    check_equal(tags, 0u);
    check_equal(raw_sends, 0u);
    check_equal(cnet_websocket_transport_destroy(&ws_transport), SALTS_EBUSY);
    int rc = cnet_websocket_transport_advance(&ws_transport, 1u, &events);
    check(rc == SALTS_OK || rc == SALTS_ECANCELED);
    check_equal(tags, 1u);
    check_equal(tag_status, SALTS_ECANCELED);
    check_equal(cnet_websocket_transport_destroy(&ws_transport), SALTS_OK);
  }
  it("requalifies a real replacement TCP lease only after WS and old Pool state drain") {
    cnet_managed_dial_snapshot s;
    cnet_managed_connection managed, old_managed;
    cnet_reconnect_ticket old_ticket;
    cnet_pool_connection competing;
    cnet_pool_lease old_lease, rejected = {0};
    cnet_pool_snapshot pool_state;
    size_t events;
    uint64_t wait;
    check_equal(bind_ws(), SALTS_OK);
    check_equal(cnet_managed_dial_get_snapshot(&dial, &s), SALTS_OK);
    old_ticket = s.recovery_ticket;
    old_managed = s.managed;
    check_equal(cnet_managed_dial_protocol_ready(&dial, s.recovery_ticket, cmeta_monotonic_ms()),
                SALTS_OK);
    check_equal(cnet_pool_bind_ready(&pool, physical, s.managed, 1u), SALTS_OK);
    check_equal(cnet_pool_try_acquire(&pool, &key, NULL, &lease, &managed), SALTS_OK);
    old_lease = lease;
    check_equal(cnet_websocket_send_tagged(ws, CNET_WEBSOCKET_MESSAGE_BINARY, "abc", 3u, 10u),
                SALTS_OK);
    check_equal(cnet_pool_begin_drain(&pool, physical), SALTS_OK);
    check_equal(cnet_close(&client, outgoing), SALTS_OK);
    const uint64_t until = cmeta_monotonic_ms() + WAIT_MS;
    do {
      progress();
      check_equal(cnet_managed_dial_get_snapshot(&dial, &s), SALTS_OK);
    } while (s.managed.slot != 0u && cmeta_monotonic_ms() < until);
    check_equal(s.managed.slot, (size_t)0u);
    const uint64_t eligible = s.recovery.next_attempt_ms;
    size_t calls = gate_calls;
    check_equal(cnet_managed_dial_advance(&dial, eligible - 1u, &wait), SALTS_EBUSY);
    check_equal(gate_calls, calls);
    check_equal(wait, UINT64_C(1));
    check_equal(cnet_managed_dial_advance(&dial, eligible, &wait), SALTS_EBUSY);
    check_equal(cnet_pool_terminal(&pool, physical), SALTS_OK);
    check_equal(cnet_websocket_transport_advance(&ws_transport, 1u, &events), SALTS_OK);
    check_equal(cnet_websocket_transport_destroy(&ws_transport), SALTS_OK);
    check_equal(cnet_managed_dial_advance(&dial, eligible, &wait), SALTS_EBUSY); /* lease remains */
    check_equal(cnet_pool_release(&pool, lease), SALTS_OK);
    lease = (cnet_pool_lease){0};
    physical = (cnet_pool_connection){0};
    check_equal(cnet_pool_reserve_connecting(&pool, &key, &competing), SALTS_OK);
    check_equal(cnet_managed_dial_advance(&dial, eligible, &wait), SALTS_ENOBUFS);
    check_equal(cnet_managed_dial_get_snapshot(&dial, &s), SALTS_OK);
    check_equal(s.recovery.attempts, (uint32_t)0u);
    check_equal(s.connection.slot, (uint32_t)0u);
    check_equal(cnet_pool_terminal(&pool, competing), SALTS_OK);
    check_equal(cnet_managed_dial_advance(&dial, eligible, &wait), SALTS_OK);
    wait_connected(2u);
    check_equal(cnet_managed_dial_get_snapshot(&dial, &s), SALTS_OK);
    check_equal(s.recovery.attempts, (uint32_t)1u);
    check_equal(tags, 1u);
    check_equal(raw_sends, 0u); /* no WS/application replay during reconnect */
    check(s.recovery.awaiting_protocol);
    check(!s.recovery.protocol_ready);
    check(s.managed.slot != 0u);
    check(s.managed.generation != old_managed.generation);
    check(s.recovery_ticket.generation != old_ticket.generation);
    check_equal(cnet_managed_dial_protocol_ready(&dial, old_ticket, cmeta_monotonic_ms()),
                SALTS_ENOENT);
    check_equal(cnet_pool_try_acquire(&pool, &key, NULL, &rejected, &managed), SALTS_ENOBUFS);
    check_equal(rejected.slot, (size_t)0u);
    {
      cnet_manager_entry recycled = {0};
      check_equal(cnet_manager_lookup(&manager, old_managed, &recycled), SALTS_ENOENT);
    }

    /* Full protocol qualification is owned by the host, not TCP CONNECTED.
     * Exchange actual new-generation payload in both directions before READY. */
    outgoing = s.connection;
    const size_t old_received = received;
    const uint64_t exchange_until = cmeta_monotonic_ms() + WAIT_MS;
    send_raw(outgoing, "PING", 4u);
    while (received < old_received + 4u && cmeta_monotonic_ms() < exchange_until)
      progress();
    check_equal(received, old_received + 4u);
    check_equal(memcmp(wire + old_received, "PING", 4u), 0);
    expected_client_reply = 4u;
    check_equal(cnet_receive(&client, outgoing, 1u), SALTS_OK);
    {
      mem_buffer_t *reply = bytes("PONG", 4u);
      check_equal(cnet_send_buffer(&peers, accepted[1], reply), SALTS_OK);
      mem_buffer_release(reply);
    }
    while (client_received < 4u && cmeta_monotonic_ms() < exchange_until)
      progress();
    check_equal(client_received, (size_t)4u);

    check_equal(cnet_managed_dial_protocol_ready(&dial, s.recovery_ticket, cmeta_monotonic_ms()),
                SALTS_OK);
    check_equal(cnet_pool_bind_ready(&pool, physical, s.managed, 1u), SALTS_OK);
    check_equal(cnet_pool_try_acquire(&pool, &key, NULL, &lease, &managed), SALTS_OK);
    check_equal(managed.generation, s.managed.generation);
    check_equal(cnet_pool_release(&pool, old_lease), SALTS_ENOENT);
    check_equal(cnet_pool_get_snapshot(&pool, &pool_state), SALTS_OK);
    check_equal(pool_state.ready, (size_t)1u);
    check_equal(pool_state.active_leases, (size_t)1u);
    check_equal(raw_sends, (size_t)1u); /* only the new explicit PING */
    check_equal(tags, (size_t)1u);      /* the retired WS tag did not replay */
  }
  it("settles the accepted tagged write once across an abortive peer close, without replay") {
    size_t events, work;
    const uint64_t until = cmeta_monotonic_ms() + WAIT_MS;
    check_equal(bind_ws(), SALTS_OK);
    /* The peer closes with SO_LINGER(0), but a locally accepted TCP write may
     * still finish successfully before the RST is observed, especially with a
     * separate TLS record / NativeIO completion already in flight. Kernel
     * write completion never proves peer application delivery. Keep both valid
     * completion orders observable instead of assuming every send must fail.
     */
    stopping = true;
    check_equal(cnet_client_stop(&peers, WAIT_MS), SALTS_OK);
    check_equal(cnet_websocket_send_tagged(ws, CNET_WEBSOCKET_MESSAGE_BINARY,
                                           "abcdefghijklmnopqrstuvwx", 24u, 11u),
                SALTS_OK);
    check_equal(cnet_websocket_transport_advance(&ws_transport, 1u, &events), SALTS_OK);
    /* The bridge owns writes only. If the local write completes successfully,
     * host receive demand is still needed to observe the peer's EOF/reset. */
    check_equal(cnet_receive(&client, outgoing, 1u), SALTS_OK);
    while (terminals == 0u && cmeta_monotonic_ms() < until) {
      check_equal(cnet_client_poll(&client, 1u, &events), SALTS_OK);
      check_equal(cnet_manager_advance(&manager, 2u, &work), SALTS_OK);
      (void)cnet_websocket_transport_advance(&ws_transport, 1u, &events);
    }
    check_equal(terminals, 1u);
    check_equal(tags, 1u); /* one authoritative terminal, OK or native failure */
    check_equal(raw_sends, 0u); /* WS notifications never leak into raw sends */
    check_equal(client_received, (size_t)0u);
    {
      const int settled_status = tag_status;
      /* Once the peer has terminated, another bounded advance may return
       * the retained native bridge EIO even while draining is complete.
       * The public transport API explicitly permits that error. Neither
       * outcome may create a second terminal or replay application data.
       */
      const int rc = cnet_websocket_transport_advance(&ws_transport, 8u, &events);
      check(rc == SALTS_OK || rc == SALTS_EIO);
      check_equal(tags, 1u); /* no duplicate settlement or implicit DATA retry */
      check_equal(tag_status, settled_status);
      check_equal(raw_sends, 0u);
    }
    check_equal(cnet_websocket_transport_destroy(&ws_transport), SALTS_OK);
  }
  it("rejects impossible encoded frame budgets before binding the connection") {
    cnet_websocket_config c = ws_config();
    c.max_frame_bytes = SEND_BYTES;
    c.max_message_bytes = SEND_BYTES;
    const cnet_websocket_tagged_policy p = {sizeof(p), CNET_WEBSOCKET_TAGGED_SEND_VERSION, 3u,
                                            tag_cb, NULL};
    check_equal(cnet_websocket_transport_init(&ws_transport, &client, outgoing, &c, &p), SALTS_EMSGSIZE);
    check_null(ws_transport.impl);
    check_equal(bind_ws(), SALTS_OK);
  }
}
