#include <cnet/cnet.h>
#include <cnet/sg_host.h>
#include <cnet/owner_placement.h>
#include <cnet/destination_policy.h>
#include <cnet/manager.h>
#include <cnet/client_pool.h>
#include <cnet/managed_dial.h>
#include <salts/native_io_sharded.h>
#include <salts/clock.h>
#include <salts/thread.h>
#include <tinytest.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#if defined(__linux__)
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#endif

/* Real CNet TCP inbound/outbound and UDP sharing a NativeIO SG backend, with one short
 * owner-progress task per cycle. SG-owned pipe terminal uses the same observe
 * authority; no Actor, duplicate NativeIO observe, or blocking forever task. */
enum { HOST_BATCH = 16u, HOST_TIMEOUT_MS = 8000u, HOST_MAX_OWNERS = 4u,
       HOST_POLICY_VERSION = 1u };

/* Configurator/host maps deployment data into one immutable copied policy
 * plan before SG starts. This test does not create a policy runtime/parser. */
typedef struct host_policy_plan {
  uint32_t version;
  cnet_owner_placement_kind server;
  cnet_destination_policy_kind client;
  uint64_t allowed_endpoint_id;
} host_policy_plan;

typedef struct hosted_state hosted_state;
typedef struct hosted_probe {
  hosted_state *host;
  cnet_client *client;
  const char *expected;
  size_t bytes, sent, connected, terminal;
} hosted_probe;
struct hosted_state {
  native_io_sharded *sg;
  size_t owner_shard, owner_count;
  host_policy_plan policy;
  size_t server_policy_calls, client_policy_calls;
  native_io_sharded_host_lease lease;
  native_io_backend *backend;
  cnet_client inbound, outbound;
  cnet_datagram datagrams[2];
  size_t udp_received, udp_sent;
  cnet_listener listener;
  /* Helpers borrow one outbound CNet client on the same SG Owner lane.
   * They do not add a backend, progress thread, poll authority, or queue. */
  cnet_manager manager;
  cnet_managed_dial dial;
  cnet_client_pool pool;
  cnet_pool_key pool_key;
  cnet_pool_connection physical;
  cnet_pool_lease operation_lease;
  size_t ready_published, pool_leases_settled;
  cnet_stream_endpoint local;
  cnet_connection accepted, outgoing;
  hosted_probe in_probe, out_probe;
  const void *owner_thread;
  int status;
  bool accepting, data_started, closing, done, clients_destroyed;
#if defined(__linux__)
  int pipe_read, pipe_write;
  native_io_sharded_endpoint pipe_endpoint;
  native_io_sharded_request pipe_request;
  char pipe_byte;
  size_t sg_terminal, sg_finalize;
#endif
};

/* A foreign shard must not manage another Owner's live CNet helper state.
 * The fixture runs these probes only between completed SG task rounds, so
 * the target Owner cannot be mutating its borrowed helpers concurrently. */
typedef struct host_foreign_managed_probe {
  hosted_state *target;
  size_t caller_shard, observed_shard;
  bool same_thread;
  int manager_snapshot_rc, manager_reserve_rc, manager_seal_rc;
  int dial_snapshot_rc, dial_advance_rc, dial_seal_rc;
  int pool_snapshot_rc, pool_reserve_rc, pool_acquire_rc;
  int pool_drain_rc, pool_seal_rc;
  cnet_managed_connection reserved_managed, acquired_managed;
  cnet_pool_connection reserved_physical;
  cnet_pool_lease acquired_lease;
  uint64_t dial_wait_ms;
} host_foreign_managed_probe;

/* Unsupported/invalid static policy fails fast. Never fall back to another
 * Owner, a different TLS authority, raw CNet or a default strategy. */
static int host_policy_validate(const host_policy_plan *plan) {
  if (plan == NULL || plan->version != HOST_POLICY_VERSION ||
      plan->allowed_endpoint_id == 0u)
    return SALTS_EINVAL;
  switch (plan->server) {
    case CNET_OWNER_PLACE_EXPLICIT:
    case CNET_OWNER_PLACE_ROUND_ROBIN:
    case CNET_OWNER_PLACE_LOWEST_PRESSURE:
    case CNET_OWNER_PLACE_STRICT_KEY: break;
    default: return SALTS_EINVAL;
  }
  switch (plan->client) {
    case CNET_DESTINATION_EXPLICIT:
    case CNET_DESTINATION_ROUND_ROBIN:
    case CNET_DESTINATION_WEIGHTED_RR:
    case CNET_DESTINATION_LEAST_INFLIGHT:
    case CNET_DESTINATION_STRICT_KEY: break;
    default: return SALTS_EINVAL;
  }
  return SALTS_OK;
}

/* Called only at inbound connection admission, not for each packet. The
 * chosen Owner is only advisory; real CNet admission is a separate commit.
 * This fixture accepts locally and rejects foreign choices without a hop. */
