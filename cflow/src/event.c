#include <cflow/event.h>

#include "event_internal.h"

#include <thread.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct cflow_event_slot {
    size_t type_index;
} cflow_event_slot;

typedef enum cflow_mailbox_terminal {
    CFLOW_MAILBOX_TERMINAL_OPEN = 0,
    CFLOW_MAILBOX_TERMINAL_DRAINING,
    CFLOW_MAILBOX_TERMINAL_CANCELLED
} cflow_mailbox_terminal;

typedef struct cflow_mailbox_impl {
    cflow_event_type *schema;
    cflow_event_slot *slots;
    unsigned char *payloads;
    size_t schema_count;
    size_t capacity;
    size_t payload_stride;
    size_t max_payload_size;
    size_t reserved_payload_bytes;
    size_t head;
    size_t count;
    size_t peak_pending;
    uint64_t accepted;
    uint64_t received;
    uint64_t rejected_full;
    uint64_t rejected_closed;
    uint64_t rejected_cancelled;
    uint64_t cancelled;
    cflow_mailbox_terminal terminal;
    cflow_waker waiter;
    cmeta_mutex_t lock;
} cflow_mailbox_impl;

static void cflow_counter_increment(uint64_t *counter) {
    if (*counter != UINT64_MAX) ++*counter;
}

static void cflow_counter_add_size(uint64_t *counter, size_t amount) {
    const uint64_t converted = (uint64_t)amount;
    if (UINT64_MAX - *counter < converted)
        *counter = UINT64_MAX;
    else
        *counter += converted;
}

static void cflow_waker_invoke(cflow_waker waker) {
    if (waker.wake != NULL) waker.wake(waker.user);
}

static bool cflow_size_multiply(size_t left, size_t right, size_t *result) {
    if (result == NULL || left == 0u || right == 0u || left > SIZE_MAX / right)
        return false;
    *result = left * right;
    return true;
}

static bool cflow_alignment_valid(size_t alignment) {
    return alignment != 0u && (alignment & (alignment - 1u)) == 0u &&
           alignment <= _Alignof(cmeta_capture_storage);
}

static bool cflow_align_size(size_t size, size_t alignment, size_t *result) {
    size_t padding;

    if (result == NULL || size == 0u || !cflow_alignment_valid(alignment))
        return false;
    padding = alignment - 1u;
    if (size > SIZE_MAX - padding) return false;
    *result = (size + padding) & ~padding;
    return true;
}

static bool cflow_schema_measure(const cflow_event_type *schema,
                                 size_t schema_count,
                                 size_t *max_payload_size,
                                 size_t *max_payload_alignment) {
    const cmeta_trait_flags required =
        CMETA_TRAIT_TRIVIAL_COPY | CMETA_TRAIT_TRIVIAL_DESTROY;
    size_t largest_size = 0u;
    size_t largest_alignment = 0u;
    size_t index;

    if (schema == NULL || schema_count == 0u || max_payload_size == NULL ||
        max_payload_alignment == NULL)
        return false;

    for (index = 0u; index < schema_count; ++index) {
        const cmeta_type_desc *type = schema[index].payload_type;
        size_t previous;

        if (schema[index].id == 0u || !cmeta_type_desc_valid(type) ||
            type->size == 0u || !cflow_alignment_valid(type->align) ||
            cmeta_type_require_traits(type, required) != CMETA_OK)
            return false;
        for (previous = 0u; previous < index; ++previous) {
            if (schema[previous].id == schema[index].id) return false;
        }
        if (type->size > largest_size) largest_size = type->size;
        if (type->align > largest_alignment) largest_alignment = type->align;
    }

    *max_payload_size = largest_size;
    *max_payload_alignment = largest_alignment;
    return true;
}

