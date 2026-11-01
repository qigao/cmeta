#include <cflow/publishers.h>
#include <thread.h>

#include "publishers_internal.h"
#include "value_storage.h"

#include <stdlib.h>
#include <string.h>

static void source_no_cancel(void *state) { (void)state; }
static void source_no_terminal_bind(void *state, cflow_waker waker) { (void)state; (void)waker; }
static cflow_publisher_terminal source_open_terminal(void *state, const char **error) {
    (void)state; (void)error; return CFLOW_PUBLISHER_OPEN;
}

/* ---------------- array ---------------- */
typedef struct array_state {
    const unsigned char *data;
    const cmeta_type_desc *type;
    size_t count;
    size_t index;
} array_state;

static cflow_step array_resume(void *state, cflow_publish_context *ctx, void *out) {
    const void *element;

    (void)ctx;
    array_state *s = (array_state *)state;
    if (!s || s->index >= s->count) return (cflow_step){ CFLOW_STEP_DONE, {0}, NULL };
    element = s->data + s->index * s->type->size;
    if (cflow_value_storage_type_supported(s->type)) {
        memcpy(out, element, s->type->size);
    } else if (!s->type->traits->copy_construct(out, element)) {
        return (cflow_step){ CFLOW_STEP_ERROR, {0},
                             "array value construction failed" };
    }
    ++s->index;
    return (cflow_step){ s->index == s->count ? CFLOW_STEP_VALUE_AND_DONE : CFLOW_STEP_VALUE, {0}, NULL };
}
static void simple_free(void *state) { free(state); }
static const char *array_name(void *state) { (void)state; return "array"; }
static const cmeta_type_desc *array_type(void *state) {
    array_state *s = (array_state *)state; return s ? s->type : NULL;
}
CMETA_IMPLEMENTS(cflow_publisher, array_source,
    CFLOW_PUBLISHER_CAP_CONSTRUCTS_VALUES,
    .name = array_name,
    .output_type = array_type,
    .resume = array_resume,
    .cancel = source_no_cancel,
    .destroy = simple_free,
    .bind_terminal_waker = source_no_terminal_bind,
    .poll_terminal = source_open_terminal
);

cmeta_status cflow_publisher_from_array_checked(
    cflow_publisher *out,
    const cmeta_type_desc *type,
    const void *data,
    size_t count) {
    array_state *s;

    if (!out || cflow_publisher_valid(out) || !cmeta_type_desc_valid(type) ||
        (count && !data) ||
        (count != 0u && type->size > SIZE_MAX / count))
        return CMETA_INVALID_ARGUMENT;
    if (!cflow_value_type_supported(type))
        return CMETA_TRAIT_MISSING;
    s = calloc(1, sizeof(*s));
    if (!s) return CMETA_OUT_OF_MEMORY;
    s->data = (const unsigned char *)data; s->type = type; s->count = count;
    *out = array_source_as_cflow_publisher(s);
    return CMETA_OK;
}

bool cflow_publisher_from_array(cflow_publisher *out,
                             const cmeta_type_desc *type,
                             const void *data,
                             size_t count) {
    return cflow_publisher_from_array_checked(out, type, data, count) == CMETA_OK;
}

/* ---------------- CMeta Range ---------------- */
typedef struct range_state {
    cmeta_range range;
    cmeta_range_cursor cursor;
} range_state;

static cflow_step range_resume(void *state, cflow_publish_context *ctx, void *out) {
    range_state *s = (range_state *)state;
    cmeta_gen_status status;
    (void)ctx;
    if (!s) return (cflow_step){ CFLOW_STEP_ERROR, {0}, "range source unavailable" };
    status = cmeta_range_next(&s->range, &s->cursor, out);
    switch (status) {
        case CMETA_GEN_VALUE:
            return (cflow_step){ CFLOW_STEP_VALUE, {0}, NULL };
        case CMETA_GEN_VALUE_AND_DONE:
            return (cflow_step){ CFLOW_STEP_VALUE_AND_DONE, {0}, NULL };
        case CMETA_GEN_DONE:
            return (cflow_step){ CFLOW_STEP_DONE, {0}, NULL };
        case CMETA_GEN_MUTATED:
            return (cflow_step){ CFLOW_STEP_ERROR, {0}, "range owner mutated" };
        case CMETA_GEN_ERROR:
            return (cflow_step){ CFLOW_STEP_ERROR, {0}, "range iteration failed" };
    }
    return (cflow_step){ CFLOW_STEP_ERROR, {0}, "invalid range status" };
}
static const char *range_name(void *state) { (void)state; return "range"; }
static const cmeta_type_desc *range_type(void *state) {
    range_state *s = (range_state *)state;
    return s ? s->range.element_type : NULL;
}
CMETA_IMPLEMENTS(cflow_publisher, range_source,
    CFLOW_PUBLISHER_CAP_CONSTRUCTS_VALUES,
    .name = range_name,
    .output_type = range_type,
    .resume = range_resume,
    .cancel = source_no_cancel,
    .destroy = simple_free,
    .bind_terminal_waker = source_no_terminal_bind,
    .poll_terminal = source_open_terminal
);

