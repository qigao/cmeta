#include <cflow/readiness.h>

#include "readiness_internal.h"

#include <error_codes.h>
#include <thread.h>

#include "value_storage.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { CFLOW_REACTOR_ERROR_CAPACITY = 96 };

typedef enum cflow_reactor_adapter_phase {
    CFLOW_REACTOR_ADAPTER_IDLE,
    CFLOW_REACTOR_ADAPTER_ARMING,
    CFLOW_REACTOR_ADAPTER_ARMED,
    CFLOW_REACTOR_ADAPTER_NOTIFIED,
    CFLOW_REACTOR_ADAPTER_ERROR,
    CFLOW_REACTOR_ADAPTER_CLOSED
} cflow_reactor_adapter_phase;

typedef struct cflow_reactor_adapter_state {
    const char *name;
    const cmeta_type_desc *type;
    cflow_read_fn read;
    cflow_resource_close_fn user_close;
    void *user;
    cmeta_readiness_events events;
    cmeta_readiness_registration registration;
    cmeta_mutex_t lock;
    cmeta_cond_t changed;
    cflow_reactor_adapter_phase phase;
    cflow_waker waker;
    size_t references;
    int exact_status;
    bool publisher_live;
    bool owner_live;
    bool cancelled;
    bool cleanup_inflight;
    bool cleanup_complete;
    bool user_closed;
    char error[CFLOW_REACTOR_ERROR_CAPACITY];
} cflow_reactor_adapter_state;

static void cflow_reactor_set_error_locked(
    cflow_reactor_adapter_state *state, const char *operation, int status) {
    state->exact_status = status;
    state->phase = CFLOW_REACTOR_ADAPTER_ERROR;
    (void)snprintf(state->error, sizeof(state->error),
                   "reactor readiness %s failed: %d", operation, status);
}

static void cflow_reactor_callback(void *user,
                                   cmeta_readiness_events events,
                                   int status) {
    cflow_reactor_adapter_state *state =
        (cflow_reactor_adapter_state *)user;
    cflow_waker waker = {0};

    (void)events;
    if (!state)
        return;

    /* Platform keeps callback_user borrowed until this callback returns. */
    cmeta_mutex_lock(&state->lock);
    ++state->references;
    if (!state->cancelled &&
        (state->phase == CFLOW_REACTOR_ADAPTER_ARMING ||
         state->phase == CFLOW_REACTOR_ADAPTER_ARMED)) {
        if (status == SALTS_OK)
            state->phase = CFLOW_REACTOR_ADAPTER_NOTIFIED;
        else
            cflow_reactor_set_error_locked(state, "backend", status);
        waker = state->waker;
        state->waker = (cflow_waker){0};
    }
    cmeta_mutex_unlock(&state->lock);

    if (waker.wake)
        waker.wake(waker.user);

    cmeta_mutex_lock(&state->lock);
    --state->references;
    cmeta_cond_broadcast(&state->changed);
    cmeta_mutex_unlock(&state->lock);
}

static bool cflow_reactor_arm(void *self, cflow_waker waker) {
    cflow_reactor_adapter_state *state =
        (cflow_reactor_adapter_state *)self;
    cflow_waker wake_now = {0};
    int status;

    if (!state || !waker.wake)
        return false;

    cmeta_mutex_lock(&state->lock);
    if (state->cancelled || state->cleanup_inflight ||
        state->phase == CFLOW_REACTOR_ADAPTER_CLOSED) {
        cflow_reactor_set_error_locked(state, "cancelled", SALTS_ESHUTDOWN);
        wake_now = waker;
        cmeta_mutex_unlock(&state->lock);
        wake_now.wake(wake_now.user);
        return true;
    }
    if (state->phase != CFLOW_REACTOR_ADAPTER_IDLE) {
        cmeta_mutex_unlock(&state->lock);
        return false;
    }
    state->phase = CFLOW_REACTOR_ADAPTER_ARMING;
    state->waker = waker;
    cmeta_mutex_unlock(&state->lock);

    status = cmeta_readiness_arm(&state->registration, state->events,
                                 cflow_reactor_callback, state);

    cmeta_mutex_lock(&state->lock);
    if (status != SALTS_OK &&
        state->phase == CFLOW_REACTOR_ADAPTER_ARMING) {
        cflow_reactor_set_error_locked(state, "arm", status);
        if (!state->cancelled) {
            wake_now = state->waker;
            state->waker = (cflow_waker){0};
        }
    } else if (status == SALTS_OK &&
               state->phase == CFLOW_REACTOR_ADAPTER_ARMING) {
        state->phase = CFLOW_REACTOR_ADAPTER_ARMED;
    }
    cmeta_mutex_unlock(&state->lock);

    /* A synchronous arm failure is a successful CFlow arm followed by wake. */
    if (wake_now.wake)
        wake_now.wake(wake_now.user);
    return true;
}

