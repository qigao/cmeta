#ifndef CMETA_POOL_H
#define CMETA_POOL_H

#include <cmeta/lifecycle.h>
#include <memory/object_pool.h>

/*
 * CMeta lifecycle adapter over Core-owned pool storage/lease semantics.
 *
 * Core owns capacity, storage, slot leases and thread-affinity policy. CMeta
 * owns only payload lifecycle binding through canonical DataDesc.
 */
typedef struct cmeta_pool_state {
    object_pool_owner_state owner;
    const cmeta_data_construct_ops *ops;
} cmeta_pool_state;

typedef struct cmeta_pool_lease {
    object_pool_lease owner;
} cmeta_pool_lease;

CMETA_INLINE cmeta_status cmeta_pool_owner_status(int status) {
    switch (status) {
        case SALTS_OK: return CMETA_OK;
        case SALTS_EBUSY: return CMETA_BUSY;
        case SALTS_ENOBUFS:
        case SALTS_ERANGE: return CMETA_CAPACITY_EXCEEDED;
        case SALTS_ENOMEM: return CMETA_OUT_OF_MEMORY;
        default: return CMETA_INVALID_ARGUMENT;
    }
}

CMETA_INLINE cmeta_status cmeta_pool_check(cmeta_pool_state *pool) {
    return cmeta_pool_owner_status(object_pool_owner_check(
        pool != NULL ? &pool->owner : NULL));
}

CMETA_INLINE cmeta_status cmeta_pool_init(
    cmeta_pool_state *pool, const cmeta_data_desc *data,
    size_t size, size_t align, size_t capacity) {
    const cmeta_data_construct_ops *ops;
    cmeta_status status;
    int owner_status;

    if (pool == NULL || pool->owner.self != NULL || pool->owner.storage != NULL)
        return CMETA_INVALID_ARGUMENT;

    owner_status = object_pool_owner_init(
        &pool->owner, size, align, capacity);
    if (owner_status != SALTS_OK)
        return cmeta_pool_owner_status(owner_status);

    status = cmeta_lifecycle_bind(data, size, align, &ops);
    if (status != CMETA_OK) {
        owner_status = object_pool_owner_destroy(&pool->owner);
        return owner_status == SALTS_OK ? status : cmeta_pool_owner_status(owner_status);
    }

    pool->ops = ops;
    return CMETA_OK;
}

CMETA_INLINE cmeta_status cmeta_pool_acquire(
    cmeta_pool_state *pool, cmeta_pool_lease *lease) {
    void *value;
    cmeta_status status;
    int owner_status;

    if (pool == NULL || lease == NULL || pool->ops == NULL)
        return CMETA_INVALID_ARGUMENT;

    owner_status = object_pool_owner_acquire(&pool->owner, &lease->owner);
    if (owner_status != SALTS_OK)
        return cmeta_pool_owner_status(owner_status);

    value = object_pool_lease_get(&pool->owner, &lease->owner);
    if (value == NULL) {
        owner_status = object_pool_owner_release(&pool->owner, &lease->owner);
        return owner_status == SALTS_OK ? CMETA_INVALID_ARGUMENT : cmeta_pool_owner_status(owner_status);
    }

    owner_status = object_pool_owner_begin(&pool->owner);
    if (owner_status != SALTS_OK) return cmeta_pool_owner_status(owner_status);
    status = pool->ops->init_zero(value);
    if (status != CMETA_OK) pool->ops->restore_zero(value);
    owner_status = object_pool_owner_end(&pool->owner);
    if (owner_status != SALTS_OK) return cmeta_pool_owner_status(owner_status);
    if (status != CMETA_OK) {
        owner_status = object_pool_owner_release(&pool->owner, &lease->owner);
        if (owner_status != SALTS_OK) return cmeta_pool_owner_status(owner_status);
    }
    return status;
}

CMETA_INLINE cmeta_status cmeta_pool_lease_check(
    cmeta_pool_state *pool, cmeta_pool_lease *lease) {
    if (pool == NULL || lease == NULL)
        return CMETA_INVALID_ARGUMENT;
    return cmeta_pool_owner_status(
        object_pool_lease_check(&pool->owner, &lease->owner));
}

