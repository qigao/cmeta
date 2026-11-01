#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "io_native_internal.h"

#include <error_codes.h>
#include <thread.h>

#include <errno.h>
#include <fcntl.h>
#include <linux/io_uring.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <unistd.h>

#if defined(IOV_MAX)
_Static_assert(CFLOW_IO_NATIVE_VECTOR_MAX <= IOV_MAX,
               "vectored TCP requires the full fixed io_uring span budget");
#endif

typedef enum cflow_uring_record_phase {
    CFLOW_URING_RECORD_FREE = 0,
    CFLOW_URING_RECORD_PENDING
} cflow_uring_record_phase;

typedef enum cflow_uring_resource_kind {
    CFLOW_URING_RESOURCE_SOCKET = 0,
    CFLOW_URING_RESOURCE_FILE
} cflow_uring_resource_kind;

typedef struct cflow_uring_record {
    cflow_uring_record_phase phase;
    uint32_t index;
    uint32_t generation;
    uint64_t native_token;
    cflow_io_request_id request_id;
    cflow_io_actor *actor;
    cflow_uring_resource_kind resource_kind;
    cflow_io_native_operation *operation;
    cflow_io_native_file_operation *file_operation;
    struct iovec vector;
    struct iovec vector_buffers[CFLOW_IO_NATIVE_VECTOR_MAX];
    struct msghdr message;
    cflow_io_native_vector_operation_kind vector_kind;
    size_t vector_buffer_count;
    int vector_fd;
    struct sockaddr_storage peer_address;
    socklen_t peer_address_length;
    bool cancel_requested;
} cflow_uring_record;

typedef struct cflow_uring_impl {
    cflow_io_native_impl base;
    cmeta_mutex_t gate;
    cmeta_thread_t worker;
    cflow_uring_record *records;
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
    int ring_fd;
    void *sq_ring;
    void *cq_ring;
    struct io_uring_sqe *sqes;
    size_t sq_ring_size;
    size_t cq_ring_size;
    size_t sqes_size;
    unsigned *sq_head;
    unsigned *sq_tail;
    unsigned *sq_mask;
    unsigned *sq_entries;
    unsigned *sq_array;
    unsigned *cq_head;
    unsigned *cq_tail;
    unsigned *cq_mask;
    struct io_uring_cqe *cqes;
    bool single_mmap;
    bool admission_open;
    bool worker_running;
    bool stopping;
    bool shutdown_complete;
} cflow_uring_impl;

enum { CFLOW_URING_CANCEL_TOKEN = 0u, CFLOW_URING_STOP_TOKEN = 1u };

static uint64_t uring_make_record_token(uint32_t index,
                                        uint32_t generation) {
    return ((uint64_t)generation << 32u) | (uint64_t)index;
}

static uint32_t uring_next_generation(uint32_t generation) {
    return generation + 1u;
}

static void uring_counter_increment(uint64_t *counter) {
    if (*counter != UINT64_MAX)
        ++*counter;
}

static int uring_enter(cflow_uring_impl *impl, unsigned submit,
                       unsigned minimum, unsigned flags) {
    int status;
    do {
        status = (int)syscall(__NR_io_uring_enter, impl->ring_fd, submit,
                              minimum, flags, NULL, 0u);
    } while (status < 0 && errno == EINTR);
    return status < 0 ? -errno : status;
}

static cflow_uring_record *uring_find_free_locked(cflow_uring_impl *impl) {
    size_t index;
    for (index = 0u; index < impl->request_capacity; ++index) {
        if (impl->records[index].phase == CFLOW_URING_RECORD_FREE &&
            impl->records[index].generation != UINT32_MAX)
            return &impl->records[index];
    }
    return NULL;
}

static cflow_uring_record *uring_find_request_locked(
    cflow_uring_impl *impl, cflow_io_request_id request_id) {
    size_t index;
    for (index = 0u; index < impl->request_capacity; ++index) {
        if (impl->records[index].phase == CFLOW_URING_RECORD_PENDING &&
            impl->records[index].request_id == request_id)
            return &impl->records[index];
    }
    return NULL;
}

