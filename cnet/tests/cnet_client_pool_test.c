#include <cnet/client_pool.h>
#include <salts/thread.h>
#include <salts/clock.h>
#include <tinytest.h>
#include <stdio.h>
#include <string.h>

enum { POOL_TEST_CAPACITY = 3u };
static cnet_client test_client;
static cnet_listener test_listener;
static cnet_manager test_manager;
static cnet_client_pool test_pool;
static cnet_managed_connection records[POOL_TEST_CAPACITY];
static char test_uri[100];

static native_io_backend_kind test_backend(void) {
#if defined(_WIN32)
  return NATIVE_IO_BACKEND_IOCP;
#elif defined(__APPLE__)
  return NATIVE_IO_BACKEND_KQUEUE;
#else
  return NATIVE_IO_BACKEND_EPOLL;
#endif
}
static void state_cb(void *user, cnet_connection connection,
                     cnet_connection_state state, const cnet_error *error) {
  (void)user; (void)connection; (void)state; (void)error;
}
static cnet_pool_key pool_key(uint64_t authority) {
  cnet_pool_key key = {0};
  key.size = sizeof(key);
  key.version = CNET_CLIENT_POOL_VERSION;
  key.runtime_id = 17u;
  key.owner_id = 5u;
  key.endpoint_id = 101u;
  key.peer_generation = 2u;
  key.authority_id = authority;
  key.transport_id = 1u;
  key.tls_trust_id = 99u;
  key.protocol_id = 20u;
  key.session_id = 44u;
  return key;
}
static cnet_pool_config pool_config(void) {
  cnet_pool_config config = {0};
  config.size = sizeof(config);
  config.version = CNET_CLIENT_POOL_VERSION;
  config.manager = &test_manager;
  config.owner_id = 5u;
  config.max_connections = POOL_TEST_CAPACITY;
  config.max_connecting = 2u;
  config.max_leases = 4u;
  return config;
}
static cnet_managed_connection bind_manager(size_t index) {
  const cnet_manager_attachment attachment = {
      .observer = {.on_state = state_cb}};
  const cnet_connect_options options = {.uri = test_uri};
  cnet_connection connection = {0};
  check_equal(cnet_manager_reserve(&test_manager, &attachment, &records[index]), SALTS_OK);
  check_equal(cnet_manager_connect(&test_manager, records[index], &options, &connection), SALTS_OK);
  check(connection.slot != 0u);
  return records[index];
}
static void finish_managed_connection(cnet_managed_connection managed) {
  cnet_manager_entry entry = {0};
  int status = cnet_manager_lookup(&test_manager, managed, &entry);
  const uint64_t deadline = cmeta_monotonic_ms() + 3000u;
  check_equal(status, SALTS_OK);
  if (entry.state == CNET_MANAGER_BOUND) {
    status = cnet_close(&test_client, entry.connection);
    check(status == SALTS_OK || status == SALTS_EALREADY || status == SALTS_ENOENT);
  }
  while (entry.state != CNET_MANAGER_RETIRED && cmeta_monotonic_ms() < deadline) {
    size_t events = 0u;
    check_equal(cnet_client_poll(&test_client, 1u, &events), SALTS_OK);
    check_equal(cnet_manager_lookup(&test_manager, managed, &entry), SALTS_OK);
  }
  check_equal(entry.state, CNET_MANAGER_RETIRED);
}
typedef struct protocol_probe { size_t used, released; } protocol_probe;
static int reserve_protocol(void *arg, cnet_managed_connection managed, uint64_t *token) {
  protocol_probe *probe = (protocol_probe *)arg;
  if (managed.slot == 0u || token == NULL) return SALTS_EINVAL;
  ++probe->used;
  *token = probe->used;
  return SALTS_OK;
}
static void release_protocol(void *arg, uint64_t token) {
  protocol_probe *probe = (protocol_probe *)arg;
  check(token != 0u);
  ++probe->released;
}

