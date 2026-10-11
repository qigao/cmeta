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
#include <stdatomic.h>
#include <string.h>

/* Real 2-SG-Owner TCP accept/handoff/adopt with ONE NativeIO observe authority
 * per shard. No Actor, second executor, static raw backend ownership or
 * per-packet placement. Producer moves the accepted socket only after
 * destination inbox credit was reserved; the final Owner owns its callbacks. */
enum { REMOTE_SHARDS = 2u, REMOTE_TARGET = 1u,
       REMOTE_BATCH = 16u, REMOTE_DEADLINE_MS = 12000u };
typedef enum remote_mode {
  REMOTE_SUCCESS, REMOTE_INVALID_TLS, REMOTE_SEALED,
  REMOTE_MANAGER_FULL, REMOTE_RECORD_HELD, REMOTE_TRANSPORT_FULL
} remote_mode;

typedef struct remote_case remote_case;
typedef struct remote_lane {
  remote_case *test;
  size_t shard;
  native_io_sharded_host_lease lease;
  native_io_backend *backend;
  cnet_client client;
  cnet_listener listener;      /* source, plus raw-capacity loopback on target */
  cnet_manager manager;        /* destination Owner only */
  cnet_stream_endpoint local;
  cnet_connection connection;
  cnet_connection occupying_connection;
  cnet_managed_connection managed;
  cnet_managed_connection blocker;
  cnet_handoff_ticket taken_ticket, held_ticket;
  cnet_accepted_stream held_accepted;
  const void *owner_token;
  size_t connected, terminal, bytes, sent;
  size_t recycled, accept_completions, placement_calls;
  size_t published, taken, wake_submitted, wake_full, rejected_adopts;
  size_t failed_publishes;
  size_t rejected_reserves, occupying_connected, occupying_terminal;
  bool sending, close_allowed, closing, done, client_destroyed, allow_adopt;
  bool publication_held, allow_publish_after_seal, allow_empty_target_shutdown;
  bool release_context_allowed, context_released, occupying_closing;
  int status;
} remote_lane;

struct remote_case {
  native_io_sharded *sg;
  cnet_handoff inbox;
  remote_lane lane[REMOTE_SHARDS];
  remote_mode mode;
  bool fail_adopt, seal_before_publish, seal_verified;
};

static remote_case *remote_active;

static bool remote_capacity_case(const remote_case *test) {
  return test->mode >= REMOTE_MANAGER_FULL;
}

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
      !((lane->test->fail_adopt || lane->test->seal_before_publish) &&
        lane->shard == 0u))
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

