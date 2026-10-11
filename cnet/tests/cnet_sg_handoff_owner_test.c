#include <cnet/cnet.h>
#include <cnet/handoff.h>
#include <cnet/manager.h>
#include <cnet/owner_placement.h>
#include <cnet/sg_host.h>
#include <salts/clock.h>
#include <salts/native_io_sharded.h>
#include <salts/thread.h>
#include <tinytest.h>

#include <stdint.h>
#include <string.h>

/* Real 2-SG-Owner TCP accept/handoff/adopt with ONE NativeIO observe authority
 * per shard. No Actor, second executor, static raw backend ownership or
 * per-packet placement. Producer moves the accepted socket only after
 * destination inbox credit was reserved; the final Owner owns its callbacks. */
enum { REMOTE_SHARDS = 2u, REMOTE_TARGET = 1u,
       REMOTE_BATCH = 16u, REMOTE_DEADLINE_MS = 12000u };

typedef struct remote_case remote_case;
typedef struct remote_lane {
  remote_case *test;
  size_t shard;
  native_io_sharded_host_lease lease;
  native_io_backend *backend;
  cnet_client client;
  cnet_listener listener;      /* source Owner only */
  cnet_manager manager;        /* destination Owner only */
  cnet_stream_endpoint local;
  cnet_connection connection;
  cnet_managed_connection managed;
  cnet_handoff_ticket taken_ticket;
  const void *owner_token;
  size_t connected, terminal, bytes, sent;
  size_t recycled, accept_completions, placement_calls;
  size_t published, taken, wake_submitted, wake_full, rejected_adopts;
  bool sending, close_allowed, closing, done, client_destroyed, allow_adopt;
  int status;
} remote_lane;

struct remote_case {
  native_io_sharded *sg;
  cnet_handoff inbox;
  remote_lane lane[REMOTE_SHARDS];
  bool fail_adopt;
};

static native_io_backend_kind remote_backend(void) {
#if defined(_WIN32)
  return NATIVE_IO_BACKEND_IOCP;
#elif defined(__APPLE__)
  return NATIVE_IO_BACKEND_KQUEUE;
#else
  return NATIVE_IO_BACKEND_EPOLL;
#endif
}

static cnet_client_config remote_client_config(void) {
  cnet_client_config config = {0};
  config.backend = remote_backend();
  config.connection_capacity = 2u;
  config.command_capacity = 8u;
  config.request_capacity = REMOTE_BATCH;
  config.completion_batch_capacity = REMOTE_BATCH;
  config.event_capacity = 8u;
  config.max_send_bytes = 1024u;
  config.receive_buffer_bytes = 1024u;
  config.connect_timeout_ms = REMOTE_DEADLINE_MS;
  config.read_timeout_ms = REMOTE_DEADLINE_MS;
  config.write_timeout_ms = REMOTE_DEADLINE_MS;
  return config;
}

static void remote_error(remote_lane *lane, int status) {
  if (lane->status == SALTS_OK) lane->status = status;
}

#define REMOTE_OK(lane, call) do { int remote_rc_ = (call); \
  if (remote_rc_ != SALTS_OK) { remote_error((lane), remote_rc_); return; } \
} while (0)

static void remote_state(void *user, cnet_connection connection,
                         cnet_connection_state state, const cnet_error *error) {
  remote_lane *lane = (remote_lane *)user;
  (void)connection;
  (void)error;
  if (cmeta_thread_current_token() != lane->owner_token) {
    remote_error(lane, SALTS_EPERM);
    return;
  }
  if (state == CNET_CONNECTION_CONNECTED) ++lane->connected;
  if (state == CNET_CONNECTION_CLOSED || state == CNET_CONNECTION_FAILED)
    ++lane->terminal;
  /* A rejected final-Owner adopt closes its real detached TCP descriptor.
   * The source may observe CLOSED or an OS-specific reset; neither is an
   * application ACK. Other transport failures must still fail this fixture. */
  if (state == CNET_CONNECTION_FAILED &&
      !(lane->test->fail_adopt && lane->shard == 0u))
    remote_error(lane, SALTS_ECONNRESET);
}