static int host_select_server_owner(hosted_state *f, size_t *out_owner) {
  cnet_owner_placement_hint owners[HOST_MAX_OWNERS] = {{0}};
  cnet_owner_placement_input input = {0};
  if (f == NULL || out_owner == NULL || f->owner_count == 0u ||
      f->owner_count > HOST_MAX_OWNERS ||
      f->owner_shard >= f->owner_count ||
      host_policy_validate(&f->policy) != SALTS_OK)
    return SALTS_EINVAL;
  for (size_t i = 0u; i < f->owner_count; ++i) owners[i].eligible = true;
  input.size = sizeof(input);
  input.version = CNET_OWNER_PLACEMENT_VERSION;
  input.kind = f->policy.server;
  input.owners = owners;
  input.owner_count = f->owner_count;
  input.explicit_owner = f->owner_shard;
  input.sequence = (uint64_t)f->owner_shard;
  input.key_hash = (uint64_t)f->owner_shard;
  input.key_known = true;
  ++f->server_policy_calls;
  return cnet_owner_placement_choose(&input, out_owner);
}

/* The caller authorizes a stable, immutable endpoint-set snapshot before
 * this advisory selection. Only an exact selected identity may map to the
 * real loopback listener; a separate CNet operation commits the connection. */
static int host_select_client_destination(hosted_state *f,
                                          cnet_destination_result *out) {
  cnet_destination_hint endpoint = {0};
  cnet_destination_selection selection = {0};
  if (f == NULL || out == NULL ||
      host_policy_validate(&f->policy) != SALTS_OK)
    return SALTS_EINVAL;
  endpoint.endpoint_id = f->policy.allowed_endpoint_id;
  endpoint.weight = 1u;
  endpoint.eligible = true;
  selection.size = sizeof(selection);
  selection.version = CNET_DESTINATION_POLICY_VERSION;
  selection.kind = f->policy.client;
  selection.endpoints = &endpoint;
  selection.endpoint_count = 1u;
  selection.snapshot_generation = 1u;
  selection.expires_at_ms = UINT64_MAX;
  selection.now_ms = cmeta_monotonic_ms();
  selection.sequence = (uint64_t)f->owner_shard;
  selection.explicit_endpoint_id = f->policy.allowed_endpoint_id;
  selection.key_hash = (uint64_t)f->owner_shard;
  selection.key_known = true;
  ++f->client_policy_calls;
  return cnet_destination_choose(&selection, out);
}

static native_io_backend_kind host_kind(void) {
#if defined(_WIN32)
  return NATIVE_IO_BACKEND_IOCP;
#elif defined(__APPLE__)
  return NATIVE_IO_BACKEND_KQUEUE;
#else
  return NATIVE_IO_BACKEND_EPOLL;
#endif
}
static cnet_client_config host_config(void) {
  cnet_client_config c = {0};
  c.backend = host_kind();
  c.connection_capacity = 1u;
  c.command_capacity = 8u;
  /* Each client's configured completion batch must fit its request slots. */
  c.request_capacity = HOST_BATCH;
  c.completion_batch_capacity = HOST_BATCH;
  c.event_capacity = 8u;
  c.max_send_bytes = 1024u;
  c.receive_buffer_bytes = 1024u;
  c.connect_timeout_ms = HOST_TIMEOUT_MS;
  c.read_timeout_ms = HOST_TIMEOUT_MS;
  c.write_timeout_ms = HOST_TIMEOUT_MS;
  return c;
}
static void host_record_error(hosted_state *f, int status) {
  if (f->status == SALTS_OK) f->status = status;
}
static void host_on_state(void *arg, cnet_connection handle,
                          cnet_connection_state state, const cnet_error *error) {
  hosted_probe *p = (hosted_probe *)arg;
  (void)handle; (void)error;
  if (cmeta_thread_current_token() != p->host->owner_thread) {
    host_record_error(p->host, SALTS_EPERM);
    return;
  }
  if (state == CNET_CONNECTION_CONNECTED) ++p->connected;
  if (state == CNET_CONNECTION_FAILED) host_record_error(p->host, SALTS_ECONNRESET);
  if (state == CNET_CONNECTION_FAILED || state == CNET_CONNECTION_CLOSED) ++p->terminal;
}
static void host_on_receive(void *arg, cnet_connection handle, const cnet_receive_view *view) {
  hosted_probe *p = (hosted_probe *)arg;
  size_t size = strlen(p->expected);
  if (cmeta_thread_current_token() != p->host->owner_thread || view == NULL ||
      p->bytes > size || view->size > size - p->bytes ||
      memcmp(view->data, p->expected + p->bytes, view->size) != 0) {
    host_record_error(p->host, SALTS_EPROTO);
    return;
  }
  p->bytes += view->size;
  if (p->bytes != size) {
    int status = cnet_receive(p->client, handle, 1u);
    if (status != SALTS_OK) host_record_error(p->host, status);
  }
}
static void host_on_send(void *arg, cnet_connection handle, size_t size) {
  hosted_probe *p = (hosted_probe *)arg;
  (void)handle;
  if (cmeta_thread_current_token() != p->host->owner_thread)
    host_record_error(p->host, SALTS_EPERM);
  p->sent += size;
}
static cnet_observer host_observer(hosted_probe *p) {
  cnet_observer o = {0};
  o.on_state = host_on_state;
  o.on_receive = host_on_receive;
  o.on_send = host_on_send;
  o.user = p;
  return o;
}
static bool host_quiescent(void *arg) {
  hosted_state *f = (hosted_state *)arg;
  return f->clients_destroyed && !f->listener.impl &&
         !f->inbound.impl && !f->outbound.impl &&
         !f->manager.impl && !f->dial.impl && !f->pool.impl;
}
#if defined(__linux__)
static void host_pipe_terminal(native_io_sharded_context *context,
                               const native_io_sharded_completion *event, void *arg) {
  hosted_state *f = (hosted_state *)arg;
  if (native_io_sharded_context_shard(context) != f->owner_shard ||
      event->status != SALTS_OK || event->bytes != 1u || f->pipe_byte != 'X')
    host_record_error(f, SALTS_EPROTO);
  ++f->sg_terminal;
}
static void host_pipe_finalize(void *arg) {
  ++((hosted_state *)arg)->sg_finalize;
}
#endif
#define HOST_TASK_OK(f, expr) do { int host_rc = (expr); \
  if (host_rc != SALTS_OK) { host_record_error((f), host_rc); return; } \
} while (0)