static int uring_publish_sqe_locked(cflow_uring_impl *impl,
                                    const struct io_uring_sqe *prepared) {
    unsigned head = atomic_load_explicit(
        (_Atomic unsigned *)impl->sq_head, memory_order_acquire);
    unsigned tail = atomic_load_explicit(
        (_Atomic unsigned *)impl->sq_tail, memory_order_relaxed);
    unsigned index;
    if (tail - head >= *impl->sq_entries)
        return SALTS_EBUSY;
    index = tail & *impl->sq_mask;
    impl->sqes[index] = *prepared;
    impl->sq_array[index] = index;
    atomic_store_explicit((_Atomic unsigned *)impl->sq_tail, tail + 1u,
                          memory_order_release);
    {
        const int submitted = uring_enter(impl, 1u, 0u, 0u);
        if (submitted == 1)
            return SALTS_OK;
        atomic_store_explicit((_Atomic unsigned *)impl->sq_tail, tail,
                              memory_order_release);
        return submitted < 0 ? submitted : SALTS_EIO;
    }
}

static void uring_prepare_operation(cflow_uring_record *record,
                                    struct io_uring_sqe *sqe) {
    cflow_io_native_operation *operation = record->operation;
    memset(sqe, 0, sizeof(*sqe));
    if (record->resource_kind == CFLOW_URING_RESOURCE_FILE) {
        cflow_io_native_file_operation *file_operation =
            record->file_operation;
        sqe->fd = (int)file_operation->handle;
        sqe->user_data = record->native_token;
        if (file_operation->kind == CFLOW_IO_NATIVE_FILE_FLUSH) {
            sqe->opcode = IORING_OP_FSYNC;
            sqe->fsync_flags = 0u;
        } else {
            sqe->opcode =
                file_operation->kind == CFLOW_IO_NATIVE_FILE_READ_AT
                    ? IORING_OP_READ : IORING_OP_WRITE;
            sqe->addr =
                (uint64_t)(uintptr_t)file_operation->buffer;
            sqe->len = (uint32_t)file_operation->length;
            sqe->off = file_operation->offset;
        }
        return;
    }
    if (record->vector_buffer_count != 0u) {
        memset(&record->message, 0, sizeof(record->message));
        record->message.msg_iov = record->vector_buffers;
        record->message.msg_iovlen = record->vector_buffer_count;
        sqe->fd = record->vector_fd;
        sqe->user_data = record->native_token;
        sqe->opcode = record->vector_kind == CFLOW_IO_NATIVE_TCP_RECV_VECTOR
                          ? IORING_OP_RECVMSG : IORING_OP_SENDMSG;
        sqe->addr = (uint64_t)(uintptr_t)&record->message;
        sqe->len = 1u;
#if defined(MSG_NOSIGNAL)
        if (record->vector_kind == CFLOW_IO_NATIVE_TCP_SEND_VECTOR)
            sqe->msg_flags = MSG_NOSIGNAL;
#endif
        return;
    }
    sqe->fd = (int)operation->socket;
    sqe->user_data = record->native_token;
    switch (operation->kind) {
        case CFLOW_IO_NATIVE_TCP_RECV:
            sqe->opcode = IORING_OP_RECV;
            sqe->addr = (uint64_t)(uintptr_t)operation->buffer;
            sqe->len = (uint32_t)operation->length;
            break;
        case CFLOW_IO_NATIVE_TCP_SEND:
            sqe->opcode = IORING_OP_SEND;
            sqe->addr = (uint64_t)(uintptr_t)operation->buffer;
            sqe->len = (uint32_t)operation->length;
#if defined(MSG_NOSIGNAL)
            sqe->msg_flags = MSG_NOSIGNAL;
#endif
            break;
        case CFLOW_IO_NATIVE_UDP_RECV_FROM:
            record->vector.iov_base = operation->buffer;
            record->vector.iov_len = operation->length;
            memset(&record->message, 0, sizeof(record->message));
            record->message.msg_name = operation->address;
            record->message.msg_namelen =
                (socklen_t)operation->address_capacity;
            record->message.msg_iov = &record->vector;
            record->message.msg_iovlen = 1u;
            sqe->opcode = IORING_OP_RECVMSG;
            sqe->addr = (uint64_t)(uintptr_t)&record->message;
            sqe->len = 1u;
            break;
        case CFLOW_IO_NATIVE_UDP_SEND_TO:
            record->vector.iov_base = operation->buffer;
            record->vector.iov_len = operation->length;
            memset(&record->message, 0, sizeof(record->message));
            record->message.msg_name = operation->address;
            record->message.msg_namelen =
                (socklen_t)operation->address_length;
            record->message.msg_iov = &record->vector;
            record->message.msg_iovlen = 1u;
            sqe->opcode = IORING_OP_SENDMSG;
            sqe->addr = (uint64_t)(uintptr_t)&record->message;
            sqe->len = 1u;
#if defined(MSG_NOSIGNAL)
            sqe->msg_flags = MSG_NOSIGNAL;
#endif
            break;
        case CFLOW_IO_NATIVE_TCP_ACCEPT:
            memset(&record->peer_address, 0,
                   sizeof(record->peer_address));
            record->peer_address_length =
                (socklen_t)sizeof(record->peer_address);
            sqe->opcode = IORING_OP_ACCEPT;
            sqe->addr =
                (uint64_t)(uintptr_t)&record->peer_address;
            sqe->addr2 =
                (uint64_t)(uintptr_t)&record->peer_address_length;
            sqe->accept_flags = SOCK_NONBLOCK | SOCK_CLOEXEC;
            break;
        case CFLOW_IO_NATIVE_TCP_CONNECT:
            sqe->opcode = IORING_OP_CONNECT;
            sqe->addr = (uint64_t)(uintptr_t)operation->address;
            sqe->off = (uint64_t)operation->address_length;
            break;
    }
}

