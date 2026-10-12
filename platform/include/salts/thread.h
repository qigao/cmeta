#ifndef SALTS_THREAD_PRIMITIVES_H
#define SALTS_THREAD_PRIMITIVES_H

#include <salts/error_codes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The POSIX once primitive is an OS synchronization event even when Salts
 * itself was built without TSan. This is an intentional current-ABI change;
 * every dependent SDK must be rebuilt against this same header. */
#if !defined(_WIN32)
#include <pthread.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

#ifndef SALTS_THREAD_LOCAL
  #if defined(__cplusplus)
    #define SALTS_THREAD_LOCAL thread_local
  #elif defined(_MSC_VER)
    #define SALTS_THREAD_LOCAL __declspec(thread)
  #elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L && !defined(__STDC_NO_THREADS__)
    #define SALTS_THREAD_LOCAL _Thread_local
  #elif defined(__GNUC__) || defined(__clang__)
    #define SALTS_THREAD_LOCAL __thread
  #else
    #error "SALTS_THREAD_LOCAL is not supported by this compiler"
  #endif
#endif

typedef void *cmeta_mutex_t;
typedef void *cmeta_cond_t;
typedef void *cmeta_thread_t;
typedef void *cmeta_rwlock_t;

typedef struct cmeta_once_s {
#if defined(_WIN32)
  volatile int state;
#else
  pthread_once_t native;
#endif
} cmeta_once_t;
#if defined(_WIN32)
#define SALTS_ONCE_INIT {0}
#else
#define SALTS_ONCE_INIT {PTHREAD_ONCE_INIT}
#endif

typedef void (*cmeta_thread_cb)(void *arg);

const void *cmeta_thread_current_token(void);

/*
 * Address-stable thread-affinity state owned by Salts::Platform.
 * This carries no value lifecycle callbacks and owns no payload.
 */
typedef struct cmeta_thread_affine_state {
  const void *owner;
  const void *thread;
  bool busy;
} cmeta_thread_affine_state;

static inline int cmeta_thread_affine_init(cmeta_thread_affine_state *state, const void *owner) {
  if (state == NULL || owner == NULL || state->owner != NULL) return SALTS_EINVAL;
  state->owner = owner;
  state->thread = cmeta_thread_current_token();
  state->busy = false;
  return SALTS_OK;
}

static inline int cmeta_thread_affine_check(const cmeta_thread_affine_state *state,
                                            const void *owner) {
  if (state == NULL || owner == NULL || state->owner != owner ||
      state->thread != cmeta_thread_current_token())
    return SALTS_EINVAL;
  return state->busy ? SALTS_EBUSY : SALTS_OK;
}

static inline int cmeta_thread_affine_set_busy(cmeta_thread_affine_state *state, const void *owner,
                                               bool busy) {
  if (state == NULL || owner == NULL || state->owner != owner ||
      state->thread != cmeta_thread_current_token())
    return SALTS_EINVAL;
  state->busy = busy;
  return SALTS_OK;
}

static inline int cmeta_thread_affine_reset(cmeta_thread_affine_state *state, const void *owner) {
  int status = cmeta_thread_affine_check(state, owner);
  if (status != SALTS_OK) return status;
  state->owner = NULL;
  state->thread = NULL;
  state->busy = false;
  return SALTS_OK;
}

void cmeta_mutex_init(cmeta_mutex_t *mutex);
void cmeta_mutex_destroy(cmeta_mutex_t *mutex);
void cmeta_mutex_lock(cmeta_mutex_t *mutex);
void cmeta_mutex_unlock(cmeta_mutex_t *mutex);

int cmeta_rwlock_init(cmeta_rwlock_t *lock);
void cmeta_rwlock_destroy(cmeta_rwlock_t *lock);
void cmeta_rwlock_rdlock(cmeta_rwlock_t *lock);
void cmeta_rwlock_rdunlock(cmeta_rwlock_t *lock);
void cmeta_rwlock_wrlock(cmeta_rwlock_t *lock);
void cmeta_rwlock_wrunlock(cmeta_rwlock_t *lock);

void cmeta_cond_init(cmeta_cond_t *cond);
void cmeta_cond_destroy(cmeta_cond_t *cond);
void cmeta_cond_signal(cmeta_cond_t *cond);
void cmeta_cond_broadcast(cmeta_cond_t *cond);
void cmeta_cond_wait(cmeta_cond_t *cond, cmeta_mutex_t *mutex);
int cmeta_cond_timedwait(cmeta_cond_t *cond, cmeta_mutex_t *mutex, uint64_t timeout_ns);

int cmeta_thread_create(cmeta_thread_t *thread, cmeta_thread_cb entry, void *arg);
int cmeta_thread_join(cmeta_thread_t *thread);
void cmeta_thread_destroy(cmeta_thread_t *thread);
void cmeta_once(cmeta_once_t *guard, void (*callback)(void));
void cmeta_sleep_ms(uint32_t ms);
void cmeta_thread_yield(void);
/* Identity of the current live OS thread. Compare only while both threads
 * remain alive; token addresses may be reused after thread exit. */
const void *cmeta_thread_current_token(void);
int cmeta_cpu_count(void);

#ifdef __cplusplus
}
#endif

#endif /* SALTS_THREAD_PRIMITIVES_H */