bool cflow_mailbox_storage_requirements_internal(
    const cflow_event_type *schema, size_t schema_count, size_t capacity,
    size_t *out_bytes) {
    size_t max_payload_size, max_payload_alignment, payload_stride;
    size_t schema_bytes, slot_bytes, payload_bytes, total;
    if (out_bytes == NULL ||
        !cflow_size_multiply(schema_count, sizeof(cflow_event_type),
                             &schema_bytes) ||
        !cflow_size_multiply(capacity, sizeof(cflow_event_slot),
                             &slot_bytes) ||
        !cflow_schema_measure(schema, schema_count, &max_payload_size,
                              &max_payload_alignment) ||
        !cflow_align_size(max_payload_size, max_payload_alignment,
                          &payload_stride) ||
        !cflow_size_multiply(capacity, payload_stride, &payload_bytes) ||
        sizeof(cflow_mailbox_impl) > SIZE_MAX - schema_bytes)
        return false;
    total = sizeof(cflow_mailbox_impl) + schema_bytes;
    if (slot_bytes > SIZE_MAX - total) return false;
    total += slot_bytes;
    if (payload_bytes > SIZE_MAX - total) return false;
    *out_bytes = total + payload_bytes;
    return true;
}

static void cflow_mailbox_impl_free(cflow_mailbox_impl *impl) {
    if (impl == NULL) return;
    if (impl->lock != NULL) cmeta_mutex_destroy(&impl->lock);
    free(impl->payloads);
    free(impl->slots);
    free(impl->schema);
    free(impl);
}

static int cflow_event_type_compare(const void *left, const void *right) {
    const cflow_event_type *a = (const cflow_event_type *)left;
    const cflow_event_type *b = (const cflow_event_type *)right;
    return a->id < b->id ? -1 : a->id > b->id;
}

cflow_mailbox_status cflow_mailbox_init(cflow_mailbox *mailbox,
                                        const cflow_event_type *schema,
                                        size_t schema_count,
                                        size_t capacity) {
    cflow_mailbox_impl *impl;
    size_t max_payload_size;
    size_t max_payload_alignment;
    size_t payload_stride;
    size_t schema_bytes;
    size_t slot_bytes;
    size_t payload_bytes;

    if (mailbox == NULL || mailbox->impl != NULL || schema == NULL ||
        schema_count == 0u || capacity == 0u ||
        !cflow_size_multiply(schema_count, sizeof(*schema), &schema_bytes) ||
        !cflow_size_multiply(capacity, sizeof(cflow_event_slot), &slot_bytes))
        return CFLOW_MAILBOX_INVALID_ARGUMENT;
    if (!cflow_schema_measure(schema, schema_count, &max_payload_size,
                              &max_payload_alignment) ||
        !cflow_align_size(max_payload_size, max_payload_alignment,
                          &payload_stride) ||
        !cflow_size_multiply(capacity, payload_stride, &payload_bytes))
        return CFLOW_MAILBOX_INVALID_ARGUMENT;

    impl = (cflow_mailbox_impl *)calloc(1u, sizeof(*impl));
    if (impl == NULL) return CFLOW_MAILBOX_ALLOCATION_FAILED;
    impl->schema = (cflow_event_type *)malloc(schema_bytes);
    impl->slots = (cflow_event_slot *)calloc(1u, slot_bytes);
    impl->payloads = (unsigned char *)malloc(payload_bytes);
    cmeta_mutex_init(&impl->lock);
    if (impl->schema == NULL || impl->slots == NULL || impl->payloads == NULL ||
        impl->lock == NULL) {
        cflow_mailbox_impl_free(impl);
        return CFLOW_MAILBOX_ALLOCATION_FAILED;
    }

    memcpy(impl->schema, schema, schema_bytes);
    qsort(impl->schema, schema_count, sizeof(*impl->schema), cflow_event_type_compare);
    impl->schema_count = schema_count;
    impl->capacity = capacity;
    impl->payload_stride = payload_stride;
    impl->max_payload_size = max_payload_size;
    impl->reserved_payload_bytes = payload_bytes;
    mailbox->impl = impl;
    return CFLOW_MAILBOX_OK;
}