static void remote_occupying_state(void *user, cnet_connection connection,
                                   cnet_connection_state state,
                                   const cnet_error *error) {
  remote_lane *lane = (remote_lane *)user;
  (void)connection;
  (void)error;
  if (cmeta_thread_current_token() != lane->owner_token)
    remote_error(lane, SALTS_EPERM);
  if (state == CNET_CONNECTION_CONNECTED) ++lane->occupying_connected;
  if (state == CNET_CONNECTION_CLOSED || state == CNET_CONNECTION_FAILED)
    ++lane->occupying_terminal;
  if (state == CNET_CONNECTION_FAILED) remote_error(lane, SALTS_ECONNRESET);
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
  if (lane->shard == REMOTE_TARGET && test->mode == REMOTE_TRANSPORT_FULL)
    network.connection_capacity = 1u;
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
    if (test->mode == REMOTE_MANAGER_FULL || test->mode == REMOTE_RECORD_HELD) {
      cnet_manager_attachment attachment = {0};
      attachment.observer = remote_observer(lane);
      attachment.on_recycle = remote_recycle;
      attachment.hold_context = true;
      REMOTE_OK(lane, cnet_manager_reserve(&lane->manager, &attachment,
                                           &lane->blocker));
      if (test->mode == REMOTE_RECORD_HELD)
        REMOTE_OK(lane, cnet_manager_cancel(&lane->manager, lane->blocker));
    }
  }
  if (lane->shard == 0u || test->mode == REMOTE_TRANSPORT_FULL) {
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
    if (lane->shard == REMOTE_TARGET) {
      /* A real loopback connection occupies the raw client's only slot.
       * Its local listener has no external requests and creates no observer. */
      cnet_observer observer = {0};
      observer.on_state = remote_occupying_state;
      observer.user = lane;
      REMOTE_OK(lane, cnet_connect_endpoint(&lane->client, &lane->local, NULL,
                                             &observer, &lane->occupying_connection));
      return;
    }
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
  cnet_accepted_stream *accepted = &lane->held_accepted;
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

  rc = cnet_listener_accept_detached(&lane->listener, accepted);
  if (rc != SALTS_OK) { remote_error(lane, rc); return; }
  rc = cnet_handoff_reserve(&test->inbox, &ticket);
  if (rc != SALTS_OK) {
    (void)cnet_accepted_stream_close(accepted);
    remote_error(lane, rc);
    return;
  }
  lane->held_ticket = ticket;
  /* Capacity is authoritative; candidate selection is NOT admission credit. */
  if (cnet_handoff_reserve(&test->inbox, &refused) != SALTS_ENOBUFS ||
      refused.slot != 0u || accepted->internal_active == 0u) {
    remote_error(lane, SALTS_EPROTO);
    return;
  }
  stale = ticket;
  ++stale.generation;
  if (cnet_handoff_publish(&test->inbox, stale, accepted) != SALTS_ENOENT ||
      accepted->internal_active == 0u) {
    remote_error(lane, SALTS_EPROTO);
    return;
  }
  /* The source owns both descriptor and RESERVED ticket until publish succeeds.
   * Hold them across an actual SG task barrier to let final Owner seal first. */
  if (test->seal_before_publish) {
    lane->publication_held = true;
    return;
  }
  rc = cnet_handoff_publish(&test->inbox, ticket, accepted);
  if (rc != SALTS_OK) { remote_error(lane, rc); return; }
  lane->held_ticket = (cnet_handoff_ticket){0};
  if (accepted->internal_active != 0u) {
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
  cnet_accepted_stream *stream = &lane->held_accepted;
  cnet_handoff_ticket ticket = {0};
  cnet_manager_attachment attachment = {0};
  cnet_managed_connection refused = {0};
  cnet_manager_snapshot manager = {0};
  int rc = cnet_handoff_take(&test->inbox, &ticket, stream);
  if (rc == SALTS_ENOENT) return;
  if (rc != SALTS_OK) { remote_error(lane, rc); return; }
  ++lane->taken;
  lane->taken_ticket = ticket;
  attachment.observer = remote_observer(lane);
  attachment.on_recycle = remote_recycle;
  attachment.hold_context = remote_capacity_case(test);
  rc = cnet_manager_reserve(&lane->manager, &attachment, &lane->managed);
  if (test->mode == REMOTE_MANAGER_FULL || test->mode == REMOTE_RECORD_HELD) {
    size_t work = 0u;
    if (rc != SALTS_ENOBUFS || lane->managed.slot != 0u ||
        stream->internal_active != 1u ||
        cnet_manager_get_snapshot(&lane->manager, &manager) != SALTS_OK ||
        manager.context_holds != 1u || manager.bound != 0u ||
        manager.reserved != (test->mode == REMOTE_MANAGER_FULL ? 1u : 0u) ||
        manager.retired != (test->mode == REMOTE_RECORD_HELD ? 1u : 0u)) {
      remote_error(lane, SALTS_EPROTO);
      return;
    }
    /* Handoff admission did not reserve Manager capacity. The final Owner
     * still owns this active stream and must close it on reserve rejection. */
    REMOTE_OK(lane, cnet_accepted_stream_close(stream));
    if (stream->internal_active != 0u ||
        cnet_accepted_stream_close(stream) != SALTS_EALREADY) {
      remote_error(lane, SALTS_EPROTO);
      return;
    }
    ++lane->rejected_reserves;
    if (test->mode == REMOTE_MANAGER_FULL)
      REMOTE_OK(lane, cnet_manager_cancel(&lane->manager, lane->blocker));
    REMOTE_OK(lane, cnet_manager_advance(&lane->manager, 1u, &work));
    if (work != 0u || lane->recycled != 0u ||
        cnet_manager_destroy(&lane->manager) != SALTS_EBUSY) {
      remote_error(lane, SALTS_EPROTO);
    }
    return;
  }
  if (rc != SALTS_OK) {
    /* A failed Manager admission does not retroactively transfer storage;
     * after take the FINAL owner must close and release the child. */
    (void)cnet_accepted_stream_close(stream);
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
    rc = cnet_manager_adopt(&lane->manager, lane->managed, stream,
                            test->mode == REMOTE_TRANSPORT_FULL ? NULL : &invalid_tls,
                            &lane->connection);
    if (rc != (test->mode == REMOTE_TRANSPORT_FULL ? SALTS_ENOBUFS : SALTS_EINVAL) ||
        stream->internal_active != 0u ||
        cnet_accepted_stream_close(stream) != SALTS_EALREADY ||
        lane->connection.slot != 0u ||
        cnet_manager_get_snapshot(&lane->manager, &after_failure) != SALTS_OK ||
        after_failure.reserved != 0u || after_failure.bound != 0u ||
        after_failure.retired != 1u ||
        after_failure.context_holds != (remote_capacity_case(test) ? 1u : 0u)) {
      remote_error(lane, SALTS_EPROTO);
      return;
    }
    ++lane->rejected_adopts;
    return;
  }
  rc = cnet_manager_adopt(&lane->manager, lane->managed, stream,
                          NULL, &lane->connection);
  if (rc != SALTS_OK) { remote_error(lane, rc); return; }
  if (stream->internal_active != 0u || lane->connection.slot == 0u) {
    remote_error(lane, SALTS_EPROTO);
    return;
  }
}

/* The final Owner seals its MPSC inbox after source reserves but before
 * source publish, with no task concurrently mutating the reserved record. */
static void remote_seal_target(native_io_sharded_context *context, void *user) {
  remote_lane *lane = (remote_lane *)user;
  remote_case *test = lane->test;
  cnet_handoff_snapshot snapshot = {0};
  if (lane->shard != REMOTE_TARGET ||
      native_io_sharded_context_shard(context) != REMOTE_TARGET ||
      cmeta_thread_current_token() != lane->owner_token) {
    remote_error(lane, SALTS_EPERM);
    return;
  }
  REMOTE_OK(lane, cnet_handoff_seal(&test->inbox));
  REMOTE_OK(lane, cnet_handoff_get_snapshot(&test->inbox, &snapshot));
  if (!snapshot.sealed || snapshot.drained ||
      snapshot.reserved != 1u || snapshot.queued != 0u ||
      snapshot.taken != 0u) {
    remote_error(lane, SALTS_EPROTO);
    return;
  }
  test->seal_verified = true;
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
    if (accepts != 1u || lane->published != 0u ||
        lane->publication_held) {
      remote_error(lane, SALTS_EPROTO);
      return;
    }
    remote_publish(lane);
    if (lane->status != SALTS_OK) return;
  }
  REMOTE_OK(lane, cnet_client_advance_external(&lane->client, &events));
  if (lane->shard == REMOTE_TARGET) {
    size_t work = 0u;
    if (remote_capacity_case(test) && lane->release_context_allowed &&
        !lane->context_released) {
      REMOTE_OK(lane, cnet_manager_release_context(&lane->manager,
          test->mode == REMOTE_TRANSPORT_FULL ? lane->managed : lane->blocker));
      lane->context_released = true;
    }
    REMOTE_OK(lane, cnet_manager_advance(&lane->manager, 1u, &work));
    if (remote_capacity_case(test) &&
        lane->rejected_adopts + lane->rejected_reserves == 1u &&
        !lane->release_context_allowed &&
        (lane->recycled != 0u ||
         cnet_manager_destroy(&lane->manager) != SALTS_EBUSY)) {
      remote_error(lane, SALTS_EPROTO);
      return;
    }
  }
  /* A final-Owner shutdown interleaving cannot revoke a source-owned
   * RESERVED credit or close the detached stream on publish failure. Stop
   * producing after ESHUTDOWN, close the source-owned socket, release its
   * credit once, and continue native progress to the real terminal. */
  if (test->seal_before_publish) {
    if (lane->shard == 0u) {
      if (lane->publication_held && lane->allow_publish_after_seal) {
        const uintptr_t socket = lane->held_accepted.internal_socket;
        const int publish_rc = cnet_handoff_publish(
            &test->inbox, lane->held_ticket, &lane->held_accepted);
        if (publish_rc != SALTS_ESHUTDOWN ||
            lane->held_accepted.internal_active != 1u ||
            lane->held_accepted.internal_socket != socket) {
          remote_error(lane, SALTS_EPROTO);
          return;
        }
        REMOTE_OK(lane, cnet_handoff_release(&test->inbox, lane->held_ticket));
        if (cnet_handoff_release(&test->inbox, lane->held_ticket) != SALTS_ENOENT) {
          remote_error(lane, SALTS_EPROTO);
          return;
        }
        REMOTE_OK(lane, cnet_accepted_stream_close(&lane->held_accepted));
        if (lane->held_accepted.internal_active != 0u) {
          remote_error(lane, SALTS_EPROTO);
          return;
        }
        lane->publication_held = false;
        ++lane->failed_publishes;
      }
      if (lane->failed_publishes != 0u && lane->connected != 0u &&
          !lane->closing) {
        int close_rc = cnet_close(&lane->client, lane->connection);
        if (close_rc != SALTS_OK && close_rc != SALTS_EALREADY &&
            close_rc != SALTS_ENOENT) {
          remote_error(lane, close_rc);
          return;
        }
        lane->closing = true;
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
    } else if (lane->allow_empty_target_shutdown) {
      cnet_manager_snapshot manager = {0};
      cnet_handoff_snapshot handoff = {0};
      REMOTE_OK(lane, cnet_manager_get_snapshot(&lane->manager, &manager));
      REMOTE_OK(lane, cnet_handoff_get_snapshot(&test->inbox, &handoff));
      if (!manager.drained || !handoff.sealed || !handoff.drained ||
          handoff.reserved != 0u || handoff.queued != 0u ||
          handoff.taken != 0u) {
        remote_error(lane, SALTS_EPROTO);
        return;
      }
      REMOTE_OK(lane, cnet_manager_destroy(&lane->manager));
      REMOTE_OK(lane, cnet_client_stop_external(&lane->client));
      REMOTE_OK(lane, cnet_client_destroy(&lane->client));
      lane->client_destroyed = true;
      REMOTE_OK(lane, native_io_sharded_context_release_host(context, lane->lease));
      lane->done = true;
    }
    return;
  }

  /* Failfast Manager adopt must still recycle its reservation and
   * terminalize the handed-off descriptor without inventing a transport
   * callback. The source connection shuts down independently after the
   * host's inter-round barrier observes final-owner rejection. */
  if (test->fail_adopt) {
    if (lane->shard == REMOTE_TARGET &&
        lane->rejected_adopts + lane->rejected_reserves == 1u) {
      cnet_manager_snapshot manager = {0};
      if (lane->recycled == 0u) return;
      if (test->mode == REMOTE_TRANSPORT_FULL) {
        if (!lane->occupying_closing) {
          REMOTE_OK(lane, cnet_close(&lane->client, lane->occupying_connection));
          lane->occupying_closing = true;
        }
        if (lane->occupying_terminal == 0u) return;
        REMOTE_OK(lane, cnet_listener_close(&lane->listener));
        REMOTE_OK(lane, cnet_listener_destroy(&lane->listener));
      }
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

static void remote_run_handoff(remote_mode mode) {
  /* Static fixture storage survives a fatal assertion until owner teardown. */
  static remote_case test;
  const bool fail_adopt = mode != REMOTE_SUCCESS && mode != REMOTE_SEALED;
  const bool seal_before_publish = mode == REMOTE_SEALED;
  native_io_sharded_task setup[REMOTE_SHARDS] = {{0}};
  native_io_sharded_task progress[REMOTE_SHARDS] = {{0}};
  native_io_sharded_task dial = {0};
  native_io_sharded_task target_seal = {0};
  const native_io_sharded_config config = {
      REMOTE_SHARDS, 8u,
      {remote_backend(), 24u, 48u, REMOTE_BATCH}};
  const uint64_t deadline = cmeta_monotonic_ms() + REMOTE_DEADLINE_MS;
  bool done = false;

  check_null(test.sg);
  memset(&test, 0, sizeof(test));
  remote_active = &test;
  test.mode = mode;
  test.fail_adopt = fail_adopt;
  test.seal_before_publish = seal_before_publish;
  check_equal(native_io_sharded_create(&config, &test.sg), SALTS_OK);
  check_not_null(test.sg);
  for (size_t i = 0u; i < REMOTE_SHARDS; ++i) {
    remote_lane *lane = &test.lane[i];
    lane->test = &test;
    lane->shard = i;
    lane->allow_adopt = !fail_adopt && !seal_before_publish;
    setup[i] = (native_io_sharded_task){remote_init, NULL, NULL, lane};
    progress[i] = (native_io_sharded_task){remote_progress, NULL, NULL, lane};
  }
  for (size_t i = 0u; i < REMOTE_SHARDS; ++i) {
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

  if (seal_before_publish) {
    /* Stop at real source RESERVE (with active detached TCP descriptor).
     * SG task barriers establish producer/owner quiescence for this ordered
     * adversarial interleaving without threads or wall-clock guesses. */
    while ((!test.lane[0].publication_held ||
            test.lane[0].connected == 0u) &&
           cmeta_monotonic_ms() < deadline) {
      for (size_t i = 0u; i < REMOTE_SHARDS; ++i)
        check_equal(native_io_sharded_submit_to(
            test.sg, i, &progress[i]), SALTS_OK);
      check_equal(native_io_sharded_wait(test.sg), SALTS_OK);
      check_equal(test.lane[0].status, SALTS_OK);
      check_equal(test.lane[1].status, SALTS_OK);
      cmeta_sleep_ms(1u);
    }
    check(test.lane[0].publication_held);
    check_equal(test.lane[0].connected, (size_t)1u);
    check_equal(test.lane[0].held_accepted.internal_active, 1u);
    {
      cnet_handoff_snapshot snapshot = {0};
      check_equal(cnet_handoff_get_snapshot(&test.inbox, &snapshot), SALTS_OK);
      check_equal(snapshot.reserved, (size_t)1u);
      check_equal(snapshot.queued, (size_t)0u);
      check_equal(snapshot.taken, (size_t)0u);
    }
    target_seal = (native_io_sharded_task){
        remote_seal_target, NULL, NULL, &test.lane[REMOTE_TARGET]};
    check_equal(native_io_sharded_submit_to(
        test.sg, REMOTE_TARGET, &target_seal), SALTS_OK);
    check_equal(native_io_sharded_wait(test.sg), SALTS_OK);
    check_equal(test.lane[1].status, SALTS_OK);
    check(test.seal_verified);
    test.lane[0].allow_publish_after_seal = true;
  }

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
    if (seal_before_publish) {
      /* The destination must not destroy its host/backend until the source
       * has failed publication, returned RESERVED credit, closed its child
       * descriptor and finished its own native terminal/progress. */
      if (test.lane[0].done) test.lane[1].allow_empty_target_shutdown = true;
    } else if (fail_adopt) {
      /* The source's real CONNECTED callback must complete before Owner 1
       * can reject/close its detached accepted child. All reads/writes here
       * occur only after native_io_sharded_wait's task barrier. */
      if (test.lane[0].connected == 1u && test.lane[0].published == 1u &&
          (mode != REMOTE_TRANSPORT_FULL || test.lane[1].occupying_connected == 1u))
        test.lane[1].allow_adopt = true;
      if (remote_capacity_case(&test) &&
          test.lane[1].rejected_adopts + test.lane[1].rejected_reserves == 1u) {
        test.lane[1].release_context_allowed = true;
      }
      if (test.lane[1].rejected_adopts + test.lane[1].rejected_reserves == 1u &&
          test.lane[1].recycled == 1u)
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
    check_equal(lane->connected, (size_t)((fail_adopt || seal_before_publish) &&
                                           i == REMOTE_TARGET ? 0u : 1u));
    check_equal(lane->terminal, (size_t)((fail_adopt || seal_before_publish) &&
                                          i == REMOTE_TARGET ? 0u : 1u));
    check_equal(lane->bytes, (size_t)(fail_adopt || seal_before_publish ? 0u : 4u));
    check_equal(lane->sent, (size_t)(fail_adopt || seal_before_publish ? 0u : 4u));
    check_null(lane->client.impl);
  }
  check_equal(test.lane[0].accept_completions, (size_t)1u);
  check_equal(test.lane[0].placement_calls, (size_t)1u);
  check_equal(test.lane[0].published, (size_t)(seal_before_publish ? 0u : 1u));
  check_equal(test.lane[0].wake_submitted + test.lane[0].wake_full,
              (size_t)(seal_before_publish ? 0u : 1u));
  check_equal(test.lane[1].taken, (size_t)(seal_before_publish ? 0u : 1u));
  check_equal(test.lane[1].recycled, (size_t)(seal_before_publish ? 0u : 1u));
  check_equal(test.lane[1].rejected_adopts,
              (size_t)(mode == REMOTE_INVALID_TLS || mode == REMOTE_TRANSPORT_FULL ? 1u : 0u));
  check_equal(test.lane[1].rejected_reserves,
              (size_t)(mode == REMOTE_MANAGER_FULL || mode == REMOTE_RECORD_HELD ? 1u : 0u));
  check_equal(test.lane[1].occupying_connected,
              (size_t)(mode == REMOTE_TRANSPORT_FULL ? 1u : 0u));
  check_equal(test.lane[1].occupying_terminal,
              (size_t)(mode == REMOTE_TRANSPORT_FULL ? 1u : 0u));
  check_equal(test.lane[0].failed_publishes,
              (size_t)(seal_before_publish ? 1u : 0u));
  if (seal_before_publish) check(test.seal_verified);
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
  test.sg = NULL;
}

/* Fatal test assertions must still settle owner-local state on its SG lane.
 * Never force-free a client/backend when native cancellation is pending. */
static void remote_cleanup(native_io_sharded_context *context, void *user) {
  remote_lane *lane = (remote_lane *)user;
  remote_case *test = lane->test;
  size_t work = 0u;
  bool client_pending = false;
  int rc;
  if (lane->done) return;
  if (lane->held_accepted.internal_active)
    REMOTE_OK(lane, cnet_accepted_stream_close(&lane->held_accepted));
  if (test->inbox.impl != NULL) {
    REMOTE_OK(lane, cnet_handoff_seal(&test->inbox));
    if (lane->shard == REMOTE_TARGET) {
      for (;;) {
        cnet_accepted_stream stream = CNET_ACCEPTED_STREAM_INIT;
        cnet_handoff_ticket ticket = {0};
        rc = cnet_handoff_take(&test->inbox, &ticket, &stream);
        if (rc == SALTS_ENOENT) break;
        if (rc != SALTS_OK) { remote_error(lane, rc); return; }
        REMOTE_OK(lane, cnet_accepted_stream_close(&stream));
        REMOTE_OK(lane, cnet_handoff_release(&test->inbox, ticket));
      }
    } else if (lane->held_ticket.slot != 0u) {
      rc = cnet_handoff_release(&test->inbox, lane->held_ticket);
      if (rc != SALTS_OK && rc != SALTS_ENOENT && rc != SALTS_EBUSY) {
        remote_error(lane, rc);
        return;
      }
      lane->held_ticket = (cnet_handoff_ticket){0};
    }
  }
  if (lane->manager.impl != NULL) {
    REMOTE_OK(lane, cnet_manager_request_close(&lane->manager));
    if (remote_capacity_case(test) && !lane->context_released) {
      const cnet_managed_connection held =
          test->mode == REMOTE_TRANSPORT_FULL ? lane->managed : lane->blocker;
      if (held.slot != 0u) {
        rc = cnet_manager_release_context(&lane->manager, held);
        if (rc != SALTS_OK && rc != SALTS_EALREADY && rc != SALTS_ENOENT) {
          remote_error(lane, rc);
          return;
        }
      }
      lane->context_released = true;
    }
    REMOTE_OK(lane, cnet_manager_advance(&lane->manager, 1u, &work));
  }
  if (lane->listener.impl != NULL) {
    rc = cnet_listener_close(&lane->listener);
    if (rc == SALTS_OK || rc == SALTS_EALREADY)
      REMOTE_OK(lane, cnet_listener_destroy(&lane->listener));
    else if (rc != SALTS_EBUSY) { remote_error(lane, rc); return; }
  }
  if (lane->client.impl != NULL) {
    if (lane->connection.slot != 0u && !lane->closing) {
      rc = cnet_close(&lane->client, lane->connection);
      if (rc != SALTS_OK && rc != SALTS_EALREADY && rc != SALTS_ENOENT) {
        remote_error(lane, rc);
        return;
      }
      lane->closing = true;
    }
    if (lane->occupying_connection.slot != 0u && !lane->occupying_closing) {
      rc = cnet_close(&lane->client, lane->occupying_connection);
      if (rc != SALTS_OK && rc != SALTS_EALREADY && rc != SALTS_ENOENT) {
        remote_error(lane, rc);
        return;
      }
      lane->occupying_closing = true;
    }
    rc = cnet_client_stop_external(&lane->client);
    client_pending = rc == SALTS_EBUSY;
    if (rc != SALTS_OK && rc != SALTS_EALREADY && !client_pending) {
      remote_error(lane, rc);
      return;
    }
  }
  if (client_pending || lane->listener.impl != NULL) {
    native_io_sharded_completion completions[REMOTE_BATCH];
    cnet_client *clients[] = {&lane->client};
    cnet_sg_host_routes routes = {
        sizeof(cnet_sg_host_routes), CNET_SG_HOST_ROUTING_VERSION,
        lane->shard == 0u && lane->listener.impl ? &lane->listener : NULL,
        clients, client_pending ? 1u : 0u};
    size_t count = 0u, accepts = 0u, sharded = 0u;
    if (client_pending)
      REMOTE_OK(lane, cnet_client_advance_external(&lane->client, &work));
    rc = native_io_sharded_context_observe_host(
        context, lane->lease, completions, REMOTE_BATCH, 0u, &count);
    if (rc != SALTS_OK && rc != SALTS_ETIMEDOUT) { remote_error(lane, rc); return; }
    REMOTE_OK(lane, cnet_sg_host_route_batch(
        completions, count, &routes, &accepts, &sharded));
    return;
  }
  if (lane->manager.impl != NULL) {
    REMOTE_OK(lane, cnet_manager_advance(&lane->manager, 1u, &work));
    REMOTE_OK(lane, cnet_manager_destroy(&lane->manager));
  }
  if (lane->client.impl != NULL)
    REMOTE_OK(lane, cnet_client_destroy(&lane->client));
  lane->client_destroyed = true;
  if (lane->taken_ticket.slot != 0u && test->inbox.impl != NULL) {
    rc = cnet_handoff_release(&test->inbox, lane->taken_ticket);
    if (rc != SALTS_OK && rc != SALTS_ENOENT) { remote_error(lane, rc); return; }
    lane->taken_ticket = (cnet_handoff_ticket){0};
  }
  if (lane->backend != NULL)
    REMOTE_OK(lane, native_io_sharded_context_release_host(context, lane->lease));
  lane->done = true;
}

static void remote_teardown(void) {
  remote_case *test = remote_active;
  if (test == NULL || test->sg == NULL) return;
  check_equal(native_io_sharded_wait(test->sg), SALTS_OK);
  const uint64_t end = cmeta_monotonic_ms() + REMOTE_DEADLINE_MS;
  bool done = false;
  while (!done && cmeta_monotonic_ms() < end) {
    done = true;
    for (size_t i = 0u; i < REMOTE_SHARDS; ++i) {
      if (test->lane[i].done) continue;
      const native_io_sharded_task task = {remote_cleanup, NULL, NULL, &test->lane[i]};
      check_equal(native_io_sharded_submit_to(test->sg, i, &task), SALTS_OK);
      done = false;
    }
    check_equal(native_io_sharded_wait(test->sg), SALTS_OK);
  }
  check(done);
  if (test->inbox.impl != NULL)
    check_equal(cnet_handoff_destroy(&test->inbox), SALTS_OK);
  check_equal(native_io_sharded_shutdown(test->sg), SALTS_OK);
  check_equal(native_io_sharded_destroy(test->sg), SALTS_OK);
  test->sg = NULL;
}

/* Three real listener Owners publish into one final Owner's MPSC inbox.
 * Owner 0 publishes before seal and pauses in its actual publish -> wake tail;
 * Owners 1/2 race publication against Owner 3 sealing admission. Queued children
 * are canceled (take/close/release) on Owner 3, not adopted into a second
 * transport. This isolates producer quiescence from the earlier adopt tests.
 * Atomic gates deliberately pause test tasks, with a finite failure deadline;
 * ordinary transport progress remains in short, single-observe SG turns. */
enum { RACE_PRODUCERS = 3u, RACE_TARGET = 3u, RACE_SHARDS = 4u,
       RACE_TASK_CAPACITY = 8u };
typedef struct race_case race_case;
typedef struct race_lane {
  race_case *test;
  size_t shard;
  native_io_sharded_host_lease lease;
  native_io_backend *backend;
  cnet_client client;
  cnet_listener listener;
  cnet_connection connection;
  cnet_accepted_stream accepted;
  cnet_handoff_ticket ticket;
  const void *owner_token;
  size_t connected, terminal, accepts, wakes, taken, queue_tasks;
  bool receiving, closing, released;
  atomic_bool publish_finished, producer_done;
  int status, publish_status, wake_status, early_release;
} race_lane;
struct race_case {
  native_io_sharded *sg;
  cnet_handoff inbox;
  race_lane lane[RACE_SHARDS];
  atomic_size_t race_ready;
  atomic_bool start, wake_allowed, abort, target_probed, target_allowed;
  bool full_wake_queue;
};
static race_case race_test;

#define RACE_OK(lane, call) do { int race_rc_ = (call); \
  if (race_rc_ != SALTS_OK) { (lane)->status = race_rc_; return; } \
} while (0)

static bool race_gate(race_lane *lane, atomic_bool *gate) {
  const uint64_t end = cmeta_monotonic_ms() + REMOTE_DEADLINE_MS;
  while (!atomic_load(gate)) {
    if (atomic_load(&lane->test->abort)) return false;
    if (cmeta_monotonic_ms() >= end) {
      lane->status = SALTS_ETIMEDOUT;
      return false;
    }
    cmeta_thread_yield();
  }
  return !atomic_load(&lane->test->abort);
}

static bool race_quiescent(void *user) {
  race_lane *lane = user;
  if (lane->client.impl != NULL || lane->listener.impl != NULL) return false;
  if (lane->shard == RACE_TARGET) {
    for (size_t i = 0u; i < RACE_PRODUCERS; ++i)
      if (!atomic_load(&lane->test->lane[i].producer_done)) return false;
  }
  return true;
}

static void race_state(void *user, cnet_connection connection,
                       cnet_connection_state state, const cnet_error *error) {
  race_lane *lane = user;
  (void)connection;
  (void)error;
  if (cmeta_thread_current_token() != lane->owner_token) {
    lane->status = SALTS_EPERM;
    return;
  }
  if (state == CNET_CONNECTION_CONNECTED) ++lane->connected;
  if (state == CNET_CONNECTION_CLOSED || state == CNET_CONNECTION_FAILED)
    ++lane->terminal; /* Actual peer close/reset, never an application ACK. */
}

static void race_receive(void *user, cnet_connection connection,
                         const cnet_receive_view *view) {
  race_lane *lane = user;
  (void)connection;
  (void)view;
  /* These children are canceled without application data. Receiving demand
   * observes real EOF/reset; an unexpected payload is a fixture failure. */
  lane->status = SALTS_EPROTO;
}

static void race_init(native_io_sharded_context *context, void *user) {
  race_lane *lane = user;
  cnet_client_config config = remote_client_config();
  if (native_io_sharded_context_shard(context) != lane->shard) {
    lane->status = SALTS_EPERM;
    return;
  }
  lane->owner_token = cmeta_thread_current_token();
  RACE_OK(lane, native_io_sharded_context_acquire_host(
      context, race_quiescent, lane, &lane->lease, &lane->backend));
  if (lane->shard == RACE_TARGET) {
    const cnet_handoff_config inbox = {
        sizeof(cnet_handoff_config), CNET_HANDOFF_VERSION,
        RACE_PRODUCERS, RACE_PRODUCERS};
    RACE_OK(lane, cnet_handoff_init(&lane->test->inbox, &inbox));
  } else {
    cnet_stream_endpoint bind = CNET_STREAM_ENDPOINT_INIT, local;
    native_io_request request = {0};
    cnet_observer observer = {0};
    bind.family = CNET_DATAGRAM_ADDRESS_IPV4;
    bind.address[0] = 127u;
    bind.address[3] = 1u;
    observer.on_state = race_state;
    observer.on_receive = race_receive;
    observer.user = lane;
    RACE_OK(lane, cnet_client_init_external(&lane->client, &config, lane->backend));
    RACE_OK(lane, cnet_listener_open(&lane->listener, remote_backend(),
                                    CNET_DATAGRAM_ADDRESS_IPV4));
    RACE_OK(lane, cnet_listener_bind_open_endpoint(&lane->listener, &bind));
    RACE_OK(lane, cnet_listener_local_endpoint(&lane->listener, &local));
    RACE_OK(lane, cnet_listener_listen(&lane->listener, 4u));
    RACE_OK(lane, cnet_listener_attach_external(&lane->listener, lane->backend));
    RACE_OK(lane, cnet_listener_submit_external_accept(&lane->listener, &request));
    RACE_OK(lane, cnet_connect_endpoint(&lane->client, &local, NULL,
                                       &observer, &lane->connection));
  }
}

static void race_progress(native_io_sharded_context *context, void *user) {
  race_lane *lane = user;
  native_io_sharded_completion completions[REMOTE_BATCH];
  cnet_client *clients[] = {&lane->client};
  cnet_sg_host_routes routes = {
      sizeof(cnet_sg_host_routes), CNET_SG_HOST_ROUTING_VERSION,
      lane->listener.impl != NULL ? &lane->listener : NULL, clients, 1u};
  size_t events = 0u, count = 0u, accepts = 0u, sg_owned = 0u;
  int rc;
  RACE_OK(lane, cnet_client_advance_external(&lane->client, &events));
  rc = native_io_sharded_context_observe_host(
      context, lane->lease, completions, REMOTE_BATCH, 0u, &count);
  if (rc != SALTS_OK && rc != SALTS_ETIMEDOUT) { lane->status = rc; return; }
  RACE_OK(lane, cnet_sg_host_route_batch(
      completions, count, &routes, &accepts, &sg_owned));
  if (accepts != 0u) {
    lane->accepts += accepts;
    if (lane->accepts != 1u) { lane->status = SALTS_EPROTO; return; }
    RACE_OK(lane, cnet_listener_accept_detached(&lane->listener, &lane->accepted));
    if (!atomic_load(&lane->test->abort))
      RACE_OK(lane, cnet_handoff_reserve(&lane->test->inbox, &lane->ticket));
  }
  if (lane->connected != 0u && !lane->receiving && lane->terminal == 0u &&
      !atomic_load(&lane->test->abort)) {
    RACE_OK(lane, cnet_receive(&lane->client, lane->connection, 1u));
    lane->receiving = true;
  }
}

static void race_wake(native_io_sharded_context *context, void *user) {
  race_lane *lane = user;
  native_io_sharded_completion completions[REMOTE_BATCH];
  size_t count = 0u;
  int rc;
  if (native_io_sharded_context_shard(context) != RACE_TARGET ||
      cmeta_thread_current_token() != lane->owner_token ||
      !atomic_load(&lane->test->target_probed) || lane->released) {
    lane->status = SALTS_EPERM;
    return;
  }
  /* A delayed real SG wake still reaches the leased backend after inbox drain. */
  rc = native_io_sharded_context_observe_host(
      context, lane->lease, completions, REMOTE_BATCH, 0u, &count);
  if (rc != SALTS_OK && rc != SALTS_ETIMEDOUT) { lane->status = rc; return; }
  for (size_t i = 0u; i < count; ++i)
    if (!completions[i].sharded_owned) { lane->status = SALTS_EPROTO; return; }
  ++lane->wakes;
}

static void race_queue_occupant(native_io_sharded_context *context, void *user) {
  race_lane *lane = user;
  if (native_io_sharded_context_shard(context) != RACE_TARGET ||
      cmeta_thread_current_token() != lane->owner_token || lane->released) {
    lane->status = SALTS_EPERM;
    return;
  }
  ++lane->queue_tasks;
}

static void race_publish(native_io_sharded_context *context, void *user) {
  race_lane *lane = user;
  race_case *test = lane->test;
  const uintptr_t socket = lane->accepted.internal_socket;
  (void)context;
  if (lane->shard != 0u) {
    atomic_fetch_add(&test->race_ready, 1u);
    if (!race_gate(lane, &test->start)) goto finished;
  }
  lane->publish_status = cnet_handoff_publish(&test->inbox, lane->ticket,
                                             &lane->accepted);
  if (lane->publish_status == SALTS_OK) {
    if (lane->accepted.internal_active != 0u) lane->status = SALTS_EPROTO;
  } else if (lane->publish_status == SALTS_ESHUTDOWN) {
    if (lane->accepted.internal_active != 1u ||
        lane->accepted.internal_socket != socket) lane->status = SALTS_EPROTO;
    int rc = cnet_handoff_release(&test->inbox, lane->ticket);
    if (rc != SALTS_OK) lane->status = rc;
    if (cnet_handoff_release(&test->inbox, lane->ticket) != SALTS_ENOENT)
      lane->status = SALTS_EPROTO;
    rc = cnet_accepted_stream_close(&lane->accepted);
    if (rc != SALTS_OK) lane->status = rc;
  } else lane->status = lane->publish_status;
  atomic_store(&lane->publish_finished, true);
  if (lane->publish_status == SALTS_OK &&
      race_gate(lane, &test->wake_allowed)) {
    const native_io_sharded_task wake = {
        race_wake, NULL, NULL, &test->lane[RACE_TARGET]};
    lane->wake_status = native_io_sharded_try_submit_to(test->sg, RACE_TARGET, &wake);
    if (lane->wake_status != SALTS_OK && lane->wake_status != SALTS_ENOBUFS)
      lane->status = lane->wake_status;
    /* Even a full wake queue cannot return the moved socket or its ticket. */
  }
finished:
  atomic_store(&lane->producer_done, true);
}

static void race_seal_and_probe(native_io_sharded_context *context, void *user) {
  race_lane *lane = user;
  race_case *test = lane->test;
  cnet_handoff_snapshot snapshot = {0};
  atomic_fetch_add(&test->race_ready, 1u);
  if (!race_gate(lane, &test->start)) return;
  RACE_OK(lane, cnet_handoff_seal(&test->inbox));
  for (size_t i = 0u; i < RACE_PRODUCERS; ++i)
    if (!race_gate(lane, &test->lane[i].publish_finished)) return;
  for (size_t i = 0u; i < RACE_PRODUCERS; ++i) {
    cnet_accepted_stream stream = CNET_ACCEPTED_STREAM_INIT;
    cnet_handoff_ticket ticket = {0};
    int rc = cnet_handoff_take(&test->inbox, &ticket, &stream);
    if (rc == SALTS_ENOENT) break;
    if (rc != SALTS_OK) { lane->status = rc; return; }
    RACE_OK(lane, cnet_accepted_stream_close(&stream));
    RACE_OK(lane, cnet_handoff_release(&test->inbox, ticket));
    if (cnet_handoff_release(&test->inbox, ticket) != SALTS_ENOENT) {
      lane->status = SALTS_EPROTO;
      return;
    }
    ++lane->taken;
  }
  RACE_OK(lane, cnet_handoff_get_snapshot(&test->inbox, &snapshot));
  if (!snapshot.sealed || !snapshot.drained) { lane->status = SALTS_EPROTO; return; }
  /* Target owns no raw endpoints/client now. Only the live publish -> wake
   * producer tail prevents release; credit drain alone cannot authorize it. */
  lane->early_release = native_io_sharded_context_release_host(context, lane->lease);
  if (lane->early_release != SALTS_EBUSY) { lane->status = SALTS_EPROTO; return; }
  atomic_store(&test->target_probed, true);
  if (test->full_wake_queue) (void)race_gate(lane, &test->target_allowed);
}

static void race_cleanup(native_io_sharded_context *context, void *user) {
  race_lane *lane = user;
  race_case *test = lane->test;
  int rc;
  if (lane->released) return;
  if (lane->shard == RACE_TARGET) {
    if (test->inbox.impl != NULL) {
      cnet_accepted_stream stream;
      cnet_handoff_ticket ticket;
      RACE_OK(lane, cnet_handoff_seal(&test->inbox));
      while ((rc = cnet_handoff_take(&test->inbox, &ticket, &stream)) == SALTS_OK) {
        RACE_OK(lane, cnet_accepted_stream_close(&stream));
        RACE_OK(lane, cnet_handoff_release(&test->inbox, ticket));
      }
      if (rc != SALTS_ENOENT) { lane->status = rc; return; }
    }
  } else {
    if (lane->accepted.internal_active) {
      RACE_OK(lane, cnet_accepted_stream_close(&lane->accepted));
      if (lane->ticket.slot != 0u) {
        RACE_OK(lane, cnet_handoff_release(&test->inbox, lane->ticket));
      }
    }
    if (lane->client.impl != NULL && lane->connection.slot != 0u &&
        lane->terminal == 0u && !lane->closing) {
      rc = cnet_close(&lane->client, lane->connection);
      if (rc != SALTS_OK && rc != SALTS_EALREADY && rc != SALTS_ENOENT) {
        lane->status = rc;
        return;
      }
      lane->closing = true;
    }
    if (lane->listener.impl != NULL) {
      rc = cnet_listener_close(&lane->listener);
      if (rc == SALTS_EBUSY) { race_progress(context, user); return; }
      if (rc != SALTS_OK && rc != SALTS_EALREADY) { lane->status = rc; return; }
      RACE_OK(lane, cnet_listener_destroy(&lane->listener));
    }
    if (lane->client.impl != NULL) {
      rc = cnet_client_stop_external(&lane->client);
      if (rc == SALTS_EBUSY) { race_progress(context, user); return; }
      if (rc != SALTS_OK) { lane->status = rc; return; }
      RACE_OK(lane, cnet_client_destroy(&lane->client));
    }
  }
  if (lane->backend != NULL) {
    RACE_OK(lane, native_io_sharded_context_release_host(context, lane->lease));
  }
  lane->released = true;
}

static void race_submit(size_t shard, native_io_sharded_task_fn run) {
  const native_io_sharded_task task = {run, NULL, NULL, &race_test.lane[shard]};
  check_equal(native_io_sharded_submit_to(race_test.sg, shard, &task), SALTS_OK);
}

static void race_wait_flag(atomic_bool *flag) {
  const uint64_t end = cmeta_monotonic_ms() + REMOTE_DEADLINE_MS;
  while (!atomic_load(flag) && cmeta_monotonic_ms() < end) cmeta_thread_yield();
  check(atomic_load(flag));
}

spec("CNet SG remote Owner detached TCP handoff/credit lifecycle") {
  after_each() { remote_teardown(); }
  it("accepts on Owner 0, adopts on Owner 1, and recycles after real terminal") {
    remote_run_handoff(REMOTE_SUCCESS);
  }
  it("consumes invalid final-Owner Manager adopt and reclaims handoff credit") {
    remote_run_handoff(REMOTE_INVALID_TLS);
  }
  it("preserves source descriptor when final Owner seals before publish") {
    remote_run_handoff(REMOTE_SEALED);
  }
  it("closes a handed-off child when final-Owner Manager admission is full") {
    remote_run_handoff(REMOTE_MANAGER_FULL);
  }
  it("rejects handoff admission while a retired Manager context holds its record") {
    remote_run_handoff(REMOTE_RECORD_HELD);
  }
  it("consumes handed-off adoption when a real connection fills raw CNet capacity") {
    remote_run_handoff(REMOTE_TRANSPORT_FULL);
  }
  group("concurrent real SG producers and shutdown wake tails") {
    before_each() {
      memset(&race_test, 0, sizeof(race_test));
      atomic_init(&race_test.race_ready, 0u);
      atomic_init(&race_test.start, false);
      atomic_init(&race_test.wake_allowed, false);
      atomic_init(&race_test.abort, false);
      atomic_init(&race_test.target_probed, false);
      atomic_init(&race_test.target_allowed, false);
      for (size_t i = 0u; i < RACE_SHARDS; ++i) {
        race_lane *lane = &race_test.lane[i];
        lane->test = &race_test;
        lane->shard = i;
        lane->accepted = (cnet_accepted_stream)CNET_ACCEPTED_STREAM_INIT;
        lane->publish_status = lane->wake_status = SALTS_EBUSY;
        atomic_init(&lane->publish_finished, false);
        atomic_init(&lane->producer_done, false);
      }
    }
    after_each() {
      if (race_test.sg != NULL) {
        /* Abort gates before joining even when a fatal assertion fired. Keep
         * fixture storage alive until every producer and real native request
         * has settled; no timeout-authorized forced destruction. */
        atomic_store(&race_test.abort, true);
        check_equal(native_io_sharded_wait(race_test.sg), SALTS_OK);
        for (size_t i = 0u; i < RACE_PRODUCERS; ++i)
          atomic_store(&race_test.lane[i].producer_done, true);
        const uint64_t end = cmeta_monotonic_ms() + REMOTE_DEADLINE_MS;
        bool done = false;
        while (!done && cmeta_monotonic_ms() < end) {
          done = true;
          for (size_t i = 0u; i < RACE_SHARDS; ++i) {
            if (!race_test.lane[i].released) {
              race_submit(i, race_cleanup);
              done = false;
            }
          }
          check_equal(native_io_sharded_wait(race_test.sg), SALTS_OK);
        }
        check(done);
        if (race_test.inbox.impl != NULL)
          check_equal(cnet_handoff_destroy(&race_test.inbox), SALTS_OK);
        check_equal(native_io_sharded_shutdown(race_test.sg), SALTS_OK);
        check_equal(native_io_sharded_destroy(race_test.sg), SALTS_OK);
        race_test.sg = NULL;
      }
    }
    for (size_t wake_mode = 0u; wake_mode < 2u; ++wake_mode) {
      it("settles racing publications and retains a drained target with %s wake queue",
         wake_mode == 0u ? "available" : "full") {
        race_test.full_wake_queue = wake_mode != 0u;
        const native_io_sharded_config config = {
            RACE_SHARDS, RACE_TASK_CAPACITY, {remote_backend(), 24u, 48u, REMOTE_BATCH}};
        const uint64_t end = cmeta_monotonic_ms() + REMOTE_DEADLINE_MS;
        cnet_handoff_snapshot snapshot = {0};
        bool ready = false;
        check_equal(native_io_sharded_create(&config, &race_test.sg), SALTS_OK);
        race_submit(RACE_TARGET, race_init);
        check_equal(native_io_sharded_wait(race_test.sg), SALTS_OK);
        check_equal(race_test.lane[RACE_TARGET].status, SALTS_OK);
        for (size_t i = 0u; i < RACE_PRODUCERS; ++i) race_submit(i, race_init);
        check_equal(native_io_sharded_wait(race_test.sg), SALTS_OK);
        while (!ready && cmeta_monotonic_ms() < end) {
          for (size_t i = 0u; i < RACE_PRODUCERS; ++i) race_submit(i, race_progress);
          check_equal(native_io_sharded_wait(race_test.sg), SALTS_OK);
          ready = true;
          for (size_t i = 0u; i < RACE_PRODUCERS; ++i) {
            race_lane *lane = &race_test.lane[i];
            check_equal(lane->status, SALTS_OK);
            if (lane->connected != 1u || lane->accepted.internal_active != 1u) ready = false;
            check(lane->owner_token != race_test.lane[RACE_TARGET].owner_token);
          }
        }
        check(ready);
        for (size_t i = 0u; i < RACE_SHARDS; ++i) {
          for (size_t j = i + 1u; j < RACE_SHARDS; ++j) {
            check(race_test.lane[i].owner_token != race_test.lane[j].owner_token);
            check(race_test.lane[i].backend != race_test.lane[j].backend);
          }
        }
        check_equal(cnet_handoff_get_snapshot(&race_test.inbox, &snapshot), SALTS_OK);
        check_equal(snapshot.reserved, (size_t)RACE_PRODUCERS);
        race_submit(0u, race_publish);
        race_wait_flag(&race_test.lane[0].publish_finished);
        race_submit(1u, race_publish);
        race_submit(2u, race_publish);
        race_submit(RACE_TARGET, race_seal_and_probe);
        while (atomic_load(&race_test.race_ready) != 3u && cmeta_monotonic_ms() < end)
          cmeta_thread_yield();
        check_equal(atomic_load(&race_test.race_ready), (size_t)3u);
        atomic_store(&race_test.start, true);
        race_wait_flag(&race_test.target_probed);
        check_false(atomic_load(&race_test.lane[0].producer_done));
        check_equal(cnet_handoff_get_snapshot(&race_test.inbox, &snapshot), SALTS_OK);
        check(snapshot.sealed && snapshot.drained);
        check_equal(snapshot.reserved + snapshot.queued + snapshot.taken, (size_t)0u);
        size_t queued = 0u;
        if (race_test.full_wake_queue) {
          /* Owner 3 is parked after the release probe. Occupy its actual bounded
           * SG task queue so every source's post-publication wake fails FULL.
           * No fabricated completion and no ownership rollback/republication. */
          const native_io_sharded_task task = {
              race_queue_occupant, NULL, NULL, &race_test.lane[RACE_TARGET]};
          int rc = SALTS_OK;
          for (size_t i = 0u; i <= RACE_TASK_CAPACITY; ++i) {
            rc = native_io_sharded_try_submit_to(race_test.sg, RACE_TARGET, &task);
            if (rc != SALTS_OK) break;
            ++queued;
          }
          check_equal(rc, SALTS_ENOBUFS);
          check_equal(queued, (size_t)RACE_TASK_CAPACITY);
        }
        atomic_store(&race_test.wake_allowed, true);
        if (race_test.full_wake_queue) {
          for (size_t i = 0u; i < RACE_PRODUCERS; ++i)
            race_wait_flag(&race_test.lane[i].producer_done);
          atomic_store(&race_test.target_allowed, true);
        }
        check_equal(native_io_sharded_wait(race_test.sg), SALTS_OK);
        size_t published = 0u, wake_accepted = 0u;
        for (size_t i = 0u; i < RACE_PRODUCERS; ++i) {
          race_lane *lane = &race_test.lane[i];
          check_equal(lane->status, SALTS_OK);
          check(atomic_load(&lane->producer_done));
          check(lane->publish_status == SALTS_OK || lane->publish_status == SALTS_ESHUTDOWN);
          check_equal(lane->accepted.internal_active, 0u);
          if (lane->publish_status == SALTS_OK) {
            ++published;
            check_equal(lane->wake_status,
                        race_test.full_wake_queue ? SALTS_ENOBUFS : SALTS_OK);
            if (lane->wake_status == SALTS_OK) ++wake_accepted;
          } else check_equal(lane->wake_status, SALTS_EBUSY); /* No wake on rejection. */
        }
        check_equal(race_test.lane[0].publish_status, SALTS_OK);
        check_equal(race_test.lane[RACE_TARGET].status, SALTS_OK);
        check_equal(race_test.lane[RACE_TARGET].early_release, SALTS_EBUSY);
        check_equal(race_test.lane[RACE_TARGET].taken, published);
        check_equal(race_test.lane[RACE_TARGET].wakes, wake_accepted);
        check_equal(race_test.lane[RACE_TARGET].queue_tasks, queued);
        ready = false;
        while (!ready && cmeta_monotonic_ms() < end) {
          for (size_t i = 0u; i < RACE_PRODUCERS; ++i) race_submit(i, race_progress);
          check_equal(native_io_sharded_wait(race_test.sg), SALTS_OK);
          ready = true;
          for (size_t i = 0u; i < RACE_PRODUCERS; ++i) {
            check_equal(race_test.lane[i].status, SALTS_OK);
            if (race_test.lane[i].terminal != 1u) ready = false;
          }
        }
        check(ready);
        for (size_t i = 0u; i < RACE_PRODUCERS; ++i) {
          check_equal(race_test.lane[i].accepts, (size_t)1u);
          check_equal(race_test.lane[i].connected, (size_t)1u);
          check_equal(race_test.lane[i].terminal, (size_t)1u);
        }
      }
    }
  }
}