static cflow_uring_record *uring_record_for_token(cflow_uring_impl *impl,
                                                  uint64_t token) {
    const uint32_t index = (uint32_t)token;
    const uint32_t generation = (uint32_t)(token >> 32u);
    if (generation == 0u || (size_t)index >= impl->request_capacity)
        return NULL;
    return &impl->records[index];
}

static void uring_finish(cflow_uring_impl *impl, uint64_t native_token,
                         int result) {
    cflow_uring_record *record;
    cflow_io_actor *actor;
    cflow_io_request_id request_id;
    cflow_io_native_operation *operation;
    cflow_io_native_file_operation *file_operation;
    cflow_uring_resource_kind resource_kind;
    cflow_io_native_vector_operation_kind vector_kind;
    size_t vector_buffer_count;
    cflow_io_completion completion;
    cflow_io_complete_status delivery_status;
    struct sockaddr_storage peer_address;
    socklen_t peer_address_length = 0u;
    int accepted_fd = -1;
    int effective_result = result;
    bool cancelled;

    cmeta_mutex_lock(&impl->gate);
    record = uring_record_for_token(impl, native_token);
    if (record == NULL || record->phase != CFLOW_URING_RECORD_PENDING ||
        record->native_token != native_token) {
        uring_counter_increment(&impl->stale_native_completions);
        cmeta_mutex_unlock(&impl->gate);
        return;
    }
    actor = record->actor;
    request_id = record->request_id;
    resource_kind = record->resource_kind;
    operation = record->operation;
    file_operation = record->file_operation;
    vector_kind = record->vector_kind;
    vector_buffer_count = record->vector_buffer_count;
    cancelled = record->cancel_requested && result == -ECANCELED;
    if (resource_kind == CFLOW_URING_RESOURCE_SOCKET &&
        vector_buffer_count == 0u &&
        operation->kind == CFLOW_IO_NATIVE_TCP_ACCEPT && result >= 0) {
        peer_address = record->peer_address;
        peer_address_length = record->peer_address_length;
    }
    if (resource_kind == CFLOW_URING_RESOURCE_SOCKET && result >= 0 &&
        vector_buffer_count == 0u &&
        operation->kind == CFLOW_IO_NATIVE_UDP_RECV_FROM)
        operation->address_length = (size_t)record->message.msg_namelen;
    record->phase = CFLOW_URING_RECORD_FREE;
    record->request_id = 0u;
    record->native_token = 0u;
    record->actor = NULL;
    record->resource_kind = CFLOW_URING_RESOURCE_SOCKET;
    record->operation = NULL;
    record->file_operation = NULL;
    record->vector_buffer_count = 0u;
    record->cancel_requested = false;
    --impl->active_requests;
    uring_counter_increment(&impl->completed);
    if (cancelled) uring_counter_increment(&impl->cancelled);
    cmeta_mutex_unlock(&impl->gate);

    if (resource_kind == CFLOW_URING_RESOURCE_SOCKET &&
        vector_buffer_count == 0u &&
        operation->kind == CFLOW_IO_NATIVE_TCP_ACCEPT && result >= 0) {
        accepted_fd = result;
        effective_result = SALTS_OK;
        if (operation->address != NULL) {
            if ((size_t)peer_address_length >
                operation->address_capacity) {
                effective_result = SALTS_ERANGE;
            } else {
                memcpy(operation->address, &peer_address,
                       (size_t)peer_address_length);
                operation->address_length =
                    (size_t)peer_address_length;
            }
        }
        if (effective_result == SALTS_OK)
            operation->result_socket = (uintptr_t)accepted_fd;
    }

    if (cancelled) {
        completion = (cflow_io_completion){
            CFLOW_IO_COMPLETION_CANCELLED, 0u, SALTS_OK};
    } else if (effective_result < 0) {
        completion = (cflow_io_completion){
            CFLOW_IO_COMPLETION_FAILED, 0u, effective_result};
    } else if (((vector_buffer_count != 0u &&
                  vector_kind == CFLOW_IO_NATIVE_TCP_RECV_VECTOR) ||
                (vector_buffer_count == 0u &&
                 resource_kind == CFLOW_URING_RESOURCE_SOCKET &&
                 operation->kind == CFLOW_IO_NATIVE_TCP_RECV) ||
                (resource_kind == CFLOW_URING_RESOURCE_FILE &&
                 file_operation->kind == CFLOW_IO_NATIVE_FILE_READ_AT)) &&
               result == 0) {
        completion = (cflow_io_completion){
            CFLOW_IO_COMPLETION_EOF, 0u, SALTS_OK};
    } else {
        completion = (cflow_io_completion){
            CFLOW_IO_COMPLETION_OK,
            vector_buffer_count == 0u &&
                    resource_kind == CFLOW_URING_RESOURCE_SOCKET &&
                    (operation->kind == CFLOW_IO_NATIVE_TCP_ACCEPT ||
                     operation->kind == CFLOW_IO_NATIVE_TCP_CONNECT)
                ? 0u : (size_t)effective_result,
            SALTS_OK};
    }
    delivery_status = cflow_io_actor_complete(
        actor, request_id, &completion);
    if (resource_kind == CFLOW_URING_RESOURCE_SOCKET &&
        vector_buffer_count == 0u && accepted_fd >= 0 &&
        (completion.kind != CFLOW_IO_COMPLETION_OK ||
         delivery_status != CFLOW_IO_COMPLETE_ACCEPTED)) {
        (void)close(accepted_fd);
        operation->result_socket = CFLOW_IO_NATIVE_INVALID_SOCKET;
        operation->address_length = 0u;
    }
}

