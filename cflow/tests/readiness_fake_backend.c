#include "readiness_fake_backend.h"
#include "readiness_backend_contract.h"

#include <readiness_internal.h>

#include <core/error_codes.h>
#include <threads/threads.h>

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

typedef struct readiness_fake_record {
  intptr_t native_resource;
  uint64_t token;
  uint64_t arm_token;
  readiness_events events;
  int active;
} readiness_fake_record;

struct readiness_contract_fixture {
  readiness_reactor *reactor;
  readiness_fake_record *records;
  size_t record_capacity;
  sync_mutex_t mutex;
  sync_cond_t changed;
  int hook_blocked[READINESS_CONTRACT_HOOK_COUNT];
  size_t hook_block_on_call[READINESS_CONTRACT_HOOK_COUNT];
  size_t hook_calls[READINESS_CONTRACT_HOOK_COUNT];
  uint64_t hook_last_sequence[READINESS_CONTRACT_HOOK_COUNT];
  uint64_t hook_sequence;
  int hook_fail_status[READINESS_CONTRACT_HOOK_COUNT];
  size_t hook_fail_remaining[READINESS_CONTRACT_HOOK_COUNT];
  int next_arm_error;
  atomic_size_t reentrant_checks;
};

static readiness_fake_record *fake_find_resource(readiness_contract_fixture *fixture,
                                                 intptr_t native_resource) {
  for (size_t i = 0; i < fixture->record_capacity; ++i) {
    if (fixture->records[i].active && fixture->records[i].native_resource == native_resource)
      return &fixture->records[i];
  }
  return NULL;
}

static readiness_fake_record *fake_find_token(readiness_contract_fixture *fixture, uint64_t token) {
  for (size_t i = 0; i < fixture->record_capacity; ++i) {
    if (fixture->records[i].active && fixture->records[i].token == token)
      return &fixture->records[i];
  }
  return NULL;
}

static void fake_hook_enter(readiness_contract_fixture *fixture, readiness_contract_hook hook) {
  size_t call;
  sync_mutex_lock(&fixture->mutex);
  fixture->hook_calls[hook] += 1u;
  call = fixture->hook_calls[hook];
  fixture->hook_sequence += 1u;
  fixture->hook_last_sequence[hook] = fixture->hook_sequence;
  sync_cond_broadcast(&fixture->changed);
  while (fixture->hook_blocked[hook] || fixture->hook_block_on_call[hook] == call)
    sync_cond_wait(&fixture->changed, &fixture->mutex);
  sync_mutex_unlock(&fixture->mutex);
}

static int fake_hook_error(readiness_contract_fixture *fixture, readiness_contract_hook hook) {
  int status = SALTS_OK;
  sync_mutex_lock(&fixture->mutex);
  if (fixture->hook_fail_remaining[hook] != 0) {
    fixture->hook_fail_remaining[hook] -= 1u;
    status = fixture->hook_fail_status[hook];
  }
  sync_mutex_unlock(&fixture->mutex);
  return status;
}

static void fake_check_reentrant(readiness_contract_fixture *fixture) {
  readiness_stats stats;
  if (readiness_reactor_stats(fixture->reactor, &stats) == SALTS_OK)
    atomic_fetch_add(&fixture->reentrant_checks, 1u);
}

static int fake_register_resource(void *user, intptr_t native_resource, uint64_t token) {
  readiness_contract_fixture *fixture = (readiness_contract_fixture *)user;
  fake_check_reentrant(fixture);
  fake_hook_enter(fixture, READINESS_CONTRACT_HOOK_REGISTER);
  int hook_status = fake_hook_error(fixture, READINESS_CONTRACT_HOOK_REGISTER);
  if (hook_status != SALTS_OK) return hook_status;
  sync_mutex_lock(&fixture->mutex);
  if (fake_find_resource(fixture, native_resource) != NULL) {
    sync_mutex_unlock(&fixture->mutex);
    return SALTS_EALREADY;
  }
  for (size_t i = 0; i < fixture->record_capacity; ++i) {
    if (!fixture->records[i].active) {
      fixture->records[i].native_resource = native_resource;
      fixture->records[i].token = token;
      fixture->records[i].events = 0;
      fixture->records[i].active = 1;
      sync_mutex_unlock(&fixture->mutex);
      return SALTS_OK;
    }
  }
  sync_mutex_unlock(&fixture->mutex);
  return SALTS_ENOBUFS;
}

