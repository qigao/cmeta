#include "io_publisher_internal.h"

#include <stdlib.h>
#include <string.h>

enum {
    CFLOW_IO_PUBLISHER_CAPACITY = 1,
    CFLOW_IO_PUBLISHER_LEASE_ID = 1
};

typedef struct io_source_wake_frame {
    cflow_io_publisher_state *state;
    struct io_source_wake_frame *previous;
} io_source_wake_frame;

static const char io_source_unavailable_error[] =
    "IO source is unavailable";
static const char io_source_cancelled_error[] =
    "IO source is cancelled";
static const char io_source_prepare_error[] =
    "IO source preparation failed";
static const char io_source_prepare_status_error[] =
    "IO source preparation returned an invalid status";
static const char io_source_operation_error[] =
    "IO source operation is missing a release callback";
static const char io_source_submit_full_error[] =
    "IO source Actor admission is full";
static const char io_source_submit_closed_error[] =
    "IO source Actor admission is closed";
static const char io_source_submit_lease_error[] =
    "IO source Actor lease is already active";
static const char io_source_submit_id_error[] =
    "IO source Actor request IDs are exhausted";
static const char io_source_submit_invalid_error[] =
    "IO source Actor rejected the operation";
static const char io_source_encode_error[] =
    "IO source completion encoding failed";
static const char io_source_encode_status_error[] =
    "IO source completion encoder returned WOULD_BLOCK";
static const char io_source_encode_invalid_status_error[] =
    "IO source completion encoder returned an invalid status";
static const char io_source_result_state_error[] =
    "IO source completion result state is invalid";

static SALTS_THREAD_LOCAL io_source_wake_frame *io_source_wake_stack;

static cflow_waitable io_source_waitable_as_cflow_waitable(void *self);
static const char *io_source_submit_error(cflow_io_submit_status status);

static bool io_source_type_valid(const cmeta_type_desc *type) {
    return cmeta_type_desc_valid(type) &&
           cflow_value_storage_type_supported(type) &&
           type->size != 0u && type->align != 0u &&
           (type->align & (type->align - 1u)) == 0u &&
           type->size <= SIZE_MAX - (type->align - 1u);
}

static bool io_source_windowed(const cflow_io_publisher_state *state) {
    return state->window_capacity > 1u;
}

static cflow_io_publisher_entry *io_source_window_find_free_locked(
    cflow_io_publisher_state *state) {
    size_t index;

    for (index = 0u; index < state->window_capacity; ++index) {
        if (!state->entries[index].occupied)
            return &state->entries[index];
    }
    return NULL;
}

static cflow_io_publisher_entry *io_source_window_find_ready_locked(
    cflow_io_publisher_state *state) {
    cflow_io_publisher_entry *ready = NULL;
    size_t index;

    for (index = 0u; index < state->window_capacity; ++index) {
        cflow_io_publisher_entry *candidate = &state->entries[index];
        if (candidate->result_ready &&
            (ready == NULL || candidate->delivery_sequence <
                                  ready->delivery_sequence))
            ready = candidate;
    }
    return ready;
}

static cflow_io_publisher_entry *io_source_window_find_delivered_locked(
    cflow_io_publisher_state *state) {
    cflow_io_publisher_entry *delivered = NULL;
    size_t index;

    for (index = 0u; index < state->window_capacity; ++index) {
        cflow_io_publisher_entry *candidate = &state->entries[index];
        if (candidate->occupied && candidate->completion_delivered &&
            !candidate->acknowledged && candidate->request_id != 0u &&
            (delivered == NULL || candidate->request_id <
                                      delivered->request_id))
            delivered = candidate;
    }
    return delivered;
}

static void io_source_window_release_entry_locked(
    cflow_io_publisher_state *state, cflow_io_publisher_entry *entry) {
    if (entry == NULL || !entry->occupied)
        return;
    cflow_value_slot_reset(&entry->result);
    entry->request_id = 0u;
    entry->delivery_sequence = 0u;
    entry->result_status = CFLOW_READ_WOULD_BLOCK;
    entry->result_error = NULL;
    entry->occupied = false;
    entry->submission_in_progress = false;
    entry->result_encoding = false;
    entry->result_ready = false;
    entry->completion_delivered = false;
    entry->acknowledged = false;
    entry->demand_reserved = false;
    --state->window_occupied;
}

static void io_source_window_discard_result_locked(
    cflow_io_publisher_state *state, cflow_io_publisher_entry *entry) {
    if (entry == NULL || !entry->occupied)
        return;
    if (entry->result_ready) {
        cflow_value_slot_reset(&entry->result);
        entry->result_ready = false;
        entry->result_error = NULL;
        --state->window_results_ready;
    }
    if (entry->completion_delivered && entry->demand_reserved) {
        entry->demand_reserved = false;
        --state->window_demand_reserved;
    }
    if (entry->acknowledged && !entry->result_encoding &&
        !entry->demand_reserved)
        io_source_window_release_entry_locked(state, entry);
}

static cflow_step io_source_terminal_step(
    cflow_io_publisher_state *state) {
    if (state->terminal == CFLOW_PUBLISHER_DONE)
        return (cflow_step){CFLOW_STEP_DONE, {0}, NULL};
    if (state->terminal == CFLOW_PUBLISHER_ERROR)
        return (cflow_step){CFLOW_STEP_ERROR, {0},
                            state->terminal_error};
    return (cflow_step){CFLOW_STEP_ERROR, {0},
                        io_source_unavailable_error};
}

static cflow_step io_source_set_terminal(
    cflow_io_publisher_state *state,
    cflow_publisher_terminal terminal,
    const char *error) {
    cflow_step step;

    sync_mutex_lock(&state->gate);
    state->submission_in_progress = false;
    if (state->terminal == CFLOW_PUBLISHER_OPEN) {
        state->terminal = terminal;
        state->terminal_error = error;
    }
    step = io_source_terminal_step(state);
    sync_mutex_unlock(&state->gate);
    return step;
}