static void uring_fail_all(cflow_uring_impl *impl, int status) {
    for (size_t index = 0u; index < impl->request_capacity; ++index) {
        cflow_uring_record *record = &impl->records[index];
        uint64_t native_token;
        cmeta_mutex_lock(&impl->gate);
        if (record->phase != CFLOW_URING_RECORD_PENDING) {
            cmeta_mutex_unlock(&impl->gate);
            continue;
        }
        native_token = record->native_token;
        cmeta_mutex_unlock(&impl->gate);
        uring_finish(impl, native_token, status);
    }
}

static void uring_worker(void *user) {
    cflow_uring_impl *impl = (cflow_uring_impl *)user;
    sigset_t blocked;
    int terminal_status = SALTS_OK;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGPIPE);
    terminal_status = pthread_sigmask(SIG_BLOCK, &blocked, NULL);
    if (terminal_status != 0) {
        terminal_status = -terminal_status;
        goto failed;
    }
    for (;;) {
        unsigned head;
        unsigned tail;
        size_t drained = 0u;
        int status = uring_enter(impl, 0u, 1u, IORING_ENTER_GETEVENTS);
        if (status < 0) {
            terminal_status = status;
            break;
        }
        head = atomic_load_explicit((_Atomic unsigned *)impl->cq_head,
                                    memory_order_relaxed);
        tail = atomic_load_explicit((_Atomic unsigned *)impl->cq_tail,
                                    memory_order_acquire);
        while (head != tail &&
               drained < impl->completion_batch_capacity) {
            struct io_uring_cqe *cqe =
                &impl->cqes[head & *impl->cq_mask];
            const uint64_t token = cqe->user_data;
            const int result = cqe->res;
            ++head;
            ++drained;
            atomic_store_explicit((_Atomic unsigned *)impl->cq_head, head,
                                  memory_order_release);
            if (token == CFLOW_URING_CANCEL_TOKEN)
                continue;
            if (token == CFLOW_URING_STOP_TOKEN)
                goto stopped;
            if (uring_record_for_token(impl, token) == NULL) {
                cmeta_mutex_lock(&impl->gate);
                uring_counter_increment(&impl->stale_native_completions);
                cmeta_mutex_unlock(&impl->gate);
                continue;
            }
            uring_finish(impl, token, result);
        }
    }