static int fake_arm(void *user, uint64_t token, uint64_t arm_token,
                    readiness_events events) {
  readiness_contract_fixture *fixture = (readiness_contract_fixture *)user;
  readiness_fake_record *record;
  int status = SALTS_OK;
  fake_check_reentrant(fixture);
  fake_hook_enter(fixture, READINESS_CONTRACT_HOOK_ARM);
  status = fake_hook_error(fixture, READINESS_CONTRACT_HOOK_ARM);
  if (status != SALTS_OK) return status;
  sync_mutex_lock(&fixture->mutex);
  if (fixture->next_arm_error != SALTS_OK) {
    status = fixture->next_arm_error;
    fixture->next_arm_error = SALTS_OK;
  } else {
    record = fake_find_token(fixture, token);
    if (record == NULL) status = SALTS_EINVAL;
    else {
      record->arm_token = arm_token;
      record->events = events;
    }
  }
  sync_mutex_unlock(&fixture->mutex);
  return status;
}

static int fake_unarm(void *user, uint64_t token) {
  readiness_contract_fixture *fixture = (readiness_contract_fixture *)user;
  readiness_fake_record *record;
  int status = SALTS_OK;
  fake_check_reentrant(fixture);
  fake_hook_enter(fixture, READINESS_CONTRACT_HOOK_UNARM);
  status = fake_hook_error(fixture, READINESS_CONTRACT_HOOK_UNARM);
  if (status != SALTS_OK) return status;
  sync_mutex_lock(&fixture->mutex);
  record = fake_find_token(fixture, token);
  if (record == NULL) status = SALTS_EINVAL;
  else record->events = 0;
  sync_mutex_unlock(&fixture->mutex);
  return status;
}

static int fake_close(void *user, uint64_t token) {
  readiness_contract_fixture *fixture = (readiness_contract_fixture *)user;
  readiness_fake_record *record;
  int status = SALTS_OK;
  fake_check_reentrant(fixture);
  fake_hook_enter(fixture, READINESS_CONTRACT_HOOK_CLOSE);
  status = fake_hook_error(fixture, READINESS_CONTRACT_HOOK_CLOSE);
  if (status != SALTS_OK) return status;
  sync_mutex_lock(&fixture->mutex);
  record = fake_find_token(fixture, token);
  if (record == NULL) status = SALTS_EINVAL;
  else memset(record, 0, sizeof(*record));
  sync_mutex_unlock(&fixture->mutex);
  return status;
}

static int fake_shutdown(void *user) {
  readiness_contract_fixture *fixture = (readiness_contract_fixture *)user;
  fake_check_reentrant(fixture);
  fake_hook_enter(fixture, READINESS_CONTRACT_HOOK_SHUTDOWN);
  return fake_hook_error(fixture, READINESS_CONTRACT_HOOK_SHUTDOWN);
}

static void fake_backend_destroy(void *user) { (void)user; }

static const readiness_backend_ops fake_backend_ops = {fake_register_resource, fake_arm,
                                                             fake_unarm, fake_close, fake_shutdown,
                                                             fake_backend_destroy};

static readiness_contract_fixture *fake_create(readiness_config config,
                                               readiness_reactor *reactor, int *status) {
  readiness_contract_fixture *fixture;
  if (reactor != NULL) reactor->impl = NULL;
  if (status == NULL) return NULL;
  *status = SALTS_EINVAL;
  if (reactor == NULL) return NULL;

  fixture = (readiness_contract_fixture *)calloc(1, sizeof(*fixture));
  if (fixture == NULL) {
    *status = SALTS_ENOMEM;
    return NULL;
  }
  fixture->reactor = reactor;
  atomic_init(&fixture->reentrant_checks, 0u);
  sync_mutex_init(&fixture->mutex);
  sync_cond_init(&fixture->changed);
  if (fixture->mutex == NULL || fixture->changed == NULL) {
    sync_cond_destroy(&fixture->changed);
    sync_mutex_destroy(&fixture->mutex);
    free(fixture);
    *status = SALTS_ENOMEM;
    return NULL;
  }
  *status = readiness_reactor_init_backend(reactor, &config, &fake_backend_ops, fixture);
  if (*status != SALTS_OK) {
    sync_cond_destroy(&fixture->changed);
    sync_mutex_destroy(&fixture->mutex);
    free(fixture);
    return NULL;
  }
  fixture->records =
      (readiness_fake_record *)calloc(config.registration_capacity, sizeof(*fixture->records));
  if (fixture->records == NULL) {
    (void)readiness_reactor_shutdown(reactor);
    (void)readiness_reactor_destroy(reactor);
    sync_cond_destroy(&fixture->changed);
    sync_mutex_destroy(&fixture->mutex);
    free(fixture);
    *status = SALTS_ENOMEM;
    return NULL;
  }
  fixture->record_capacity = config.registration_capacity;
  return fixture;
}