static bool cflow_mailbox_type_index(const cflow_mailbox_impl *impl,
                                     cflow_event_id id,
                                     size_t *type_index) {
    size_t left = 0u;
    size_t right;

    if (impl == NULL || type_index == NULL || id == 0u) return false;
    /* O(log schema_count) lookup, O(1) additional storage. The copied immutable
     * schema is sorted once; producer count never changes this index. */
    right = impl->schema_count;
    while (left < right) {
        const size_t middle = left + (right - left) / 2u;
        if (impl->schema[middle].id == id) {
            *type_index = middle;
            return true;
        }
        if (impl->schema[middle].id < id)
            left = middle + 1u;
        else
            right = middle;
    }
    return false;
}

cflow_mailbox_status cflow_mailbox_try_send(cflow_mailbox *mailbox,
                                            const cflow_event_view *event) {
    cflow_waker waker = {0};
    const cflow_mailbox_status status =
        cflow_mailbox_try_send_detach_internal(mailbox, event, &waker);
    cflow_waker_invoke(waker);
    return status;
}

cflow_mailbox_status cflow_mailbox_try_send_detach_internal(
    cflow_mailbox *mailbox, const cflow_event_view *event,
    cflow_waker *out_waker) {
    cflow_mailbox_impl *impl =
        mailbox != NULL ? (cflow_mailbox_impl *)mailbox->impl : NULL;
    const cmeta_type_desc *schema_type;
    size_t type_index;
    size_t tail;

    if (out_waker != NULL) *out_waker = (cflow_waker){0};
    if (impl == NULL || out_waker == NULL || event == NULL || event->payload_type == NULL ||
        event->payload == NULL ||
        !cflow_mailbox_type_index(impl, event->id, &type_index))
        return CFLOW_MAILBOX_INVALID_ARGUMENT;
    schema_type = impl->schema[type_index].payload_type;
    /* The canonical descriptor was validated at init and is immutable. Other
     * descriptor addresses still require CMeta semantic equality (TU/DSO). */
    if (schema_type != event->payload_type &&
        !cmeta_type_equal(schema_type, event->payload_type))
        return CFLOW_MAILBOX_TYPE_MISMATCH;

    cmeta_mutex_lock(&impl->lock);
    if (impl->terminal == CFLOW_MAILBOX_TERMINAL_CANCELLED) {
        cflow_counter_increment(&impl->rejected_cancelled);
        cmeta_mutex_unlock(&impl->lock);
        return CFLOW_MAILBOX_CANCELLED;
    }
    if (impl->terminal == CFLOW_MAILBOX_TERMINAL_DRAINING) {
        cflow_counter_increment(&impl->rejected_closed);
        cmeta_mutex_unlock(&impl->lock);
        return CFLOW_MAILBOX_CLOSED;
    }
    if (impl->count == impl->capacity) {
        cflow_counter_increment(&impl->rejected_full);
        cmeta_mutex_unlock(&impl->lock);
        return CFLOW_MAILBOX_FULL;
    }
    tail = impl->count >= impl->capacity - impl->head
        ? impl->count - (impl->capacity - impl->head)
        : impl->head + impl->count;
    memcpy(impl->payloads + tail * impl->payload_stride,
           event->payload, schema_type->size);
    impl->slots[tail].type_index = type_index;
    ++impl->count;
    if (impl->count > impl->peak_pending)
        impl->peak_pending = impl->count;
    cflow_counter_increment(&impl->accepted);
    *out_waker = impl->waiter;
    impl->waiter = (cflow_waker){0};
    cmeta_mutex_unlock(&impl->lock);
    return CFLOW_MAILBOX_OK;
}

