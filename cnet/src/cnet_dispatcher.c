#include "cnet_dispatcher.h"

#include <salts/clock.h>
#include <salts/thread.h>

#include <stdatomic.h>
#include <stdbool.h>
#if defined(CNET_VSOCK_STOP_DIAGNOSTIC)
#include <stdio.h>
#endif
#include <stdlib.h>
#include <string.h>

typedef struct cnet_dispatcher_impl cnet_dispatcher_impl;
typedef int (*cnet_dispatch_release_fn)(void *context, const cnet_dispatch_view *view,
                                        uint64_t token);

typedef struct cnet_dispatch_job {
  cnet_dispatch_fn invoke;
  void *context;
  cnet_event event;
  cnet_dispatch_release_fn release;
  void *release_context;
  uint64_t release_token;
} cnet_dispatch_job;

typedef struct cnet_dispatch_entry {
  cnet_dispatcher_impl *dispatcher;
  cnet_shard_connection connection;
  cnet_dispatch_fn observer;
  void *observer_context;
  bool active;
  bool close_requested;
} cnet_dispatch_entry;

typedef struct cnet_dispatch_lane {
  atomic_flag driving;
  atomic_bool pending;
  cnet_event_view event;
} cnet_dispatch_lane;

struct cnet_dispatcher_impl {
  cnet_shards *shards;
  cnet_dispatch_entry *entries;
  cnet_dispatch_lane *lanes;
  size_t shard_count;
  size_t connection_capacity_per_shard;
  size_t active_count;
  atomic_int first_error;
  bool admission_open;
  bool drained;
#if defined(CNET_INTERNAL_PROFILING)
  cnet_dispatcher_profile profile;
  bool profile_active;
#endif
};

static cnet_dispatcher_impl *cnet_dispatcher_get(cnet_dispatcher *dispatcher) {
  return dispatcher != NULL ? (cnet_dispatcher_impl *)dispatcher->impl : NULL;
}

#if defined(CNET_INTERNAL_PROFILING)
static uint64_t cnet_dispatcher_profile_start(const cnet_dispatcher_impl *impl) {
  return impl->profile_active ? cmeta_hrtime() : 0u;
}

static void cnet_dispatcher_profile_finish(cnet_dispatcher_impl *impl, uint64_t started,
                                           uint64_t *total_ns, uint64_t *calls) {
  if (started == 0u) return;
  *total_ns += cmeta_hrtime() - started;
  ++*calls;
}
#endif

static cnet_dispatch_entry *cnet_dispatcher_entry(cnet_dispatcher_impl *impl,
                                                  cnet_shard_connection connection) {
  size_t index;
  if (impl == NULL || !cnet_shard_connection_valid(connection) ||
      (size_t)connection.shard >= impl->shard_count || connection.session.slot == 0u ||
      (size_t)connection.session.slot > impl->connection_capacity_per_shard)
    return NULL;
  index = (size_t)connection.shard * impl->connection_capacity_per_shard +
          (size_t)connection.session.slot - 1u;
  return &impl->entries[index];
}

static bool cnet_dispatcher_terminal_state(cnet_event_state state) {
  return state == CNET_EVENT_STATE_CLOSED || state == CNET_EVENT_STATE_FAILED;
}

static void cnet_dispatcher_record_error(cnet_dispatcher_impl *impl, int status) {
  int expected = SALTS_OK;
  if (status == SALTS_OK) return;
  (void)atomic_compare_exchange_strong_explicit(&impl->first_error, &expected, status,
                                                memory_order_acq_rel, memory_order_acquire);
}