static void fake_destroy(readiness_contract_fixture *fixture) {
  free(fixture->records);
  sync_cond_destroy(&fixture->changed);
  sync_mutex_destroy(&fixture->mutex);
  free(fixture);
}

static int fake_emit_resource(readiness_contract_fixture *fixture, intptr_t native_resource,
                              readiness_events events) {
  uint64_t token = 0;
  sync_mutex_lock(&fixture->mutex);
  readiness_fake_record *record = fake_find_resource(fixture, native_resource);
  if (record != NULL && record->events != 0) token = record->token;
  sync_mutex_unlock(&fixture->mutex);
  return token == 0 ? SALTS_OK
                    : readiness_backend_dispatch(fixture->reactor, token, events, SALTS_OK);
}

static int fake_emit_token(readiness_contract_fixture *fixture, uint64_t token,
                           readiness_events events, int status) {
  return readiness_backend_dispatch(fixture->reactor, token, events, status);
}

static int fake_fail_backend(readiness_contract_fixture *fixture, int status) {
  return readiness_backend_fail(fixture->reactor, status);
}

static uint64_t fake_token_for_resource(readiness_contract_fixture *fixture,
                                        intptr_t native_resource) {
  readiness_fake_record *record;
  uint64_t token;
  sync_mutex_lock(&fixture->mutex);
  record = fake_find_resource(fixture, native_resource);
  token = record != NULL ? record->token : 0;
  sync_mutex_unlock(&fixture->mutex);
  return token;
}

static uint64_t fake_arm_token_for_resource(readiness_contract_fixture *fixture,
                                            intptr_t native_resource) {
  readiness_fake_record *record;
  uint64_t token;
  sync_mutex_lock(&fixture->mutex);
  record = fake_find_resource(fixture, native_resource);
  token = record != NULL ? record->arm_token : 0;
  sync_mutex_unlock(&fixture->mutex);
  return token;
}

static int fake_emit_arm_token(readiness_contract_fixture *fixture, uint64_t token,
                               uint64_t arm_token, readiness_events events) {
  return readiness_backend_dispatch_generation(fixture->reactor, token, arm_token, events,
                                                     SALTS_OK);
}

static void fake_fail_next_arm(readiness_contract_fixture *fixture, int status) {
  sync_mutex_lock(&fixture->mutex);
  fixture->next_arm_error = status;
  sync_mutex_unlock(&fixture->mutex);
}

static void fake_fail_hook(readiness_contract_fixture *fixture, readiness_contract_hook hook,
                           int status, size_t calls) {
  sync_mutex_lock(&fixture->mutex);
  fixture->hook_fail_status[hook] = status;
  fixture->hook_fail_remaining[hook] = calls;
  sync_mutex_unlock(&fixture->mutex);
}

static size_t fake_backend_close_calls(readiness_contract_fixture *fixture) {
  size_t calls;
  sync_mutex_lock(&fixture->mutex);
  calls = fixture->hook_calls[READINESS_CONTRACT_HOOK_CLOSE];
  sync_mutex_unlock(&fixture->mutex);
  return calls;
}

static size_t fake_backend_unarm_calls(readiness_contract_fixture *fixture) {
  size_t calls;
  sync_mutex_lock(&fixture->mutex);
  calls = fixture->hook_calls[READINESS_CONTRACT_HOOK_UNARM];
  sync_mutex_unlock(&fixture->mutex);
  return calls;
}