static cflow_waker io_source_take_waker_locked(
    cflow_io_publisher_state *state) {
    cflow_waker waker = state->source_waker;

    state->source_waker = (cflow_waker){0};
    if (waker.wake != NULL)
        ++state->wake_inflight;
    return waker;
}

static void io_source_retain_waker_locked(
    cflow_io_publisher_state *state, cflow_waker waker) {
    if (waker.wake != NULL)
        ++state->wake_inflight;
}

static void io_source_wake(
    cflow_io_publisher_state *state, cflow_waker waker) {
    io_source_wake_frame frame;

    if (waker.wake == NULL)
        return;
    frame.state = state;
    frame.previous = io_source_wake_stack;
    io_source_wake_stack = &frame;
    waker.wake(waker.user);
    io_source_wake_stack = frame.previous;
    sync_mutex_lock(&state->gate);
    --state->wake_inflight;
    if (state->wake_waiters != 0u)
        sync_cond_broadcast(&state->changed);
    sync_mutex_unlock(&state->gate);
}

static void io_source_wait_wakers_locked(
    cflow_io_publisher_state *state) {
    io_source_wake_frame *frame = io_source_wake_stack;
    size_t current_thread_credits = 0u;

    while (frame != NULL) {
        if (frame->state == state)
            ++current_thread_credits;
        frame = frame->previous;
    }
    /* These frames cannot return their credits until cancel unwinds. */
    while (state->wake_inflight > current_thread_credits) {
        ++state->wake_waiters;
        sync_cond_wait(&state->changed, &state->gate);
        --state->wake_waiters;
    }
}

static bool io_source_record_drive_locked(
    cflow_io_publisher_state *state) {
    if (state->drive_pending)
        return false;
    state->drive_pending = true;
    /* The coalesced credit remains authoritative after the generation
       saturates, so counter exhaustion cannot lose a future edge. */
    if (state->drive_generation != UINT64_MAX)
        ++state->drive_generation;
    return true;
}

static bool io_source_retain_drive_locked(
    cflow_io_publisher_state *state,
    cflow_io_wake_fn *drive,
    void **drive_user) {
    if (!state->owner_live || state->drive == NULL)
        return false;
    ++state->drive_inflight;
    *drive = state->drive;
    *drive_user = state->drive_user;
    return true;
}

static void io_source_invoke_drive(
    cflow_io_publisher_state *state,
    cflow_io_wake_fn drive,
    void *drive_user) {
    if (drive == NULL)
        return;
    drive(drive_user);
    sync_mutex_lock(&state->gate);
    --state->drive_inflight;
    sync_mutex_unlock(&state->gate);
}

static void io_source_actor_drive(void *user) {
    cflow_io_publisher_state *state =
        (cflow_io_publisher_state *)user;
    cflow_io_wake_fn drive = NULL;
    void *drive_user = NULL;

    if (state == NULL)
        return;
    if (state->drive == NULL &&
        !atomic_load_explicit(&state->driver_active, memory_order_acquire))
        return;
    sync_mutex_lock(&state->gate);
    if (io_source_record_drive_locked(state) &&
        !atomic_load_explicit(&state->driver_active, memory_order_relaxed))
        (void)io_source_retain_drive_locked(
            state, &drive, &drive_user);
    sync_mutex_unlock(&state->gate);
    io_source_invoke_drive(state, drive, drive_user);
}

static bool io_source_take_result_locked(
    cflow_io_publisher_state *state,
    void *out_value,
    cflow_step *step) {
    cflow_read_status status;
    const char *error;

    if (!state->result_ready)
        return false;
    status = state->result_status;
    error = state->result_error;
    state->result_ready = false;
    state->result_error = NULL;
    if (state->request_id == 0u)
        state->acknowledged = false;

    if (status == CFLOW_READ_VALUE ||
        status == CFLOW_READ_VALUE_AND_DONE) {
        if (!state->result.live) {
            state->terminal = CFLOW_PUBLISHER_ERROR;
            state->terminal_error = io_source_result_state_error;
            *step = io_source_terminal_step(state);
            return true;
        }
        memcpy(out_value, state->result.storage, state->type->size);
        cflow_value_slot_reset(&state->result);
        if (status == CFLOW_READ_VALUE_AND_DONE)
            state->terminal = CFLOW_PUBLISHER_DONE;
        *step = (cflow_step){
            status == CFLOW_READ_VALUE_AND_DONE
                ? CFLOW_STEP_VALUE_AND_DONE : CFLOW_STEP_VALUE,
            {0}, NULL};
        return true;
    }

    cflow_value_slot_reset(&state->result);
    if (status == CFLOW_READ_DONE) {
        state->terminal = CFLOW_PUBLISHER_DONE;
        *step = (cflow_step){CFLOW_STEP_DONE, {0}, NULL};
    } else {
        state->terminal = CFLOW_PUBLISHER_ERROR;
        state->terminal_error = error != NULL
            ? error : io_source_encode_error;
        *step = io_source_terminal_step(state);
    }
    return true;
}

static bool io_source_window_take_result_locked(
    cflow_io_publisher_state *state, void *out_value,
    cflow_step *step, bool *close_actor) {
    cflow_io_publisher_entry *entry =
        io_source_window_find_ready_locked(state);
    cflow_read_status status;
    const char *error;

    if (entry == NULL)
        return false;
    status = entry->result_status;
    error = entry->result_error;
    entry->result_ready = false;
    entry->result_error = NULL;
    --state->window_results_ready;
    if (entry->demand_reserved) {
        entry->demand_reserved = false;
        --state->window_demand_reserved;
    }

    if (status == CFLOW_READ_VALUE ||
        status == CFLOW_READ_VALUE_AND_DONE) {
        if (!entry->result.live) {
            state->terminal = CFLOW_PUBLISHER_ERROR;
            state->terminal_error = io_source_result_state_error;
            *close_actor = true;
            *step = io_source_terminal_step(state);
        } else {
            memcpy(out_value, entry->result.storage, state->type->size);
            cflow_value_slot_reset(&entry->result);
            if (status == CFLOW_READ_VALUE_AND_DONE) {
                state->terminal = CFLOW_PUBLISHER_DONE;
                *close_actor = true;
            }
            *step = (cflow_step){
                status == CFLOW_READ_VALUE_AND_DONE
                    ? CFLOW_STEP_VALUE_AND_DONE : CFLOW_STEP_VALUE,
                {0}, NULL};
        }
    } else {
        cflow_value_slot_reset(&entry->result);
        if (status == CFLOW_READ_DONE) {
            state->terminal = CFLOW_PUBLISHER_DONE;
            *step = (cflow_step){CFLOW_STEP_DONE, {0}, NULL};
        } else {
            state->terminal = CFLOW_PUBLISHER_ERROR;
            state->terminal_error = error != NULL
                ? error : io_source_encode_error;
            *step = io_source_terminal_step(state);
        }
        *close_actor = true;
    }
    if (entry->acknowledged)
        io_source_window_release_entry_locked(state, entry);
    return true;
}