static void remote_receive(void *user, cnet_connection connection,
                           const cnet_receive_view *view) {
  remote_lane *lane = (remote_lane *)user;
  const char *expected = lane->shard == REMOTE_TARGET ? "ping" : "pong";
  if (cmeta_thread_current_token() != lane->owner_token || view == NULL ||
      lane->bytes > 4u || view->size > 4u - lane->bytes ||
      memcmp(view->data, expected + lane->bytes, view->size) != 0) {
    remote_error(lane, SALTS_EPROTO);
    return;
  }
  lane->bytes += view->size;
  if (lane->bytes < 4u) {
    int status = cnet_receive(&lane->client, connection, 1u);
    if (status != SALTS_OK) remote_error(lane, status);
  }
}

static void remote_sent(void *user, cnet_connection connection, size_t size) {
  remote_lane *lane = (remote_lane *)user;
  (void)connection;
  if (cmeta_thread_current_token() != lane->owner_token) {
    remote_error(lane, SALTS_EPERM);
    return;
  }
  lane->sent += size;
}

static void remote_recycle(void *user) {
  remote_lane *lane = (remote_lane *)user;
  if (cmeta_thread_current_token() != lane->owner_token)
    remote_error(lane, SALTS_EPERM);
  ++lane->recycled;
}

static cnet_observer remote_observer(remote_lane *lane) {
  cnet_observer observer = {0};
  observer.on_state = remote_state;
  observer.on_receive = remote_receive;
  observer.on_send = remote_sent;
  observer.user = lane;
  return observer;
}

static bool remote_quiescent(void *user) {
  remote_lane *lane = (remote_lane *)user;
  return lane->client_destroyed && lane->client.impl == NULL &&
         lane->listener.impl == NULL && lane->manager.impl == NULL;
}

static void remote_init(native_io_sharded_context *context, void *user) {
  remote_lane *lane = (remote_lane *)user;
  remote_case *test = lane->test;
  cnet_client_config network = remote_client_config();
  if (native_io_sharded_context_shard(context) != lane->shard) {
    remote_error(lane, SALTS_EPERM);
    return;
  }
  lane->owner_token = cmeta_thread_current_token();
  REMOTE_OK(lane, native_io_sharded_context_acquire_host(
      context, remote_quiescent, lane, &lane->lease, &lane->backend));
  REMOTE_OK(lane, cnet_client_init_external(&lane->client, &network,
                                           lane->backend));
  if (lane->shard == REMOTE_TARGET) {
    const cnet_manager_config manager_config = {
        sizeof(cnet_manager_config), CNET_MANAGER_VERSION,
        &lane->client, 1u, 1u};
    const cnet_handoff_config handoff_config = {
        sizeof(cnet_handoff_config), CNET_HANDOFF_VERSION, 1u, 1u};
    REMOTE_OK(lane, cnet_manager_init(&lane->manager, &manager_config));
    REMOTE_OK(lane, cnet_handoff_init(&test->inbox, &handoff_config));
  } else {
    cnet_stream_endpoint bind = CNET_STREAM_ENDPOINT_INIT;
    native_io_request accept_request = {0};
    bind.family = CNET_DATAGRAM_ADDRESS_IPV4;
    bind.address[0] = 127u;
    bind.address[3] = 1u;
    REMOTE_OK(lane, cnet_listener_open(&lane->listener, remote_backend(),
                                       CNET_DATAGRAM_ADDRESS_IPV4));
    REMOTE_OK(lane, cnet_listener_bind_open_endpoint(&lane->listener, &bind));
    REMOTE_OK(lane, cnet_listener_local_endpoint(&lane->listener, &lane->local));
    REMOTE_OK(lane, cnet_listener_listen(&lane->listener, 4u));
    REMOTE_OK(lane, cnet_listener_attach_external(&lane->listener,
                                                  lane->backend));
    REMOTE_OK(lane, cnet_listener_submit_external_accept(
        &lane->listener, &accept_request));
  }
}

