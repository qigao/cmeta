#include <salts/thread.h>

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

static SALTS_THREAD_LOCAL unsigned char cmeta_thread_token;
const void *cmeta_thread_current_token(void) { return &cmeta_thread_token; }

struct cmeta_thread_wrapper_ctx {
  cmeta_thread_cb entry;
  void *arg;
};

typedef struct cmeta_posix_cond_s {
  pthread_cond_t native;
} cmeta_posix_cond_t;

void cmeta_mutex_init(cmeta_mutex_t *mutex) {
  pthread_mutex_t *native;
  if (mutex == NULL) return;
  *mutex = NULL;
  native = (pthread_mutex_t *)malloc(sizeof(*native));
  if (native == NULL) return;
  if (pthread_mutex_init(native, NULL) != 0) {
    free(native);
    return;
  }
  *mutex = native;
}

void cmeta_mutex_destroy(cmeta_mutex_t *mutex) {
  if (mutex == NULL || *mutex == NULL) return;
  (void)pthread_mutex_destroy((pthread_mutex_t *)*mutex);
  free(*mutex);
  *mutex = NULL;
}

void cmeta_mutex_lock(cmeta_mutex_t *mutex) {
  if (mutex == NULL || *mutex == NULL) return;
  (void)pthread_mutex_lock((pthread_mutex_t *)*mutex);
}

void cmeta_mutex_unlock(cmeta_mutex_t *mutex) {
  if (mutex == NULL || *mutex == NULL) return;
  (void)pthread_mutex_unlock((pthread_mutex_t *)*mutex);
}

int cmeta_rwlock_init(cmeta_rwlock_t *lock) {
  pthread_rwlock_t *native;
  int rc;
  if (lock == NULL) return -EINVAL;
  *lock = NULL;
  native = (pthread_rwlock_t *)malloc(sizeof(*native));
  if (native == NULL) return -ENOMEM;
  rc = pthread_rwlock_init(native, NULL);
  if (rc != 0) {
    free(native);
    return -rc;
  }
  *lock = native;
  return 0;
}

void cmeta_rwlock_destroy(cmeta_rwlock_t *lock) {
  if (lock == NULL || *lock == NULL) return;
  (void)pthread_rwlock_destroy((pthread_rwlock_t *)*lock);
  free(*lock);
  *lock = NULL;
}

void cmeta_rwlock_rdlock(cmeta_rwlock_t *lock) {
  if (lock == NULL || *lock == NULL) return;
  (void)pthread_rwlock_rdlock((pthread_rwlock_t *)*lock);
}

void cmeta_rwlock_rdunlock(cmeta_rwlock_t *lock) {
  if (lock == NULL || *lock == NULL) return;
  (void)pthread_rwlock_unlock((pthread_rwlock_t *)*lock);
}

void cmeta_rwlock_wrlock(cmeta_rwlock_t *lock) {
  if (lock == NULL || *lock == NULL) return;
  (void)pthread_rwlock_wrlock((pthread_rwlock_t *)*lock);
}

void cmeta_rwlock_wrunlock(cmeta_rwlock_t *lock) {
  if (lock == NULL || *lock == NULL) return;
  (void)pthread_rwlock_unlock((pthread_rwlock_t *)*lock);
}

void cmeta_cond_init(cmeta_cond_t *cond) {
  cmeta_posix_cond_t *wrapper;
  int rc;

  if (cond == NULL) return;
  *cond = NULL;
  wrapper = (cmeta_posix_cond_t *)malloc(sizeof(*wrapper));
  if (wrapper == NULL) return;

#if defined(__APPLE__)
  rc = pthread_cond_init(&wrapper->native, NULL);
#else
  {
    pthread_condattr_t attr;
    int attr_initialized = 0;
    rc = pthread_condattr_init(&attr);
    if (rc == 0) {
      attr_initialized = 1;
      rc = pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    }
    if (rc == 0) rc = pthread_cond_init(&wrapper->native, &attr);
    if (attr_initialized) (void)pthread_condattr_destroy(&attr);
  }
#endif

  if (rc != 0) {
    free(wrapper);
    return;
  }
  *cond = wrapper;
}

void cmeta_cond_destroy(cmeta_cond_t *cond) {
  cmeta_posix_cond_t *wrapper;
  if (cond == NULL || *cond == NULL) return;
  wrapper = (cmeta_posix_cond_t *)*cond;
  (void)pthread_cond_destroy(&wrapper->native);
  free(wrapper);
  *cond = NULL;
}

void cmeta_cond_signal(cmeta_cond_t *cond) {
  cmeta_posix_cond_t *wrapper;
  if (cond == NULL || *cond == NULL) return;
  wrapper = (cmeta_posix_cond_t *)*cond;
  (void)pthread_cond_signal(&wrapper->native);
}

void cmeta_cond_broadcast(cmeta_cond_t *cond) {
  cmeta_posix_cond_t *wrapper;
  if (cond == NULL || *cond == NULL) return;
  wrapper = (cmeta_posix_cond_t *)*cond;
  (void)pthread_cond_broadcast(&wrapper->native);
}