static cflow_step io_source_window_resume(
    cflow_io_publisher_state *state, cflow_publish_context *ctx,
    void *out_value) {
    const size_t hinted = ctx != NULL && ctx->downstream_demand != 0u
        ? ctx->downstream_demand : 1u;
    const size_t target = hinted < state->window_capacity
        ? hinted : state->window_capacity;

    for (;;) {
        cflow_io_publisher_entry *entry;
        cflow_io_operation operation = {0};
        cflow_io_submit_result submitted;
        cflow_io_publisher_prepare_status prepared;
        const char *error = NULL;
        cflow_step step;
        bool close_actor = false;

        sync_mutex_lock(&state->gate);
        if (state->terminal != CFLOW_PUBLISHER_OPEN) {
            step = io_source_terminal_step(state);
            sync_mutex_unlock(&state->gate);
            return step;
        }
        if (state->close_requested || !state->publisher_live) {
            sync_mutex_unlock(&state->gate);
            return io_source_set_terminal(
                state, CFLOW_PUBLISHER_ERROR, io_source_cancelled_error);
        }
        if (io_source_window_take_result_locked(
                state, out_value, &step, &close_actor)) {
            sync_mutex_unlock(&state->gate);
            if (close_actor)
                (void)cflow_io_actor_close(&state->actor);
            return step;
        }
        if (state->prepare_done) {
            if (state->window_demand_reserved == 0u) {
                state->terminal = CFLOW_PUBLISHER_DONE;
                step = io_source_terminal_step(state);
                sync_mutex_unlock(&state->gate);
                return step;
            }
            sync_mutex_unlock(&state->gate);
            return (cflow_step){
                CFLOW_STEP_WAIT,
                io_source_waitable_as_cflow_waitable(state), NULL};
        }
        if (state->window_demand_reserved >= target ||
            state->window_occupied >= state->window_capacity) {
            sync_mutex_unlock(&state->gate);
            return (cflow_step){
                CFLOW_STEP_WAIT,
                io_source_waitable_as_cflow_waitable(state), NULL};
        }
        entry = io_source_window_find_free_locked(state);
        if (entry == NULL) {
            state->terminal = CFLOW_PUBLISHER_ERROR;
            state->terminal_error = io_source_result_state_error;
            step = io_source_terminal_step(state);
            sync_mutex_unlock(&state->gate);
            (void)cflow_io_actor_close(&state->actor);
            return step;
        }
        entry->occupied = true;
        entry->submission_in_progress = true;
        ++state->window_occupied;
        if (state->window_occupied > state->window_peak_occupied)
            state->window_peak_occupied = state->window_occupied;
        state->submission_in_progress = true;
        sync_mutex_unlock(&state->gate);

        prepared = state->prepare(state->user, &operation, &error);
        if (prepared == CFLOW_IO_PUBLISHER_PREPARE_DONE) {
            sync_mutex_lock(&state->gate);
            io_source_window_release_entry_locked(state, entry);
            state->submission_in_progress = false;
            state->prepare_done = true;
            sync_mutex_unlock(&state->gate);
            continue;
        }
        if (prepared != CFLOW_IO_PUBLISHER_PREPARE_OPERATION ||
            operation.release == NULL) {
            const char *terminal_error = prepared ==
                    CFLOW_IO_PUBLISHER_PREPARE_ERROR
                ? (error != NULL && error[0] != '\0'
                       ? error : io_source_prepare_error)
                : (prepared == CFLOW_IO_PUBLISHER_PREPARE_OPERATION
                       ? io_source_operation_error
                       : io_source_prepare_status_error);
            sync_mutex_lock(&state->gate);
            io_source_window_release_entry_locked(state, entry);
            state->submission_in_progress = false;
            state->terminal = CFLOW_PUBLISHER_ERROR;
            state->terminal_error = terminal_error;
            step = io_source_terminal_step(state);
            sync_mutex_unlock(&state->gate);
            (void)cflow_io_actor_close(&state->actor);
            return step;
        }

        sync_mutex_lock(&state->gate);
        entry->demand_reserved = true;
        ++state->window_demand_reserved;
        sync_mutex_unlock(&state->gate);
        submitted = cflow_io_actor_try_submit(
            &state->actor, entry->lease_id, &operation);
        if (submitted.status != CFLOW_IO_SUBMIT_ACCEPTED) {
            const char *submit_error =
                io_source_submit_error(submitted.status);
            operation.release(operation.user);
            sync_mutex_lock(&state->gate);
            --state->window_demand_reserved;
            io_source_window_release_entry_locked(state, entry);
            state->submission_in_progress = false;
            state->terminal = CFLOW_PUBLISHER_ERROR;
            state->terminal_error = submit_error;
            step = io_source_terminal_step(state);
            sync_mutex_unlock(&state->gate);
            (void)cflow_io_actor_close(&state->actor);
            return step;
        }

        sync_mutex_lock(&state->gate);
        entry->submission_in_progress = false;
        if (entry->request_id == 0u)
            entry->request_id = submitted.request_id;
        else if (entry->request_id != submitted.request_id) {
            state->terminal = CFLOW_PUBLISHER_ERROR;
            state->terminal_error = io_source_result_state_error;
        }
        state->submission_in_progress = false;
        sync_mutex_unlock(&state->gate);
    }
}