static void remote_start_connect(native_io_sharded_context *context,
                                 void *user) {
  remote_lane *lane = (remote_lane *)user;
  cnet_observer observer = remote_observer(lane);
  if (native_io_sharded_context_shard(context) != lane->shard ||
      lane->shard == REMOTE_TARGET) {
    remote_error(lane, SALTS_EPERM);
    return;
  }
  REMOTE_OK(lane, cnet_connect_endpoint(
      &lane->client, &lane->local, NULL, &observer, &lane->connection));
}

/* This is the one real SG Owner-0 admission boundary. Negative paths do not
 * transfer the detached socket or release someone else's reservation. */
static void remote_progress(native_io_sharded_context *context, void *user);

static void remote_publish(remote_lane *lane) {
  remote_case *test = lane->test;
  cnet_accepted_stream accepted = CNET_ACCEPTED_STREAM_INIT;
  cnet_handoff_ticket ticket = {0}, refused = {0}, stale = {0};
  cnet_owner_placement_hint owners[REMOTE_SHARDS] = {{0}};
  cnet_owner_placement_input placement = {0};
  native_io_sharded_task wake = {0};
  size_t owner = SIZE_MAX;
  int rc;
  owners[0].eligible = true;
  owners[1].eligible = true;
  placement.size = sizeof(placement);
  placement.version = CNET_OWNER_PLACEMENT_VERSION;
  placement.kind = CNET_OWNER_PLACE_EXPLICIT;
  placement.owners = owners;
  placement.owner_count = REMOTE_SHARDS;
  placement.explicit_owner = REMOTE_TARGET;
  ++lane->placement_calls;
  rc = cnet_owner_placement_choose(&placement, &owner);
  if (rc != SALTS_OK || owner != REMOTE_TARGET) {
    remote_error(lane, SALTS_EPROTO);
    return;
  }

  rc = cnet_listener_accept_detached(&lane->listener, &accepted);
  if (rc != SALTS_OK) { remote_error(lane, rc); return; }
  rc = cnet_handoff_reserve(&test->inbox, &ticket);
  if (rc != SALTS_OK) {
    (void)cnet_accepted_stream_close(&accepted);
    remote_error(lane, rc);
    return;
  }
  /* Capacity is authoritative; candidate selection is NOT admission credit. */
  if (cnet_handoff_reserve(&test->inbox, &refused) != SALTS_ENOBUFS ||
      refused.slot != 0u || accepted.internal_active == 0u) {
    remote_error(lane, SALTS_EPROTO);
    return;
  }
  stale = ticket;
  ++stale.generation;
  if (cnet_handoff_publish(&test->inbox, stale, &accepted) != SALTS_ENOENT ||
      accepted.internal_active == 0u) {
    remote_error(lane, SALTS_EPROTO);
    return;
  }
  rc = cnet_handoff_publish(&test->inbox, ticket, &accepted);
  if (rc != SALTS_OK) { remote_error(lane, rc); return; }
  if (accepted.internal_active != 0u) {
    remote_error(lane, SALTS_EPROTO);
    return;
  }
  ++lane->published;
  /* Never release a published ticket from this producer: target could have
   * already taken it on another thread. One bounded SG task is a wake hint,
   * NOT a second backend observer or an authorization to publish twice. */
  wake.run = remote_progress;
  wake.arg = &test->lane[REMOTE_TARGET];
  rc = native_io_sharded_try_submit_to(test->sg, REMOTE_TARGET, &wake);
  if (rc == SALTS_OK) ++lane->wake_submitted;
  else if (rc == SALTS_ENOBUFS) ++lane->wake_full;
  else remote_error(lane, rc);
}