/* Owner-local connection admission only. The configured policy previously
 * selected one authorized endpoint; this helper performs the real bounded
 * Pool, Manager, and ManagedDial reservations against that same identity. */
static int host_start_managed_client(hosted_state *f,
                                     const cnet_observer *observer) {
  const cnet_manager_config manager_config = {
    sizeof(cnet_manager_config), CNET_MANAGER_VERSION, &f->outbound, 1u, 1u
  };
  const cnet_pool_config pool_config = {
    sizeof(cnet_pool_config), CNET_CLIENT_POOL_VERSION,
    &f->manager, (uint64_t)f->owner_shard + 1u, 1u, 1u, 1u
  };
  cnet_managed_dial_config cfg = {0};
  cnet_managed_dial_snapshot snap = {0};
  cnet_pool_connection refused = {0};
  uint64_t next_wait = 0u;
  char uri[96];
  int n, rc;

  n = snprintf(uri, sizeof(uri), "tcp://127.0.0.1:%u",
               (unsigned)f->local.port);
  if (n <= 0 || (size_t)n >= sizeof(uri)) return SALTS_EINVAL;
  rc = cnet_manager_init(&f->manager, &manager_config);
  if (rc != SALTS_OK) return rc;
  rc = cnet_pool_init(&f->pool, &pool_config);
  if (rc != SALTS_OK) return rc;

  f->pool_key = (cnet_pool_key){0};
  f->pool_key.size = sizeof(f->pool_key);
  f->pool_key.version = CNET_CLIENT_POOL_VERSION;
  f->pool_key.runtime_id = (uint64_t)f->owner_shard + 1u;
  f->pool_key.owner_id = (uint64_t)f->owner_shard + 1u;
  f->pool_key.endpoint_id = f->policy.allowed_endpoint_id;
  f->pool_key.authority_id = f->policy.allowed_endpoint_id;
  f->pool_key.transport_id = 1u;
  f->pool_key.protocol_id = 1u;
  rc = cnet_pool_reserve_connecting(&f->pool, &f->pool_key,
                                    &f->physical);
  if (rc != SALTS_OK) return rc;
  if (cnet_pool_reserve_connecting(&f->pool, &f->pool_key,
                                   &refused) != SALTS_ENOBUFS ||
      refused.slot != 0u) return SALTS_EPROTO;

  cfg.size = sizeof(cfg);
  cfg.version = CNET_MANAGED_DIAL_VERSION;
  cfg.manager = &f->manager;
  cfg.client = &f->outbound;
  cfg.connection.uri = uri; /* ManagedDial copies URI into its own storage. */
  cfg.connection.observer = *observer;
  cfg.recovery = (cnet_reconnect_config){
    sizeof(cnet_reconnect_config), CNET_RECOVERY_POLICY_VERSION,
    1u, cmeta_monotonic_ms() + HOST_TIMEOUT_MS, 0u, 0u, 17u
  };
  cfg.recovery_episode_ms = HOST_TIMEOUT_MS;
  /* An unspecified classifier is permanent failure, never DATA replay. */
  rc = cnet_managed_dial_init(&f->dial, &cfg);
  if (rc != SALTS_OK) return rc;
  rc = cnet_managed_dial_advance(&f->dial, cmeta_monotonic_ms(),
                                 &next_wait);
  if (rc != SALTS_OK) return rc;
  rc = cnet_managed_dial_get_snapshot(&f->dial, &snap);
  if (rc != SALTS_OK) return rc;
  if (snap.connection.slot == 0u || snap.managed.slot == 0u ||
      snap.recovery_ticket.generation == 0u) return SALTS_EPROTO;
  /* A valid attempt ticket exists while CONNECTING, but may not yet assert
   * protocol READY: the actual transport callback + peer exchange are pending. */
  if (cnet_managed_dial_protocol_ready(
        &f->dial, snap.recovery_ticket, cmeta_monotonic_ms()) != SALTS_EBUSY)
    return SALTS_EPROTO;
  f->outgoing = snap.connection;
  return SALTS_OK;
}

/* Explicit protocol READY follows the actual bidirectional application
 * exchange. CONNECTED by itself must not authorize Pool reuse or reset
 * ManagedDial's recovery episode. */