static bool io_source_wait_arm(void *self, cflow_waker waker) {
    cflow_io_publisher_state *state = (cflow_io_publisher_state *)self;
    cflow_waker wake_now = {0};
    bool ready;
    bool waitable;

    if (state == NULL || waker.wake == NULL)
        return false;
    sync_mutex_lock(&state->gate);
    if (!state->publisher_live || state->close_requested) {
        sync_mutex_unlock(&state->gate);
        return false;
    }
    ready = io_source_windowed(state)
        ? state->window_results_ready != 0u ||
              (state->prepare_done &&
               state->window_demand_reserved == 0u)
        : state->result_ready || state->acknowledged;
    waitable = io_source_windowed(state)
        ? state->window_occupied != 0u
        : state->request_id != 0u;
    if (state->terminal != CFLOW_PUBLISHER_OPEN || ready) {
        wake_now = waker;
        io_source_retain_waker_locked(state, wake_now);
    } else if (!waitable || state->source_waker.wake != NULL) {
        sync_mutex_unlock(&state->gate);
        return false;
    } else {
        state->source_waker = waker;
    }
    sync_mutex_unlock(&state->gate);
    io_source_wake(state, wake_now);
    return true;
}

static void io_source_wait_cancel(void *self) {
    cflow_io_publisher_state *state = (cflow_io_publisher_state *)self;
    bool close_actor = false;
    size_t index;

    if (state == NULL)
        return;
    sync_mutex_lock(&state->gate);
    state->source_waker = (cflow_waker){0};
    if (!state->close_requested) {
        state->close_requested = true;
        close_actor = true;
    }
    if (io_source_windowed(state)) {
        for (index = 0u; index < state->window_capacity; ++index)
            io_source_window_discard_result_locked(
                state, &state->entries[index]);
    }
    sync_mutex_unlock(&state->gate);
    if (close_actor)
        (void)cflow_io_actor_close(&state->actor);
    sync_mutex_lock(&state->gate);
    io_source_wait_wakers_locked(state);
    sync_mutex_unlock(&state->gate);
}

CMETA_IMPLEMENTS(cflow_waitable, io_source_waitable, 0,
    .arm = io_source_wait_arm,
    .cancel = io_source_wait_cancel
);

static const char *io_source_submit_error(cflow_io_submit_status status) {
    switch (status) {
        case CFLOW_IO_SUBMIT_FULL:
            return io_source_submit_full_error;
        case CFLOW_IO_SUBMIT_CLOSED:
            return io_source_submit_closed_error;
        case CFLOW_IO_SUBMIT_LEASE_IN_USE:
            return io_source_submit_lease_error;
        case CFLOW_IO_SUBMIT_ID_EXHAUSTED:
            return io_source_submit_id_error;
        case CFLOW_IO_SUBMIT_INVALID_ARGUMENT:
        case CFLOW_IO_SUBMIT_ACCEPTED:
            return io_source_submit_invalid_error;
    }
    return io_source_submit_invalid_error;
}

static cflow_step io_source_resume(
    void *self, cflow_publish_context *ctx, void *out_value) {
    cflow_io_publisher_state *state = (cflow_io_publisher_state *)self;
    cflow_io_operation operation = {0};
    cflow_io_submit_result submitted;
    cflow_io_publisher_prepare_status prepared;
    const char *error = NULL;
    cflow_step step;

    (void)ctx;
    if (state == NULL || out_value == NULL)
        return (cflow_step){CFLOW_STEP_ERROR, {0},
                            io_source_unavailable_error};
    if (io_source_windowed(state))
        return io_source_window_resume(state, ctx, out_value);

    sync_mutex_lock(&state->gate);
    if (state->terminal != CFLOW_PUBLISHER_OPEN) {
        step = io_source_terminal_step(state);
        sync_mutex_unlock(&state->gate);
        return step;
    }
    if (state->close_requested || !state->publisher_live) {
        sync_mutex_unlock(&state->gate);
        return io_source_set_terminal(
            state, CFLOW_PUBLISHER_ERROR, io_source_cancelled_error);
    }
    if (io_source_take_result_locked(state, out_value, &step)) {
        sync_mutex_unlock(&state->gate);
        return step;
    }
    if (state->acknowledged && state->request_id == 0u)
        state->acknowledged = false;
    if (state->submission_in_progress || state->request_id != 0u ||
        state->result_encoding || state->completion_delivered) {
        sync_mutex_unlock(&state->gate);
        return (cflow_step){
            CFLOW_STEP_WAIT,
            io_source_waitable_as_cflow_waitable(state), NULL};
    }
    state->submission_in_progress = true;
    state->window_peak_occupied = 1u;
    sync_mutex_unlock(&state->gate);

    prepared = state->prepare(state->user, &operation, &error);
    if (prepared == CFLOW_IO_PUBLISHER_PREPARE_DONE)
        return io_source_set_terminal(
            state, CFLOW_PUBLISHER_DONE, NULL);
    if (prepared == CFLOW_IO_PUBLISHER_PREPARE_ERROR)
        return io_source_set_terminal(
            state, CFLOW_PUBLISHER_ERROR,
            error != NULL && error[0] != '\0'
                ? error : io_source_prepare_error);
    if (prepared != CFLOW_IO_PUBLISHER_PREPARE_OPERATION)
        return io_source_set_terminal(
            state, CFLOW_PUBLISHER_ERROR,
            io_source_prepare_status_error);
    if (operation.release == NULL)
        return io_source_set_terminal(
            state, CFLOW_PUBLISHER_ERROR, io_source_operation_error);

    submitted = cflow_io_actor_try_submit(
        &state->actor, CFLOW_IO_PUBLISHER_LEASE_ID, &operation);
    if (submitted.status != CFLOW_IO_SUBMIT_ACCEPTED) {
        const char *submit_error = io_source_submit_error(
            submitted.status);
        operation.release(operation.user);
        return io_source_set_terminal(
            state, CFLOW_PUBLISHER_ERROR, submit_error);
    }

    sync_mutex_lock(&state->gate);
    state->submission_in_progress = false;
    if (state->request_id == 0u && !state->acknowledged)
        state->request_id = submitted.request_id;
    else if (state->request_id != 0u &&
             state->request_id != submitted.request_id) {
        state->terminal = CFLOW_PUBLISHER_ERROR;
        state->terminal_error = io_source_result_state_error;
    }
    sync_mutex_unlock(&state->gate);
    return (cflow_step){
        CFLOW_STEP_WAIT,
        io_source_waitable_as_cflow_waitable(state), NULL};
}