static size_t fake_backend_reentrant_checks(readiness_contract_fixture *fixture) {
  return atomic_load(&fixture->reentrant_checks);
}

static void fake_block_hook(readiness_contract_fixture *fixture, readiness_contract_hook hook) {
  sync_mutex_lock(&fixture->mutex);
  fixture->hook_blocked[hook] = 1;
  sync_mutex_unlock(&fixture->mutex);
}

static void fake_block_hook_on_call(readiness_contract_fixture *fixture,
                                    readiness_contract_hook hook, size_t call) {
  sync_mutex_lock(&fixture->mutex);
  fixture->hook_block_on_call[hook] = call;
  sync_mutex_unlock(&fixture->mutex);
}

static void fake_release_hook(readiness_contract_fixture *fixture, readiness_contract_hook hook) {
  sync_mutex_lock(&fixture->mutex);
  fixture->hook_blocked[hook] = 0;
  fixture->hook_block_on_call[hook] = 0;
  sync_cond_broadcast(&fixture->changed);
  sync_mutex_unlock(&fixture->mutex);
}

static int fake_wait_hook_calls(readiness_contract_fixture *fixture, readiness_contract_hook hook,
                                size_t calls, uint64_t timeout_ns) {
  int status = SALTS_OK;
  sync_mutex_lock(&fixture->mutex);
  while (fixture->hook_calls[hook] < calls && status == SALTS_OK)
    status = sync_cond_timedwait(&fixture->changed, &fixture->mutex, timeout_ns);
  sync_mutex_unlock(&fixture->mutex);
  return status;
}

static int fake_wait_admission_closed(readiness_contract_fixture *fixture) {
  return readiness_backend_wait_admission_closed(fixture->reactor);
}

static uint64_t fake_hook_last_sequence(readiness_contract_fixture *fixture,
                                        readiness_contract_hook hook) {
  uint64_t sequence;
  sync_mutex_lock(&fixture->mutex);
  sequence = fixture->hook_last_sequence[hook];
  sync_mutex_unlock(&fixture->mutex);
  return sequence;
}

const readiness_contract_factory *readiness_contract_factory_get(void) {
  static const readiness_contract_factory factory = {fake_create,
                                                     fake_destroy,
                                                     fake_emit_resource,
                                                     fake_emit_token,
                                                     fake_fail_backend,
                                                     fake_token_for_resource,
                                                     fake_arm_token_for_resource,
                                                     fake_emit_arm_token,
                                                     fake_fail_next_arm,
                                                     fake_fail_hook,
                                                     fake_backend_close_calls,
                                                     fake_backend_unarm_calls,
                                                     fake_backend_reentrant_checks,
                                                     fake_block_hook,
                                                     fake_block_hook_on_call,
                                                     fake_release_hook,
                                                     fake_wait_hook_calls,
                                                     fake_wait_admission_closed,
                                                     fake_hook_last_sequence};
  return &factory;
}

static readiness_backend_contract_fixture *fake_backend_contract_create(
    readiness_config config, readiness_reactor *reactor, int *status) {
  return (readiness_backend_contract_fixture *)fake_create(config, reactor, status);
}

static void fake_backend_contract_destroy(readiness_backend_contract_fixture *fixture) {
  fake_destroy((readiness_contract_fixture *)fixture);
}

static intptr_t fake_backend_contract_resource(readiness_backend_contract_fixture *fixture,
                                               size_t index) {
  (void)fixture;
  return (intptr_t)(1001u + index);
}

static int fake_backend_contract_make_readable(readiness_backend_contract_fixture *fixture,
                                               size_t index) {
  return fake_emit_resource((readiness_contract_fixture *)fixture,
                            fake_backend_contract_resource(fixture, index),
                            SALTS_READINESS_EVENT_READ);
}

static int fake_backend_contract_drain_readable(readiness_backend_contract_fixture *fixture,
                                                size_t index) {
  (void)fixture;
  (void)index;
  return SALTS_OK;
}

const readiness_backend_contract_factory *readiness_backend_contract_factory_get(void) {
  static const readiness_backend_contract_factory factory = {
      fake_backend_contract_create, fake_backend_contract_destroy,
      fake_backend_contract_resource, fake_backend_contract_make_readable,
      fake_backend_contract_drain_readable};
  return &factory;
}