static int cnet_dispatcher_recycle(cnet_dispatch_entry *entry, const cnet_dispatch_view *view) {
  cnet_dispatcher_impl *impl = entry->dispatcher;
  cnet_session_terminal terminal = {0};
  bool recycled = false;
  int status;

  status = cnet_shards_recycle(impl->shards, entry->connection, &terminal);
  if (status == SALTS_OK) {
    recycled = true;
    const bool valid_closed = view->state == CNET_EVENT_STATE_CLOSED &&
                              terminal.kind == CNET_SESSION_TERMINAL_CLOSED &&
                              terminal.status == SALTS_OK;
    const bool valid_failed = view->state == CNET_EVENT_STATE_FAILED &&
                              terminal.kind == CNET_SESSION_TERMINAL_FAILED &&
                              terminal.status == view->status && terminal.stage == view->stage;
    if (!valid_closed && !valid_failed) status = SALTS_EPROTO;
  }

  if (recycled) {
    if (entry->active && entry->connection.session.slot == view->session.slot &&
        entry->connection.session.generation == view->session.generation) {
      entry->active = false;
      entry->close_requested = false;
      entry->observer = NULL;
      entry->observer_context = NULL;
      if (impl->active_count == 0u) status = SALTS_EPROTO;
      else --impl->active_count;
    } else if (status == SALTS_OK) {
      status = SALTS_EPROTO;
    }
  }
  cnet_dispatcher_record_error(impl, status);
  return status;
}

static int cnet_dispatcher_release_lease(void *context, const cnet_dispatch_view *view,
                                         uint64_t token) {
  cnet_dispatch_entry *entry = (cnet_dispatch_entry *)context;
  cnet_dispatcher_impl *impl;
  cnet_event_view event;
  bool terminal_event;
  int status;

  if (entry == NULL || view == NULL || token == 0u) return SALTS_EINVAL;
  impl = entry->dispatcher;
  if (impl == NULL) return SALTS_EINVAL;
  event = (cnet_event_view){view->kind, view->session, view->state,    view->status, view->stage,
                            view->data, view->size,    view->argument, token, view->backing};
  terminal_event = view->kind == CNET_EVENT_STATE && cnet_dispatcher_terminal_state(view->state);
  status = cnet_shards_release_event(impl->shards, entry->connection.shard, &event);
  if (status != SALTS_OK || !terminal_event) {
    cnet_dispatcher_record_error(impl, status);
    return status;
  }
  return cnet_dispatcher_recycle(entry, view);
}

static int cnet_dispatcher_release_direct(void *context, const cnet_dispatch_view *view,
                                          uint64_t token) {
  cnet_dispatch_entry *entry = (cnet_dispatch_entry *)context;
  (void)token;
  if (entry == NULL || view == NULL || entry->dispatcher == NULL) return SALTS_EINVAL;
  return cnet_dispatcher_recycle(entry, view);
}

static int cnet_dispatcher_prepare(cnet_dispatcher_impl *impl, uint32_t shard,
                                   const cnet_event *event, cnet_dispatch_release_fn release,
                                   uint64_t release_token, cnet_dispatch_job *out_job) {
  const cnet_shard_connection connection = {shard, event->session};
  cnet_dispatch_entry *entry;
  int status = SALTS_OK;
#if defined(CNET_INTERNAL_PROFILING)
  const uint64_t profile_started = cnet_dispatcher_profile_start(impl);
#endif

  entry = cnet_dispatcher_entry(impl, connection);
  if (entry == NULL || !entry->active ||
      entry->connection.session.generation != connection.session.generation) {
    status = SALTS_EBUSY;
  } else {
    *out_job = (cnet_dispatch_job){.invoke = entry->observer,
                                   .context = entry->observer_context,
                                   .event = {event->kind, event->session, event->state, event->status,
                                             event->stage, event->data, event->size, event->argument,
                                             event->backing},
                                   .release = release,
                                   .release_context = entry,
                                   .release_token = release_token};
  }
#if defined(CNET_INTERNAL_PROFILING)
  cnet_dispatcher_profile_finish(impl, profile_started, &impl->profile.prepare_ns,
                                 &impl->profile.prepare_calls);
#endif
  return status;
}