static void io_source_cancel(void *self) {
    io_source_wait_cancel(self);
}

static void io_source_destroy(void *self) {
    cflow_io_publisher_state *state = (cflow_io_publisher_state *)self;

    if (state == NULL)
        return;
    io_source_cancel(state);
    sync_mutex_lock(&state->gate);
    state->publisher_live = false;
    state->source_waker = (cflow_waker){0};
    sync_mutex_unlock(&state->gate);
}

static const char *io_source_name(void *self) {
    cflow_io_publisher_state *state = (cflow_io_publisher_state *)self;
    return state != NULL && state->name != NULL
        ? state->name : "io-source";
}

static const cmeta_type_desc *io_source_output_type(void *self) {
    cflow_io_publisher_state *state = (cflow_io_publisher_state *)self;
    return state != NULL ? state->type : NULL;
}

static void io_source_bind_terminal_waker(
    void *self, cflow_waker waker) {
    (void)self;
    (void)waker;
}

static cflow_publisher_terminal io_source_poll_terminal(
    void *self, const char **error) {
    cflow_io_publisher_state *state = (cflow_io_publisher_state *)self;
    cflow_publisher_terminal terminal;

    if (state == NULL)
        return CFLOW_PUBLISHER_ERROR;
    sync_mutex_lock(&state->gate);
    terminal = state->terminal;
    if (terminal == CFLOW_PUBLISHER_ERROR && error != NULL)
        *error = state->terminal_error;
    sync_mutex_unlock(&state->gate);
    return terminal;
}

CMETA_IMPLEMENTS(cflow_publisher, io_actor_source, 0,
    .name = io_source_name,
    .output_type = io_source_output_type,
    .resume = io_source_resume,
    .cancel = io_source_cancel,
    .destroy = io_source_destroy,
    .bind_terminal_waker = io_source_bind_terminal_waker,
    .poll_terminal = io_source_poll_terminal
);

static void io_source_window_completion(
    cflow_io_publisher_state *state,
    cflow_io_request_id request_id,
    cflow_io_lease_id lease_id,
    void *operation_user,
    const cflow_io_completion *completion) {
    cflow_io_publisher_entry *entry = NULL;
    cflow_read_status encoded = CFLOW_READ_ERROR;
    const char *error = NULL;
    cflow_waker waker = {0};
    bool encode = false;
    bool close_actor = false;
    size_t index;

    if (lease_id == 0u || lease_id > state->window_capacity)
        index = state->window_capacity;
    else
        index = (size_t)(lease_id - 1u);
    sync_mutex_lock(&state->gate);
    if (index < state->window_capacity)
        entry = &state->entries[index];
    if (entry == NULL || !entry->occupied ||
        entry->lease_id != lease_id ||
        (entry->request_id != 0u &&
         entry->request_id != request_id) ||
        entry->result_encoding || entry->completion_delivered) {
        state->terminal = CFLOW_PUBLISHER_ERROR;
        state->terminal_error = io_source_result_state_error;
        close_actor = true;
        waker = io_source_take_waker_locked(state);
    } else {
        if (entry->request_id == 0u)
            entry->request_id = request_id;
        if (state->close_requested ||
            state->terminal != CFLOW_PUBLISHER_OPEN ||
            state->terminal_delivery_seen) {
            entry->completion_delivered = true;
            if (entry->demand_reserved) {
                entry->demand_reserved = false;
                --state->window_demand_reserved;
            }
            waker = io_source_take_waker_locked(state);
        } else {
            entry->result_encoding = true;
            encode = true;
        }
    }
    sync_mutex_unlock(&state->gate);
    if (!encode) {
        if (close_actor)
            (void)cflow_io_actor_close(&state->actor);
        io_source_wake(state, waker);
        return;
    }

    encoded = state->encode(
        state->user, request_id, lease_id, operation_user,
        completion, entry->result.storage, &error);

    sync_mutex_lock(&state->gate);
    entry->result_encoding = false;
    if (state->close_requested ||
        state->terminal != CFLOW_PUBLISHER_OPEN ||
        state->terminal_delivery_seen) {
        entry->completion_delivered = true;
        if (entry->demand_reserved) {
            entry->demand_reserved = false;
            --state->window_demand_reserved;
        }
        if (entry->acknowledged)
            io_source_window_release_entry_locked(state, entry);
        waker = io_source_take_waker_locked(state);
        sync_mutex_unlock(&state->gate);
        io_source_wake(state, waker);
        return;
    }
    if (state->next_delivery_sequence == UINT64_MAX) {
        encoded = CFLOW_READ_ERROR;
        error = io_source_result_state_error;
    } else {
        entry->delivery_sequence = ++state->next_delivery_sequence;
    }
    if (encoded == CFLOW_READ_VALUE ||
        encoded == CFLOW_READ_VALUE_AND_DONE) {
        entry->result.live = true;
        entry->result_status = encoded;
        entry->result_error = NULL;
    } else if (encoded == CFLOW_READ_DONE) {
        entry->result_status = encoded;
        entry->result_error = NULL;
    } else if (encoded == CFLOW_READ_ERROR) {
        entry->result_status = CFLOW_READ_ERROR;
        entry->result_error = error != NULL && error[0] != '\0'
            ? error : io_source_encode_error;
    } else {
        entry->result_status = CFLOW_READ_ERROR;
        entry->result_error = encoded == CFLOW_READ_WOULD_BLOCK
            ? io_source_encode_status_error
            : io_source_encode_invalid_status_error;
    }
    if (entry->result_status != CFLOW_READ_VALUE)
        state->terminal_delivery_seen = true;
    entry->result_ready = true;
    entry->completion_delivered = true;
    ++state->window_results_ready;
    waker = io_source_take_waker_locked(state);
    sync_mutex_unlock(&state->gate);
    io_source_wake(state, waker);
}