failed:
    cmeta_mutex_lock(&impl->gate);
    impl->admission_open = false;
    cmeta_mutex_unlock(&impl->gate);
    uring_fail_all(impl, terminal_status);

stopped:
    cmeta_mutex_lock(&impl->gate);
    impl->worker_running = false;
    cmeta_mutex_unlock(&impl->gate);
}

static int uring_submit_record(
    cflow_uring_impl *impl, cflow_io_actor *actor,
    cflow_io_request_id request_id, cflow_uring_resource_kind resource_kind,
    cflow_io_native_operation *operation,
    cflow_io_native_vector_operation *vector_operation,
    cflow_io_native_file_operation *file_operation) {
    cflow_uring_record *record;
    struct io_uring_sqe sqe;
    int status;
    cmeta_mutex_lock(&impl->gate);
    if (!impl->admission_open) {
        cmeta_mutex_unlock(&impl->gate);
        return SALTS_ESHUTDOWN;
    }
    record = uring_find_free_locked(impl);
    if (record == NULL) {
        const int capacity_status = impl->active_requests == 0u
                                        ? -EOVERFLOW : SALTS_EBUSY;
        uring_counter_increment(&impl->rejected_full);
        cmeta_mutex_unlock(&impl->gate);
        return capacity_status;
    }
    record->phase = CFLOW_URING_RECORD_PENDING;
    record->generation = uring_next_generation(record->generation);
    record->native_token = uring_make_record_token(
        record->index, record->generation);
    record->request_id = request_id;
    record->actor = actor;
    record->resource_kind = resource_kind;
    record->operation = operation;
    record->vector_buffer_count = 0u;
    if (vector_operation != NULL) {
        record->vector_kind = vector_operation->kind;
        record->vector_buffer_count = vector_operation->buffer_count;
        record->vector_fd = (int)vector_operation->socket;
        for (size_t index = 0u; index < record->vector_buffer_count; ++index) {
            record->vector_buffers[index].iov_base =
                vector_operation->buffers[index].data;
            record->vector_buffers[index].iov_len =
                vector_operation->buffers[index].length;
        }
    }
    record->file_operation = file_operation;
    record->cancel_requested = false;
    uring_prepare_operation(record, &sqe);
    status = uring_publish_sqe_locked(impl, &sqe);
    if (status == SALTS_OK) {
        ++impl->active_requests;
        uring_counter_increment(&impl->submitted);
    } else {
        record->phase = CFLOW_URING_RECORD_FREE;
        record->native_token = 0u;
        record->request_id = 0u;
        record->actor = NULL;
        record->resource_kind = CFLOW_URING_RESOURCE_SOCKET;
        record->operation = NULL;
        record->file_operation = NULL;
        record->vector_buffer_count = 0u;
        record->cancel_requested = false;
        uring_counter_increment(&impl->native_submit_errors);
    }
    cmeta_mutex_unlock(&impl->gate);
    return status;
}

static int uring_submit(cflow_io_native_impl *base,
                        cflow_io_actor *actor,
                        cflow_io_request_id request_id,
                        cflow_io_native_operation *operation) {
    cflow_uring_impl *impl = (cflow_uring_impl *)base;
    if (operation->socket > (uintptr_t)INT_MAX)
        return SALTS_EINVAL;
    if (operation->kind == CFLOW_IO_NATIVE_TCP_ACCEPT ||
        operation->kind == CFLOW_IO_NATIVE_TCP_CONNECT) {
        int flags;
        do {
            flags = fcntl((int)operation->socket, F_GETFL);
        } while (flags < 0 && errno == EINTR);
        if (flags < 0)
            return -errno;
        if ((flags & O_NONBLOCK) == 0)
            return SALTS_EINVAL;
    }
    return uring_submit_record(
        impl, actor, request_id, CFLOW_URING_RESOURCE_SOCKET,
        operation, NULL, NULL);
}