static int cnet_dispatcher_invoke(cnet_dispatcher_impl *impl, const cnet_dispatch_job *job) {
  const cnet_dispatch_view view = {job->event.kind,   job->event.session, job->event.state,
                                   job->event.status, job->event.stage,   job->event.data,
                                   job->event.size,   job->event.argument, job->event.backing};
  int status = SALTS_OK;
#if defined(CNET_INTERNAL_PROFILING)
  const uint64_t invoke_started = cnet_dispatcher_profile_start(impl);
  const uint64_t observer_started = cnet_dispatcher_profile_start(impl);
#endif

  job->invoke(job->context, &view);
#if defined(CNET_INTERNAL_PROFILING)
  cnet_dispatcher_profile_finish(impl, observer_started, &impl->profile.observer_ns,
                                 &impl->profile.observer_calls);
#endif
  if (job->release != NULL) {
#if defined(CNET_INTERNAL_PROFILING)
    const uint64_t release_started = cnet_dispatcher_profile_start(impl);
#endif
    status = job->release(job->release_context, &view, job->release_token);
#if defined(CNET_INTERNAL_PROFILING)
    cnet_dispatcher_profile_finish(impl, release_started, &impl->profile.release_ns,
                                   &impl->profile.release_calls);
#endif
  }
#if defined(CNET_INTERNAL_PROFILING)
  cnet_dispatcher_profile_finish(impl, invoke_started, &impl->profile.invoke_ns,
                                 &impl->profile.invoke_calls);
#endif
  return status;
}

int cnet_dispatcher_init(cnet_dispatcher *dispatcher, cnet_shards *shards) {
  cnet_dispatcher_impl *impl;
  cnet_shards_layout layout = {0};
  size_t entry_count;
  size_t index;

  if (dispatcher == NULL || shards == NULL) return SALTS_EINVAL;
  if (dispatcher->impl != NULL) return SALTS_EALREADY;
  if (!cnet_shards_get_layout(shards, &layout) || layout.shard_count == 0u ||
      layout.connection_capacity_per_shard == 0u)
    return SALTS_EINVAL;
  if (layout.shard_count > SIZE_MAX / layout.connection_capacity_per_shard) return SALTS_ERANGE;
  entry_count = layout.shard_count * layout.connection_capacity_per_shard;
  if (entry_count > SIZE_MAX / sizeof(cnet_dispatch_entry) ||
      layout.shard_count > SIZE_MAX / sizeof(cnet_dispatch_lane))
    return SALTS_ERANGE;

  impl = (cnet_dispatcher_impl *)calloc(1u, sizeof(*impl));
  if (impl == NULL) return SALTS_ENOMEM;
  impl->entries = (cnet_dispatch_entry *)calloc(entry_count, sizeof(*impl->entries));
  impl->lanes = (cnet_dispatch_lane *)calloc(layout.shard_count, sizeof(*impl->lanes));
  if (impl->entries == NULL || impl->lanes == NULL) {
    free(impl->lanes);
    free(impl->entries);
    free(impl);
    return SALTS_ENOMEM;
  }
  impl->shards = shards;
  impl->shard_count = layout.shard_count;
  impl->connection_capacity_per_shard = layout.connection_capacity_per_shard;
  impl->admission_open = true;
  atomic_init(&impl->first_error, SALTS_OK);
  for (index = 0u; index < entry_count; ++index)
    impl->entries[index].dispatcher = impl;
  for (index = 0u; index < impl->shard_count; ++index)
    atomic_flag_clear_explicit(&impl->lanes[index].driving, memory_order_release);
  for (index = 0u; index < impl->shard_count; ++index)
    atomic_init(&impl->lanes[index].pending, false);
  dispatcher->impl = impl;
  return SALTS_OK;
}

int cnet_dispatcher_register(cnet_dispatcher *dispatcher, cnet_shard_connection connection,
                             cnet_dispatch_fn observer, void *observer_context) {
  cnet_dispatcher_impl *impl = cnet_dispatcher_get(dispatcher);
  cnet_dispatch_entry *entry;
  cnet_session_state state = CNET_SESSION_FREE;
  int status;

  if (impl == NULL || observer == NULL) return SALTS_EINVAL;
  if (!impl->admission_open) return SALTS_ESHUTDOWN;
  status = cnet_shards_state(impl->shards, connection, &state);
  if (status != SALTS_OK) return status;
  entry = cnet_dispatcher_entry(impl, connection);
  if (entry == NULL) return SALTS_ENOENT;
  if (entry->active) return SALTS_EALREADY;

  entry->connection = connection;
  entry->observer = observer;
  entry->observer_context = observer_context;
  entry->active = true;
  entry->close_requested = state == CNET_SESSION_DRAINING || state == CNET_SESSION_TERMINAL;
  ++impl->active_count;
  return SALTS_OK;
}