static void io_source_completion(
    void *completion_user,
    cflow_io_request_id request_id,
    cflow_io_lease_id lease_id,
    void *operation_user,
    const cflow_io_completion *completion) {
    cflow_io_publisher_state *state =
        (cflow_io_publisher_state *)completion_user;
    cflow_read_status encoded = CFLOW_READ_ERROR;
    const char *error = NULL;
    cflow_waker waker = {0};
    bool encode = false;

    if (state == NULL || completion == NULL)
        return;
    if (io_source_windowed(state)) {
        io_source_window_completion(
            state, request_id, lease_id, operation_user, completion);
        return;
    }
    sync_mutex_lock(&state->gate);
    if (state->request_id == 0u && state->submission_in_progress)
        state->request_id = request_id;
    if (state->request_id == request_id &&
        !state->result_encoding && !state->result_ready &&
        !state->completion_delivered && !state->result.live) {
        if (state->close_requested) {
            state->completion_delivered = true;
            waker = io_source_take_waker_locked(state);
        } else {
            state->result_encoding = true;
            encode = true;
        }
    } else {
        state->result_status = CFLOW_READ_ERROR;
        state->result_error = io_source_result_state_error;
        state->result_ready = true;
        state->completion_delivered = true;
        waker = io_source_take_waker_locked(state);
    }
    sync_mutex_unlock(&state->gate);
    if (!encode) {
        io_source_wake(state, waker);
        return;
    }

    encoded = state->encode(
        state->user, request_id, lease_id, operation_user,
        completion, state->result.storage, &error);

    sync_mutex_lock(&state->gate);
    state->result_encoding = false;
    if (encoded == CFLOW_READ_VALUE ||
        encoded == CFLOW_READ_VALUE_AND_DONE) {
        state->result.live = true;
        state->result_status = encoded;
        state->result_error = NULL;
    } else if (encoded == CFLOW_READ_DONE) {
        state->result_status = encoded;
        state->result_error = NULL;
    } else if (encoded == CFLOW_READ_ERROR) {
        state->result_status = CFLOW_READ_ERROR;
        state->result_error = error != NULL && error[0] != '\0'
            ? error : io_source_encode_error;
    } else {
        state->result_status = CFLOW_READ_ERROR;
        state->result_error = encoded == CFLOW_READ_WOULD_BLOCK
            ? io_source_encode_status_error
            : io_source_encode_invalid_status_error;
    }
    state->result_ready = true;
    state->completion_delivered = true;
    waker = io_source_take_waker_locked(state);
    sync_mutex_unlock(&state->gate);
    io_source_wake(state, waker);
}

static int io_source_construct(
    cflow_publisher *out,
    cflow_io_publisher_owner *owner,
    const cflow_io_publisher_config *config,
    size_t window_capacity) {
    const bool out_zero = out != NULL &&
        out->self == NULL && out->vtable == NULL;
    const bool owner_zero = owner != NULL && owner->impl == NULL;
    cflow_io_publisher_state *state = NULL;
    cflow_io_actor_config actor_config = {0};
    bool gate_initialized = false;
    bool changed_initialized = false;
    bool result_initialized = false;
    size_t entries_initialized = 0u;
    bool executor_initialized = false;
    int status = SALTS_ENOMEM;

    if (!out_zero || !owner_zero || config == NULL ||
        config->backend.submit == NULL || config->prepare == NULL ||
        config->encode == NULL ||
        !io_source_type_valid(config->type) || window_capacity == 0u ||
        window_capacity > CFLOW_IO_PUBLISHER_MAX_WINDOW ||
        window_capacity > SIZE_MAX / sizeof(cflow_io_publisher_entry))
        return SALTS_EINVAL;

    state = (cflow_io_publisher_state *)calloc(1u, sizeof(*state));
    if (state == NULL)
        goto cleanup;
    atomic_init(&state->driver_active, false);
    state->window_capacity = window_capacity;
    sync_mutex_init(&state->gate);
    if (state->gate == NULL)
        goto cleanup;
    gate_initialized = true;
    sync_cond_init(&state->changed);
    if (state->changed == NULL)
        goto cleanup;
    changed_initialized = true;
    if (window_capacity == 1u) {
        if (!cflow_value_slot_init(&state->result, config->type))
            goto cleanup;
        result_initialized = true;
    } else {
        size_t index;

        state->entries = (cflow_io_publisher_entry *)calloc(
            window_capacity, sizeof(*state->entries));
        if (state->entries == NULL)
            goto cleanup;
        for (index = 0u; index < window_capacity; ++index) {
            if (!cflow_value_slot_init(
                    &state->entries[index].result, config->type))
                goto cleanup;
            state->entries[index].lease_id =
                (cflow_io_lease_id)(index + 1u);
            ++entries_initialized;
        }
    }
    if (!cflow_executor_manual_init_with_capacity(
            &state->executor, window_capacity))
        goto cleanup;
    executor_initialized = true;

    actor_config.request_capacity = window_capacity;
    actor_config.command_capacity = window_capacity;
    actor_config.executor = &state->executor;
    actor_config.backend = config->backend;
    actor_config.backend_user = config->backend_user;
    actor_config.completion = io_source_completion;
    actor_config.completion_user = state;
    actor_config.wake = io_source_actor_drive;
    actor_config.wake_user = state;
    state->drive = config->drive;
    state->drive_user = config->drive_user;
    status = cflow_io_actor_init(&state->actor, &actor_config);
    if (status != SALTS_OK)
        goto cleanup;

    state->name = config->name;
    state->type = config->type;
    state->prepare = config->prepare;
    state->encode = config->encode;
    state->user = config->user;
    state->publisher_live = true;
    state->owner_live = true;
    state->terminal = CFLOW_PUBLISHER_OPEN;
    *out = io_actor_source_as_cflow_publisher(state);
    owner->impl = state;
    return SALTS_OK;

cleanup:
    if (executor_initialized)
        cflow_executor_destroy(&state->executor);
    if (result_initialized)
        cflow_value_slot_destroy(&state->result);
    if (state != NULL) {
        while (entries_initialized != 0u) {
            --entries_initialized;
            cflow_value_slot_destroy(
                &state->entries[entries_initialized].result);
        }
        free(state->entries);
    }
    if (changed_initialized)
        sync_cond_destroy(&state->changed);
    if (gate_initialized)
        sync_mutex_destroy(&state->gate);
    free(state);
    return status;
}