static int host_confirm_protocol_ready(hosted_state *f) {
  cnet_managed_dial_snapshot snap = {0};
  cnet_reconnect_ticket stale = {0};
  cnet_pool_key wrong_authority = f->pool_key;
  cnet_pool_lease rejected = {0};
  cnet_managed_connection resolved = {0};
  int rc = cnet_managed_dial_get_snapshot(&f->dial, &snap);
  if (rc != SALTS_OK) return rc;
  if (!snap.recovery.awaiting_protocol ||
      snap.recovery.protocol_ready || snap.managed.slot == 0u ||
      snap.recovery_ticket.generation == 0u) return SALTS_EPROTO;
  stale = snap.recovery_ticket;
  ++stale.generation;
  if (cnet_managed_dial_protocol_ready(
        &f->dial, stale, cmeta_monotonic_ms()) != SALTS_ENOENT)
    return SALTS_EPROTO;
  rc = cnet_managed_dial_protocol_ready(
    &f->dial, snap.recovery_ticket, cmeta_monotonic_ms());
  if (rc != SALTS_OK) return rc;
  if (cnet_managed_dial_protocol_ready(
        &f->dial, snap.recovery_ticket,
        cmeta_monotonic_ms()) != SALTS_EALREADY)
    return SALTS_EPROTO;
  ++f->ready_published;

  rc = cnet_pool_bind_ready(&f->pool, f->physical, snap.managed, 1u);
  if (rc != SALTS_OK) return rc;
  wrong_authority.authority_id++;
  if (cnet_pool_try_acquire(&f->pool, &wrong_authority, NULL,
                            &rejected, &resolved) != SALTS_ENOBUFS ||
      rejected.slot != 0u) return SALTS_EPROTO;
  rc = cnet_pool_try_acquire(&f->pool, &f->pool_key, NULL,
                             &f->operation_lease, &resolved);
  if (rc != SALTS_OK) return rc;
  if (resolved.manager != snap.managed.manager ||
      resolved.incarnation != snap.managed.incarnation ||
      resolved.generation != snap.managed.generation ||
      resolved.slot != snap.managed.slot) return SALTS_EPROTO;
  if (cnet_pool_try_acquire(&f->pool, &f->pool_key, NULL,
                            &rejected, &resolved) != SALTS_ENOBUFS)
    return SALTS_EPROTO;
  /* App-level terminal assertions cannot overrule the real Manager state. */
  if (cnet_pool_terminal(&f->pool, f->physical) != SALTS_EBUSY)
    return SALTS_EPROTO;
  rc = cnet_pool_begin_drain(&f->pool, f->physical);
  if (rc != SALTS_OK) return rc;
  rc = cnet_pool_seal(&f->pool);
  if (rc != SALTS_OK) return rc;
  if (cnet_pool_try_acquire(&f->pool, &f->pool_key, NULL,
                            &rejected, &resolved) != SALTS_ESHUTDOWN)
    return SALTS_EPROTO;
  return SALTS_OK;
}

/* Release Pool only after the real transport is retired by Manager and
 * ManagedDial's recycle callback has revoked its generation-scoped record. */
static int host_finish_managed_client(hosted_state *f) {
  cnet_manager_snapshot manager_state = {0};
  cnet_pool_snapshot pool_state = {0};
  int rc = cnet_pool_terminal(&f->pool, f->physical);
  if (rc != SALTS_OK) return rc;
  rc = cnet_pool_get_snapshot(&f->pool, &pool_state);
  if (rc != SALTS_OK) return rc;
  if (pool_state.active_leases != 1u ||
      pool_state.terminal_waiting_for_leases != 1u ||
      cnet_pool_destroy(&f->pool) != SALTS_EBUSY) return SALTS_EPROTO;
  rc = cnet_pool_release(&f->pool, f->operation_lease);
  if (rc != SALTS_OK) return rc;
  if (cnet_pool_release(&f->pool, f->operation_lease) != SALTS_ENOENT)
    return SALTS_EPROTO;
  ++f->pool_leases_settled;
  rc = cnet_pool_get_snapshot(&f->pool, &pool_state);
  if (rc != SALTS_OK) return rc;
  if (!pool_state.drained || pool_state.active_leases != 0u)
    return SALTS_EPROTO;
  rc = cnet_pool_destroy(&f->pool);
  if (rc != SALTS_OK) return rc;
  rc = cnet_managed_dial_destroy(&f->dial);
  if (rc != SALTS_OK) return rc;
  rc = cnet_manager_get_snapshot(&f->manager, &manager_state);
  if (rc != SALTS_OK) return rc;
  if (!manager_state.drained ||
      manager_state.reserved != 0u ||
      manager_state.bound != 0u ||
      manager_state.retired != 0u) return SALTS_EPROTO;
  return SALTS_OK;
}