static int cflow_reactor_close(cflow_reactor_adapter_state *state) {
    cflow_resource_close_fn user_close = NULL;
    void *user = NULL;
    int status;

    if (!state)
        return SALTS_EINVAL;

    cmeta_mutex_lock(&state->lock);
    while (state->cleanup_inflight)
        cmeta_cond_wait(&state->changed, &state->lock);
    if (state->cleanup_complete) {
        cmeta_mutex_unlock(&state->lock);
        return SALTS_OK;
    }
    state->cancelled = true;
    state->waker = (cflow_waker){0};
    state->cleanup_inflight = true;
    cmeta_mutex_unlock(&state->lock);

    /* Platform close is the quiescence and ownership-transfer boundary. */
    status = cmeta_readiness_close(&state->registration);
    if (status == SALTS_OK) {
        cmeta_mutex_lock(&state->lock);
        if (!state->user_closed) {
            user_close = state->user_close;
            user = state->user;
        }
        cmeta_mutex_unlock(&state->lock);

        if (user_close)
            user_close(user);
    }

    cmeta_mutex_lock(&state->lock);
    if (status == SALTS_OK) {
        state->user_closed = true;
        state->cleanup_complete = true;
        state->phase = CFLOW_REACTOR_ADAPTER_CLOSED;
    } else {
        cflow_reactor_set_error_locked(state, "close", status);
    }
    state->cleanup_inflight = false;
    cmeta_cond_broadcast(&state->changed);
    cmeta_mutex_unlock(&state->lock);
    return status;
}

static void cflow_reactor_unarm(void *self) {
    (void)cflow_reactor_close((cflow_reactor_adapter_state *)self);
}

CMETA_IMPLEMENTS(cflow_waitable, cflow_reactor_waitable, 0,
    .arm = cflow_reactor_arm,
    .cancel = cflow_reactor_unarm
);

static cflow_step cflow_reactor_resume(void *self,
                                       cflow_publish_context *ctx,
                                       void *out_value) {
    cflow_reactor_adapter_state *state =
        (cflow_reactor_adapter_state *)self;
    const char *read_error = NULL;
    cflow_read_status read_status;

    (void)ctx;
    if (!state || !out_value)
        return (cflow_step){CFLOW_STEP_ERROR, {0},
                            "reactor readiness source unavailable"};

    cmeta_mutex_lock(&state->lock);
    if (state->phase == CFLOW_REACTOR_ADAPTER_ERROR) {
        const char *error = state->error;
        cmeta_mutex_unlock(&state->lock);
        return (cflow_step){CFLOW_STEP_ERROR, {0}, error};
    }
    if (state->cancelled || state->phase == CFLOW_REACTOR_ADAPTER_CLOSED) {
        cmeta_mutex_unlock(&state->lock);
        return (cflow_step){CFLOW_STEP_ERROR, {0},
                            "reactor readiness source cancelled"};
    }
    if (state->phase == CFLOW_REACTOR_ADAPTER_ARMING ||
        state->phase == CFLOW_REACTOR_ADAPTER_ARMED) {
        cmeta_mutex_unlock(&state->lock);
        return (cflow_step){
            CFLOW_STEP_WAIT,
            cflow_reactor_waitable_as_cflow_waitable(state), NULL};
    }
    state->phase = CFLOW_REACTOR_ADAPTER_IDLE;
    cmeta_mutex_unlock(&state->lock);

    read_status = state->read(state->user, out_value, &read_error);
    switch (read_status) {
        case CFLOW_READ_VALUE:
            return (cflow_step){CFLOW_STEP_VALUE, {0}, NULL};
        case CFLOW_READ_VALUE_AND_DONE:
            return (cflow_step){CFLOW_STEP_VALUE_AND_DONE, {0}, NULL};
        case CFLOW_READ_WOULD_BLOCK:
            return (cflow_step){
                CFLOW_STEP_WAIT,
                cflow_reactor_waitable_as_cflow_waitable(state), NULL};
        case CFLOW_READ_DONE:
            return (cflow_step){CFLOW_STEP_DONE, {0}, NULL};
        case CFLOW_READ_ERROR:
            return (cflow_step){
                CFLOW_STEP_ERROR, {0},
                read_error && read_error[0]
                    ? read_error
                    : "reactor readiness source error"};
    }
    return (cflow_step){CFLOW_STEP_ERROR, {0},
                        "invalid reactor readiness read status"};
}

static void cflow_reactor_cancel(void *self) {
    (void)cflow_reactor_close((cflow_reactor_adapter_state *)self);
}

static void cflow_reactor_destroy(void *self) {
    cflow_reactor_adapter_state *state =
        (cflow_reactor_adapter_state *)self;
    bool free_state = false;

    if (!state)
        return;

    (void)cflow_reactor_close(state);

    cmeta_mutex_lock(&state->lock);
    if (state->publisher_live) {
        state->publisher_live = false;
        --state->references;
        free_state = state->references == 0u;
        cmeta_cond_broadcast(&state->changed);
    }
    cmeta_mutex_unlock(&state->lock);

    if (free_state) {
        cmeta_cond_destroy(&state->changed);
        cmeta_mutex_destroy(&state->lock);
        free(state);
    }
}