cmeta_status cflow_publisher_from_range_checked(cflow_publisher *out,
                                              cmeta_range range,
                                              const char **out_error) {
    range_state *s;

    if (out_error)
        *out_error = NULL;
    if (!out || cflow_publisher_valid(out) || !range.object || !range.next ||
        !cmeta_type_desc_valid(range.element_type)) {
        if (out_error)
            *out_error = out && cflow_publisher_valid(out)
                ? "range source destination is occupied"
                : "invalid range source";
        return CMETA_INVALID_ARGUMENT;
    }
    if (!cflow_value_type_supported(range.element_type)) {
        if (out_error)
            *out_error =
                "range element type lacks required lifecycle traits";
        return CMETA_TRAIT_MISSING;
    }
    if (!cflow_value_storage_type_supported(range.element_type) &&
        (range.flags & CMETA_RANGE_CONSTRUCTS_VALUES) == 0u) {
        if (out_error)
            *out_error = "managed range must construct values";
        return CMETA_INVALID_ARGUMENT;
    }
    s = calloc(1, sizeof(*s));
    if (!s) {
        if (out_error)
            *out_error = "range source allocation failed";
        return CMETA_OUT_OF_MEMORY;
    }
    s->range = range;
    *out = range_source_as_cflow_publisher(s);
    return CMETA_OK;
}

bool cflow_publisher_from_range(cflow_publisher *out, cmeta_range range) {
    return cflow_publisher_from_range_checked(out, range, NULL) == CMETA_OK;
}

/* ---------------- timer ---------------- */
typedef struct timer_state {
    size_t count;
    size_t emitted;
    uint64_t interval;
    bool ready;
    bool scheduled;
    bool callback_outstanding;
    bool publisher_live;
    cflow_scheduler *scheduler;
    cflow_task_id task;
    cflow_waker waker;
    cmeta_mutex_t lock;
    cmeta_cond_t changed;
    size_t references;
    size_t wake_inflight;
} timer_state;

static SALTS_THREAD_LOCAL timer_state *timer_active_callback;

static void timer_state_release(timer_state *s) {
    bool destroy = false;
    if (!s) return;
    cmeta_mutex_lock(&s->lock);
    if (s->references != 0u) {
        --s->references;
        destroy = s->references == 0u;
    }
    cmeta_mutex_unlock(&s->lock);
    if (!destroy) return;
    cmeta_cond_destroy(&s->changed);
    cmeta_mutex_destroy(&s->lock);
    free(s);
}