static void host_udp_received(void *user, cnet_datagram *datagram,
    const cnet_datagram_peer *peer, const cnet_receive_view *view) {
  hosted_state *f = (hosted_state *)user;
  (void)datagram; (void)peer;
  if (f->owner_thread != cmeta_thread_current_token() || view->size != 3u ||
      memcmp(view->data, "udp", 3u) != 0) host_record_error(f, SALTS_EPROTO);
  ++f->udp_received;
}
static void host_udp_sent(void *user, cnet_datagram *datagram,
    const cnet_datagram_peer *peer, size_t size, int status, uint64_t tag) {
  hosted_state *f = (hosted_state *)user;
  (void)datagram; (void)peer;
  if (f->owner_thread != cmeta_thread_current_token() || size != 3u ||
      status != SALTS_OK || tag != 1u) host_record_error(f, SALTS_EPROTO);
  ++f->udp_sent;
}
static void host_initialize(native_io_sharded_context *context, void *arg) {
  hosted_state *f = (hosted_state *)arg;
  cnet_client_config network = host_config();
  cnet_observer out = host_observer(&f->out_probe);
  cnet_stream_endpoint bind = CNET_STREAM_ENDPOINT_INIT;
  native_io_request accepted = {0};
  if (native_io_sharded_context_shard(context) != f->owner_shard) {
    host_record_error(f, SALTS_EPERM);
    return;
  }
  f->owner_thread = cmeta_thread_current_token();
  HOST_TASK_OK(f, host_policy_validate(&f->policy));
  f->in_probe.host = f; f->in_probe.client = &f->inbound;
  f->in_probe.expected = "client";
  f->out_probe.host = f; f->out_probe.client = &f->outbound;
  f->out_probe.expected = "server";

  HOST_TASK_OK(f, native_io_sharded_context_acquire_host(
      context, host_quiescent, f, &f->lease, &f->backend));
  HOST_TASK_OK(f, cnet_client_init_external(&f->inbound, &network, f->backend));
  HOST_TASK_OK(f, cnet_client_init_external(&f->outbound, &network, f->backend));
  for (size_t i = 0u; i < 2u; ++i) {
    cnet_datagram_config udp = CNET_DATAGRAM_CONFIG_INIT;
    udp.backend = host_kind(); udp.host = "127.0.0.1";
    udp.send_capacity = 2u; udp.request_capacity = 3u; udp.completion_batch_capacity = 3u;
    udp.max_datagram_bytes = 32u; udp.receive_buffer_bytes = 32u;
    udp.observer = (cnet_datagram_observer){host_udp_received, host_udp_sent, f};
    HOST_TASK_OK(f, cnet_datagram_init_external(&f->datagrams[i], &udp, f->backend));
  }
  bind.family = CNET_DATAGRAM_ADDRESS_IPV4;
  bind.address[0] = 127u; bind.address[3] = 1u;
  HOST_TASK_OK(f, cnet_listener_open(&f->listener, host_kind(),
                                     CNET_DATAGRAM_ADDRESS_IPV4));
  HOST_TASK_OK(f, cnet_listener_bind_open_endpoint(&f->listener, &bind));
  HOST_TASK_OK(f, cnet_listener_local_endpoint(&f->listener, &f->local));
  {
    cnet_destination_result selected = {0};
    HOST_TASK_OK(f, host_select_client_destination(f, &selected));
    if (selected.endpoint_id != f->policy.allowed_endpoint_id ||
        selected.snapshot_generation != 1u || selected.index != 0u) {
      host_record_error(f, SALTS_EPROTO);
      return;
    }
  }
  HOST_TASK_OK(f, cnet_listener_listen(&f->listener, 8u));
  HOST_TASK_OK(f, cnet_listener_attach_external(&f->listener, f->backend));
  HOST_TASK_OK(f, cnet_listener_submit_external_accept(&f->listener, &accepted));
  f->accepting = true;
#if defined(__linux__)
  {
    int handles[2] = {-1, -1};
    native_io_sharded_operation op = {0};
    native_io_sharded_ownership ownership = {
        host_pipe_terminal, host_pipe_finalize, f};
    if (pipe(handles) != 0) { host_record_error(f, SALTS_EIO); return; }
    f->pipe_read = handles[0]; f->pipe_write = handles[1];
    {
      int flags = fcntl(f->pipe_read, F_GETFL, 0);
      if (flags < 0 || fcntl(f->pipe_read, F_SETFL, flags | O_NONBLOCK) != 0) {
        host_record_error(f, SALTS_EIO); return;
      }
    }
    HOST_TASK_OK(f, native_io_sharded_context_attach_pipe(
        context, (uintptr_t)f->pipe_read, NATIVE_IO_PIPE_ENDPOINT_ASYNC_CAPABLE,
        &f->pipe_endpoint));
    op.kind = NATIVE_IO_OPERATION_PIPE_READ;
    op.endpoint = f->pipe_endpoint;
    op.buffer = &f->pipe_byte;
    op.length = 1u;
    HOST_TASK_OK(f, native_io_sharded_context_submit_owned(
        context, &op, &ownership, &f->pipe_request));
    if (write(f->pipe_write, "X", 1u) != 1) {
      host_record_error(f, SALTS_EIO); return;
    }
  }
#endif
  HOST_TASK_OK(f, host_start_managed_client(f, &out));
}