/* The FINAL owner alone takes the detached child and creates CNet transport
 * state. Manager credits are distinct from the producer inbox credit. */
static void remote_adopt(remote_lane *lane) {
  remote_case *test = lane->test;
  cnet_accepted_stream stream = CNET_ACCEPTED_STREAM_INIT;
  cnet_handoff_ticket ticket = {0};
  cnet_manager_attachment attachment = {0};
  cnet_managed_connection refused = {0};
  cnet_manager_snapshot manager = {0};
  int rc = cnet_handoff_take(&test->inbox, &ticket, &stream);
  if (rc == SALTS_ENOENT) return;
  if (rc != SALTS_OK) { remote_error(lane, rc); return; }
  ++lane->taken;
  attachment.observer = remote_observer(lane);
  attachment.on_recycle = remote_recycle;
  rc = cnet_manager_reserve(&lane->manager, &attachment, &lane->managed);
  if (rc != SALTS_OK) {
    /* A failed Manager admission does not retroactively transfer storage;
     * after take the FINAL owner must close and release the child. */
    (void)cnet_accepted_stream_close(&stream);
    (void)cnet_handoff_release(&test->inbox, ticket);
    remote_error(lane, rc);
    return;
  }
  if (cnet_manager_reserve(&lane->manager, &attachment,
                            &refused) != SALTS_ENOBUFS ||
      refused.slot != 0u ||
      cnet_manager_get_snapshot(&lane->manager, &manager) != SALTS_OK ||
      manager.reserved != 1u) {
    remote_error(lane, SALTS_EPROTO);
    return;
  }
  /* A real final-Owner Manager admission may consume and reject an already
   * published/taken descriptor. An uninitialized TLS server profile is a
   * deterministic invalid admission, not a fabricated CNet terminal. */
  if (test->fail_adopt) {
    const cnet_tls_server invalid_tls = {0};
    cnet_manager_snapshot after_failure = {0};
    rc = cnet_manager_adopt(&lane->manager, lane->managed, &stream,
                            &invalid_tls, &lane->connection);
    if (rc != SALTS_EINVAL || stream.internal_active != 0u ||
        lane->connection.slot != 0u ||
        cnet_manager_get_snapshot(&lane->manager, &after_failure) != SALTS_OK ||
        after_failure.reserved != 0u || after_failure.bound != 0u ||
        after_failure.retired != 1u) {
      remote_error(lane, SALTS_EPROTO);
      return;
    }
    lane->taken_ticket = ticket; /* Only final Owner may release TAKEN credit. */
    ++lane->rejected_adopts;
    return;
  }
  rc = cnet_manager_adopt(&lane->manager, lane->managed, &stream,
                          NULL, &lane->connection);
  if (rc != SALTS_OK) { remote_error(lane, rc); return; }
  if (stream.internal_active != 0u || lane->connection.slot == 0u) {
    remote_error(lane, SALTS_EPROTO);
    return;
  }
  lane->taken_ticket = ticket;
}

static void remote_send(remote_lane *lane, const char *bytes) {
  mem_buffer_t *buffer = mem_get_buffer(mem_global(), 4u);
  int rc;
  if (buffer == NULL) { remote_error(lane, SALTS_ENOMEM); return; }
  memcpy(mem_buffer_data(buffer), bytes, 4u);
  mem_set_used(buffer, 4u);
  rc = cnet_send_buffer(&lane->client, lane->connection, buffer);
  mem_buffer_release(buffer);
  if (rc != SALTS_OK) remote_error(lane, rc);
}