CMETA_INLINE void *cmeta_pool_get(
    cmeta_pool_state *pool, cmeta_pool_lease *lease) {
    return cmeta_pool_lease_check(pool, lease) == CMETA_OK
               ? lease->owner.value
               : NULL;
}

CMETA_INLINE cmeta_status cmeta_pool_release(
    cmeta_pool_state *pool, cmeta_pool_lease *lease) {
    cmeta_status status = cmeta_pool_lease_check(pool, lease);
    int owner_status;

    if (status != CMETA_OK)
        return status;

    owner_status = object_pool_owner_begin(&pool->owner);
    if (owner_status != SALTS_OK) return cmeta_pool_owner_status(owner_status);
    pool->ops->restore_zero(lease->owner.value);
    owner_status = object_pool_owner_end(&pool->owner);
    if (owner_status != SALTS_OK) return cmeta_pool_owner_status(owner_status);

    owner_status = object_pool_owner_release(&pool->owner, &lease->owner);
    return cmeta_pool_owner_status(owner_status);
}

/* Destination must be external, live semantic-zero storage. The moved-from
 * lease remains live and must still be released. */
CMETA_INLINE cmeta_status cmeta_pool_move_out(
    cmeta_pool_state *pool, cmeta_pool_lease *lease, void *destination) {
    cmeta_status status = cmeta_pool_lease_check(pool, lease);

    if (status != CMETA_OK)
        return status;
    int owner_status = object_pool_owner_check_destination(
        &pool->owner, &lease->owner, destination);
    if (owner_status != SALTS_OK) return cmeta_pool_owner_status(owner_status);
    if (pool->ops == NULL || pool->ops->move == NULL)
        return CMETA_TRAIT_MISSING;

    owner_status = object_pool_owner_begin(&pool->owner);
    if (owner_status != SALTS_OK) return cmeta_pool_owner_status(owner_status);
    pool->ops->move(destination, lease->owner.value);
    return cmeta_pool_owner_status(object_pool_owner_end(&pool->owner));
}

CMETA_INLINE cmeta_status cmeta_pool_destroy(cmeta_pool_state *pool) {
    int status;
    if (pool == NULL || pool->ops == NULL)
        return CMETA_INVALID_ARGUMENT;
    status = object_pool_owner_destroy(&pool->owner);
    if (status != SALTS_OK)
        return cmeta_pool_owner_status(status);
    pool->ops = NULL;
    return CMETA_OK;
}

/*
 * Explicit typed lifecycle adapter. This is intentionally not registered as a
 * cmeta_type(...) generic: pool runtime ownership belongs to Salts::Core.
 */
#define cmeta_pool_type(name_, type_) \
    typedef struct name_ { cmeta_pool_state state; } name_; \
    typedef struct name_##_lease { cmeta_pool_lease state; } name_##_lease; \
    CMETA_INLINE cmeta_status name_##_init(name_ *p, size_t capacity) { \
        return cmeta_pool_init( \
            p != NULL ? &p->state : NULL, CMETA_DATA_ACCESSOR_(type_)(), \
            sizeof(type_), CMETA_ALIGNOF(type_), capacity); \
    } \
    CMETA_INLINE cmeta_status name_##_acquire(name_ *p, name_##_lease *l) { \
        return cmeta_pool_acquire( \
            p != NULL ? &p->state : NULL, l != NULL ? &l->state : NULL); \
    } \
    CMETA_INLINE type_ *name_##_get(name_ *p, name_##_lease *l) { \
        return (type_ *)cmeta_pool_get( \
            p != NULL ? &p->state : NULL, l != NULL ? &l->state : NULL); \
    } \
    CMETA_INLINE cmeta_status name_##_release(name_ *p, name_##_lease *l) { \
        return cmeta_pool_release( \
            p != NULL ? &p->state : NULL, l != NULL ? &l->state : NULL); \
    } \
    CMETA_INLINE cmeta_status name_##_move_out( \
        name_ *p, name_##_lease *l, type_ *dst) { \
        return cmeta_pool_move_out( \
            p != NULL ? &p->state : NULL, l != NULL ? &l->state : NULL, dst); \
    } \
    CMETA_INLINE cmeta_status name_##_destroy(name_ *p) { \
        return cmeta_pool_destroy(p != NULL ? &p->state : NULL); \
    } \
    typedef type_ name_##_value_type

#endif