int cnet_dispatcher_publish(cnet_dispatcher *dispatcher, uint32_t shard, const cnet_event *event) {
  cnet_dispatcher_impl *impl = cnet_dispatcher_get(dispatcher);
  cnet_dispatch_job job = {0};
  cnet_dispatch_release_fn release = NULL;
  int status;

  if (impl == NULL || event == NULL || (size_t)shard >= impl->shard_count) return SALTS_EINVAL;
  if (event->kind == CNET_EVENT_STATE && cnet_dispatcher_terminal_state(event->state))
    release = cnet_dispatcher_release_direct;
  status = cnet_dispatcher_prepare(impl, shard, event, release, 0u, &job);
  if (status == SALTS_OK) status = cnet_dispatcher_invoke(impl, &job);
  return status;
}

int cnet_dispatcher_drive(cnet_dispatcher *dispatcher, uint32_t shard) {
  cnet_dispatcher_impl *impl = cnet_dispatcher_get(dispatcher);
  cnet_dispatch_lane *lane;
  cnet_dispatch_job job = {0};
  int status = SALTS_OK;

  if (impl == NULL || (size_t)shard >= impl->shard_count) return SALTS_EINVAL;
  lane = &impl->lanes[shard];
  if (atomic_flag_test_and_set_explicit(&lane->driving, memory_order_acquire)) return SALTS_EBUSY;
  if (!atomic_load_explicit(&lane->pending, memory_order_acquire)) {
    status = cnet_shards_take_event(impl->shards, shard, &lane->event);
    if (status != SALTS_OK) {
      atomic_flag_clear_explicit(&lane->driving, memory_order_release);
      return status;
    }
    atomic_store_explicit(&lane->pending, true, memory_order_release);
  }

  if (status == SALTS_OK) {
    const cnet_event event = {lane->event.kind,   lane->event.session, lane->event.state,
                              lane->event.status, lane->event.stage,   lane->event.data,
                              lane->event.size,   lane->event.argument, lane->event.backing};
    status = cnet_dispatcher_prepare(impl, shard, &event, cnet_dispatcher_release_lease,
                                     lane->event._sequence, &job);
  }
  if (status == SALTS_OK) status = cnet_dispatcher_invoke(impl, &job);
  if (status == SALTS_OK) {
    memset(&lane->event, 0, sizeof(lane->event));
    atomic_store_explicit(&lane->pending, false, memory_order_release);
  } else if (status != SALTS_ENOBUFS && status != SALTS_EBUSY) {
    cnet_dispatcher_record_error(impl, status);
  }
  atomic_flag_clear_explicit(&lane->driving, memory_order_release);
  return status;
}

static bool cnet_dispatcher_is_idle(cnet_dispatcher_impl *impl) {
  size_t index;
  if (impl->active_count != 0u) return false;
  for (index = 0u; index < impl->shard_count; ++index)
    if (atomic_load_explicit(&impl->lanes[index].pending, memory_order_acquire)) return false;
  return true;
}

/* Isolated #1220 QA trace ONLY. Never built or installed by master. Do not
 * change drain progress, ownership, or the existing 5000ms cutoff. */