static void remote_progress(native_io_sharded_context *context, void *user) {
  remote_lane *lane = (remote_lane *)user;
  remote_case *test = lane->test;
  native_io_sharded_completion completions[REMOTE_BATCH];
  cnet_client *clients[1] = {&lane->client};
  cnet_sg_host_routes routes = {
    sizeof(cnet_sg_host_routes), CNET_SG_HOST_ROUTING_VERSION,
    lane->shard == 0u ? &lane->listener : NULL, clients, 1u
  };
  size_t count = 0u, events = 0u, accepts = 0u, sharded = 0u;
  int rc;
  if (lane->done || lane->status != SALTS_OK) return;
  if (native_io_sharded_context_shard(context) != lane->shard ||
      cmeta_thread_current_token() != lane->owner_token) {
    remote_error(lane, SALTS_EPERM);
    return;
  }
  if (lane->shard == REMOTE_TARGET && lane->taken == 0u &&
      lane->allow_adopt) {
    remote_adopt(lane);
    if (lane->status != SALTS_OK) return;
  }
  REMOTE_OK(lane, cnet_client_advance_external(&lane->client, &events));
  rc = native_io_sharded_context_observe_host(
      context, lane->lease, completions, REMOTE_BATCH, 0u, &count);
  if (rc != SALTS_OK && rc != SALTS_ETIMEDOUT) {
    remote_error(lane, rc);
    return;
  }
  REMOTE_OK(lane, cnet_sg_host_route_batch(
      completions, count, &routes, &accepts, &sharded));
  if (lane->shard == 0u && accepts != 0u) {
    lane->accept_completions += accepts;
    if (accepts != 1u || lane->published != 0u) {
      remote_error(lane, SALTS_EPROTO);
      return;
    }
    remote_publish(lane);
    if (lane->status != SALTS_OK) return;
  }
  REMOTE_OK(lane, cnet_client_advance_external(&lane->client, &events));
  if (lane->shard == REMOTE_TARGET) {
    size_t work = 0u;
    REMOTE_OK(lane, cnet_manager_advance(&lane->manager, 1u, &work));
  }
  /* Failfast Manager adopt must still recycle its reservation and
   * terminalize the handed-off descriptor without inventing a transport
   * callback. The source connection shuts down independently after the
   * host's inter-round barrier observes final-owner rejection. */
  if (test->fail_adopt) {
    if (lane->shard == REMOTE_TARGET && lane->rejected_adopts == 1u) {
      cnet_manager_snapshot manager = {0};
      if (lane->recycled == 0u) return;
      REMOTE_OK(lane, cnet_manager_get_snapshot(&lane->manager, &manager));
      if (!manager.drained || manager.reserved != 0u ||
          manager.bound != 0u || manager.retired != 0u ||
          cnet_handoff_release(&test->inbox, lane->taken_ticket) != SALTS_OK ||
          cnet_handoff_release(&test->inbox, lane->taken_ticket) != SALTS_ENOENT) {
        remote_error(lane, SALTS_EPROTO);
        return;
      }
      REMOTE_OK(lane, cnet_manager_destroy(&lane->manager));
      REMOTE_OK(lane, cnet_client_stop_external(&lane->client));
      REMOTE_OK(lane, cnet_client_destroy(&lane->client));
      lane->client_destroyed = true;
      REMOTE_OK(lane, native_io_sharded_context_release_host(context, lane->lease));
      lane->done = true;
    } else if (lane->shard == 0u) {
      if (lane->connected != 0u && lane->close_allowed && !lane->closing) {
        lane->closing = true;
        REMOTE_OK(lane, cnet_close(&lane->client, lane->connection));
      }
      if (lane->closing && lane->terminal != 0u) {
        REMOTE_OK(lane, cnet_listener_close(&lane->listener));
        REMOTE_OK(lane, cnet_listener_destroy(&lane->listener));
        REMOTE_OK(lane, cnet_client_stop_external(&lane->client));
        REMOTE_OK(lane, cnet_client_destroy(&lane->client));
        lane->client_destroyed = true;
        REMOTE_OK(lane, native_io_sharded_context_release_host(context, lane->lease));
        lane->done = true;
      }
    }
    return;
  }

  if (lane->connected != 0u && !lane->sending) {
    lane->sending = true;
    REMOTE_OK(lane, cnet_receive(&lane->client, lane->connection, 1u));
    remote_send(lane, lane->shard == 0u ? "ping" : "pong");
    if (lane->status != SALTS_OK) return;
  }
  if (lane->close_allowed && lane->sending &&
      lane->bytes == 4u && lane->sent == 4u && !lane->closing) {
    lane->closing = true;
    REMOTE_OK(lane, cnet_close(&lane->client, lane->connection));
  }
  if (lane->closing && lane->terminal != 0u) {
    if (lane->shard == REMOTE_TARGET) {
      cnet_manager_snapshot snapshot = {0};
      if (lane->recycled == 0u) return;
      REMOTE_OK(lane, cnet_manager_get_snapshot(&lane->manager, &snapshot));
      if (!snapshot.drained || snapshot.bound != 0u ||
          snapshot.retired != 0u || snapshot.reserved != 0u ||
          cnet_handoff_release(&test->inbox, lane->taken_ticket) != SALTS_OK ||
          cnet_handoff_release(&test->inbox, lane->taken_ticket) != SALTS_ENOENT) {
        remote_error(lane, SALTS_EPROTO);
        return;
      }
      REMOTE_OK(lane, cnet_manager_destroy(&lane->manager));
    } else {
      REMOTE_OK(lane, cnet_listener_close(&lane->listener));
      REMOTE_OK(lane, cnet_listener_destroy(&lane->listener));
    }
    REMOTE_OK(lane, cnet_client_stop_external(&lane->client));
    REMOTE_OK(lane, cnet_client_destroy(&lane->client));
    lane->client_destroyed = true;
    REMOTE_OK(lane, native_io_sharded_context_release_host(context, lane->lease));
    lane->done = true;
  }
}

