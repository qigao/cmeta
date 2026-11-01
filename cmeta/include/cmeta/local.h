#ifndef CMETA_LOCAL_H
#define CMETA_LOCAL_H

#include <cmeta/lifecycle.h>
#include <thread.h>

#include <stdbool.h>

/*
 * CMeta lifecycle adapter over Platform-owned thread affinity.
 *
 * Platform owns thread identity/affinity/TLS policy. CMeta owns only the
 * DataDesc lifecycle binding for the payload.
 */
typedef struct cmeta_local_state {
    cmeta_thread_affine_state affinity;
    const cmeta_data_construct_ops *ops;
} cmeta_local_state;

CMETA_INLINE cmeta_status cmeta_local_check(
    cmeta_local_state *state, const void *owner) {
    int status = cmeta_thread_affine_check(
        state != NULL ? &state->affinity : NULL, owner);
    if (status == SALTS_OK)
        return CMETA_OK;
    return status == SALTS_EBUSY ? CMETA_BUSY : CMETA_INVALID_ARGUMENT;
}

CMETA_INLINE cmeta_status cmeta_local_init(
    cmeta_local_state *state, void *owner, void *value,
    const cmeta_data_desc *data, size_t size, size_t align) {
    const cmeta_data_construct_ops *ops;
    cmeta_status status;

    if (state == NULL || owner == NULL || value == NULL ||
        state->affinity.owner != NULL)
        return CMETA_INVALID_ARGUMENT;

    status = cmeta_lifecycle_bind(data, size, align, &ops);
    if (status != CMETA_OK)
        return status;

    if (cmeta_thread_affine_init(&state->affinity, owner) != SALTS_OK)
        return CMETA_INVALID_ARGUMENT;

    state->ops = ops;
    if (cmeta_thread_affine_set_busy(&state->affinity, owner, true) != SALTS_OK)
        return CMETA_INVALID_ARGUMENT;
    status = ops->init_zero(value);
    if (status != CMETA_OK) {
        ops->restore_zero(value);
        if (cmeta_thread_affine_set_busy(&state->affinity, owner, false) != SALTS_OK ||
            cmeta_thread_affine_reset(&state->affinity, owner) != SALTS_OK)
            return CMETA_INVALID_ARGUMENT;
        state->ops = NULL;
        return status;
    }

    if (cmeta_thread_affine_set_busy(&state->affinity, owner, false) != SALTS_OK)
        return CMETA_INVALID_ARGUMENT;
    return CMETA_OK;
}

CMETA_INLINE cmeta_status cmeta_local_destroy(
    cmeta_local_state *state, void *owner, void *value) {
    cmeta_status status = cmeta_local_check(state, owner);
    if (status != CMETA_OK)
        return status;
    if (value == NULL || state->ops == NULL)
        return CMETA_INVALID_ARGUMENT;

    if (cmeta_thread_affine_set_busy(&state->affinity, owner, true) != SALTS_OK)
        return CMETA_INVALID_ARGUMENT;
    state->ops->restore_zero(value);
    if (cmeta_thread_affine_set_busy(&state->affinity, owner, false) != SALTS_OK ||
        cmeta_thread_affine_reset(&state->affinity, owner) != SALTS_OK)
        return CMETA_INVALID_ARGUMENT;
    state->ops = NULL;
    return CMETA_OK;
}

/*
 * Explicit typed lifecycle adapter. This is intentionally not registered as a
 * cmeta_type(...) generic: Local runtime ownership belongs to Platform.
 */
#define cmeta_local_type(name_, type_) \
    typedef struct name_ { cmeta_local_state state; type_ value; } name_; \
    CMETA_INLINE cmeta_status name_##_init(name_ *p) { \
        if (p == NULL) return CMETA_INVALID_ARGUMENT; \
        return cmeta_local_init(&p->state, p, &p->value, \
                                CMETA_DATA_ACCESSOR_(type_)(), \
                                sizeof(type_), CMETA_ALIGNOF(type_)); \
    } \
    CMETA_INLINE type_ *name_##_get(name_ *p) { \
        return p != NULL && cmeta_local_check(&p->state, p) == CMETA_OK \
                   ? &p->value : NULL; \
    } \
    CMETA_INLINE cmeta_status name_##_destroy(name_ *p) { \
        return p != NULL \
                   ? cmeta_local_destroy(&p->state, p, &p->value) \
                   : CMETA_INVALID_ARGUMENT; \
    } \
    typedef type_ name_##_value_type

#endif