#if defined(CNET_VSOCK_STOP_DIAGNOSTIC)
static void cnet_qa_dump_dispatcher_timeout(cnet_dispatcher_impl *impl) {
  const size_t entries = impl->shard_count * impl->connection_capacity_per_shard;
  size_t recorded = 0u;
  size_t index;
  fprintf(stderr,
          "CNET1220_DRAIN_TIMEOUT active=%zu shards=%zu "
          "capacity_per_shard=%zu first_error=%d admission_open=%d drained=%d\n",
          impl->active_count, impl->shard_count,
          impl->connection_capacity_per_shard,
          atomic_load_explicit(&impl->first_error, memory_order_acquire),
          impl->admission_open ? 1 : 0, impl->drained ? 1 : 0);
  for (index = 0u; index < entries; ++index) {
    cnet_dispatch_entry *entry = &impl->entries[index];
    cnet_session_state state = CNET_SESSION_FREE;
    int status;
    if (!entry->active) continue;
    ++recorded;
    status = cnet_shards_state(impl->shards, entry->connection, &state);
    fprintf(stderr,
            "CNET1220_ENTRY idx=%zu shard=%u slot=%u generation=%u "
            "close_requested=%d state_status=%d session_state=%d\n",
            index, (unsigned)entry->connection.shard,
            (unsigned)entry->connection.session.slot,
            (unsigned)entry->connection.session.generation,
            entry->close_requested ? 1 : 0, status, (int)state);
  }
  for (index = 0u; index < impl->shard_count; ++index) {
    cnet_dispatch_lane *lane = &impl->lanes[index];
    const bool pending =
        atomic_load_explicit(&lane->pending, memory_order_acquire);
    fprintf(stderr,
            "CNET1220_LANE shard=%zu pending=%d event_kind=%d "
            "event_slot=%u event_generation=%u event_state=%d\n",
            index, pending ? 1 : 0,
            pending ? (int)lane->event.kind : -1,
            pending ? (unsigned)lane->event.session.slot : 0u,
            pending ? (unsigned)lane->event.session.generation : 0u,
            pending ? (int)lane->event.state : -1);
  }
  fprintf(stderr, "CNET1220_DRAIN_TOTAL recorded=%zu active=%zu\n",
          recorded, impl->active_count);
  fflush(stderr);
}
#endif

static int cnet_dispatcher_drive_all(cnet_dispatcher_impl *impl) {
  cnet_dispatcher dispatcher = {impl};
  size_t index;
  int first_status = SALTS_ETIMEDOUT;
  for (index = 0u; index < impl->shard_count; ++index) {
    const int status = cnet_dispatcher_drive(&dispatcher, (uint32_t)index);
    if (status == SALTS_OK) first_status = SALTS_OK;
    else if (status != SALTS_ETIMEDOUT && status != SALTS_ENOBUFS && status != SALTS_EBUSY)
      return status;
  }
  return first_status;
}

int cnet_dispatcher_wait_idle(cnet_dispatcher *dispatcher, uint32_t timeout_ms) {
  cnet_dispatcher_impl *impl = cnet_dispatcher_get(dispatcher);
  const uint64_t started_ms = cmeta_monotonic_ms();
  if (impl == NULL) return SALTS_EINVAL;
  for (;;) {
    const int error = atomic_load_explicit(&impl->first_error, memory_order_acquire);
    int status;
    if (error != SALTS_OK) return error;
    status = cnet_dispatcher_drive_all(impl);
    if (status != SALTS_OK && status != SALTS_ETIMEDOUT) return status;
    if (cnet_dispatcher_is_idle(impl)) return SALTS_OK;
    if (cmeta_monotonic_ms() - started_ms >= timeout_ms) return SALTS_ETIMEDOUT;
    cmeta_sleep_ms(1u);
  }
}

static int cnet_dispatcher_request_closes(cnet_dispatcher_impl *impl) {
  const size_t entry_count = impl->shard_count * impl->connection_capacity_per_shard;
  size_t index;
  int status = SALTS_OK;

  for (index = 0u; index < entry_count; ++index) {
    cnet_dispatch_entry *entry = &impl->entries[index];
    cnet_shard_connection connection = {0};
    bool claimed = false;
    int close_status;

    if (entry->active && !entry->close_requested) {
      connection = entry->connection;
      entry->close_requested = true;
      claimed = true;
    }
    if (!claimed) continue;

    close_status = cnet_shards_close(impl->shards, connection);
    if (close_status == SALTS_OK || close_status == SALTS_EALREADY || close_status == SALTS_ENOENT)
      continue;

    if (entry->active && entry->connection.shard == connection.shard &&
        entry->connection.session.slot == connection.session.slot &&
        entry->connection.session.generation == connection.session.generation)
      entry->close_requested = false;
    if (close_status != SALTS_ENOBUFS && status == SALTS_OK) {
      status = close_status;
    }
  }
  return status;
}