static int uring_submit_vector(
    cflow_io_native_impl *base, cflow_io_actor *actor,
    cflow_io_request_id request_id,
    cflow_io_native_vector_operation *operation) {
    cflow_uring_impl *impl = (cflow_uring_impl *)base;
    if (operation->socket > (uintptr_t)INT_MAX)
        return SALTS_EINVAL;
    return uring_submit_record(
        impl, actor, request_id, CFLOW_URING_RESOURCE_SOCKET,
        NULL, operation, NULL);
}

static int uring_submit_file(
    cflow_io_native_impl *base, cflow_io_actor *actor,
    cflow_io_request_id request_id,
    cflow_io_native_file_operation *operation) {
    cflow_uring_impl *impl = (cflow_uring_impl *)base;
    struct stat status_buffer;
    int status;
    if (operation->handle > (uintptr_t)INT_MAX)
        return SALTS_EINVAL;
    do {
        status = fstat((int)operation->handle, &status_buffer);
    } while (status < 0 && errno == EINTR);
    if (status < 0)
        return -errno;
    if (!S_ISREG(status_buffer.st_mode))
        return SALTS_EINVAL;
    return uring_submit_record(
        impl, actor, request_id, CFLOW_URING_RESOURCE_FILE,
        NULL, NULL, operation);
}

static int uring_cancel(cflow_io_native_impl *base,
                        cflow_io_request_id request_id) {
    cflow_uring_impl *impl = (cflow_uring_impl *)base;
    cflow_uring_record *record;
    struct io_uring_sqe sqe;
    int status;
    cmeta_mutex_lock(&impl->gate);
    record = uring_find_request_locked(impl, request_id);
    if (record == NULL) {
        cmeta_mutex_unlock(&impl->gate);
        return SALTS_ENOENT;
    }
    memset(&sqe, 0, sizeof(sqe));
    sqe.opcode = IORING_OP_ASYNC_CANCEL;
    sqe.fd = -1;
    sqe.addr = record->native_token;
    sqe.user_data = CFLOW_URING_CANCEL_TOKEN;
    status = uring_publish_sqe_locked(impl, &sqe);
    if (status == SALTS_OK)
        record->cancel_requested = true;
    else
        uring_counter_increment(&impl->native_cancel_errors);
    cmeta_mutex_unlock(&impl->gate);
    return status;
}

