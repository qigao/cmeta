#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE
#endif

#include "io_native_internal.h"

#include <error_codes.h>
#include <thread.h>

#include <aio.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

/* aio_suspend cannot be notified about a later submission. Bound the wait so
 * newly admitted requests enter the observer's next snapshot within 10 ms. */
enum { DARWIN_AIO_OBSERVE_INTERVAL_MS = 10 };

typedef struct cflow_darwin_aio_record {
    struct aiocb control;
    cflow_io_actor *actor;
    cflow_io_request_id request_id;
    cflow_io_native_file_operation_kind kind;
    bool active;
} cflow_darwin_aio_record;

typedef struct cflow_darwin_aio_impl {
    cflow_io_native_impl base;
    cmeta_mutex_t gate;
    cmeta_cond_t changed;
    cmeta_thread_t worker;
    cflow_darwin_aio_record *records;
    const struct aiocb **waiting;
    size_t request_capacity;
    size_t completion_batch_capacity;
    size_t active_requests;
    uint64_t submitted;
    uint64_t completed;
    uint64_t cancelled;
    uint64_t rejected_full;
    uint64_t stale_native_completions;
    uint64_t native_submit_errors;
    uint64_t native_cancel_errors;
    bool admission_open;
    bool worker_running;
    bool stopping;
    bool shutdown_inflight;
    bool shutdown_complete;
} cflow_darwin_aio_impl;

static void darwin_aio_increment(uint64_t *counter) {
    if (*counter != UINT64_MAX) ++*counter;
}

/* Only this thread calls aio_return or recycles control blocks. Cancellation
 * never releases a record: AIO_NOTCANCELED and AIO_ALLDONE still need reaping. */
static void darwin_aio_worker(void *user) {
    cflow_darwin_aio_impl *impl = (cflow_darwin_aio_impl *)user;
    const struct timespec timeout = {
        0, DARWIN_AIO_OBSERVE_INTERVAL_MS * 1000000L};
    size_t cursor = 0u;
    for (;;) {
        size_t delivered = 0u;
        size_t waiting_count = 0u;
        cmeta_mutex_lock(&impl->gate);
        while (impl->active_requests == 0u && !impl->stopping)
            cmeta_cond_wait(&impl->changed, &impl->gate);
        if (impl->stopping) {
            impl->worker_running = false;
            cmeta_mutex_unlock(&impl->gate);
            return;
        }
        cmeta_mutex_unlock(&impl->gate);

        for (size_t scanned = 0u; scanned < impl->request_capacity; ++scanned) {
            cflow_darwin_aio_record *record = &impl->records[cursor];
            cflow_io_actor *actor;
            cflow_io_request_id request_id;
            cflow_io_completion completion;
            int error;
            ssize_t bytes;
            cursor = (cursor + 1u) % impl->request_capacity;
            cmeta_mutex_lock(&impl->gate);
            if (!record->active) {
                cmeta_mutex_unlock(&impl->gate);
                continue;
            }
            error = aio_error(&record->control);
            if (error == EINPROGRESS) {
                cmeta_mutex_unlock(&impl->gate);
                continue;
            }
            if (error < 0) error = errno;
            /* Exactly one return for each accepted native operation, including
             * cancelled and failed operations, before this address is reused. */
            bytes = aio_return(&record->control);
            if (bytes < 0 && error == 0) error = errno;
            actor = record->actor;
            request_id = record->request_id;
            if (error == ECANCELED) {
                completion = (cflow_io_completion){
                    CFLOW_IO_COMPLETION_CANCELLED, 0u, SALTS_OK};
                darwin_aio_increment(&impl->cancelled);
            } else if (error != 0) {
                completion = (cflow_io_completion){
                    CFLOW_IO_COMPLETION_FAILED, 0u, -error};
            } else {
                completion = (cflow_io_completion){
                    record->kind == CFLOW_IO_NATIVE_FILE_READ_AT && bytes == 0
                        ? CFLOW_IO_COMPLETION_EOF : CFLOW_IO_COMPLETION_OK,
                    (size_t)bytes, SALTS_OK};
            }
            record->active = false;
            --impl->active_requests;
            darwin_aio_increment(&impl->completed);
            cmeta_mutex_unlock(&impl->gate);
            /* Actor completion may issue an advisory wake. No external call
             * runs under gate; shutdown joins this entire publication tail. */
            if (cflow_io_actor_complete(actor, request_id, &completion) !=
                CFLOW_IO_COMPLETE_ACCEPTED) {
                cmeta_mutex_lock(&impl->gate);
                darwin_aio_increment(&impl->stale_native_completions);
                cmeta_mutex_unlock(&impl->gate);
            }
            if (++delivered == impl->completion_batch_capacity) break;
        }
        if (delivered != 0u) continue;

        cmeta_mutex_lock(&impl->gate);
        for (size_t index = 0u; index < impl->request_capacity; ++index)
            if (impl->records[index].active)
                impl->waiting[waiting_count++] = &impl->records[index].control;
        cmeta_mutex_unlock(&impl->gate);
        if (waiting_count != 0u &&
            aio_suspend(impl->waiting, (int)waiting_count, &timeout) < 0 &&
            errno != EAGAIN && errno != EINTR) {
            /* Waiting is advisory, never an I/O terminal result. A wait error
             * cannot authorize reclaiming kernel-borrowed buffers. Recheck
             * aio_error after a bounded delay without spinning on wait errors. */
            cmeta_sleep_ms(DARWIN_AIO_OBSERVE_INTERVAL_MS);
        }
    }
}