int cflow_publisher_from_io_actor(
    cflow_publisher *out,
    cflow_io_publisher_owner *owner,
    const cflow_io_publisher_config *config) {
    return io_source_construct(
        out, owner, config, CFLOW_IO_PUBLISHER_CAPACITY);
}

int cflow_publisher_from_io_actor_windowed(
    cflow_publisher *out,
    cflow_io_publisher_owner *owner,
    const cflow_io_publisher_config *config,
    size_t window_capacity) {
    return io_source_construct(
        out, owner, config, window_capacity);
}

static void io_source_acknowledge_delivered(
    cflow_io_publisher_state *state,
    size_t max_steps,
    size_t *count,
    int *status,
    bool *made_progress) {
    while (*count < max_steps) {
        cflow_io_request_id delivered_request_id = 0u;
        cflow_io_publisher_entry *delivered_entry = NULL;
        cflow_io_ack_status ack_status;

        sync_mutex_lock(&state->gate);
        if (io_source_windowed(state)) {
            delivered_entry = io_source_window_find_delivered_locked(state);
            if (delivered_entry != NULL)
                delivered_request_id = delivered_entry->request_id;
        } else if (state->completion_delivered && state->request_id != 0u) {
            delivered_request_id = state->request_id;
        }
        sync_mutex_unlock(&state->gate);
        if (delivered_request_id == 0u)
            break;

        ack_status = cflow_io_actor_acknowledge(
            &state->actor, delivered_request_id);
        if (ack_status == CFLOW_IO_ACK_RELEASED) {
            cflow_waker waker = {0};

            sync_mutex_lock(&state->gate);
            if (io_source_windowed(state) && delivered_entry != NULL &&
                delivered_entry->occupied &&
                delivered_entry->request_id == delivered_request_id &&
                delivered_entry->completion_delivered) {
                delivered_entry->acknowledged = true;
                if (!delivered_entry->result_ready &&
                    !delivered_entry->demand_reserved)
                    io_source_window_release_entry_locked(
                        state, delivered_entry);
                waker = io_source_take_waker_locked(state);
            } else if (!io_source_windowed(state) &&
                       state->request_id == delivered_request_id &&
                       state->completion_delivered) {
                state->request_id = 0u;
                state->completion_delivered = false;
                state->acknowledged = true;
                waker = io_source_take_waker_locked(state);
            }
            sync_mutex_unlock(&state->gate);
            ++*count;
            *made_progress = true;
            io_source_wake(state, waker);
            continue;
        }
        *status = ack_status == CFLOW_IO_ACK_BUSY
            ? SALTS_EBUSY : SALTS_EPROTO;
        break;
    }
}

static int io_source_owner_run_ready_impl(
    cflow_io_publisher_owner *owner,
    size_t max_steps,
    size_t *progressed,
    bool serial_batch_phase) {
    cflow_io_publisher_state *state;
    size_t count = 0u;
    uint64_t observed_generation;
    int status = SALTS_OK;

    if (progressed != NULL)
        *progressed = 0u;
    if (owner == NULL || owner->impl == NULL || max_steps == 0u ||
        progressed == NULL)
        return SALTS_EINVAL;
    state = (cflow_io_publisher_state *)owner->impl;
    sync_mutex_lock(&state->gate);
    if (serial_batch_phase && state->drive != NULL) {
        sync_mutex_unlock(&state->gate);
        return SALTS_ENOTSUP;
    }
    if (atomic_load_explicit(&state->driver_active, memory_order_relaxed) ||
        !state->owner_live) {
        sync_mutex_unlock(&state->gate);
        return SALTS_EBUSY;
    }
    atomic_store_explicit(
        &state->driver_active, true, memory_order_release);
    state->drive_pending = false;
    observed_generation = state->drive_generation;
    sync_mutex_unlock(&state->gate);

    for (;;) {
        while (count < max_steps) {
            bool made_progress = false;
            bool executor_progress = false;
            cflow_io_run_result actor_result;

            io_source_acknowledge_delivered(
                state, max_steps, &count, &status, &made_progress);
            if (status != SALTS_OK || count >= max_steps)
                break;

            actor_result = cflow_io_actor_run_ready(
                &state->actor, max_steps - count);
            if (actor_result.status == CFLOW_IO_RUN_PROGRESSED) {
                count += actor_result.progressed;
                made_progress = true;
            }
            if (actor_result.status == CFLOW_IO_RUN_BUSY) {
                status = SALTS_EBUSY;
                break;
            }
            if (actor_result.status == CFLOW_IO_RUN_INVALID_ARGUMENT) {
                status = SALTS_EINVAL;
                break;
            }
            while (count < max_steps &&
                   cflow_executor_run_one(&state->executor)) {
                ++count;
                made_progress = true;
                executor_progress = true;
            }
            if (serial_batch_phase && executor_progress && count < max_steps)
                io_source_acknowledge_delivered(
                    state, max_steps, &count, &status, &made_progress);
            if (!made_progress || serial_batch_phase)
                break;
        }

        {
            cflow_io_wake_fn drive = NULL;
            void *drive_user = NULL;
            bool continue_draining = false;
            bool pending_drive;

            /* This gate orders the final pending-edge observation against
               driver release; callbacks remain outside the critical section. */
            sync_mutex_lock(&state->gate);
            pending_drive = state->drive_pending ||
                state->drive_generation != observed_generation;
            if (!serial_batch_phase && pending_drive && status == SALTS_OK &&
                count < max_steps) {
                state->drive_pending = false;
                observed_generation = state->drive_generation;
                continue_draining = true;
            } else {
                atomic_store_explicit(
                    &state->driver_active, false, memory_order_release);
                if (pending_drive) {
                    state->drive_pending = false;
                    (void)io_source_retain_drive_locked(
                        state, &drive, &drive_user);
                }
            }
            sync_mutex_unlock(&state->gate);
            if (continue_draining)
                continue;
            io_source_invoke_drive(state, drive, drive_user);
        }
        break;
    }

    *progressed = count;
    return status;
}