static bool uring_get_stats(const cflow_io_native_impl *base,
                            cflow_io_native_backend_stats *out) {
    cflow_uring_impl *impl = (cflow_uring_impl *)base;
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

static int uring_forget_socket(cflow_io_native_impl *base,
                               uintptr_t closed_socket) {
    cflow_uring_impl *impl = (cflow_uring_impl *)base;
    (void)closed_socket;
    cmeta_mutex_lock(&impl->gate);
    if (impl->active_requests != 0u) {
        cmeta_mutex_unlock(&impl->gate);
        return SALTS_EBUSY;
    }
    cmeta_mutex_unlock(&impl->gate);
    return SALTS_OK;
}

static int uring_forget_file(cflow_io_native_impl *base,
                             uintptr_t closed_handle) {
    return uring_forget_socket(base, closed_handle);
}

static int uring_shutdown(cflow_io_native_impl *base) {
    cflow_uring_impl *impl = (cflow_uring_impl *)base;
    struct io_uring_sqe sqe;
    bool wake_worker;
    int status;
    cmeta_mutex_lock(&impl->gate);
    if (impl->shutdown_complete) {
        cmeta_mutex_unlock(&impl->gate);
        return SALTS_EALREADY;
    }
    impl->admission_open = false;
    if (impl->active_requests != 0u) {
        cmeta_mutex_unlock(&impl->gate);
        return SALTS_EBUSY;
    }
    wake_worker = impl->worker_running;
    status = SALTS_OK;
    if (wake_worker) {
        memset(&sqe, 0, sizeof(sqe));
        sqe.opcode = IORING_OP_NOP;
        sqe.fd = -1;
        sqe.user_data = CFLOW_URING_STOP_TOKEN;
        status = uring_publish_sqe_locked(impl, &sqe);
    }
    cmeta_mutex_unlock(&impl->gate);
    if (status != SALTS_OK)
        return status;
    status = cmeta_thread_join(&impl->worker);
    if (status != SALTS_OK)
        return status;
    cmeta_thread_destroy(&impl->worker);
    cmeta_mutex_lock(&impl->gate);
    impl->shutdown_complete = true;
    cmeta_mutex_unlock(&impl->gate);
    return SALTS_OK;
}

static void uring_unmap(cflow_uring_impl *impl) {
    if (impl->sqes != MAP_FAILED && impl->sqes != NULL)
        (void)munmap(impl->sqes, impl->sqes_size);
    if (impl->cq_ring != MAP_FAILED && impl->cq_ring != NULL &&
        !impl->single_mmap)
        (void)munmap(impl->cq_ring, impl->cq_ring_size);
    if (impl->sq_ring != MAP_FAILED && impl->sq_ring != NULL)
        (void)munmap(impl->sq_ring, impl->sq_ring_size);
}

static int uring_destroy(cflow_io_native_impl *base) {
    cflow_uring_impl *impl = (cflow_uring_impl *)base;
    cmeta_mutex_lock(&impl->gate);
    if (!impl->shutdown_complete) {
        cmeta_mutex_unlock(&impl->gate);
        return SALTS_EBUSY;
    }
    cmeta_mutex_unlock(&impl->gate);
    uring_unmap(impl);
    (void)close(impl->ring_fd);
    cmeta_mutex_destroy(&impl->gate);
    free(impl->records);
    free(impl);
    return SALTS_OK;
}

static const cflow_io_native_impl_ops uring_ops = {
    .submit = uring_submit,
    .submit_vector = uring_submit_vector,
    .submit_file = uring_submit_file,
    .cancel = uring_cancel,
    .get_stats = uring_get_stats,
    .forget_socket = uring_forget_socket,
    .forget_file = uring_forget_file,
    .shutdown = uring_shutdown,
    .destroy = uring_destroy};

static bool uring_mapped_extent(size_t offset, size_t count,
                                size_t element_size, size_t *out) {
    if (element_size == 0u || count > (SIZE_MAX - offset) / element_size)
        return false;
    *out = offset + count * element_size;
    return true;
}

static bool uring_mapped_field_fits(size_t offset, size_t field_size,
                                    size_t mapped_size) {
    return offset <= mapped_size && field_size <= mapped_size - offset;
}

static void *uring_mapped_field(void *mapping, unsigned offset) {
    /* Linux supplies naturally aligned offsets for every io_uring field. */
    return (void *)((unsigned char *)mapping + (size_t)offset);
}

static int uring_map(cflow_uring_impl *impl, struct io_uring_params *params) {
    size_t shared_size;
    if (params->sq_entries == 0u || params->cq_entries == 0u ||
        !uring_mapped_extent(params->sq_off.array, params->sq_entries,
                             sizeof(unsigned), &impl->sq_ring_size) ||
        !uring_mapped_extent(params->cq_off.cqes, params->cq_entries,
                             sizeof(struct io_uring_cqe),
                             &impl->cq_ring_size) ||
        !uring_mapped_extent(0u, params->sq_entries,
                             sizeof(struct io_uring_sqe),
                             &impl->sqes_size) ||
        !uring_mapped_field_fits(params->sq_off.head, sizeof(unsigned),
                                 impl->sq_ring_size) ||
        !uring_mapped_field_fits(params->sq_off.tail, sizeof(unsigned),
                                 impl->sq_ring_size) ||
        !uring_mapped_field_fits(params->sq_off.ring_mask, sizeof(unsigned),
                                 impl->sq_ring_size) ||
        !uring_mapped_field_fits(params->sq_off.ring_entries,
                                 sizeof(unsigned), impl->sq_ring_size) ||
        !uring_mapped_field_fits(params->cq_off.head, sizeof(unsigned),
                                 impl->cq_ring_size) ||
        !uring_mapped_field_fits(params->cq_off.tail, sizeof(unsigned),
                                 impl->cq_ring_size) ||
        !uring_mapped_field_fits(params->cq_off.ring_mask, sizeof(unsigned),
                                 impl->cq_ring_size))
        return SALTS_ERANGE;
    impl->single_mmap = (params->features & IORING_FEAT_SINGLE_MMAP) != 0u;
    shared_size = impl->sq_ring_size > impl->cq_ring_size
                      ? impl->sq_ring_size : impl->cq_ring_size;
    if (impl->single_mmap) {
        impl->sq_ring_size = shared_size;
        impl->cq_ring_size = shared_size;
    }
    impl->sq_ring = mmap(NULL, impl->sq_ring_size, PROT_READ | PROT_WRITE,
                         MAP_SHARED | MAP_POPULATE, impl->ring_fd,
                         IORING_OFF_SQ_RING);
    if (impl->sq_ring == MAP_FAILED)
        return -errno;
    if (impl->single_mmap) {
        impl->cq_ring = impl->sq_ring;
    } else {
        impl->cq_ring = mmap(NULL, impl->cq_ring_size,
                             PROT_READ | PROT_WRITE,
                             MAP_SHARED | MAP_POPULATE, impl->ring_fd,
                             IORING_OFF_CQ_RING);
        if (impl->cq_ring == MAP_FAILED)
            return -errno;
    }
    impl->sqes = mmap(
        NULL, impl->sqes_size, PROT_READ | PROT_WRITE,
        MAP_SHARED | MAP_POPULATE, impl->ring_fd, IORING_OFF_SQES);
    if (impl->sqes == MAP_FAILED)
        return -errno;
    impl->sq_head = uring_mapped_field(impl->sq_ring, params->sq_off.head);
    impl->sq_tail = uring_mapped_field(impl->sq_ring, params->sq_off.tail);
    impl->sq_mask = uring_mapped_field(impl->sq_ring,
                                       params->sq_off.ring_mask);
    impl->sq_entries = uring_mapped_field(impl->sq_ring,
                                          params->sq_off.ring_entries);
    impl->sq_array = uring_mapped_field(impl->sq_ring,
                                        params->sq_off.array);
    impl->cq_head = uring_mapped_field(impl->cq_ring, params->cq_off.head);
    impl->cq_tail = uring_mapped_field(impl->cq_ring, params->cq_off.tail);
    impl->cq_mask = uring_mapped_field(impl->cq_ring,
                                       params->cq_off.ring_mask);
    impl->cqes = uring_mapped_field(impl->cq_ring, params->cq_off.cqes);
    return SALTS_OK;
}

int cflow_io_native_io_uring_init(
    cflow_io_native_backend *backend,
    const cflow_io_native_backend_config *config) {
    cflow_uring_impl *impl;
    struct io_uring_params params;
    unsigned entries;
    int status;
    if (config->kind != CFLOW_IO_NATIVE_IO_URING)
        return SALTS_ENOTSUP;
    if (config->request_capacity > UINT32_MAX / 2u ||
        config->request_capacity > SIZE_MAX / sizeof(cflow_uring_record))
        return SALTS_ERANGE;
    entries = (unsigned)config->request_capacity * 2u;
    impl = (cflow_uring_impl *)calloc(1u, sizeof(*impl));
    if (impl == NULL)
        return SALTS_ENOMEM;
    impl->records = (cflow_uring_record *)calloc(
        config->request_capacity, sizeof(*impl->records));
    if (impl->records == NULL) {
        free(impl);
        return SALTS_ENOMEM;
    }
    impl->ring_fd = -1;
    impl->base.ops = &uring_ops;
    impl->base.kind = config->kind;
    impl->request_capacity = config->request_capacity;
    impl->completion_batch_capacity = config->completion_batch_capacity;
    impl->admission_open = true;
    for (size_t index = 0u; index < impl->request_capacity; ++index)
        impl->records[index].index = (uint32_t)index;
    cmeta_mutex_init(&impl->gate);
    if (impl->gate == NULL) {
        free(impl->records);
        free(impl);
        return SALTS_ENOMEM;
    }
    memset(&params, 0, sizeof(params));
    impl->ring_fd = (int)syscall(__NR_io_uring_setup, entries, &params);
    if (impl->ring_fd < 0) {
        status = -errno;
        cmeta_mutex_destroy(&impl->gate);
        free(impl->records);
        free(impl);
        return status;
    }
    status = uring_map(impl, &params);
    if (status != SALTS_OK) {
        uring_unmap(impl);
        (void)close(impl->ring_fd);
        cmeta_mutex_destroy(&impl->gate);
        free(impl->records);
        free(impl);
        return status;
    }
    status = cmeta_thread_create(&impl->worker, uring_worker, impl);
    if (status != SALTS_OK) {
        uring_unmap(impl);
        (void)close(impl->ring_fd);
        cmeta_mutex_destroy(&impl->gate);
        free(impl->records);
        free(impl);
        return status;
    }
    impl->worker_running = true;
    backend->impl = impl;
    return SALTS_OK;
}