static void host_send(hosted_state *f, cnet_client *client,
                      cnet_connection connection, const char *payload, size_t size) {
  mem_buffer_t *buffer = mem_get_buffer(mem_global(), size);
  int status;
  if (buffer == NULL) { host_record_error(f, SALTS_ENOMEM); return; }
  memcpy(mem_buffer_data(buffer), payload, size);
  mem_set_used(buffer, size);
  status = cnet_send_buffer(client, connection, buffer);
  mem_buffer_release(buffer);
  if (status != SALTS_OK) host_record_error(f, status);
}
static void host_progress(native_io_sharded_context *context, void *arg) {
  hosted_state *f = (hosted_state *)arg;
  native_io_sharded_completion observed[HOST_BATCH];
  size_t count = 0u, events = 0u;
  int status;
  if (f->status != SALTS_OK || f->done) return;
  if (native_io_sharded_context_shard(context) != f->owner_shard) {
    host_record_error(f, SALTS_EPERM);
    return;
  }
  HOST_TASK_OK(f, cnet_client_advance_external(&f->inbound, &events));
  HOST_TASK_OK(f, cnet_client_advance_external(&f->outbound, &events));
  for (size_t i = 0u; i < 2u; ++i)
    HOST_TASK_OK(f, cnet_datagram_advance_external(&f->datagrams[i], &events));
  status = native_io_sharded_context_observe_host(
      context, f->lease, observed, HOST_BATCH, 0u, &count);
  if (status != SALTS_OK && status != SALTS_ETIMEDOUT) {
    host_record_error(f, status); return;
  }
  {
    cnet_client *clients[2] = {&f->inbound, &f->outbound};
    cnet_sg_host_routes routes = {
        sizeof(routes), CNET_SG_HOST_ROUTING_VERSION, &f->listener, clients, 2u};
    size_t accepts = 0u, sg_completed = 0u;
    cnet_datagram *datagrams[2] = {&f->datagrams[1], &f->datagrams[0]};
    status = cnet_sg_host_route_batch_with_datagrams(
        observed, count, &routes, datagrams, 2u, &accepts, &sg_completed);
    if (status != SALTS_OK) { host_record_error(f, status); return; }
    if (accepts != 0u && f->accepting) {
      size_t chosen_owner = SIZE_MAX;
      cnet_observer in = host_observer(&f->in_probe);
      HOST_TASK_OK(f, host_select_server_owner(f, &chosen_owner));
      if (chosen_owner != f->owner_shard) {
        host_record_error(f, SALTS_EPERM);
        return;
      }
      HOST_TASK_OK(f, cnet_listener_accept(
          &f->listener, &f->inbound, &in, &f->accepted));
      f->accepting = false;
    }
  }
  HOST_TASK_OK(f, cnet_client_advance_external(&f->inbound, &events));
  HOST_TASK_OK(f, cnet_client_advance_external(&f->outbound, &events));
  {
    size_t managed_work = 0u;
    HOST_TASK_OK(f, cnet_manager_advance(&f->manager, 1u, &managed_work));
  }
  if (!f->data_started && f->in_probe.connected && f->out_probe.connected) {
    f->data_started = true;
    HOST_TASK_OK(f, cnet_receive(&f->inbound, f->accepted, 1u));
    HOST_TASK_OK(f, cnet_receive(&f->outbound, f->outgoing, 1u));
    host_send(f, &f->outbound, f->outgoing, "client", 6u);
    host_send(f, &f->inbound, f->accepted, "server", 6u);
    cnet_datagram_peer peer = {0};
    peer.family = CNET_DATAGRAM_ADDRESS_IPV4; peer.address[0] = 127u; peer.address[3] = 1u;
    HOST_TASK_OK(f, cnet_datagram_port(&f->datagrams[1], &peer.port));
    HOST_TASK_OK(f, cnet_datagram_receive(&f->datagrams[1], 1u));
    HOST_TASK_OK(f, cnet_datagram_send(&f->datagrams[0], &peer, "udp", 3u, 1u));
    if (f->status != SALTS_OK) return;
  }
  if (!f->closing && f->data_started &&
      f->in_probe.bytes == 6u && f->out_probe.bytes == 6u &&
      f->in_probe.sent == 6u && f->out_probe.sent == 6u &&
      f->udp_received == 1u && f->udp_sent == 1u
#if defined(__linux__)
      && f->sg_terminal == 1u && f->sg_finalize == 1u
#endif
      ) {
    HOST_TASK_OK(f, host_confirm_protocol_ready(f));
    f->closing = true;
    HOST_TASK_OK(f, cnet_listener_close(&f->listener));
    HOST_TASK_OK(f, cnet_listener_destroy(&f->listener));
    HOST_TASK_OK(f, cnet_close(&f->inbound, f->accepted));
    HOST_TASK_OK(f, cnet_managed_dial_seal(&f->dial));
  }
  if (f->closing && f->in_probe.terminal && f->out_probe.terminal) {
    cnet_managed_dial_snapshot dial_state = {0};
    HOST_TASK_OK(f, cnet_managed_dial_get_snapshot(&f->dial, &dial_state));
    /* CLOSED callback is not on_recycle: wait for real Manager settlement
     * on the same SG Owner, without spinning or a second progress task. */
    if (dial_state.managed.slot != 0u) return;
    HOST_TASK_OK(f, host_finish_managed_client(f));
    for (size_t i = 0u; i < 2u; ++i) {
      bool quiet = false;
      HOST_TASK_OK(f, cnet_datagram_stop_external(&f->datagrams[i], &quiet));
      if (!quiet) { host_record_error(f, SALTS_EBUSY); return; }
      HOST_TASK_OK(f, cnet_datagram_destroy(&f->datagrams[i]));
    }
    HOST_TASK_OK(f, cnet_client_stop_external(&f->inbound));
    HOST_TASK_OK(f, cnet_client_stop_external(&f->outbound));
    HOST_TASK_OK(f, cnet_manager_destroy(&f->manager));
    HOST_TASK_OK(f, cnet_client_destroy(&f->inbound));
    HOST_TASK_OK(f, cnet_client_destroy(&f->outbound));
#if defined(__linux__)
    HOST_TASK_OK(f, native_io_sharded_context_release_pipe(
        context, f->pipe_endpoint));
    (void)close(f->pipe_read); (void)close(f->pipe_write);
    f->pipe_read = -1; f->pipe_write = -1;
#endif
    f->clients_destroyed = true;
    HOST_TASK_OK(f, native_io_sharded_context_release_host(context, f->lease));
    f->done = true;
  }
}