static void timer_fire(void *user) {
    timer_state *s = (timer_state *)user;
    timer_state *previous;
    cflow_waker w = {0};
    if (!s) return;
    cmeta_mutex_lock(&s->lock);
    if (s->scheduled) {
        s->scheduled = false;
        s->task = 0u;
        if (s->publisher_live) {
            s->ready = true;
            w = s->waker;
            if (w.wake) ++s->wake_inflight;
        }
        s->waker = (cflow_waker){0};
    }
    s->callback_outstanding = false;
    cmeta_cond_broadcast(&s->changed);
    cmeta_mutex_unlock(&s->lock);
    if (w.wake) {
        previous = timer_active_callback;
        timer_active_callback = s;
        w.wake(w.user);
        timer_active_callback = previous;
        cmeta_mutex_lock(&s->lock);
        --s->wake_inflight;
        cmeta_cond_broadcast(&s->changed);
        cmeta_mutex_unlock(&s->lock);
    }
    timer_state_release(s);
}
static bool timer_arm(void *state, cflow_waker w) {
    timer_state *s = (timer_state *)state;
    cflow_scheduler *scheduler;
    cflow_task_id task;
    uint64_t interval;
    if (!s || !w.wake) return false;
    cmeta_mutex_lock(&s->lock);
    if (!s->publisher_live || !s->scheduler || s->scheduled ||
        s->callback_outstanding || s->ready) {
        cmeta_mutex_unlock(&s->lock);
        return false;
    }
    scheduler = s->scheduler;
    interval = s->interval;
    s->scheduled = true;
    s->callback_outstanding = true;
    s->waker = w;
    s->references += 2u; /* posting call and accepted callback */
    cmeta_mutex_unlock(&s->lock);

    task = cflow_scheduler_post_after(scheduler, interval, timer_fire, s);
    cmeta_mutex_lock(&s->lock);
    if (task != 0u && s->scheduled)
        s->task = task;
    else if (task == 0u) {
        s->scheduled = false;
        s->callback_outstanding = false;
        s->waker = (cflow_waker){0};
        cmeta_cond_broadcast(&s->changed);
    }
    cmeta_mutex_unlock(&s->lock);
    if (task == 0u)
        timer_state_release(s); /* rejected callback reference */
    timer_state_release(s); /* posting reference */
    return task != 0u;
}
static void timer_wait_cancel(void *state) {
    timer_state *s = (timer_state *)state;
    cflow_scheduler *scheduler = NULL;
    cflow_task_id task = 0u;
    bool cancelled = false;
    if (!s) return;
    cmeta_mutex_lock(&s->lock);
    if (s->scheduled) {
        scheduler = s->scheduler;
        task = s->task;
        s->scheduled = false;
        s->task = 0u;
    }
    s->ready = false;
    s->waker = (cflow_waker){0};
    cmeta_mutex_unlock(&s->lock);
    if (scheduler && task != 0u)
        cancelled = cflow_scheduler_cancel(scheduler, task);
    if (cancelled) {
        cmeta_mutex_lock(&s->lock);
        s->callback_outstanding = false;
        cmeta_cond_broadcast(&s->changed);
        cmeta_mutex_unlock(&s->lock);
        timer_state_release(s);
    }
    cmeta_mutex_lock(&s->lock);
    while (s->wake_inflight != 0u && timer_active_callback != s)
        cmeta_cond_wait(&s->changed, &s->lock);
    cmeta_mutex_unlock(&s->lock);
}
CMETA_IMPLEMENTS(cflow_waitable, timer_waitable, 0,
    .arm = timer_arm,
    .cancel = timer_wait_cancel
);
static cflow_step timer_resume(void *state, cflow_publish_context *ctx, void *out) {
    timer_state *s = (timer_state *)state;
    cflow_step_kind kind;
    size_t value;
    if (!s || !ctx || !ctx->scheduler) return (cflow_step){ CFLOW_STEP_ERROR, {0}, "timer has no scheduler" };
    cmeta_mutex_lock(&s->lock);
    if (!s->publisher_live) {
        cmeta_mutex_unlock(&s->lock);
        return (cflow_step){ CFLOW_STEP_ERROR, {0}, "timer source unavailable" };
    }
    s->scheduler = ctx->scheduler;
    if (s->emitted >= s->count) {
        cmeta_mutex_unlock(&s->lock);
        return (cflow_step){ CFLOW_STEP_DONE, {0}, NULL };
    }
    if (!s->ready) {
        cmeta_mutex_unlock(&s->lock);
        return (cflow_step){ CFLOW_STEP_WAIT, timer_waitable_as_cflow_waitable(s), NULL };
    }
    s->ready = false;
    value = s->emitted++;
    kind = s->emitted == s->count
        ? CFLOW_STEP_VALUE_AND_DONE : CFLOW_STEP_VALUE;
    cmeta_mutex_unlock(&s->lock);
    memcpy(out, &value, sizeof value);
    return (cflow_step){ kind, {0}, NULL };
}
static void timer_cancel(void *state) { timer_wait_cancel(state); }
static void timer_destroy(void *state) {
    timer_state *s = (timer_state *)state;
    if (!s) return;
    cmeta_mutex_lock(&s->lock);
    s->publisher_live = false;
    cmeta_mutex_unlock(&s->lock);
    timer_wait_cancel(s);
    timer_state_release(s);
}
static const char *timer_name(void *state) { (void)state; return "timer"; }
static const cmeta_type_desc *timer_type(void *state) { (void)state; return &cmeta_type_size; }
CMETA_IMPLEMENTS(cflow_publisher, timer_source, 0,
    .name = timer_name,
    .output_type = timer_type,
    .resume = timer_resume,
    .cancel = timer_cancel,
    .destroy = timer_destroy,
    .bind_terminal_waker = source_no_terminal_bind,
    .poll_terminal = source_open_terminal
);