static void remote_run_handoff(bool fail_adopt) {
  remote_case test = {0};
  native_io_sharded_task setup[REMOTE_SHARDS] = {{0}};
  native_io_sharded_task progress[REMOTE_SHARDS] = {{0}};
  native_io_sharded_task dial = {0};
  const native_io_sharded_config config = {
      REMOTE_SHARDS, 8u,
      {remote_backend(), 24u, 48u, REMOTE_BATCH}};
  const uint64_t deadline = cmeta_monotonic_ms() + REMOTE_DEADLINE_MS;
  bool done = false;

  test.fail_adopt = fail_adopt;
  check_equal(native_io_sharded_create(&config, &test.sg), SALTS_OK);
  check_not_null(test.sg);
  for (size_t i = 0u; i < REMOTE_SHARDS; ++i) {
    remote_lane *lane = &test.lane[i];
    lane->test = &test;
    lane->shard = i;
    lane->allow_adopt = !fail_adopt;
    setup[i] = (native_io_sharded_task){remote_init, NULL, NULL, lane};
    progress[i] = (native_io_sharded_task){remote_progress, NULL, NULL, lane};
    check_equal(native_io_sharded_submit_to(test.sg, i, &setup[i]), SALTS_OK);
  }
  check_equal(native_io_sharded_wait(test.sg), SALTS_OK);
  for (size_t i = 0u; i < REMOTE_SHARDS; ++i) {
    check_equal(test.lane[i].status, SALTS_OK);
    check_equal(test.lane[i].lease.owner_shard, (uint32_t)i);
    check_not_null(test.lane[i].backend);
  }
  check(test.lane[0].backend != test.lane[1].backend);
  check_not_null(test.inbox.impl);
  dial = (native_io_sharded_task){
      remote_start_connect, NULL, NULL, &test.lane[0]};
  check_equal(native_io_sharded_submit_to(test.sg, 0u, &dial), SALTS_OK);
  check_equal(native_io_sharded_wait(test.sg), SALTS_OK);
  check_equal(test.lane[0].status, SALTS_OK);

  while (!done && cmeta_monotonic_ms() < deadline) {
    done = true;
    for (size_t i = 0u; i < REMOTE_SHARDS; ++i) {
      check_equal(test.lane[i].status, SALTS_OK);
      if (test.lane[i].done) continue;
      done = false;
      check_equal(native_io_sharded_submit_to(
          test.sg, i, &progress[i]), SALTS_OK);
    }
    if (done) break;
    check_equal(native_io_sharded_wait(test.sg), SALTS_OK);
    /* A round barrier makes both peers' application deliveries visible.
     * Only then may the owner-local close turns begin; this avoids treating
     * a send callback as proof of the remote application's receive. */
    if (fail_adopt) {
      /* The source's real CONNECTED callback must complete before Owner 1
       * can reject/close its detached accepted child. All reads/writes here
       * occur only after native_io_sharded_wait's task barrier. */
      if (test.lane[0].connected == 1u && test.lane[0].published == 1u)
        test.lane[1].allow_adopt = true;
      if (test.lane[1].rejected_adopts == 1u && test.lane[1].recycled == 1u)
        test.lane[0].close_allowed = true;
    } else if (test.lane[0].bytes == 4u && test.lane[1].bytes == 4u &&
               test.lane[0].sent == 4u && test.lane[1].sent == 4u) {
      test.lane[0].close_allowed = true;
      test.lane[1].close_allowed = true;
    }
    cmeta_sleep_ms(1u);
  }
  check(done);
  for (size_t i = 0u; i < REMOTE_SHARDS; ++i) {
    remote_lane *lane = &test.lane[i];
    check_equal(lane->status, SALTS_OK);
    check(lane->done);
    check_equal(lane->connected, (size_t)(fail_adopt && i == REMOTE_TARGET ? 0u : 1u));
    check_equal(lane->terminal, (size_t)(fail_adopt && i == REMOTE_TARGET ? 0u : 1u));
    check_equal(lane->bytes, (size_t)(fail_adopt ? 0u : 4u));
    check_equal(lane->sent, (size_t)(fail_adopt ? 0u : 4u));
    check_null(lane->client.impl);
  }
  check_equal(test.lane[0].accept_completions, (size_t)1u);
  check_equal(test.lane[0].placement_calls, (size_t)1u);
  check_equal(test.lane[0].published, (size_t)1u);
  check_equal(test.lane[0].wake_submitted + test.lane[0].wake_full,
              (size_t)1u);
  check_equal(test.lane[1].taken, (size_t)1u);
  check_equal(test.lane[1].recycled, (size_t)1u);
  check_equal(test.lane[1].rejected_adopts, (size_t)(fail_adopt ? 1u : 0u));
  {
    cnet_handoff_snapshot snapshot = {0};
    check_equal(cnet_handoff_get_snapshot(&test.inbox, &snapshot), SALTS_OK);
    check(snapshot.drained);
    check_equal(snapshot.reserved, (size_t)0u);
    check_equal(snapshot.queued, (size_t)0u);
    check_equal(snapshot.taken, (size_t)0u);
  }
  /* SG's final barrier establishes producer/wake quiescence. The inbox does
   * not own the backend or transport and can be destroyed separately. */
  check_equal(cnet_handoff_seal(&test.inbox), SALTS_OK);
  check_equal(cnet_handoff_destroy(&test.inbox), SALTS_OK);
  check_equal(native_io_sharded_shutdown(test.sg), SALTS_OK);
  check_equal(native_io_sharded_destroy(test.sg), SALTS_OK);
}

spec("CNet SG remote Owner detached TCP handoff/credit lifecycle") {
  it("accepts on Owner 0, adopts on Owner 1, and recycles after real terminal") {
    remote_run_handoff(false);
  }
  it("consumes invalid final-Owner Manager adopt and reclaims handoff credit") {
    remote_run_handoff(true);
  }
}