static const char *cflow_reactor_name(void *self) {
    cflow_reactor_adapter_state *state =
        (cflow_reactor_adapter_state *)self;
    return state && state->name ? state->name : "reactor-readiness";
}

static const cmeta_type_desc *cflow_reactor_type(void *self) {
    cflow_reactor_adapter_state *state =
        (cflow_reactor_adapter_state *)self;
    return state ? state->type : NULL;
}

static void cflow_reactor_bind_terminal(void *self, cflow_waker waker) {
    (void)self;
    (void)waker;
}

static cflow_publisher_terminal cflow_reactor_poll_terminal(
    void *self, const char **error) {
    (void)self;
    (void)error;
    return CFLOW_PUBLISHER_OPEN;
}

CMETA_IMPLEMENTS(cflow_publisher, cflow_reactor_source, 0,
    .name = cflow_reactor_name,
    .output_type = cflow_reactor_type,
    .resume = cflow_reactor_resume,
    .cancel = cflow_reactor_cancel,
    .destroy = cflow_reactor_destroy,
    .bind_terminal_waker = cflow_reactor_bind_terminal,
    .poll_terminal = cflow_reactor_poll_terminal
);

int cflow_publisher_from_readiness_registration(
    cflow_publisher *out,
    cflow_readiness_publisher_owner *owner,
    cmeta_readiness_registration *registration,
    cmeta_readiness_events events,
    const char *name,
    const cmeta_type_desc *type,
    cflow_read_fn read,
    cflow_resource_close_fn close,
    void *user) {
    const cmeta_readiness_events supported_events =
        SALTS_READINESS_EVENT_READ | SALTS_READINESS_EVENT_WRITE |
        SALTS_READINESS_EVENT_ERROR | SALTS_READINESS_EVENT_HANGUP;
    cflow_reactor_adapter_state *state;

    if (out)
        *out = (cflow_publisher){0};
    if (owner)
        owner->impl = NULL;

    if (!out || !owner || !registration || !registration->impl || !read ||
        !cmeta_type_desc_valid(type) || events == 0u ||
        (events & ~supported_events) != 0u)
        return SALTS_EINVAL;
    if (!cflow_value_storage_type_supported(type))
        return SALTS_ENOTSUP;

    state = (cflow_reactor_adapter_state *)calloc(1, sizeof(*state));
    if (!state)
        return SALTS_ENOMEM;
    cmeta_mutex_init(&state->lock);
    cmeta_cond_init(&state->changed);
    if (!state->lock || !state->changed) {
        cmeta_cond_destroy(&state->changed);
        cmeta_mutex_destroy(&state->lock);
        free(state);
        return SALTS_ENOMEM;
    }

    state->name = name ? name : "reactor-readiness";
    state->type = type;
    state->read = read;
    state->user_close = close;
    state->user = user;
    state->events = events;
    state->registration = *registration;
    state->references = 2u;
    state->publisher_live = true;
    state->owner_live = true;
    state->phase = CFLOW_REACTOR_ADAPTER_IDLE;

    *out = cflow_reactor_source_as_cflow_publisher(state);
    owner->impl = state;
    memset(registration, 0, sizeof(*registration));
    return SALTS_OK;
}

int cflow_readiness_publisher_owner_close(cflow_readiness_publisher_owner *owner) {
    cflow_reactor_adapter_state *state;
    bool free_state;
    int status;

    if (!owner)
        return SALTS_EINVAL;
    if (!owner->impl)
        return SALTS_OK;
    state = (cflow_reactor_adapter_state *)owner->impl;

    cmeta_mutex_lock(&state->lock);
    if (state->publisher_live) {
        cmeta_mutex_unlock(&state->lock);
        return SALTS_EBUSY;
    }
    cmeta_mutex_unlock(&state->lock);

    status = cflow_reactor_close(state);
    if (status != SALTS_OK)
        return status;

    cmeta_mutex_lock(&state->lock);
    if (!state->owner_live) {
        cmeta_mutex_unlock(&state->lock);
        return SALTS_EINVAL;
    }
    state->owner_live = false;
    --state->references;
    free_state = state->references == 0u;
    owner->impl = NULL;
    cmeta_mutex_unlock(&state->lock);

    if (free_state) {
        cmeta_cond_destroy(&state->changed);
        cmeta_mutex_destroy(&state->lock);
        free(state);
    }
    return SALTS_OK;
}

cmeta_readiness_registration *
cflow_readiness_publisher_owner_observe_registration(
    cflow_readiness_publisher_owner *owner) {
    cflow_reactor_adapter_state *state =
        owner ? (cflow_reactor_adapter_state *)owner->impl : NULL;
    return state ? &state->registration : NULL;
}