bool cflow_publisher_from_timer(cflow_publisher *out,
                             size_t count,
                             uint64_t interval_ticks) {
    if (!out || cflow_publisher_valid(out)) return false;
    timer_state *s = calloc(1, sizeof(*s));
    if (!s) return false;
    cmeta_mutex_init(&s->lock);
    cmeta_cond_init(&s->changed);
    if (!s->lock || !s->changed) {
        cmeta_cond_destroy(&s->changed);
        cmeta_mutex_destroy(&s->lock);
        free(s);
        return false;
    }
    s->count = count; s->interval = interval_ticks;
    s->publisher_live = true;
    s->references = 1u;
    *out = timer_source_as_cflow_publisher(s);
    return true;
}

/* ---------------- channel ---------------- */
typedef struct channel_impl {
    const cmeta_type_desc *type;
    unsigned char *data;
    size_t capacity;
    size_t head, count;
    bool closed;
    cmeta_mutex_t lock;
    cflow_waker waiter;
    cflow_waker terminal_waker;
    size_t peak_pending;
    uint64_t accepted;
    uint64_t received;
    uint64_t rejected_full;
    uint64_t rejected_closed;
} channel_impl;

typedef struct channel_source_state { channel_impl *ch; } channel_source_state;

static channel_impl *channel_of(cflow_channel *ch) { return ch ? (channel_impl *)ch->impl : NULL; }
static const channel_impl *channel_of_const(const cflow_channel *ch) {
    return ch ? (const channel_impl *)ch->impl : NULL;
}

bool cflow_channel_init(cflow_channel *ch,
                        const cmeta_type_desc *type,
                        size_t capacity) {
    if (!ch || ch->impl || !cflow_value_storage_type_supported(type) ||
        type->size == 0u || capacity == 0u ||
        capacity > SIZE_MAX / type->size)
        return false;
    channel_impl *c = calloc(1, sizeof(*c));
    if (!c) return false;
    c->data = malloc(type->size * capacity);
    cmeta_mutex_init(&c->lock);
    if (!c->data || !c->lock) {
        cmeta_mutex_destroy(&c->lock);
        free(c->data);
        free(c);
        return false;
    }
    c->type = type; c->capacity = capacity; ch->impl = c; return true;
}

cflow_channel_status cflow_channel_try_push(cflow_channel *ch,
                                            const void *value) {
    channel_impl *c = channel_of(ch);
    if (!c || !value) return CFLOW_CHANNEL_INVALID_ARGUMENT;
    cmeta_mutex_lock(&c->lock);
    if (c->closed) {
        ++c->rejected_closed;
        cmeta_mutex_unlock(&c->lock);
        return CFLOW_CHANNEL_CLOSED;
    }
    if (c->count == c->capacity) {
        ++c->rejected_full;
        cmeta_mutex_unlock(&c->lock);
        return CFLOW_CHANNEL_FULL;
    }
    size_t tail = (c->head + c->count) % c->capacity;
    memcpy(c->data + tail * c->type->size, value, c->type->size); ++c->count;
    ++c->accepted;
    if (c->count > c->peak_pending) c->peak_pending = c->count;
    cflow_waker w = c->waiter; c->waiter = (cflow_waker){0};
    cmeta_mutex_unlock(&c->lock);
    if (w.wake) w.wake(w.user);
    return CFLOW_CHANNEL_OK;
}

bool cflow_channel_push(cflow_channel *ch, const void *value) {
    return cflow_channel_try_push(ch, value) == CFLOW_CHANNEL_OK;
}

bool cflow_channel_get_stats(const cflow_channel *ch,
                             cflow_channel_stats *out) {
    channel_impl *c = (channel_impl *)channel_of_const(ch);
    if (!c || !out) return false;
    cmeta_mutex_lock(&c->lock);
    *out = (cflow_channel_stats){
        c->capacity,
        c->count,
        c->peak_pending,
        c->accepted,
        c->received,
        c->rejected_full,
        c->rejected_closed
    };
    cmeta_mutex_unlock(&c->lock);
    return true;
}