int cflow_io_publisher_owner_run_ready(
    cflow_io_publisher_owner *owner,
    size_t max_steps,
    size_t *progressed) {
    return io_source_owner_run_ready_impl(
        owner, max_steps, progressed, false);
}

int cflow_io_publisher_owner_run_serial_batch_phase_internal(
    cflow_io_publisher_owner *owner,
    size_t max_steps,
    size_t *progressed) {
    return io_source_owner_run_ready_impl(
        owner, max_steps, progressed, true);
}

bool cflow_io_publisher_owner_is_quiescent(
    const cflow_io_publisher_owner *owner) {
    cflow_io_publisher_state *state;
    bool adapter_quiescent;

    if (owner == NULL || owner->impl == NULL)
        return false;
    state = (cflow_io_publisher_state *)owner->impl;
    sync_mutex_lock(&state->gate);
    adapter_quiescent =
        !atomic_load_explicit(&state->driver_active, memory_order_relaxed) &&
        state->wake_inflight == 0u && state->drive_inflight == 0u &&
        (!io_source_windowed(state) ||
         state->window_occupied == 0u);
    sync_mutex_unlock(&state->gate);
    return adapter_quiescent &&
        cflow_io_actor_is_quiescent(&state->actor);
}

bool cflow_io_publisher_owner_get_stats(
    const cflow_io_publisher_owner *owner,
    cflow_io_publisher_stats *out) {
    cflow_io_publisher_state *state;
    cflow_io_publisher_stats snapshot = {0};

    if (owner == NULL || owner->impl == NULL || out == NULL)
        return false;
    state = (cflow_io_publisher_state *)owner->impl;
    sync_mutex_lock(&state->gate);
    snapshot.publisher_live = state->publisher_live;
    snapshot.request_active = io_source_windowed(state)
        ? state->submission_in_progress || state->window_occupied != 0u
        : state->submission_in_progress || state->request_id != 0u;
    snapshot.result_ready = io_source_windowed(state)
        ? state->window_results_ready != 0u : state->result_ready;
    snapshot.close_requested = state->close_requested;
    sync_mutex_unlock(&state->gate);
    if (!cflow_io_actor_get_stats(&state->actor, &snapshot.actor))
        return false;
    snapshot.request_active = snapshot.request_active ||
        snapshot.actor.active_requests != 0u;
    *out = snapshot;
    return true;
}

bool cflow_io_publisher_owner_get_window_stats(
    const cflow_io_publisher_owner *owner,
    cflow_io_publisher_window_stats *out) {
    cflow_io_publisher_state *state;
    cflow_io_publisher_window_stats snapshot = {0};

    if (owner == NULL || owner->impl == NULL || out == NULL)
        return false;
    state = (cflow_io_publisher_state *)owner->impl;
    sync_mutex_lock(&state->gate);
    snapshot.capacity = state->window_capacity;
    if (io_source_windowed(state)) {
        snapshot.occupied = state->window_occupied;
        snapshot.demand_reserved = state->window_demand_reserved;
        snapshot.results_ready = state->window_results_ready;
        snapshot.peak_occupied = state->window_peak_occupied;
    } else {
        snapshot.occupied = state->submission_in_progress ||
            state->request_id != 0u || state->result_ready ||
            state->completion_delivered || state->acknowledged
            ? 1u : 0u;
        snapshot.demand_reserved = snapshot.occupied;
        snapshot.results_ready = state->result_ready ? 1u : 0u;
        snapshot.peak_occupied = state->window_peak_occupied;
    }
    sync_mutex_unlock(&state->gate);
    *out = snapshot;
    return true;
}

int cflow_io_publisher_owner_close(cflow_io_publisher_owner *owner) {
    cflow_io_publisher_state *state;
    int status;

    if (owner == NULL)
        return SALTS_EINVAL;
    if (owner->impl == NULL)
        return SALTS_OK;
    state = (cflow_io_publisher_state *)owner->impl;
    sync_mutex_lock(&state->gate);
    if (!state->owner_live || state->publisher_live ||
        atomic_load_explicit(&state->driver_active, memory_order_relaxed) ||
        state->wake_inflight != 0u || state->drive_inflight != 0u ||
        (io_source_windowed(state) &&
         state->window_occupied != 0u)) {
        sync_mutex_unlock(&state->gate);
        return SALTS_EBUSY;
    }
    sync_mutex_unlock(&state->gate);
    if (!cflow_io_actor_is_quiescent(&state->actor))
        return SALTS_EBUSY;

    status = cflow_io_actor_destroy(&state->actor);
    if (status != SALTS_OK)
        return status == SALTS_EBUSY ? SALTS_EBUSY : SALTS_EINVAL;
    sync_mutex_lock(&state->gate);
    state->owner_live = false;
    sync_mutex_unlock(&state->gate);
    (void)cflow_executor_shutdown(&state->executor);
    cflow_executor_destroy(&state->executor);
    cflow_value_slot_destroy(&state->result);
    if (state->entries != NULL) {
        size_t index;
        for (index = 0u; index < state->window_capacity; ++index)
            cflow_value_slot_destroy(&state->entries[index].result);
        free(state->entries);
    }
    owner->impl = NULL;
    sync_cond_destroy(&state->changed);
    sync_mutex_destroy(&state->gate);
    free(state);
    return SALTS_OK;
}