static void host_foreign_managed_check(native_io_sharded_context *context, void *arg) {
  host_foreign_managed_probe *probe = (host_foreign_managed_probe *)arg;
  hosted_state *f = probe->target;
  cnet_manager_snapshot manager = {0};
  cnet_managed_dial_snapshot dial = {0};
  cnet_pool_snapshot pool = {0};
  const cnet_manager_attachment attachment = {
      .observer = host_observer(&f->out_probe)
  };
  probe->observed_shard = native_io_sharded_context_shard(context);
  probe->same_thread = f->owner_thread == cmeta_thread_current_token();
  probe->manager_snapshot_rc = cnet_manager_get_snapshot(&f->manager, &manager);
  probe->dial_snapshot_rc = cnet_managed_dial_get_snapshot(&f->dial, &dial);
  probe->pool_snapshot_rc = cnet_pool_get_snapshot(&f->pool, &pool);
  probe->manager_reserve_rc = cnet_manager_reserve(
      &f->manager, &attachment, &probe->reserved_managed);
  probe->pool_reserve_rc = cnet_pool_reserve_connecting(
      &f->pool, &f->pool_key, &probe->reserved_physical);
  probe->pool_acquire_rc = cnet_pool_try_acquire(
      &f->pool, &f->pool_key, NULL, &probe->acquired_lease,
      &probe->acquired_managed);
  probe->dial_advance_rc = cnet_managed_dial_advance(
      &f->dial, cmeta_monotonic_ms(), &probe->dial_wait_ms);
  probe->pool_drain_rc = cnet_pool_begin_drain(&f->pool, f->physical);
  probe->manager_seal_rc = cnet_manager_seal(&f->manager);
  probe->dial_seal_rc = cnet_managed_dial_seal(&f->dial);
  probe->pool_seal_rc = cnet_pool_seal(&f->pool);
}

/*
 * Drive each Owner with a bounded routed task. Only its Owner sees the CNet
 * listener, clients, endpoint and completion records. Waiting for the finite
 * task round is a test barrier, not a forever-running executor task.
 * Three topologies exercise actual inbound/outbound TCP + SG-owned I/O on
 * 1/2/4 shards, without a second NativeIO observe or cross-Owner migration.
 */