void cflow_channel_close(cflow_channel *ch) {
    channel_impl *c = channel_of(ch); if (!c) return;
    cmeta_mutex_lock(&c->lock);
    c->closed = true;
    cflow_waker w = c->waiter; c->waiter = (cflow_waker){0};
    cflow_waker tw = c->terminal_waker;
    cmeta_mutex_unlock(&c->lock);
    if (w.wake) w.wake(w.user);
    if (tw.wake) tw.wake(tw.user);
}

void cflow_channel_destroy(cflow_channel *ch) {
    channel_impl *c = channel_of(ch); if (!c) return;
    cflow_channel_close(ch); cmeta_mutex_destroy(&c->lock); free(c->data); free(c); ch->impl = NULL;
}

static bool channel_arm(void *state, cflow_waker w) {
    channel_source_state *ss = (channel_source_state *)state;
    channel_impl *c = ss ? ss->ch : NULL; if (!c) return false;
    cmeta_mutex_lock(&c->lock);
    bool ready = c->count || c->closed;
    if (!ready) c->waiter = w;
    cmeta_mutex_unlock(&c->lock);
    if (ready && w.wake) w.wake(w.user);
    return true;
}
static void channel_unarm(void *state) {
    channel_source_state *ss = (channel_source_state *)state;
    channel_impl *c = ss ? ss->ch : NULL; if (!c) return;
    cmeta_mutex_lock(&c->lock);
    c->waiter = (cflow_waker){0};
    cmeta_mutex_unlock(&c->lock);
}
CMETA_IMPLEMENTS(cflow_waitable, channel_waitable, 0,
    .arm = channel_arm,
    .cancel = channel_unarm
);
static cflow_step channel_resume(void *state, cflow_publish_context *ctx, void *out) {
    (void)ctx;
    channel_source_state *ss = (channel_source_state *)state;
    channel_impl *c = ss ? ss->ch : NULL;
    if (!c) return (cflow_step){ CFLOW_STEP_ERROR, {0}, "channel unavailable" };
    cmeta_mutex_lock(&c->lock);
    if (c->count) {
        memcpy(out, c->data + c->head * c->type->size, c->type->size);
        c->head = (c->head + 1) % c->capacity; --c->count;
        ++c->received;
        bool final = c->closed && c->count == 0;
        cmeta_mutex_unlock(&c->lock);
        return (cflow_step){ final ? CFLOW_STEP_VALUE_AND_DONE : CFLOW_STEP_VALUE, {0}, NULL };
    }
    if (c->closed) { cmeta_mutex_unlock(&c->lock); return (cflow_step){ CFLOW_STEP_DONE, {0}, NULL }; }
    cmeta_mutex_unlock(&c->lock);
    return (cflow_step){ CFLOW_STEP_WAIT, channel_waitable_as_cflow_waitable(ss), NULL };
}
static void channel_source_cancel(void *state) { channel_unarm(state); }
static const char *channel_source_name(void *state) { (void)state; return "channel"; }
static const cmeta_type_desc *channel_source_type(void *state) {
    channel_source_state *ss = (channel_source_state *)state;
    return ss && ss->ch ? ss->ch->type : NULL;
}

static void channel_bind_terminal(void *state, cflow_waker w) {
    channel_source_state *ss = (channel_source_state *)state;
    channel_impl *c = ss ? ss->ch : NULL; if (!c) return;
    cmeta_mutex_lock(&c->lock);
    c->terminal_waker = w;
    cmeta_mutex_unlock(&c->lock);
}
static cflow_publisher_terminal channel_poll_terminal(void *state, const char **error) {
    (void)error;
    channel_source_state *ss = (channel_source_state *)state;
    channel_impl *c = ss ? ss->ch : NULL; if (!c) return CFLOW_PUBLISHER_ERROR;
    cmeta_mutex_lock(&c->lock);
    bool done = c->closed && c->count == 0;
    cmeta_mutex_unlock(&c->lock);
    return done ? CFLOW_PUBLISHER_DONE : CFLOW_PUBLISHER_OPEN;
}
CMETA_IMPLEMENTS(cflow_publisher, channel_source, 0,
    .name = channel_source_name,
    .output_type = channel_source_type,
    .resume = channel_resume,
    .cancel = channel_source_cancel,
    .destroy = simple_free,
    .bind_terminal_waker = channel_bind_terminal,
    .poll_terminal = channel_poll_terminal
);