spec("CNet owner-local client pool") {
  before_each() {
    const cnet_client_config client_config = {
      .backend = test_backend(), .connection_capacity = POOL_TEST_CAPACITY,
      .command_capacity = 16u, .request_capacity = 16u,
      .completion_batch_capacity = 8u, .event_capacity = 16u,
      .max_send_bytes = 1024u, .receive_buffer_bytes = 1024u,
      .connect_timeout_ms = 2000u, .write_timeout_ms = 2000u};
    const cnet_listener_config listener_config = {
      test_backend(), "127.0.0.1", 0, 8};
    const cnet_manager_config manager_config = {
      sizeof(manager_config), CNET_MANAGER_VERSION, &test_client,
      POOL_TEST_CAPACITY, POOL_TEST_CAPACITY};
    const cnet_pool_config config = pool_config();
    uint16_t port = 0u;
    memset(records, 0, sizeof(records));
    check_equal(cnet_client_init(&test_client, &client_config), SALTS_OK);
    check_equal(cnet_listener_init(&test_listener, &listener_config), SALTS_OK);
    check_equal(cnet_listener_port(&test_listener, &port), SALTS_OK);
    check(port != 0u);
    (void)snprintf(test_uri, sizeof(test_uri), "tcp://127.0.0.1:%u", (unsigned)port);
    check_equal(cnet_manager_init(&test_manager, &manager_config), SALTS_OK);
    check_equal(cnet_pool_init(&test_pool, &config), SALTS_OK);
  }
  after_each() {
    cnet_pool_snapshot pool_status = {0};
    if (test_pool.impl != NULL) {
      check_equal(cnet_pool_get_snapshot(&test_pool, &pool_status), SALTS_OK);
      check(pool_status.drained);
      check_equal(cnet_pool_destroy(&test_pool), SALTS_OK);
    }
    if (test_client.impl != NULL)
      check_equal(cnet_client_stop(&test_client, 3000u), SALTS_OK);
    if (test_manager.impl != NULL) {
      cnet_manager_snapshot snapshot = {0};
      size_t work = 0u;
      check_equal(cnet_manager_get_snapshot(&test_manager, &snapshot), SALTS_OK);
      for (size_t i = 0u; i < snapshot.record_capacity; ++i) {
        cnet_manager_entry entry;
        if (cnet_manager_inspect(&test_manager, i, &entry) == SALTS_OK &&
            entry.state == CNET_MANAGER_RESERVED)
          check_equal(cnet_manager_cancel(&test_manager, entry.managed), SALTS_OK);
      }
      check_equal(cnet_manager_advance(&test_manager, snapshot.record_capacity, &work), SALTS_OK);
      check_equal(cnet_manager_destroy(&test_manager), SALTS_OK);
    }
    if (test_client.impl != NULL) check_equal(cnet_client_destroy(&test_client), SALTS_OK);
    if (test_listener.impl != NULL) {
      check_equal(cnet_listener_close(&test_listener), SALTS_OK);
      check_equal(cnet_listener_destroy(&test_listener), SALTS_OK);
    }
  }
  it("bounds connecting admission and rejects foreign security identities") {
    cnet_pool_connection first = {0}, second = {0}, third = {0};
    cnet_pool_key key = pool_key(1u);
    cnet_pool_snapshot snapshot;
    check_equal(cnet_pool_reserve_connecting(&test_pool, &key, &first), SALTS_OK);
    check_equal(cnet_pool_reserve_connecting(&test_pool, &key, &second), SALTS_OK);
    check_equal(cnet_pool_reserve_connecting(&test_pool, &key, &third), SALTS_ENOBUFS);
    key.owner_id = 99u;
    check_equal(cnet_pool_reserve_connecting(&test_pool, &key, &third), SALTS_EINVAL);
    check_equal(cnet_pool_get_snapshot(&test_pool, &snapshot), SALTS_OK);
    check_equal(snapshot.connecting, (size_t)2u);
    check_equal(cnet_pool_terminal(&test_pool, first), SALTS_OK);
    check_equal(cnet_pool_terminal(&test_pool, first), SALTS_ENOENT);
    check_equal(cnet_pool_terminal(&test_pool, second), SALTS_OK);
  }
  it("enforces Manager BOUND identity and exact-identity reuse") {
    cnet_pool_key key = pool_key(8u), other = key;
    cnet_pool_connection physical = {0};
    cnet_pool_lease lease = {0}, reused = {0};
    cnet_managed_connection managed = {0}, bound = {0};
    const cnet_manager_attachment attachment = {.observer = {.on_state = state_cb}};
    check_equal(cnet_pool_reserve_connecting(&test_pool, &key, &physical), SALTS_OK);
    check_equal(cnet_manager_reserve(&test_manager, &attachment, &records[0]), SALTS_OK);
    check_equal(cnet_pool_bind_ready(&test_pool, physical, records[0], 1u), SALTS_EBUSY);
    check_equal(cnet_manager_cancel(&test_manager, records[0]), SALTS_OK);
    bound = bind_manager(1u);
    check_equal(cnet_pool_bind_ready(&test_pool, physical, bound, 1u), SALTS_OK);
    other.tls_trust_id++;
    check_equal(cnet_pool_try_acquire(&test_pool, &other, NULL, &lease, &managed), SALTS_ENOBUFS);
    check_equal(cnet_pool_try_acquire(&test_pool, &key, NULL, &lease, &managed), SALTS_OK);
    check_equal(managed.generation, bound.generation);
    check_equal(cnet_pool_try_acquire(&test_pool, &key, NULL, &reused, &managed), SALTS_ENOBUFS);
    check_equal(cnet_pool_release(&test_pool, lease), SALTS_OK);
    check_equal(cnet_pool_release(&test_pool, lease), SALTS_ENOENT);
    check_equal(cnet_pool_terminal(&test_pool, physical), SALTS_EBUSY);
    finish_managed_connection(bound);
    check_equal(cnet_pool_terminal(&test_pool, physical), SALTS_OK);
    check_equal(cnet_pool_get_snapshot(&test_pool, &(cnet_pool_snapshot){0}), SALTS_OK);
  }
  it("keeps two real Manager connections isolated across policy identity and drain") {
    cnet_pool_key a = pool_key(101u), b = pool_key(202u);
    cnet_pool_key incompatible[11];
    cnet_pool_connection first = {0}, second = {0}, replacement = {0};
    cnet_pool_lease first_lease = {0}, second_lease = {0}, next_lease = {0};
    cnet_pool_lease rejected = {0};
    cnet_managed_connection a_managed, b_managed, replacement_managed, selected = {0};
    cnet_pool_snapshot snapshot = {0};

    /* Identical physical destination, distinct host-authorized authorities
     * and security/protocol/session identities. These are stable IDs, not a
     * TLS identity provider or a substitute for protocol authentication. */
    b.endpoint_id++;
    b.peer_generation++;
    b.tls_trust_id++;
    b.tls_sni_id = 77u;
    b.client_identity_id = 88u;
    b.alpn_id = 99u;
    b.protocol_id++;
    b.session_id++;

    check_equal(cnet_pool_reserve_connecting(&test_pool, &a, &first), SALTS_OK);
    check_equal(cnet_pool_reserve_connecting(&test_pool, &b, &second), SALTS_OK);
    a_managed = bind_manager(0u);
    b_managed = bind_manager(1u);
    /* A real Manager BOUND record is necessary but protocol READY remains the
     * caller's responsibility; this test covers Pool admission, not auth. */
    check_equal(cnet_pool_bind_ready(&test_pool, first, a_managed, 1u), SALTS_OK);
    check_equal(cnet_pool_bind_ready(&test_pool, second, b_managed, 1u), SALTS_OK);

    for (size_t i = 0u; i < sizeof(incompatible) / sizeof(incompatible[0]); ++i)
      incompatible[i] = a;
    incompatible[0].runtime_id++;
    incompatible[1].endpoint_id++;
    incompatible[2].peer_generation++;
    incompatible[3].authority_id++;
    incompatible[4].transport_id++;
    incompatible[5].tls_trust_id++;
    incompatible[6].tls_sni_id++;
    incompatible[7].client_identity_id++;
    incompatible[8].alpn_id++;
    incompatible[9].protocol_id++;
    incompatible[10].session_id++;
    for (size_t i = 0u; i < sizeof(incompatible) / sizeof(incompatible[0]); ++i) {
      check_equal(cnet_pool_try_acquire(&test_pool, &incompatible[i],
                                        NULL, &rejected, &selected), SALTS_ENOBUFS);
      check_equal(rejected.slot, (size_t)0u);
      check_equal(selected.slot, (size_t)0u);
    }
    check_equal(cnet_pool_try_acquire(&test_pool, &a, NULL, &first_lease, &selected),
                SALTS_OK);
    check_equal(selected.slot, a_managed.slot);
    check_equal(selected.generation, a_managed.generation);
    check_equal(cnet_pool_try_acquire(&test_pool, &b, NULL, &second_lease, &selected),
                SALTS_OK);
    check_equal(selected.slot, b_managed.slot);
    check_equal(selected.generation, b_managed.generation);
    check_equal(cnet_pool_get_snapshot(&test_pool, &snapshot), SALTS_OK);
    check_equal(snapshot.ready, (size_t)2u);
    check_equal(snapshot.active_leases, (size_t)2u);

    /* Draining A must neither grant more A leases nor stop B. */
    check_equal(cnet_pool_begin_drain(&test_pool, first), SALTS_OK);
    check_equal(cnet_pool_terminal(&test_pool, first), SALTS_EBUSY);
    check_equal(cnet_pool_try_acquire(&test_pool, &a, NULL, &rejected, &selected),
                SALTS_ENOBUFS);
    check_equal(cnet_pool_release(&test_pool, second_lease), SALTS_OK);
    check_equal(cnet_pool_try_acquire(&test_pool, &b, NULL, &next_lease, &selected),
                SALTS_OK);
    check_equal(selected.slot, b_managed.slot);
    check_equal(selected.generation, b_managed.generation);
    check_equal(cnet_pool_release(&test_pool, second_lease), SALTS_ENOENT);
    check_equal(cnet_pool_release(&test_pool, next_lease), SALTS_OK);

    /* Actual transport terminal precedes Pool terminal, while the old
     * application lease intentionally survives both until explicit release. */
    finish_managed_connection(a_managed);
    check_equal(cnet_pool_terminal(&test_pool, first), SALTS_OK);
    check_equal(cnet_pool_destroy(&test_pool), SALTS_EBUSY);
    check_equal(cnet_pool_get_snapshot(&test_pool, &snapshot), SALTS_OK);
    check_equal(snapshot.terminal_waiting_for_leases, (size_t)1u);
    check_equal(snapshot.active_leases, (size_t)1u);
    check_equal(cnet_pool_release(&test_pool, first_lease), SALTS_OK);
    check_equal(cnet_pool_release(&test_pool, first_lease), SALTS_ENOENT);
    check_equal(cnet_pool_get_snapshot(&test_pool, &snapshot), SALTS_OK);
    check_equal(snapshot.physical_in_use, (size_t)1u);
    check_equal(snapshot.ready, (size_t)1u);

    /* Reusing the freed physical record cannot resurrect a stale ticket. */
    check_equal(cnet_pool_reserve_connecting(&test_pool, &a, &replacement), SALTS_OK);
    check_equal(replacement.slot, first.slot);
    check(replacement.generation != first.generation);
    replacement_managed = bind_manager(2u);
    check_equal(cnet_pool_bind_ready(&test_pool, replacement, replacement_managed, 1u),
                SALTS_OK);
    check_equal(cnet_pool_terminal(&test_pool, first), SALTS_ENOENT);
    check_equal(cnet_pool_try_acquire(&test_pool, &a, NULL, &next_lease, &selected),
                SALTS_OK);
    check_equal(selected.slot, replacement_managed.slot);
    check_equal(selected.generation, replacement_managed.generation);
    check_equal(cnet_pool_release(&test_pool, first_lease), SALTS_ENOENT);

    /* Neighbor B remains reusable across A's retirement and replacement. */
    check_equal(cnet_pool_try_acquire(&test_pool, &b, NULL, &second_lease, &selected),
                SALTS_OK);
    check_equal(selected.slot, b_managed.slot);
    check_equal(selected.generation, b_managed.generation);
    check_equal(cnet_pool_release(&test_pool, second_lease), SALTS_OK);
    check_equal(cnet_pool_release(&test_pool, next_lease), SALTS_OK);
    finish_managed_connection(b_managed);
    finish_managed_connection(replacement_managed);
    check_equal(cnet_pool_terminal(&test_pool, second), SALTS_OK);
    check_equal(cnet_pool_terminal(&test_pool, replacement), SALTS_OK);
    check_equal(cnet_pool_get_snapshot(&test_pool, &snapshot), SALTS_OK);
    check(snapshot.drained);
    check_equal(snapshot.active_leases, (size_t)0u);
    check_equal(snapshot.physical_in_use, (size_t)0u);
  }
  it("keeps terminal storage until every multiplexed lease settles") {
    cnet_pool_key key = pool_key(7u);
    cnet_pool_connection physical = {0};
    cnet_pool_lease a = {0}, b = {0}, c = {0};
    cnet_managed_connection managed = {0}, bound = {0};
    protocol_probe probe = {0};
    const cnet_pool_protocol_ops ops = {reserve_protocol, release_protocol, &probe};
    cnet_pool_snapshot snapshot;
    check_equal(cnet_pool_reserve_connecting(&test_pool, &key, &physical), SALTS_OK);
    bound = bind_manager(0u);
    check_equal(cnet_pool_bind_ready(&test_pool, physical, bound, 2u), SALTS_OK);
    check_equal(cnet_pool_try_acquire(&test_pool, &key, NULL, &c, &managed), SALTS_ENOTSUP);
    check_equal(cnet_pool_try_acquire(&test_pool, &key, &ops, &a, &managed), SALTS_OK);
    check_equal(cnet_pool_try_acquire(&test_pool, &key, &ops, &b, &managed), SALTS_OK);
    check_equal(cnet_pool_try_acquire(&test_pool, &key, &ops, &c, &managed), SALTS_ENOBUFS);
    check_equal(cnet_pool_begin_drain(&test_pool, physical), SALTS_OK);
    check_equal(cnet_pool_try_acquire(&test_pool, &key, &ops, &c, &managed), SALTS_ENOBUFS);
    check_equal(cnet_pool_terminal(&test_pool, physical), SALTS_EBUSY);
    /* Failed acquire clears its out_managed; keep authoritative Manager handle. */
    finish_managed_connection(bound);
    check_equal(cnet_pool_terminal(&test_pool, physical), SALTS_OK);
    check_equal(cnet_pool_get_snapshot(&test_pool, &snapshot), SALTS_OK);
    check_equal(snapshot.terminal_waiting_for_leases, (size_t)1u);
    check_equal(cnet_pool_destroy(&test_pool), SALTS_EBUSY);
    check_equal(cnet_pool_release(&test_pool, a), SALTS_OK);
    check_equal(cnet_pool_release(&test_pool, b), SALTS_OK);
    check_equal(probe.released, (size_t)2u);
    check_equal(cnet_pool_get_snapshot(&test_pool, &snapshot), SALTS_OK);
    check(snapshot.drained);
  }
  it("rejects stale READY admission after Manager terminal before pool notification") {
    cnet_pool_key key = pool_key(32u);
    cnet_pool_connection physical = {0};
    cnet_pool_lease lease = {0};
    cnet_managed_connection bound, managed = {0};
    cnet_pool_snapshot snapshot;
    check_equal(cnet_pool_reserve_connecting(&test_pool, &key, &physical), SALTS_OK);
    bound = bind_manager(0u);
    check_equal(cnet_pool_bind_ready(&test_pool, physical, bound, 1u), SALTS_OK);
    finish_managed_connection(bound);
    check_equal(cnet_pool_try_acquire(&test_pool, &key, NULL, &lease, &managed),
                SALTS_ENOBUFS);
    check_equal(lease.slot, (size_t)0u);
    check_equal(cnet_pool_get_snapshot(&test_pool, &snapshot), SALTS_OK);
    check_equal(snapshot.draining, (size_t)1u);
    check_equal(cnet_pool_terminal(&test_pool, physical), SALTS_OK);
  }
  it("seals without destroying a still-owned transport") {
    cnet_pool_key key = pool_key(17u);
    cnet_pool_connection physical = {0}, denied = {0};
    check_equal(cnet_pool_reserve_connecting(&test_pool, &key, &physical), SALTS_OK);
    check_equal(cnet_pool_seal(&test_pool), SALTS_OK);
    check_equal(cnet_pool_reserve_connecting(&test_pool, &key, &denied), SALTS_ESHUTDOWN);
    check_equal(cnet_pool_bind_ready(&test_pool, physical, (cnet_managed_connection){0}, 1u),
                SALTS_ESHUTDOWN);
    check_equal(cnet_pool_terminal(&test_pool, physical), SALTS_OK);
  }
}