cflow_mailbox_status cflow_mailbox_try_receive(
    cflow_mailbox *mailbox,
    cflow_event_id *out_id,
    const cmeta_type_desc **out_type,
    void *out_payload,
    size_t out_payload_capacity) {
    cflow_mailbox_impl *impl =
        mailbox != NULL ? (cflow_mailbox_impl *)mailbox->impl : NULL;
    const cflow_event_type *event_type;
    size_t head;

    if (out_id != NULL) *out_id = 0u;
    if (out_type != NULL) *out_type = NULL;
    if (impl == NULL || out_id == NULL || out_type == NULL ||
        out_payload == NULL)
        return CFLOW_MAILBOX_INVALID_ARGUMENT;

    cmeta_mutex_lock(&impl->lock);
    if (impl->count == 0u) {
        const cflow_mailbox_status status =
            impl->terminal == CFLOW_MAILBOX_TERMINAL_CANCELLED
                ? CFLOW_MAILBOX_CANCELLED
                : impl->terminal == CFLOW_MAILBOX_TERMINAL_DRAINING
                      ? CFLOW_MAILBOX_CLOSED
                      : CFLOW_MAILBOX_EMPTY;
        cmeta_mutex_unlock(&impl->lock);
        return status;
    }
    head = impl->head;
    event_type = &impl->schema[impl->slots[head].type_index];
    if (out_payload_capacity < event_type->payload_type->size) {
        cmeta_mutex_unlock(&impl->lock);
        return CFLOW_MAILBOX_BUFFER_TOO_SMALL;
    }
    if (((uintptr_t)out_payload % event_type->payload_type->align) != 0u) {
        cmeta_mutex_unlock(&impl->lock);
        return CFLOW_MAILBOX_INVALID_ARGUMENT;
    }

    memcpy(out_payload, impl->payloads + head * impl->payload_stride,
           event_type->payload_type->size);
    impl->head = head + 1u == impl->capacity ? 0u : head + 1u;
    --impl->count;
    cflow_counter_increment(&impl->received);
    *out_id = event_type->id;
    *out_type = event_type->payload_type;
    cmeta_mutex_unlock(&impl->lock);
    return CFLOW_MAILBOX_OK;
}

bool cflow_mailbox_get_stats(const cflow_mailbox *mailbox,
                             cflow_mailbox_stats *out) {
    return cflow_mailbox_get_stats_observe_internal(mailbox, out, NULL, NULL);
}

bool cflow_mailbox_get_stats_observe_internal(
    const cflow_mailbox *mailbox, cflow_mailbox_stats *out,
    void (*observe)(void *, const cflow_mailbox_stats *), void *user) {
    cflow_mailbox_impl *impl =
        mailbox != NULL ? (cflow_mailbox_impl *)mailbox->impl : NULL;
    cflow_mailbox_stats snapshot;

    if (impl == NULL || out == NULL) return false;
    cmeta_mutex_lock(&impl->lock);
    snapshot.schema_count = impl->schema_count;
    snapshot.capacity = impl->capacity;
    snapshot.pending = impl->count;
    snapshot.peak_pending = impl->peak_pending;
    snapshot.payload_stride = impl->payload_stride;
    snapshot.reserved_payload_bytes = impl->reserved_payload_bytes;
    snapshot.accepted = impl->accepted;
    snapshot.received = impl->received;
    snapshot.rejected_full = impl->rejected_full;
    snapshot.rejected_closed = impl->rejected_closed;
    snapshot.rejected_cancelled = impl->rejected_cancelled;
    snapshot.cancelled = impl->cancelled;
    if (observe != NULL) observe(user, &snapshot);
    cmeta_mutex_unlock(&impl->lock);
    *out = snapshot;
    return true;
}

cflow_mailbox_status cflow_mailbox_close(cflow_mailbox *mailbox) {
    cflow_mailbox_impl *impl =
        mailbox != NULL ? (cflow_mailbox_impl *)mailbox->impl : NULL;
    cflow_waker waker = {0};

    if (impl == NULL) return CFLOW_MAILBOX_INVALID_ARGUMENT;
    cmeta_mutex_lock(&impl->lock);
    if (impl->terminal == CFLOW_MAILBOX_TERMINAL_CANCELLED) {
        cmeta_mutex_unlock(&impl->lock);
        return CFLOW_MAILBOX_CANCELLED;
    }
    if (impl->terminal == CFLOW_MAILBOX_TERMINAL_DRAINING) {
        cmeta_mutex_unlock(&impl->lock);
        return CFLOW_MAILBOX_CLOSED;
    }
    impl->terminal = CFLOW_MAILBOX_TERMINAL_DRAINING;
    waker = impl->waiter;
    impl->waiter = (cflow_waker){0};
    cmeta_mutex_unlock(&impl->lock);
    cflow_waker_invoke(waker);
    return CFLOW_MAILBOX_OK;
}