static int darwin_aio_submit_file(
    cflow_io_native_impl *base, cflow_io_actor *actor,
    cflow_io_request_id request_id, cflow_io_native_file_operation *operation) {
    cflow_darwin_aio_impl *impl = (cflow_darwin_aio_impl *)base;
    cflow_darwin_aio_record *record = NULL;
    struct stat file_status;
    int status;
    if (operation->handle > (uintptr_t)INT_MAX) return SALTS_EINVAL;
    do {
        status = fstat((int)operation->handle, &file_status);
    } while (status < 0 && errno == EINTR);
    if (status < 0) return -errno;
    if (!S_ISREG(file_status.st_mode)) return SALTS_EINVAL;

    cmeta_mutex_lock(&impl->gate);
    if (!impl->admission_open) {
        cmeta_mutex_unlock(&impl->gate);
        return SALTS_ESHUTDOWN;
    }
    for (size_t index = 0u; index < impl->request_capacity; ++index) {
        if (!impl->records[index].active) {
            record = &impl->records[index];
            break;
        }
    }
    if (record == NULL) {
        darwin_aio_increment(&impl->rejected_full);
        cmeta_mutex_unlock(&impl->gate);
        return SALTS_ENOBUFS;
    }
    memset(&record->control, 0, sizeof(record->control));
    record->control.aio_fildes = (int)operation->handle;
    record->control.aio_offset = (off_t)operation->offset;
    record->control.aio_buf = operation->buffer;
    record->control.aio_nbytes = operation->length;
    record->control.aio_sigevent.sigev_notify = SIGEV_NONE;
    if (operation->kind == CFLOW_IO_NATIVE_FILE_FLUSH)
        status = aio_fsync(O_SYNC, &record->control);
    else if (operation->kind == CFLOW_IO_NATIVE_FILE_READ_AT)
        status = aio_read(&record->control);
    else
        status = aio_write(&record->control);
    if (status < 0) {
        status = -errno;
        darwin_aio_increment(&impl->native_submit_errors);
        cmeta_mutex_unlock(&impl->gate);
        return status;
    }
    record->actor = actor;
    record->request_id = request_id;
    record->kind = operation->kind;
    record->active = true;
    ++impl->active_requests;
    darwin_aio_increment(&impl->submitted);
    cmeta_cond_signal(&impl->changed);
    cmeta_mutex_unlock(&impl->gate);
    return SALTS_OK;
}

static int darwin_aio_cancel(cflow_io_native_impl *base,
                             cflow_io_request_id request_id) {
    cflow_darwin_aio_impl *impl = (cflow_darwin_aio_impl *)base;
    int status = SALTS_ENOENT;
    cmeta_mutex_lock(&impl->gate);
    for (size_t index = 0u; index < impl->request_capacity; ++index) {
        cflow_darwin_aio_record *record = &impl->records[index];
        if (!record->active || record->request_id != request_id) continue;
        status = aio_cancel(record->control.aio_fildes, &record->control);
        if (status < 0) {
            status = -errno;
            darwin_aio_increment(&impl->native_cancel_errors);
        } else {
            status = SALTS_OK;
        }
        break;
    }
    cmeta_mutex_unlock(&impl->gate);
    return status;
}

static bool darwin_aio_get_stats(const cflow_io_native_impl *base,
                                 cflow_io_native_backend_stats *out) {
    cflow_darwin_aio_impl *impl = (cflow_darwin_aio_impl *)base;
    cmeta_mutex_lock(&impl->gate);
    *out = (cflow_io_native_backend_stats){
        impl->request_capacity, impl->active_requests, impl->submitted,
        impl->completed, impl->cancelled, impl->rejected_full,
        impl->stale_native_completions, impl->native_submit_errors,
        impl->native_cancel_errors, impl->admission_open,
        impl->worker_running, impl->shutdown_complete};
    cmeta_mutex_unlock(&impl->gate);
    return true;
}