static void host_run_topology(size_t owner_count) {
  hosted_state hosts[HOST_MAX_OWNERS] = {0};
  native_io_sharded *sg = NULL;
  native_io_sharded_task initialize[HOST_MAX_OWNERS] = {{0}};
  native_io_sharded_task progress[HOST_MAX_OWNERS] = {{0}};
  native_io_sharded_task foreign_tasks[HOST_MAX_OWNERS] = {{0}};
  host_foreign_managed_probe foreign[HOST_MAX_OWNERS] = {{0}};
  const native_io_sharded_config config = {
      owner_count, 8u, {host_kind(), 24u, 48u, HOST_BATCH}};
  const uint64_t deadline = cmeta_monotonic_ms() + HOST_TIMEOUT_MS;
  bool all_done = false;

  check(owner_count > 0u && owner_count <= HOST_MAX_OWNERS);
  check_equal(native_io_sharded_create(&config, &sg), SALTS_OK);
  check_not_null(sg);

  for (size_t i = 0u; i < owner_count; ++i) {
    hosts[i].sg = sg;
    hosts[i].owner_shard = i;
    hosts[i].owner_count = owner_count;
    /* Explicit host configuration: no dynamic registry or per-packet choice. */
    hosts[i].policy = (host_policy_plan){
      HOST_POLICY_VERSION,
      i % 2u ? CNET_OWNER_PLACE_EXPLICIT : CNET_OWNER_PLACE_STRICT_KEY,
      i % 2u ? CNET_DESTINATION_EXPLICIT : CNET_DESTINATION_STRICT_KEY,
      (uint64_t)i + 1u
    };
    initialize[i] = (native_io_sharded_task){host_initialize, NULL, NULL, &hosts[i]};
    progress[i] = (native_io_sharded_task){host_progress, NULL, NULL, &hosts[i]};
    check_equal(native_io_sharded_submit_to(sg, i, &initialize[i]), SALTS_OK);
  }
  check_equal(native_io_sharded_wait(sg), SALTS_OK);
  for (size_t i = 0u; i < owner_count; ++i) {
    check_equal(hosts[i].status, SALTS_OK);
    check_equal(hosts[i].lease.owner_shard, (uint32_t)i);
    for (size_t j = 0u; j < i; ++j)
      check(hosts[i].backend != hosts[j].backend);
  }

  /* All home-Owner initialization is complete, and none of their progress
   * tasks is active. Submit one *other-Owner* task per live Managed set, then
   * wait at the SG task barrier before inspecting results on this test thread.
   * Unauthorized reads/mutations must fail without leaking credits or changing
   * the Manager/Dial/Pool states needed by the following real TCP exchange. */
  if (owner_count > 1u) {
    for (size_t i = 0u; i < owner_count; ++i) {
      foreign[i].target = &hosts[i];
      foreign[i].caller_shard = (i + 1u) % owner_count;
      foreign_tasks[i] = (native_io_sharded_task){
          host_foreign_managed_check, NULL, NULL, &foreign[i]};
      check_equal(native_io_sharded_submit_to(
          sg, foreign[i].caller_shard, &foreign_tasks[i]), SALTS_OK);
    }
    check_equal(native_io_sharded_wait(sg), SALTS_OK);
    for (size_t i = 0u; i < owner_count; ++i) {
      const host_foreign_managed_probe *p = &foreign[i];
      check_equal(p->observed_shard, p->caller_shard);
      check(!p->same_thread);
      check_equal(p->manager_snapshot_rc, SALTS_EPERM);
      check_equal(p->manager_reserve_rc, SALTS_EPERM);
      check_equal(p->manager_seal_rc, SALTS_EPERM);
      check_equal(p->dial_snapshot_rc, SALTS_EPERM);
      check_equal(p->dial_advance_rc, SALTS_EPERM);
      check_equal(p->dial_seal_rc, SALTS_EPERM);
      check_equal(p->pool_snapshot_rc, SALTS_EPERM);
      check_equal(p->pool_reserve_rc, SALTS_EPERM);
      check_equal(p->pool_acquire_rc, SALTS_EPERM);
      check_equal(p->pool_drain_rc, SALTS_EPERM);
      check_equal(p->pool_seal_rc, SALTS_EPERM);
      check_equal(p->reserved_managed.slot, (size_t)0u);
      check_equal(p->reserved_physical.slot, (size_t)0u);
      check_equal(p->acquired_managed.slot, (size_t)0u);
      check_equal(p->acquired_lease.slot, (size_t)0u);
      check_equal(p->dial_wait_ms, UINT64_C(0));
      check_equal(hosts[i].status, SALTS_OK);
    }
  }

  while (!all_done && cmeta_monotonic_ms() < deadline) {
    all_done = true;
    for (size_t i = 0u; i < owner_count; ++i) {
      check_equal(hosts[i].status, SALTS_OK);
      if (hosts[i].done) continue;
      all_done = false;
      check_equal(native_io_sharded_submit_to(sg, i, &progress[i]), SALTS_OK);
    }
    if (all_done) break;
    check_equal(native_io_sharded_wait(sg), SALTS_OK);
    cmeta_sleep_ms(1u);
  }

  for (size_t i = 0u; i < owner_count; ++i) {
    hosted_state *f = &hosts[i];
    check_equal(f->status, SALTS_OK);
    check(f->done);
    check_equal(f->in_probe.bytes, (size_t)6u);
    check_equal(f->out_probe.bytes, (size_t)6u);
    check_equal(f->in_probe.sent, (size_t)6u);
    check_equal(f->out_probe.sent, (size_t)6u);
    check_equal(f->in_probe.connected, (size_t)1u);
    check_equal(f->out_probe.connected, (size_t)1u);
    check_equal(f->in_probe.terminal, (size_t)1u);
    check_equal(f->out_probe.terminal, (size_t)1u);
    check_equal(f->ready_published, (size_t)1u);
    check_equal(f->pool_leases_settled, (size_t)1u);
    check_equal(f->udp_received, (size_t)1u);
    check_equal(f->udp_sent, (size_t)1u);
    check_null(f->manager.impl);
    check_null(f->dial.impl);
    check_null(f->pool.impl);
    /* One decision per admitted physical connection, never per I/O event. */
    check_equal(f->server_policy_calls, (size_t)1u);
    check_equal(f->client_policy_calls, (size_t)1u);
#if defined(__linux__)
    check_equal(f->sg_terminal, (size_t)1u);
    check_equal(f->sg_finalize, (size_t)1u);
#endif
  }
  if (all_done) {
    check_equal(native_io_sharded_shutdown(sg), SALTS_OK);
    check_equal(native_io_sharded_destroy(sg), SALTS_OK);
  }
}

spec("CNet native SG leased host with real TCP + SG-owned completions") {
  it("rejects invalid static policy plans instead of falling back") {
    host_policy_plan plan = {
      HOST_POLICY_VERSION, CNET_OWNER_PLACE_EXPLICIT,
      CNET_DESTINATION_EXPLICIT, UINT64_C(17)
    };
    check_equal(host_policy_validate(&plan), SALTS_OK);
    plan.version = 0u;
    check_equal(host_policy_validate(&plan), SALTS_EINVAL);
    plan.version = HOST_POLICY_VERSION;
    plan.server = (cnet_owner_placement_kind)0;
    check_equal(host_policy_validate(&plan), SALTS_EINVAL);
    plan.server = CNET_OWNER_PLACE_EXPLICIT;
    plan.client = (cnet_destination_policy_kind)0;
    check_equal(host_policy_validate(&plan), SALTS_EINVAL);
    plan.client = CNET_DESTINATION_EXPLICIT;
    plan.allowed_endpoint_id = 0u;
    check_equal(host_policy_validate(&plan), SALTS_EINVAL);
  }
  it("co-drives one SG Owner lane") {
    host_run_topology(1u);
  }
  it("co-drives independent inbound/outbound CNet and SG I/O on two Owner shards") {
    host_run_topology(2u);
  }
  it("co-drives independent inbound/outbound CNet and SG I/O on four Owner shards") {
    host_run_topology(4u);
  }
}