int cnet_dispatcher_drain(cnet_dispatcher *dispatcher, uint32_t timeout_ms) {
  cnet_dispatcher_impl *impl = cnet_dispatcher_get(dispatcher);
  const uint64_t started_ms = cmeta_monotonic_ms();
  int first_status;
  if (impl == NULL) return SALTS_EINVAL;
  if (impl->drained) return SALTS_EALREADY;
  impl->admission_open = false;
  first_status = atomic_load_explicit(&impl->first_error, memory_order_acquire);

  for (;;) {
    int status = cnet_dispatcher_request_closes(impl);
    if (status != SALTS_OK) return status;
    status = cnet_shards_poll(impl->shards, 1u);
    if (status != SALTS_OK && status != SALTS_ETIMEDOUT && first_status == SALTS_OK)
      first_status = status;
    status = cnet_dispatcher_drive_all(impl);
    if (status != SALTS_OK && status != SALTS_ETIMEDOUT && status != SALTS_ENOBUFS &&
        status != SALTS_EBUSY && first_status == SALTS_OK)
      first_status = status;
    status = atomic_load_explicit(&impl->first_error, memory_order_acquire);
    if (status != SALTS_OK && first_status == SALTS_OK) first_status = status;
    if (cnet_dispatcher_is_idle(impl)) break;
    if (cmeta_monotonic_ms() - started_ms >= timeout_ms) {
#if defined(CNET_VSOCK_STOP_DIAGNOSTIC)
      cnet_qa_dump_dispatcher_timeout(impl);
#endif
      return SALTS_ETIMEDOUT;
    }
  }
  impl->drained = true;
  return first_status;
}

bool cnet_dispatcher_drained(const cnet_dispatcher *dispatcher) {
  const cnet_dispatcher_impl *impl =
      dispatcher != NULL ? (const cnet_dispatcher_impl *)dispatcher->impl : NULL;
  return impl != NULL && impl->drained;
}

#if defined(CNET_INTERNAL_PROFILING)
int cnet_dispatcher_profile_begin(cnet_dispatcher *dispatcher) {
  cnet_dispatcher_impl *impl = cnet_dispatcher_get(dispatcher);
  if (impl == NULL) return SALTS_EINVAL;
  if (impl->profile_active) return SALTS_EBUSY;
  memset(&impl->profile, 0, sizeof(impl->profile));
  impl->profile_active = true;
  return SALTS_OK;
}

int cnet_dispatcher_profile_take(cnet_dispatcher *dispatcher,
                                 cnet_dispatcher_profile *out_profile) {
  cnet_dispatcher_impl *impl = cnet_dispatcher_get(dispatcher);
  if (impl == NULL || out_profile == NULL) return SALTS_EINVAL;
  if (!impl->profile_active) return SALTS_EBUSY;
  *out_profile = impl->profile;
  impl->profile_active = false;
  return SALTS_OK;
}
#endif

int cnet_dispatcher_destroy(cnet_dispatcher *dispatcher) {
  cnet_dispatcher_impl *impl = cnet_dispatcher_get(dispatcher);
  size_t index;
  if (dispatcher == NULL) return SALTS_EINVAL;
  if (impl == NULL) return SALTS_OK;
  if (!impl->drained || !cnet_dispatcher_is_idle(impl)) return SALTS_EBUSY;
  for (index = 0u; index < impl->shard_count; ++index) {
    if (atomic_load_explicit(&impl->lanes[index].pending, memory_order_acquire) ||
        atomic_flag_test_and_set_explicit(&impl->lanes[index].driving, memory_order_acquire)) {
      size_t release_index;
      for (release_index = 0u; release_index < index; ++release_index)
        atomic_flag_clear_explicit(&impl->lanes[release_index].driving, memory_order_release);
      return SALTS_EBUSY;
    }
  }
  free(impl->lanes);
  free(impl->entries);
  free(impl);
  dispatcher->impl = NULL;
  return SALTS_OK;
}