void cmeta_cond_wait(cmeta_cond_t *cond, cmeta_mutex_t *mutex) {
  cmeta_posix_cond_t *wrapper;
  if (cond == NULL || *cond == NULL || mutex == NULL || *mutex == NULL) return;
  wrapper = (cmeta_posix_cond_t *)*cond;
  (void)pthread_cond_wait(&wrapper->native, (pthread_mutex_t *)*mutex);
}

#if !defined(__APPLE__)
static void cmeta_timespec_add_ns(struct timespec *ts, uint64_t timeout_ns) {
  uint64_t seconds = timeout_ns / 1000000000ULL;
  uint64_t nanos = timeout_ns % 1000000000ULL;
  ts->tv_sec += (time_t)seconds;
  ts->tv_nsec += (long)nanos;
  if (ts->tv_nsec >= 1000000000L) {
    ts->tv_sec += 1;
    ts->tv_nsec -= 1000000000L;
  }
}
#endif

int cmeta_cond_timedwait(cmeta_cond_t *cond, cmeta_mutex_t *mutex,
                         uint64_t timeout_ns) {
  cmeta_posix_cond_t *wrapper;
  struct timespec deadline;
  int rc;

  if (cond == NULL || *cond == NULL || mutex == NULL || *mutex == NULL)
    return -EINVAL;
  wrapper = (cmeta_posix_cond_t *)*cond;

#if defined(__APPLE__)
  deadline.tv_sec = (time_t)(timeout_ns / 1000000000ULL);
  deadline.tv_nsec = (long)(timeout_ns % 1000000000ULL);
  rc = pthread_cond_timedwait_relative_np(&wrapper->native,
                                          (pthread_mutex_t *)*mutex,
                                          &deadline);
#else
  if (clock_gettime(CLOCK_MONOTONIC, &deadline) != 0) return -errno;
  cmeta_timespec_add_ns(&deadline, timeout_ns);
  rc = pthread_cond_timedwait(&wrapper->native,
                              (pthread_mutex_t *)*mutex,
                              &deadline);
#endif

  if (rc == 0) return 0;
  return rc == ETIMEDOUT ? -ETIMEDOUT : -rc;
}

static void *cmeta_thread_entry_wrapper_pthread(void *arg) {
  struct cmeta_thread_wrapper_ctx *ctx =
      (struct cmeta_thread_wrapper_ctx *)arg;
  cmeta_thread_cb entry = ctx->entry;
  void *real_arg = ctx->arg;
  free(ctx);
  entry(real_arg);
  return NULL;
}

int cmeta_thread_create(cmeta_thread_t *thread, cmeta_thread_cb entry, void *arg) {
  struct cmeta_thread_wrapper_ctx *ctx;
  pthread_t *native;
  int rc;

  if (thread == NULL || entry == NULL) return -EINVAL;
  *thread = NULL;
  ctx = (struct cmeta_thread_wrapper_ctx *)malloc(sizeof(*ctx));
  if (ctx == NULL) return -ENOMEM;
  native = (pthread_t *)malloc(sizeof(*native));
  if (native == NULL) {
    free(ctx);
    return -ENOMEM;
  }
  ctx->entry = entry;
  ctx->arg = arg;
  rc = pthread_create(native, NULL, cmeta_thread_entry_wrapper_pthread, ctx);
  if (rc != 0) {
    free(native);
    free(ctx);
    return -rc;
  }
  *thread = native;
  return 0;
}

int cmeta_thread_join(cmeta_thread_t *thread) {
  pthread_t *native;
  int rc;
  if (thread == NULL || *thread == NULL) return -EINVAL;
  native = (pthread_t *)*thread;
  rc = pthread_join(*native, NULL);
  if (rc != 0) return -rc;
  free(native);
  *thread = NULL;
  return 0;
}

void cmeta_thread_destroy(cmeta_thread_t *thread) {
  pthread_t *native;
  if (thread == NULL || *thread == NULL) return;
  native = (pthread_t *)*thread;
  (void)pthread_detach(*native);
  free(native);
  *thread = NULL;
}

void cmeta_once(cmeta_once_t *guard, void (*callback)(void)) {
  if (guard == NULL || callback == NULL) return;
  /* pthread_once publishes all callback writes with OS-level synchronization.
   * Unlike a private atomic spin guard, the interceptable acquire/release
   * boundary remains visible when this library was not TSan-instrumented but
   * its first consumer was. No duplicate runtime or fallback path. */
  (void)pthread_once(&guard->native, callback);
}

void cmeta_sleep_ms(uint32_t ms) {
  struct timespec request;
  struct timespec remaining;
  request.tv_sec = (time_t)(ms / 1000U);
  request.tv_nsec = (long)(ms % 1000U) * 1000000L;
  while (nanosleep(&request, &remaining) != 0 && errno == EINTR)
    request = remaining;
}

void cmeta_thread_yield(void) { (void)sched_yield(); }

int cmeta_cpu_count(void) {
  long count = sysconf(_SC_NPROCESSORS_ONLN);
  return count > 0 ? (int)count : 4;
}