cflow_mailbox_status cflow_mailbox_cancel(cflow_mailbox *mailbox) {
    cflow_waker waker = {0};
    const cflow_mailbox_status status =
        cflow_mailbox_cancel_detach_internal(mailbox, &waker);
    cflow_waker_invoke(waker);
    return status;
}

cflow_mailbox_status cflow_mailbox_cancel_detach_internal(
    cflow_mailbox *mailbox, cflow_waker *out_waker) {
    cflow_mailbox_impl *impl =
        mailbox != NULL ? (cflow_mailbox_impl *)mailbox->impl : NULL;

    if (out_waker != NULL) *out_waker = (cflow_waker){0};
    if (impl == NULL || out_waker == NULL)
        return CFLOW_MAILBOX_INVALID_ARGUMENT;
    cmeta_mutex_lock(&impl->lock);
    if (impl->terminal == CFLOW_MAILBOX_TERMINAL_CANCELLED) {
        cmeta_mutex_unlock(&impl->lock);
        return CFLOW_MAILBOX_CANCELLED;
    }
    impl->terminal = CFLOW_MAILBOX_TERMINAL_CANCELLED;
    cflow_counter_add_size(&impl->cancelled, impl->count);
    impl->head = 0u;
    impl->count = 0u;
    *out_waker = impl->waiter;
    impl->waiter = (cflow_waker){0};
    cmeta_mutex_unlock(&impl->lock);
    return CFLOW_MAILBOX_OK;
}

static bool cflow_mailbox_wait_arm(void *state, cflow_waker waker) {
    cflow_mailbox_impl *impl = (cflow_mailbox_impl *)state;
    bool ready;

    if (impl == NULL || waker.wake == NULL) return false;
    cmeta_mutex_lock(&impl->lock);
    ready = impl->count != 0u ||
            impl->terminal != CFLOW_MAILBOX_TERMINAL_OPEN;
    if (!ready && impl->waiter.wake != NULL) {
        cmeta_mutex_unlock(&impl->lock);
        return false;
    }
    if (!ready) impl->waiter = waker;
    cmeta_mutex_unlock(&impl->lock);
    if (ready) cflow_waker_invoke(waker);
    return true;
}

static void cflow_mailbox_wait_cancel(void *state) {
    cflow_mailbox_impl *impl = (cflow_mailbox_impl *)state;

    if (impl == NULL) return;
    cmeta_mutex_lock(&impl->lock);
    impl->waiter = (cflow_waker){0};
    cmeta_mutex_unlock(&impl->lock);
}

CMETA_IMPLEMENTS(cflow_waitable, cflow_mailbox_waitable, 0,
    .arm = cflow_mailbox_wait_arm,
    .cancel = cflow_mailbox_wait_cancel
);

cflow_waitable cflow_mailbox_as_waitable(cflow_mailbox *mailbox) {
    cflow_mailbox_impl *impl =
        mailbox != NULL ? (cflow_mailbox_impl *)mailbox->impl : NULL;
    if (impl == NULL) return (cflow_waitable){0};
    return cflow_mailbox_waitable_as_cflow_waitable(impl);
}

size_t cflow_mailbox_payload_capacity(const cflow_mailbox *mailbox) {
    const cflow_mailbox_impl *impl =
        mailbox != NULL ? (const cflow_mailbox_impl *)mailbox->impl : NULL;
    return impl != NULL ? impl->max_payload_size : 0u;
}

void cflow_mailbox_destroy(cflow_mailbox *mailbox) {
    cflow_mailbox_impl *impl;

    if (mailbox == NULL) return;
    impl = (cflow_mailbox_impl *)mailbox->impl;
    mailbox->impl = NULL;
    cflow_mailbox_impl_free(impl);
}