bool cflow_publisher_from_channel(cflow_publisher *out, cflow_channel *ch) {
    channel_impl *c = channel_of(ch); if (!out || cflow_publisher_valid(out) || !c) return false;
    channel_source_state *s = calloc(1, sizeof(*s)); if (!s) return false;
    s->ch = c; *out = channel_source_as_cflow_publisher(s); return true;
}

/* ---------------- generic readiness/completion source ---------------- */
typedef struct readiness_state {
    const char *name;
    const cmeta_type_desc *type;
    cflow_read_fn read;
    cflow_watch_fn arm;
    cflow_unwatch_fn cancel;
    cflow_resource_close_fn close;
    void *user;
} readiness_state;

static bool readiness_arm(void *state, cflow_waker w) {
    readiness_state *s = (readiness_state *)state;
    return s && s->arm && s->arm(s->user, w);
}
static void readiness_unarm(void *state) {
    readiness_state *s = (readiness_state *)state;
    if (s && s->cancel) s->cancel(s->user);
}
CMETA_IMPLEMENTS(cflow_waitable, readiness_waitable, 0,
    .arm = readiness_arm,
    .cancel = readiness_unarm
);
static cflow_step readiness_resume(void *state, cflow_publish_context *ctx, void *out) {
    (void)ctx;
    readiness_state *s = (readiness_state *)state; const char *err = NULL;
    if (!s || !s->read) return (cflow_step){ CFLOW_STEP_ERROR, {0}, "readiness source unavailable" };
    switch (s->read(s->user, out, &err)) {
        case CFLOW_READ_VALUE: return (cflow_step){ CFLOW_STEP_VALUE, {0}, NULL };
        case CFLOW_READ_VALUE_AND_DONE: return (cflow_step){ CFLOW_STEP_VALUE_AND_DONE, {0}, NULL };
        case CFLOW_READ_WOULD_BLOCK: return (cflow_step){ CFLOW_STEP_WAIT, readiness_waitable_as_cflow_waitable(s), NULL };
        case CFLOW_READ_DONE: return (cflow_step){ CFLOW_STEP_DONE, {0}, NULL };
        case CFLOW_READ_ERROR: return (cflow_step){ CFLOW_STEP_ERROR, {0}, err ? err : "readiness source error" };
    }
    return (cflow_step){ CFLOW_STEP_ERROR, {0}, "invalid readiness status" };
}
static void readiness_cancel(void *state) { readiness_unarm(state); }
static void readiness_destroy(void *state) {
    readiness_state *s = (readiness_state *)state;
    if (s) { readiness_unarm(s); if (s->close) s->close(s->user); free(s); }
}
static const char *readiness_name(void *state) {
    readiness_state *s = (readiness_state *)state; return s && s->name ? s->name : "readiness";
}
static const cmeta_type_desc *readiness_type(void *state) {
    readiness_state *s = (readiness_state *)state; return s ? s->type : NULL;
}
CMETA_IMPLEMENTS(cflow_publisher, readiness_source, 0,
    .name = readiness_name,
    .output_type = readiness_type,
    .resume = readiness_resume,
    .cancel = readiness_cancel,
    .destroy = readiness_destroy,
    .bind_terminal_waker = source_no_terminal_bind,
    .poll_terminal = source_open_terminal
);

bool cflow_publisher_from_readiness(cflow_publisher *out,
                                 const char *name,
                                 const cmeta_type_desc *type,
                                 cflow_read_fn read,
                                 cflow_watch_fn arm,
                                 cflow_unwatch_fn cancel,
                                 cflow_resource_close_fn close,
                                 void *user) {
    if (!out || cflow_publisher_valid(out) ||
        !cflow_value_storage_type_supported(type) || !read || !arm || !cancel)
        return false;
    readiness_state *s = calloc(1, sizeof(*s)); if (!s) return false;
    s->name = name ? name : "readiness"; s->type = type;
    s->read = read; s->arm = arm; s->cancel = cancel; s->close = close; s->user = user;
    *out = readiness_source_as_cflow_publisher(s);
    return true;
}