static int darwin_aio_forget_file(cflow_io_native_impl *base, uintptr_t handle) {
    cflow_darwin_aio_impl *impl = (cflow_darwin_aio_impl *)base;
    int status = SALTS_OK;
    cmeta_mutex_lock(&impl->gate);
    for (size_t index = 0u; index < impl->request_capacity; ++index) {
        if (impl->records[index].active &&
            (uintptr_t)impl->records[index].control.aio_fildes == handle) {
            status = SALTS_EBUSY;
            break;
        }
    }
    cmeta_mutex_unlock(&impl->gate);
    return status;
}

static int darwin_aio_shutdown(cflow_io_native_impl *base) {
    cflow_darwin_aio_impl *impl = (cflow_darwin_aio_impl *)base;
    int status;
    cmeta_mutex_lock(&impl->gate);
    if (impl->shutdown_complete) {
        cmeta_mutex_unlock(&impl->gate);
        return SALTS_EALREADY;
    }
    impl->admission_open = false;
    if (impl->active_requests != 0u || impl->shutdown_inflight) {
        cmeta_mutex_unlock(&impl->gate);
        return SALTS_EBUSY;
    }
    impl->shutdown_inflight = true;
    impl->stopping = true;
    cmeta_cond_signal(&impl->changed);
    cmeta_mutex_unlock(&impl->gate);
    status = cmeta_thread_join(&impl->worker);
    if (status == SALTS_OK) cmeta_thread_destroy(&impl->worker);
    cmeta_mutex_lock(&impl->gate);
    impl->shutdown_inflight = false;
    impl->shutdown_complete = status == SALTS_OK;
    cmeta_mutex_unlock(&impl->gate);
    return status;
}

static int darwin_aio_destroy(cflow_io_native_impl *base) {
    cflow_darwin_aio_impl *impl = (cflow_darwin_aio_impl *)base;
    cmeta_mutex_lock(&impl->gate);
    if (!impl->shutdown_complete) {
        cmeta_mutex_unlock(&impl->gate);
        return SALTS_EBUSY;
    }
    cmeta_mutex_unlock(&impl->gate);
    cmeta_cond_destroy(&impl->changed);
    cmeta_mutex_destroy(&impl->gate);
    free(impl->waiting);
    free(impl->records);
    free(impl);
    return SALTS_OK;
}

static const cflow_io_native_impl_ops darwin_aio_ops = {
    .submit_file = darwin_aio_submit_file,
    .cancel = darwin_aio_cancel,
    .get_stats = darwin_aio_get_stats,
    .forget_file = darwin_aio_forget_file,
    .shutdown = darwin_aio_shutdown,
    .destroy = darwin_aio_destroy};

int cflow_io_native_darwin_aio_init(
    cflow_io_native_backend *backend,
    const cflow_io_native_backend_config *config) {
    cflow_darwin_aio_impl *impl;
    int status;
    if (config->kind != CFLOW_IO_NATIVE_DARWIN_AIO) return SALTS_ENOTSUP;
    if (config->request_capacity > INT_MAX ||
        config->request_capacity > SIZE_MAX / sizeof(*impl->records) ||
        config->request_capacity > SIZE_MAX / sizeof(*impl->waiting))
        return SALTS_ERANGE;
    impl = (cflow_darwin_aio_impl *)calloc(1u, sizeof(*impl));
    if (impl == NULL) return SALTS_ENOMEM;
    impl->records = (cflow_darwin_aio_record *)calloc(
        config->request_capacity, sizeof(*impl->records));
    impl->waiting = (const struct aiocb **)calloc(
        config->request_capacity, sizeof(*impl->waiting));
    cmeta_mutex_init(&impl->gate);
    cmeta_cond_init(&impl->changed);
    status = SALTS_ENOMEM;
    if (impl->records == NULL || impl->waiting == NULL ||
        impl->gate == NULL || impl->changed == NULL)
        goto failed;
    impl->base.ops = &darwin_aio_ops;
    impl->base.kind = config->kind;
    impl->request_capacity = config->request_capacity;
    impl->completion_batch_capacity = config->completion_batch_capacity;
    impl->admission_open = true;
    impl->worker_running = true;
    status = cmeta_thread_create(&impl->worker, darwin_aio_worker, impl);
    if (status != SALTS_OK) goto failed;
    backend->impl = impl;
    return SALTS_OK;
failed:
    if (impl->changed != NULL) cmeta_cond_destroy(&impl->changed);
    if (impl->gate != NULL) cmeta_mutex_destroy(&impl->gate);
    free(impl->waiting);
    free(impl->records);
    free(impl);
    return status;
}
