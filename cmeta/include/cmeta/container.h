#ifndef CMETA_CONTAINER_H
#define CMETA_CONTAINER_H

/*
 * Typed container facade meta-schema.
 *
 * The raw container implementation remains ordinary hand-written C.  CMeta
 * generates only the repetitive typed facade. A typed container declaration is
 * a complete finite instantiation: wrapper methods, descriptors, and Range
 * adapters are header-local. Raw container algorithms remain ordinary compiled C.
 */

#include <cmeta/pp.h>
#include <cmeta/range.h>
#include <stdbool.h>
#include <stddef.h>

#define CMETA_CONTAINER_API_I(prefix, suffix) prefix##_##suffix
#define CMETA_CONTAINER_API(prefix, suffix) CMETA_CONTAINER_API_I(prefix, suffix)

CMETA_INLINE bool cmeta_container_range_constructs_values(
    const cmeta_type_desc *type) {
    return type != NULL && type->traits != NULL &&
           (type->traits->flags &
            (CMETA_TRAIT_TRIVIAL_COPY | CMETA_TRAIT_TRIVIAL_DESTROY)) !=
               (CMETA_TRAIT_TRIVIAL_COPY | CMETA_TRAIT_TRIVIAL_DESTROY) &&
           cmeta_type_require_traits(
               type, CMETA_TRAIT_COPY | CMETA_TRAIT_DESTROY) == CMETA_OK;
}

CMETA_INLINE bool cmeta_container_range_construct(
    const cmeta_type_desc *type, void *destination, const void *source) {
    if (!type || !destination || !source) return false;
    if (cmeta_container_range_constructs_values(type))
        return type->traits->copy_construct(destination, source);
    if (type->traits != NULL &&
        (type->traits->flags &
         (CMETA_TRAIT_TRIVIAL_COPY | CMETA_TRAIT_TRIVIAL_DESTROY)) !=
            (CMETA_TRAIT_TRIVIAL_COPY | CMETA_TRAIT_TRIVIAL_DESTROY))
        return false;
    memcpy(destination, source, type->size);
    return true;
}

CMETA_INLINE cmeta_range_flags cmeta_container_range_flags_for(
    const cmeta_type_desc *type, cmeta_range_flags flags) {
    return cmeta_container_range_constructs_values(type)
        ? flags | CMETA_RANGE_CONSTRUCTS_VALUES : flags;
}


/* -------------------------------------------------------------------------
 * Header-local Range capability generators
 * ------------------------------------------------------------------------- */

#define CMETA_CONTAINER1_INDEX_RANGE_DEFINE_WITH_TYPE_EXT(name, type, type_desc, prefix, flags, version_accessor, collector_factory, ext_expr) \
    CMETA_CONTAINER1_INDEX_RANGE_DEFINE_WITH_TYPE_EXT_VALUE(name, type, type_desc, prefix, flags, version_accessor, collector_factory, ext_expr, NULL, NULL)
#define CMETA_CONTAINER1_INDEX_RANGE_DEFINE_WITH_TYPE_EXT_VALUE(name, type, type_desc, prefix, flags, version_accessor, collector_factory, ext_expr, value_traits, value_identity) \
    CMETA_LOCAL const cmeta_type_desc name##_element_cmeta_type = { \
        CMETA_CONTAINER_STR(type), sizeof(type), _Alignof(type), CMETA_T_OBJECT, NULL, NULL, NULL \
    }; \
    CMETA_LOCAL const cmeta_type_desc name##_cmeta_type = { \
        CMETA_CONTAINER_STR(name), sizeof(name), _Alignof(name), CMETA_T_OBJECT, NULL, (value_traits), (value_identity) \
    }; \
    CMETA_INLINE size_t name##_cmeta_range_size(const void *object) { \
        const name *self = (const name *)object; \
        return self ? CMETA_CONTAINER_API(prefix, size)(&self->raw) : 0U; \
    } \
    CMETA_INLINE cmeta_gen_status name##_cmeta_range_next(const void *object, cmeta_range_cursor *cursor, void *out_value) { \
        const name *self = (const name *)object; \
        const type *value; \
        size_t count; \
        if (!self || !cursor || !out_value) return CMETA_GEN_ERROR; \
        count = CMETA_CONTAINER_API(prefix, size)(&self->raw); \
        if (cursor->index >= count) return CMETA_GEN_DONE; \
        value = (const type *)CMETA_CONTAINER_API(prefix, at_const)(&self->raw, cursor->index); \
        if (!value) return CMETA_GEN_ERROR; \
        if (!cmeta_container_range_construct( \
                (type_desc), \
                out_value, value)) return CMETA_GEN_ERROR; \
        ++cursor->index; \
        return cursor->index == count ? CMETA_GEN_VALUE_AND_DONE : CMETA_GEN_VALUE; \
    } \
    CMETA_INLINE cmeta_range name##_range(const name *self) { \
        cmeta_range range = { \
            self, (type_desc), \
            cmeta_container_range_flags_for( \
                (type_desc), (flags)), \
            name##_cmeta_range_size, name##_cmeta_range_next, \
            cmeta_range_capture_version((version_accessor), self), (version_accessor) \
        }; \
        return range; \
    } \
    CMETA_INLINE cmeta_range name##_cmeta_erased_range(const void *object) { \
        return name##_range((const name *)object); \
    } \
    CMETA_LOCAL cmeta_container_desc name##_cmeta_container_desc = { \
        CMETA_CONTAINER_STR(name), &name##_cmeta_type, \
        (type_desc), NULL, NULL, \
        name##_cmeta_erased_range, NULL, NULL, NULL, (collector_factory), (ext_expr) \
    };
#define CMETA_CONTAINER1_INDEX_RANGE_DEFINE_WITH_TYPE(name, type, type_desc, prefix, flags, version_accessor, collector_factory) \
    CMETA_CONTAINER1_INDEX_RANGE_DEFINE_WITH_TYPE_EXT(name, type, type_desc, prefix, flags, version_accessor, collector_factory, NULL)


#define CMETA_CONTAINER1_INDEX_RANGE_DEFINE(name, type, prefix, flags, version_accessor, collector_factory) \
    CMETA_CONTAINER1_INDEX_RANGE_DEFINE_WITH_TYPE(name, type, CMETA_TYPEOF_OR(type, &name##_element_cmeta_type), prefix, flags, version_accessor, collector_factory)
#define CMETA_CONTAINER1_LINK_RANGE_DEFINE_WITH_TYPE_EXT(name, type, type_desc, prefix, flags, version_accessor, collector_factory, ext_expr) \
    CMETA_CONTAINER1_LINK_RANGE_DEFINE_WITH_TYPE_EXT_VALUE(name, type, type_desc, prefix, flags, version_accessor, collector_factory, ext_expr, NULL, NULL)
#define CMETA_CONTAINER1_LINK_RANGE_DEFINE_WITH_TYPE_EXT_VALUE(name, type, type_desc, prefix, flags, version_accessor, collector_factory, ext_expr, value_traits, value_identity) \
    CMETA_LOCAL const cmeta_type_desc name##_element_cmeta_type = { \
        CMETA_CONTAINER_STR(type), sizeof(type), _Alignof(type), CMETA_T_OBJECT, NULL, NULL, NULL \
    }; \
    CMETA_LOCAL const cmeta_type_desc name##_cmeta_type = { \
        CMETA_CONTAINER_STR(name), sizeof(name), _Alignof(name), CMETA_T_OBJECT, NULL, (value_traits), (value_identity) \
    }; \
    CMETA_INLINE size_t name##_cmeta_range_size(const void *object) { \
        const name *self = (const name *)object; \
        return self ? CMETA_CONTAINER_API(prefix, size)(&self->raw) : 0U; \
    } \
    CMETA_INLINE cmeta_gen_status name##_cmeta_range_next(const void *object, cmeta_range_cursor *cursor, void *out_value) { \
        const name *self = (const name *)object; const void *value = NULL; \
        cmeta_range_cursor next_cursor; \
        if (!self || !cursor || !out_value) return CMETA_GEN_ERROR; \
        next_cursor = *cursor; \
        if (!CMETA_CONTAINER_API(prefix, range_next)(&self->raw, &next_cursor, &value)) return CMETA_GEN_DONE; \
        if (!cmeta_container_range_construct( \
                (type_desc), \
                out_value, value)) return CMETA_GEN_ERROR; \
        *cursor = next_cursor; \
        return cursor->state[0] == NULL ? CMETA_GEN_VALUE_AND_DONE : CMETA_GEN_VALUE; \
    } \
    CMETA_INLINE cmeta_range name##_range(const name *self) { \
        cmeta_range range = { self, (type_desc), \
            cmeta_container_range_flags_for( \
                (type_desc), (flags)), \
            name##_cmeta_range_size, name##_cmeta_range_next, \
            cmeta_range_capture_version((version_accessor), self), (version_accessor) }; \
        return range; \
    } \
    CMETA_INLINE cmeta_range name##_cmeta_erased_range(const void *object) { \
        return name##_range((const name *)object); \
    } \
    CMETA_LOCAL cmeta_container_desc name##_cmeta_container_desc = { \
        CMETA_CONTAINER_STR(name), &name##_cmeta_type, \
        (type_desc), NULL, NULL, \
        name##_cmeta_erased_range, NULL, NULL, NULL, (collector_factory), (ext_expr) \
    };
#define CMETA_CONTAINER1_LINK_RANGE_DEFINE_WITH_TYPE(name, type, type_desc, prefix, flags, version_accessor, collector_factory) \
    CMETA_CONTAINER1_LINK_RANGE_DEFINE_WITH_TYPE_EXT(name, type, type_desc, prefix, flags, version_accessor, collector_factory, NULL)


#define CMETA_CONTAINER1_LINK_RANGE_DEFINE(name, type, prefix, flags, version_accessor, collector_factory) \
    CMETA_CONTAINER1_LINK_RANGE_DEFINE_WITH_TYPE(name, type, CMETA_TYPEOF_OR(type, &name##_element_cmeta_type), prefix, flags, version_accessor, collector_factory)
/* -------------------------------------------------------------------------
 * One-type facade
 * ------------------------------------------------------------------------- */
#define CMETA_C1_INLINE_DISPATCH(kind, pub, op, extra, ctx) \
    CMETA_C1_INLINE_DISPATCH_I(kind, pub, op, extra, CMETA_PP_UNPAREN ctx)
#define CMETA_C1_INLINE_DISPATCH_I(kind, pub, op, extra, ...) \
    CMETA_PP_CAT(CMETA_C1_INLINE_, kind)(pub, op, extra, __VA_ARGS__)

#define CMETA_CONTAINER1_DECLARE(name, raw_type) \
    typedef struct name { cmeta_container_header cmeta; raw_type raw; } name

/* Replay only methods for an already declared wrapper. descriptor_expr may
 * query immutable metadata owned by another module at runtime. */
#define CMETA_CONTAINER1_METHODS_WITH_DESCRIPTOR(name, type, type_desc, raw_type, prefix, ok_code, descriptor_expr, methods) \
    methods(CMETA_C1_INLINE_DISPATCH, (name, type, type_desc, raw_type, prefix, ok_code, descriptor_expr))
#define CMETA_CONTAINER2_METHODS_WITH_DESCRIPTOR(name, key_type, value_type, key_desc, value_desc, raw_type, prefix, ok_code, descriptor_expr, methods) \
    methods(CMETA_C2_INLINE_DISPATCH, (name, key_type, value_type, key_desc, value_desc, raw_type, prefix, ok_code, descriptor_expr))

#define CMETA_CONTAINER1_DEFINE_WITH_TYPE(name, type, type_desc, raw_type, prefix, ok_code, aux, methods) \
    CMETA_CONTAINER1_DECLARE(name, raw_type); \
    CMETA_LOCAL cmeta_container_desc name##_cmeta_container_desc; \
    CMETA_CONTAINER1_METHODS_WITH_DESCRIPTOR(name, type, type_desc, raw_type, prefix, ok_code, &name##_cmeta_container_desc, methods)
#define CMETA_CONTAINER1_DEFINE(name, type, raw_type, prefix, ok_code, aux, methods) \
    CMETA_CONTAINER1_DEFINE_WITH_TYPE(name, type, CMETA_TYPEOF(type), raw_type, prefix, ok_code, aux, methods)


#define CMETA_C1_INLINE_INIT_SIZE(pub, op, extra, name, type, type_desc, raw_type, prefix, ok, aux) \
    CMETA_INLINE int name##_##pub(name *self, size_t limit) { \
        int rc; \
        if (!self) return CMETA_INVALID_ARGUMENT; \
        rc = CMETA_CONTAINER_API(prefix, op)(&self->raw, (type_desc), limit); \
        if (rc == (ok)) { \
            self->cmeta.descriptor = (aux); \
        } \
        return rc; \
    }

#define CMETA_C1_INLINE_FROM_ARRAY_SIZE(pub, op, extra, name, type, type_desc, raw_type, prefix, ok, aux) \
    CMETA_INLINE int name##_##pub(name *self, const type *values, size_t count, size_t limit) { \
        int rc; \
        if (!self) return CMETA_INVALID_ARGUMENT; \
        rc = CMETA_CONTAINER_API(prefix, op)(&self->raw, values, count, (type_desc), limit); \
        if (rc == (ok)) { \
            self->cmeta.descriptor = (aux); \
        } \
        return rc; \
    }

#define CMETA_C1_INLINE_INIT_SIZE_COMPARE(pub, op, extra, name, type, type_desc, raw_type, prefix, ok, aux) \
    CMETA_INLINE int name##_##pub(name *self, size_t limit) { \
        int rc; \
        if (!self) return CMETA_INVALID_ARGUMENT; \
        rc = CMETA_CONTAINER_API(prefix, op)(&self->raw, (type_desc), limit); \
        if (rc == (ok)) { \
            self->cmeta.descriptor = (aux); \
        } \
        return rc; \
    }

#define CMETA_C1_INLINE_FROM_ARRAY_SIZE_COMPARE(pub, op, extra, name, type, type_desc, raw_type, prefix, ok, aux) \
    CMETA_INLINE int name##_##pub(name *self, const type *values, size_t count, size_t limit) { \
        int rc; \
        if (!self) return CMETA_INVALID_ARGUMENT; \
        rc = CMETA_CONTAINER_API(prefix, op)(&self->raw, values, count, (type_desc), limit); \
        if (rc == (ok)) { \
            self->cmeta.descriptor = (aux); \
        } \
        return rc; \
    }

#define CMETA_C1_INLINE_INIT_KEY_HASH(pub, op, extra, name, type, type_desc, raw_type, prefix, ok, aux) \
    CMETA_INLINE int name##_##pub(name *self, size_t limit) { \
        int rc; \
        if (!self) return CMETA_INVALID_ARGUMENT; \
        rc = CMETA_CONTAINER_API(prefix, op)(&self->raw, (type_desc), limit); \
        if (rc == (ok)) { \
            self->cmeta.descriptor = (aux); \
        } \
        return rc; \
    }

#define CMETA_C1_INLINE_FROM_KEYS_HASH(pub, op, extra, name, type, type_desc, raw_type, prefix, ok, aux) \
    CMETA_INLINE int name##_##pub(name *self, const type *values, size_t count, size_t limit) { \
        int rc; \
        if (!self) return CMETA_INVALID_ARGUMENT; \
        rc = CMETA_CONTAINER_API(prefix, op)(&self->raw, values, count, (type_desc), limit); \
        if (rc == (ok)) { \
            self->cmeta.descriptor = (aux); \
        } \
        return rc; \
    }

#define CMETA_C1_INLINE_INIT_KEY_COMPARE CMETA_C1_INLINE_INIT_KEY_HASH
#define CMETA_C1_INLINE_FROM_KEYS_COMPARE CMETA_C1_INLINE_FROM_KEYS_HASH

#define CMETA_C1_INLINE_DESTROY(pub, op, extra, name, type, type_desc, raw_type, prefix, ok, aux) \
    CMETA_INLINE void name##_##pub(name *self) { if (self) { CMETA_CONTAINER_API(prefix, op)(&self->raw); self->cmeta.descriptor = NULL; } }
#define CMETA_C1_INLINE_CLEAR(pub, op, extra, name, type, type_desc, raw_type, prefix, ok, aux) \
    CMETA_INLINE void name##_##pub(name *self) { CMETA_CONTAINER_API(prefix, op)(&self->raw); }

#define CMETA_C1_INLINE_RESERVE(pub, op, extra, name, type, type_desc, raw_type, prefix, ok, aux) \
    CMETA_INLINE int name##_##pub(name *self, size_t capacity) { return CMETA_CONTAINER_API(prefix, op)(&self->raw, capacity); }

#define CMETA_C1_INLINE_PUSH_VALUE(pub, op, extra, name, type, type_desc, raw_type, prefix, ok, aux) \
    CMETA_INLINE int name##_##pub(name *self, type value) { return CMETA_CONTAINER_API(prefix, op)(&self->raw, &value); }

#define CMETA_C1_INLINE_PUSH_VALUE_ITER(pub, op, extra, name, type, type_desc, raw_type, prefix, ok, aux) \
    CMETA_INLINE int name##_##pub(name *self, type value) { return CMETA_CONTAINER_API(prefix, op)(&self->raw, &value, NULL); }

#define CMETA_C1_INLINE_POP_BOOL(pub, op, extra, name, type, type_desc, raw_type, prefix, ok, aux) \
    CMETA_INLINE int name##_##pub(name *self, type *out_value) { return self ? CMETA_CONTAINER_API(prefix, op)(&self->raw, out_value) : CMETA_INVALID_ARGUMENT; }

#define CMETA_C1_INLINE_PTR_INDEX(pub, op, extra, name, type, type_desc, raw_type, prefix, ok, aux) \
    CMETA_INLINE type *name##_##pub(name *self, size_t index) { return (type *)CMETA_CONTAINER_API(prefix, op)(&self->raw, index); }
#define CMETA_C1_INLINE_CONST_PTR_INDEX(pub, op, extra, name, type, type_desc, raw_type, prefix, ok, aux) \
    CMETA_INLINE const type *name##_##pub(const name *self, size_t index) { return (const type *)CMETA_CONTAINER_API(prefix, op)(&self->raw, index); }

#define CMETA_C1_INLINE_PTR(pub, op, extra, name, type, type_desc, raw_type, prefix, ok, aux) \
    CMETA_INLINE type *name##_##pub(name *self) { return (type *)CMETA_CONTAINER_API(prefix, op)(&self->raw); }
#define CMETA_C1_INLINE_CONST_PTR(pub, op, extra, name, type, type_desc, raw_type, prefix, ok, aux) \
    CMETA_INLINE const type *name##_##pub(const name *self) { return (const type *)CMETA_CONTAINER_API(prefix, op)(&self->raw); }

#define CMETA_C1_INLINE_SIZE(pub, op, extra, name, type, type_desc, raw_type, prefix, ok, aux) \
    CMETA_INLINE size_t name##_##pub(const name *self) { return CMETA_CONTAINER_API(prefix, op)(&self->raw); }
#define CMETA_C1_INLINE_BOOL(pub, op, extra, name, type, type_desc, raw_type, prefix, ok, aux) \
    CMETA_INLINE bool name##_##pub(const name *self) { return CMETA_CONTAINER_API(prefix, op)(&self->raw); }

#define CMETA_C1_INLINE_KEY_VALUE(pub, op, extra, name, type, type_desc, raw_type, prefix, ok, aux) \
    CMETA_INLINE int name##_##pub(name *self, type value) { return CMETA_CONTAINER_API(prefix, op)(&self->raw, &value); }
#define CMETA_C1_INLINE_KEY_CONTAINS(pub, op, extra, name, type, type_desc, raw_type, prefix, ok, aux) \
    CMETA_INLINE bool name##_##pub(const name *self, type value) { return CMETA_CONTAINER_API(prefix, op)(&self->raw, &value); }
#define CMETA_C1_INLINE_KEY_REMOVE_BOOL(pub, op, extra, name, type, type_desc, raw_type, prefix, ok, aux) \
    CMETA_INLINE int name##_##pub(name *self, type value) { return self ? CMETA_CONTAINER_API(prefix, op)(&self->raw, &value) : CMETA_INVALID_ARGUMENT; }

/* -------------------------------------------------------------------------
 * Two-type associative facade
 * ------------------------------------------------------------------------- */
#define CMETA_C2_INLINE_DISPATCH(kind, pub, op, extra, ctx) \
    CMETA_C2_INLINE_DISPATCH_I(kind, pub, op, extra, CMETA_PP_UNPAREN ctx)
#define CMETA_C2_INLINE_DISPATCH_I(kind, pub, op, extra, ...) \
    CMETA_PP_CAT(CMETA_C2_INLINE_, kind)(pub, op, extra, __VA_ARGS__)

#define CMETA_CONTAINER2_DECLARE(name, key_type, value_type, raw_type) \
    typedef struct name##_entry { key_type key; value_type value; } name##_entry; \
    typedef struct name { cmeta_container_header cmeta; raw_type raw; } name

#define CMETA_CONTAINER2_DEFINE_WITH_TYPES(name, key_type, value_type, key_desc, value_desc, raw_type, prefix, ok_code, aux, methods) \
    CMETA_CONTAINER2_DECLARE(name, key_type, value_type, raw_type); \
    CMETA_LOCAL cmeta_container_desc name##_cmeta_container_desc; \
    CMETA_CONTAINER2_METHODS_WITH_DESCRIPTOR(name, key_type, value_type, key_desc, value_desc, raw_type, prefix, ok_code, &name##_cmeta_container_desc, methods)
#define CMETA_CONTAINER2_DEFINE(name, key_type, value_type, raw_type, prefix, ok_code, aux, methods) \
    CMETA_CONTAINER2_DEFINE_WITH_TYPES(name, key_type, value_type, CMETA_TYPEOF(key_type), CMETA_TYPEOF(value_type), raw_type, prefix, ok_code, aux, methods)

#define CMETA_C2_INLINE_INIT_KV_HASH(pub, op, extra, name, kt, vt, key_desc, value_desc, raw_type, prefix, ok, aux) \
    CMETA_INLINE int name##_##pub(name *self, size_t limit) { \
        int rc; \
        if (!self) return CMETA_INVALID_ARGUMENT; \
        rc = CMETA_CONTAINER_API(prefix, op)(&self->raw, (key_desc), (value_desc), limit); \
        if (rc == (ok)) { \
            self->cmeta.descriptor = (aux); \
        } \
        return rc; \
    }
#define CMETA_C2_INLINE_INIT_KV_COMPARE(pub, op, extra, name, kt, vt, key_desc, value_desc, raw_type, prefix, ok, aux) \
    CMETA_INLINE int name##_##pub(name *self, size_t limit) { \
        int rc; \
        if (!self) return CMETA_INVALID_ARGUMENT; \
        rc = CMETA_CONTAINER_API(prefix, op)(&self->raw, (key_desc), (value_desc), limit); \
        if (rc == (ok)) { \
            self->cmeta.descriptor = (aux); \
        } \
        return rc; \
    }
#define CMETA_C2_INLINE_INIT_WITH_ORDER_COMPARE(pub, op, extra, name, kt, vt, key_desc, value_desc, raw_type, prefix, ok, aux) \
    CMETA_INLINE int name##_##pub(name *self, size_t min_degree, size_t limit) { \
        int rc; \
        if (!self) return CMETA_INVALID_ARGUMENT; \
        rc = CMETA_CONTAINER_API(prefix, op)(&self->raw, (key_desc), (value_desc), min_degree, limit); \
        if (rc == (ok)) { \
            self->cmeta.descriptor = (aux); \
        } \
        return rc; \
    }
#define CMETA_C2_INLINE_INIT_KV_MULTIMAP(pub, op, extra, name, kt, vt, key_desc, value_desc, raw_type, prefix, ok, aux) \
    CMETA_INLINE int name##_##pub(name *self, size_t key_limit, size_t value_limit) { \
        int rc; \
        if (!self) return CMETA_INVALID_ARGUMENT; \
        rc = CMETA_CONTAINER_API(prefix, op)(&self->raw, (key_desc), key_limit, (value_desc), value_limit); \
        if (rc == (ok)) { \
            self->cmeta.descriptor = (aux); \
        } \
        return rc; \
    }

#define CMETA_C2_INLINE_FROM_ENTRIES(pub, op, extra, name, kt, vt, key_desc, value_desc, raw_type, prefix, ok, aux) \
    CMETA_INLINE int name##_##pub(name *self, const name##_entry *entries, size_t count, size_t limit) { \
        name temporary = {0}; size_t i; int rc; uint64_t generation; \
        if (!self || (count > 0U && !entries)) return extra; \
        generation = CMETA_CONTAINER_API(prefix, generation)(&self->raw) + UINT64_C(1); \
        rc = name##_init(&temporary, limit); \
        if (rc != (ok)) return rc; \
        rc = CMETA_CONTAINER_API(prefix, reserve)(&temporary.raw, count < limit ? count : limit); \
        if (rc != (ok)) { CMETA_CONTAINER_API(prefix, destroy)(&temporary.raw); return rc; } \
        for (i = 0; i < count; ++i) { \
            rc = CMETA_CONTAINER_API(prefix, put)(&temporary.raw, &entries[i].key, &entries[i].value); \
            if (rc != (ok)) { CMETA_CONTAINER_API(prefix, destroy)(&temporary.raw); return rc; } \
        } \
        CMETA_CONTAINER_API(prefix, destroy)(&self->raw); \
        temporary.raw.generation = generation; \
        *self = temporary; \
        return (ok); \
    }

#define CMETA_C2_INLINE_FROM_ENTRIES_LINK(pub, op, extra, name, kt, vt, key_desc, value_desc, raw_type, prefix, ok, aux) \
    CMETA_INLINE int name##_##pub(name *self, const name##_entry *entries, size_t count, size_t limit) { \
        name temporary = {0}; size_t i; int rc; uint64_t generation; \
        if (!self || (count > 0U && !entries)) return extra; \
        generation = CMETA_CONTAINER_API(prefix, generation)(&self->raw) + UINT64_C(1); \
        rc = name##_init(&temporary, limit); \
        if (rc != (ok)) return rc; \
        for (i = 0; i < count; ++i) { \
            rc = CMETA_CONTAINER_API(prefix, put)(&temporary.raw, &entries[i].key, &entries[i].value); \
            if (rc != (ok)) { CMETA_CONTAINER_API(prefix, destroy)(&temporary.raw); return rc; } \
        } \
        CMETA_CONTAINER_API(prefix, destroy)(&self->raw); \
        temporary.raw.generation = generation; \
        *self = temporary; \
        return (ok); \
    }

#define CMETA_C2_INLINE_FROM_ENTRIES_MULTIMAP(pub, op, extra, name, kt, vt, key_desc, value_desc, raw_type, prefix, ok, aux) \
    CMETA_INLINE int name##_##pub(name *self, const name##_entry *entries, size_t count, size_t key_limit, size_t value_limit) { \
        name temporary = {0}; size_t i; int rc; uint64_t generation; \
        if (!self || (count > 0U && !entries)) return extra; \
        generation = CMETA_CONTAINER_API(prefix, generation)(&self->raw) + UINT64_C(1); \
        rc = name##_init(&temporary, key_limit, value_limit); \
        if (rc != (ok)) return rc; \
        for (i = 0; i < count; ++i) { \
            rc = CMETA_CONTAINER_API(prefix, put)(&temporary.raw, &entries[i].key, &entries[i].value); \
            if (rc != (ok)) { CMETA_CONTAINER_API(prefix, destroy)(&temporary.raw); return rc; } \
        } \
        CMETA_CONTAINER_API(prefix, destroy)(&self->raw); \
        temporary.raw.generation = generation; \
        *self = temporary; \
        return (ok); \
    }

#define CMETA_C2_INLINE_DESTROY(pub, op, extra, name, kt, vt, key_desc, value_desc, raw_type, prefix, ok, aux) CMETA_INLINE void name##_##pub(name *self) { if (self) { CMETA_CONTAINER_API(prefix, op)(&self->raw); self->cmeta.descriptor = NULL; } }
#define CMETA_C2_INLINE_CLEAR(pub, op, extra, name, kt, vt, key_desc, value_desc, raw_type, prefix, ok, aux) CMETA_INLINE void name##_##pub(name *self) { CMETA_CONTAINER_API(prefix, op)(&self->raw); }
#define CMETA_C2_INLINE_RESERVE(pub, op, extra, name, kt, vt, key_desc, value_desc, raw_type, prefix, ok, aux) CMETA_INLINE int name##_##pub(name *self, size_t capacity) { return CMETA_CONTAINER_API(prefix, op)(&self->raw, capacity); }
#define CMETA_C2_INLINE_PUT(pub, op, extra, name, kt, vt, key_desc, value_desc, raw_type, prefix, ok, aux) CMETA_INLINE int name##_##pub(name *self, kt key, vt value) { return CMETA_CONTAINER_API(prefix, op)(&self->raw, &key, &value); }
#define CMETA_C2_INLINE_GET(pub, op, extra, name, kt, vt, key_desc, value_desc, raw_type, prefix, ok, aux) CMETA_INLINE vt *name##_##pub(name *self, kt key) { return (vt *)CMETA_CONTAINER_API(prefix, op)(&self->raw, &key); }
#define CMETA_C2_INLINE_GET_CONST(pub, op, extra, name, kt, vt, key_desc, value_desc, raw_type, prefix, ok, aux) CMETA_INLINE const vt *name##_##pub(const name *self, kt key) { return (const vt *)CMETA_CONTAINER_API(prefix, op)(&self->raw, &key); }
#define CMETA_C2_INLINE_CONTAINS(pub, op, extra, name, kt, vt, key_desc, value_desc, raw_type, prefix, ok, aux) CMETA_INLINE bool name##_##pub(const name *self, kt key) { return CMETA_CONTAINER_API(prefix, op)(&self->raw, &key); }
#define CMETA_C2_INLINE_REMOVE_STATUS_BOOL(pub, op, extra, name, kt, vt, key_desc, value_desc, raw_type, prefix, ok, aux) CMETA_INLINE int name##_##pub(name *self, kt key, vt *out_value) { return self ? CMETA_CONTAINER_API(prefix, op)(&self->raw, &key, out_value) : CMETA_INVALID_ARGUMENT; }
#define CMETA_C2_INLINE_REMOVE_BOOL(pub, op, extra, name, kt, vt, key_desc, value_desc, raw_type, prefix, ok, aux) CMETA_INLINE bool name##_##pub(name *self, kt key, vt *out_value) { return CMETA_CONTAINER_API(prefix, op)(&self->raw, &key, out_value); }
#define CMETA_C2_INLINE_SIZE(pub, op, extra, name, kt, vt, key_desc, value_desc, raw_type, prefix, ok, aux) CMETA_INLINE size_t name##_##pub(const name *self) { return CMETA_CONTAINER_API(prefix, op)(&self->raw); }
#define CMETA_C2_INLINE_CAPACITY(pub, op, extra, name, kt, vt, key_desc, value_desc, raw_type, prefix, ok, aux) CMETA_INLINE size_t name##_##pub(const name *self) { return CMETA_CONTAINER_API(prefix, op)(&self->raw); }
#define CMETA_C2_INLINE_EMPTY(pub, op, extra, name, kt, vt, key_desc, value_desc, raw_type, prefix, ok, aux) CMETA_INLINE bool name##_##pub(const name *self) { return CMETA_CONTAINER_API(prefix, op)(&self->raw); }
#define CMETA_C2_INLINE_KEY_AT(pub, op, extra, name, kt, vt, key_desc, value_desc, raw_type, prefix, ok, aux) CMETA_INLINE kt *name##_##pub(name *self, size_t index) { return (kt *)CMETA_CONTAINER_API(prefix, op)(&self->raw, index); }
#define CMETA_C2_INLINE_KEY_AT_CONST(pub, op, extra, name, kt, vt, key_desc, value_desc, raw_type, prefix, ok, aux) CMETA_INLINE const kt *name##_##pub(const name *self, size_t index) { return (const kt *)CMETA_CONTAINER_API(prefix, op)(&self->raw, index); }
#define CMETA_C2_INLINE_VALUE_AT(pub, op, extra, name, kt, vt, key_desc, value_desc, raw_type, prefix, ok, aux) CMETA_INLINE vt *name##_##pub(name *self, size_t index) { return (vt *)CMETA_CONTAINER_API(prefix, op)(&self->raw, index); }
#define CMETA_C2_INLINE_VALUE_AT_CONST(pub, op, extra, name, kt, vt, key_desc, value_desc, raw_type, prefix, ok, aux) CMETA_INLINE const vt *name##_##pub(const name *self, size_t index) { return (const vt *)CMETA_CONTAINER_API(prefix, op)(&self->raw, index); }
#define CMETA_C2_INLINE_FIND_SLOT(pub, op, extra, name, kt, vt, key_desc, value_desc, raw_type, prefix, ok, aux) CMETA_INLINE size_t name##_##pub(name *self, kt key, bool *found) { return CMETA_CONTAINER_API(prefix, op)(&self->raw, &key, found); }
#define CMETA_C2_INLINE_RAW_PTR_KEY(pub, op, extra, name, kt, vt, key_desc, value_desc, raw_type, prefix, ok, aux) CMETA_INLINE extra *name##_##pub(name *self, kt key) { return (extra *)CMETA_CONTAINER_API(prefix, op)(&self->raw, &key); }
#define CMETA_C2_INLINE_RAW_CONST_PTR_KEY(pub, op, extra, name, kt, vt, key_desc, value_desc, raw_type, prefix, ok, aux) CMETA_INLINE const extra *name##_##pub(const name *self, kt key) { return (const extra *)CMETA_CONTAINER_API(prefix, op)(&self->raw, &key); }
#define CMETA_C2_INLINE_COUNT_KEY(pub, op, extra, name, kt, vt, key_desc, value_desc, raw_type, prefix, ok, aux) CMETA_INLINE size_t name##_##pub(const name *self, kt key) { return CMETA_CONTAINER_API(prefix, op)(&self->raw, &key); }
#define CMETA_C2_INLINE_ERASE_KEY(pub, op, extra, name, kt, vt, key_desc, value_desc, raw_type, prefix, ok, aux) CMETA_INLINE size_t name##_##pub(name *self, kt key) { return CMETA_CONTAINER_API(prefix, op)(&self->raw, &key); }


/* -------------------------------------------------------------------------
 * Sparse one-type Range (set/hash-set style)
 * ------------------------------------------------------------------------- */

#define CMETA_CONTAINER1_SLOT_RANGE_DEFINE_WITH_TYPE_EXT(name, type, type_desc, prefix, flags, version_accessor, collector_factory, ext_expr) \
    CMETA_CONTAINER1_SLOT_RANGE_DEFINE_WITH_TYPE_EXT_VALUE(name, type, type_desc, prefix, flags, version_accessor, collector_factory, ext_expr, NULL, NULL)
#define CMETA_CONTAINER1_SLOT_RANGE_DEFINE_WITH_TYPE_EXT_VALUE(name, type, type_desc, prefix, flags, version_accessor, collector_factory, ext_expr, value_traits, value_identity) \
    CMETA_LOCAL const cmeta_type_desc name##_element_cmeta_type = { \
        CMETA_CONTAINER_STR(type), sizeof(type), _Alignof(type), CMETA_T_OBJECT, NULL, NULL, NULL \
    }; \
    CMETA_LOCAL const cmeta_type_desc name##_cmeta_type = { \
        CMETA_CONTAINER_STR(name), sizeof(name), _Alignof(name), CMETA_T_OBJECT, NULL, (value_traits), (value_identity) \
    }; \
    CMETA_INLINE size_t name##_cmeta_range_size(const void *object) { \
        const name *self = (const name *)object; \
        return self ? CMETA_CONTAINER_API(prefix, size)(&self->raw) : 0U; \
    } \
    CMETA_INLINE cmeta_gen_status name##_cmeta_range_next(const void *object, cmeta_range_cursor *cursor, void *out_value) { \
        const name *self = (const name *)object; \
        cmeta_range_cursor start_cursor; \
        size_t limit; \
        if (!self || !cursor || !out_value) return CMETA_GEN_ERROR; \
        start_cursor = *cursor; \
        limit = CMETA_CONTAINER_API(prefix, capacity)(&self->raw); \
        while (cursor->index < limit) { \
            const type *value = (const type *)CMETA_CONTAINER_API(prefix, key_at)(&self->raw, cursor->index); \
            ++cursor->index; \
            if (value) { \
                if (!cmeta_container_range_construct( \
                        (type_desc), \
                        out_value, value)) { *cursor = start_cursor; return CMETA_GEN_ERROR; } \
                return CMETA_GEN_VALUE; \
            } \
        } \
        return CMETA_GEN_DONE; \
    } \
    CMETA_INLINE cmeta_range name##_range(const name *self) { \
        cmeta_range range = { \
            self, (type_desc), \
            cmeta_container_range_flags_for( \
                (type_desc), (flags)), \
            name##_cmeta_range_size, name##_cmeta_range_next, \
            cmeta_range_capture_version((version_accessor), self), (version_accessor) \
        }; \
        return range; \
    } \
    CMETA_INLINE cmeta_range name##_cmeta_erased_range(const void *object) { \
        return name##_range((const name *)object); \
    } \
    CMETA_LOCAL cmeta_container_desc name##_cmeta_container_desc = { \
        CMETA_CONTAINER_STR(name), &name##_cmeta_type, \
        (type_desc), NULL, NULL, \
        name##_cmeta_erased_range, NULL, NULL, NULL, (collector_factory), (ext_expr) \
    };
#define CMETA_CONTAINER1_SLOT_RANGE_DEFINE_WITH_TYPE(name, type, type_desc, prefix, flags, version_accessor, collector_factory) \
    CMETA_CONTAINER1_SLOT_RANGE_DEFINE_WITH_TYPE_EXT(name, type, type_desc, prefix, flags, version_accessor, collector_factory, NULL)


#define CMETA_CONTAINER1_SLOT_RANGE_DEFINE(name, type, prefix, flags, version_accessor, collector_factory) \
    CMETA_CONTAINER1_SLOT_RANGE_DEFINE_WITH_TYPE(name, type, CMETA_TYPEOF_OR(type, &name##_element_cmeta_type), prefix, flags, version_accessor, collector_factory)
/* -------------------------------------------------------------------------
 * Two-type associative Range views
 * ------------------------------------------------------------------------- */
#define CMETA_CONTAINER_STR_I(x) #x
#define CMETA_CONTAINER_STR(x) CMETA_CONTAINER_STR_I(x)


/* Generated entry descriptors have translation-unit static lifetime. Their
 * callbacks carry no mutable context and borrow the registered key/value
 * descriptors (or the generated static fallback descriptors) on each call;
 * admitting an entry therefore fails when either field lacks the requested
 * semantic lifetime traits. */
#define CMETA_CONTAINER2_ENTRY_TRAITS_DEFINE_WITH_TYPES(name, key_type, value_type, key_desc_expr, value_desc_expr, semantic_flags, equal_member, hash_member, compare_member) \
    CMETA_INLINE bool name##_entry_cmeta_copy(void *destination_, const void *source_) { \
        name##_entry *destination = (name##_entry *)destination_; const name##_entry *source = (const name##_entry *)source_; \
        const cmeta_type_desc *key_desc = (key_desc_expr); const cmeta_type_desc *value_desc = (value_desc_expr); \
        if (destination == NULL || source == NULL || cmeta_type_require_traits(key_desc, CMETA_TRAIT_COPY | CMETA_TRAIT_DESTROY) != CMETA_OK || cmeta_type_require_traits(value_desc, CMETA_TRAIT_COPY | CMETA_TRAIT_DESTROY) != CMETA_OK) return false; \
        if (!key_desc->traits->copy_construct(&destination->key, &source->key)) return false; \
        if (!value_desc->traits->copy_construct(&destination->value, &source->value)) { key_desc->traits->destroy(&destination->key); return false; } \
        return true; \
    } \
    CMETA_INLINE void name##_entry_cmeta_move(void *destination_, void *source_) { \
        name##_entry *destination = (name##_entry *)destination_; name##_entry *source = (name##_entry *)source_; \
        const cmeta_type_desc *key_desc = (key_desc_expr); const cmeta_type_desc *value_desc = (value_desc_expr); \
        if (destination == NULL || source == NULL || cmeta_type_require_traits(key_desc, CMETA_TRAIT_MOVE) != CMETA_OK || cmeta_type_require_traits(value_desc, CMETA_TRAIT_MOVE) != CMETA_OK) return; \
        key_desc->traits->move_construct(&destination->key, &source->key); value_desc->traits->move_construct(&destination->value, &source->value); \
    } \
    CMETA_INLINE void name##_entry_cmeta_destroy(void *value_) { \
        name##_entry *value = (name##_entry *)value_; const cmeta_type_desc *key_desc = (key_desc_expr); const cmeta_type_desc *value_desc = (value_desc_expr); \
        if (value == NULL) return; \
        if (cmeta_type_require_traits(value_desc, CMETA_TRAIT_DESTROY) == CMETA_OK) value_desc->traits->destroy(&value->value); \
        if (cmeta_type_require_traits(key_desc, CMETA_TRAIT_DESTROY) == CMETA_OK) key_desc->traits->destroy(&value->key); \
    } \
    CMETA_INLINE bool name##_entry_cmeta_equal(const void *left_, const void *right_) { \
        const name##_entry *left = (const name##_entry *)left_; const name##_entry *right = (const name##_entry *)right_; const cmeta_type_desc *key_desc = (key_desc_expr); \
        return left != NULL && right != NULL && cmeta_type_require_traits(key_desc, CMETA_TRAIT_EQUAL) == CMETA_OK && key_desc->traits->equal(&left->key, &right->key); \
    } \
    CMETA_INLINE uint64_t name##_entry_cmeta_hash(const void *value_) { \
        const name##_entry *value = (const name##_entry *)value_; const cmeta_type_desc *key_desc = (key_desc_expr); \
        return value != NULL && cmeta_type_require_traits(key_desc, CMETA_TRAIT_HASH) == CMETA_OK ? key_desc->traits->hash(&value->key) : UINT64_C(0); \
    } \
    CMETA_INLINE int name##_entry_cmeta_compare(const void *left_, const void *right_) { \
        const name##_entry *left = (const name##_entry *)left_; const name##_entry *right = (const name##_entry *)right_; const cmeta_type_desc *key_desc = (key_desc_expr); \
        return left != NULL && right != NULL && cmeta_type_require_traits(key_desc, CMETA_TRAIT_COMPARE) == CMETA_OK ? key_desc->traits->compare(&left->key, &right->key) : 0; \
    } \
    CMETA_LOCAL const cmeta_type_traits name##_entry_cmeta_traits = { \
        CMETA_TRAIT_COPY | CMETA_TRAIT_MOVE | CMETA_TRAIT_DESTROY | (semantic_flags), (equal_member), (hash_member), (compare_member), name##_entry_cmeta_copy, name##_entry_cmeta_move, name##_entry_cmeta_destroy \
    };

#define CMETA_CONTAINER2_ENTRY_TRAITS_DEFINE(name, key_type, value_type, semantic_flags, equal_member, hash_member, compare_member) \
    CMETA_CONTAINER2_ENTRY_TRAITS_DEFINE_WITH_TYPES(name, key_type, value_type, \
        CMETA_TYPEOF_OR(key_type, &name##_key_cmeta_type), \
        CMETA_TYPEOF_OR(value_type, &name##_value_cmeta_type), \
        semantic_flags, equal_member, hash_member, compare_member)

#define CMETA_CONTAINER2_RANGES_DEFINE_WITH_TYPES_EXT(name, key_type, value_type, key_desc, value_desc, prefix, key_at_op, value_at_op, key_flags, value_flags, entry_flags, version_accessor, collector_factory, ext_expr) \
    CMETA_CONTAINER2_RANGES_DEFINE_WITH_TYPES_EXT_VALUE(name, key_type, value_type, key_desc, value_desc, prefix, key_at_op, value_at_op, key_flags, value_flags, entry_flags, version_accessor, collector_factory, ext_expr, NULL, NULL)
#define CMETA_CONTAINER2_RANGES_DEFINE_WITH_TYPES_EXT_VALUE(name, key_type, value_type, key_desc, value_desc, prefix, key_at_op, value_at_op, key_flags, value_flags, entry_flags, version_accessor, collector_factory, ext_expr, value_traits, value_identity) \
    CMETA_LOCAL const cmeta_type_desc name##_cmeta_type = { \
        CMETA_CONTAINER_STR(name), sizeof(name), _Alignof(name), CMETA_T_OBJECT, NULL, (value_traits), (value_identity) \
    }; \
    CMETA_LOCAL const cmeta_type_desc name##_key_cmeta_type = { \
        CMETA_CONTAINER_STR(key_type), sizeof(key_type), _Alignof(key_type), CMETA_T_OBJECT, NULL, NULL, NULL \
    }; \
    CMETA_LOCAL const cmeta_type_desc name##_value_cmeta_type = { \
        CMETA_CONTAINER_STR(value_type), sizeof(value_type), _Alignof(value_type), CMETA_T_OBJECT, NULL, NULL, NULL \
    }; \
    CMETA_CONTAINER2_ENTRY_TRAITS_DEFINE_WITH_TYPES(name, key_type, value_type, key_desc, value_desc, CMETA_TRAIT_EQUAL | CMETA_TRAIT_HASH, name##_entry_cmeta_equal, name##_entry_cmeta_hash, NULL) \
    CMETA_LOCAL cmeta_type_desc name##_entry_cmeta_type = { \
        CMETA_CONTAINER_STR(name) "_entry", sizeof(name##_entry), _Alignof(name##_entry), CMETA_T_OBJECT, NULL, &name##_entry_cmeta_traits, NULL \
    }; \
    CMETA_INLINE size_t name##_cmeta_assoc_range_size(const void *object) { \
        const name *self = (const name *)object; \
        return self ? CMETA_CONTAINER_API(prefix, size)(&self->raw) : 0U; \
    } \
    CMETA_INLINE cmeta_gen_status name##_cmeta_keys_next(const void *object, cmeta_range_cursor *cursor, void *out_value) { \
        const name *self = (const name *)object; \
        cmeta_range_cursor start_cursor; \
        size_t limit; \
        if (!self || !cursor || !out_value) return CMETA_GEN_ERROR; \
        start_cursor = *cursor; \
        limit = CMETA_CONTAINER_API(prefix, capacity)(&self->raw); \
        while (cursor->index < limit) { \
            const key_type *key = (const key_type *)CMETA_CONTAINER_API(prefix, key_at_op)(&self->raw, cursor->index); \
            ++cursor->index; \
            if (key) { \
                if (!cmeta_container_range_construct( \
                        (key_desc), \
                        out_value, key)) { *cursor = start_cursor; return CMETA_GEN_ERROR; } \
                return CMETA_GEN_VALUE; \
            } \
        } \
        return CMETA_GEN_DONE; \
    } \
    CMETA_INLINE cmeta_gen_status name##_cmeta_values_next(const void *object, cmeta_range_cursor *cursor, void *out_value) { \
        const name *self = (const name *)object; \
        cmeta_range_cursor start_cursor; \
        size_t limit; \
        if (!self || !cursor || !out_value) return CMETA_GEN_ERROR; \
        start_cursor = *cursor; \
        limit = CMETA_CONTAINER_API(prefix, capacity)(&self->raw); \
        while (cursor->index < limit) { \
            const value_type *value = (const value_type *)CMETA_CONTAINER_API(prefix, value_at_op)(&self->raw, cursor->index); \
            ++cursor->index; \
            if (value) { \
                if (!cmeta_container_range_construct( \
                        (value_desc), \
                        out_value, value)) { *cursor = start_cursor; return CMETA_GEN_ERROR; } \
                return CMETA_GEN_VALUE; \
            } \
        } \
        return CMETA_GEN_DONE; \
    } \
    CMETA_INLINE cmeta_gen_status name##_cmeta_entries_next(const void *object, cmeta_range_cursor *cursor, void *out_value) { \
        const name *self = (const name *)object; \
        cmeta_range_cursor start_cursor; \
        size_t limit; \
        if (!self || !cursor || !out_value) return CMETA_GEN_ERROR; \
        start_cursor = *cursor; \
        limit = CMETA_CONTAINER_API(prefix, capacity)(&self->raw); \
        while (cursor->index < limit) { \
            size_t slot = cursor->index; \
            const key_type *key = (const key_type *)CMETA_CONTAINER_API(prefix, key_at_op)(&self->raw, slot); \
            const value_type *value = (const value_type *)CMETA_CONTAINER_API(prefix, value_at_op)(&self->raw, slot); \
            ++cursor->index; \
            if (key && value) { \
                name##_entry entry; \
                memcpy(&entry.key, key, sizeof(key_type)); \
                memcpy(&entry.value, value, sizeof(value_type)); \
                if (!cmeta_container_range_construct( \
                        &name##_entry_cmeta_type, out_value, &entry)) { \
                    *cursor = start_cursor; return CMETA_GEN_ERROR; \
                } \
                return CMETA_GEN_VALUE; \
            } \
        } \
        return CMETA_GEN_DONE; \
    } \
    CMETA_INLINE cmeta_range name##_keys_range(const name *self) { \
        cmeta_range range = { self, (key_desc), \
            cmeta_container_range_flags_for( \
                (key_desc), (key_flags)), \
            name##_cmeta_assoc_range_size, name##_cmeta_keys_next, \
            cmeta_range_capture_version((version_accessor), self), (version_accessor) }; \
        return range; \
    } \
    CMETA_INLINE cmeta_range name##_values_range(const name *self) { \
        cmeta_range range = { self, (value_desc), \
            cmeta_container_range_flags_for( \
                (value_desc), (value_flags)), \
            name##_cmeta_assoc_range_size, name##_cmeta_values_next, \
            cmeta_range_capture_version((version_accessor), self), (version_accessor) }; \
        return range; \
    } \
    CMETA_INLINE cmeta_range name##_entries_range(const name *self) { \
        cmeta_range range = { self, &name##_entry_cmeta_type, \
            cmeta_container_range_flags_for(&name##_entry_cmeta_type, (entry_flags)), \
            name##_cmeta_assoc_range_size, name##_cmeta_entries_next, \
            cmeta_range_capture_version((version_accessor), self), (version_accessor) }; \
        return range; \
    } \
    CMETA_INLINE cmeta_range name##_cmeta_erased_keys_range(const void *object) { \
        return name##_keys_range((const name *)object); \
    } \
    CMETA_INLINE cmeta_range name##_cmeta_erased_values_range(const void *object) { \
        return name##_values_range((const name *)object); \
    } \
    CMETA_INLINE cmeta_range name##_cmeta_erased_entries_range(const void *object) { \
        return name##_entries_range((const name *)object); \
    } \
    CMETA_LOCAL cmeta_container_desc name##_cmeta_container_desc = { \
        CMETA_CONTAINER_STR(name), &name##_cmeta_type, NULL, \
        (key_desc), \
        (value_desc), \
        name##_cmeta_erased_entries_range, name##_cmeta_erased_keys_range, name##_cmeta_erased_values_range, \
        name##_cmeta_erased_entries_range, (collector_factory), (ext_expr) \
    };
#define CMETA_CONTAINER2_RANGES_DEFINE_WITH_TYPES(name, key_type, value_type, key_desc, value_desc, prefix, key_at_op, value_at_op, key_flags, value_flags, entry_flags, version_accessor, collector_factory) \
    CMETA_CONTAINER2_RANGES_DEFINE_WITH_TYPES_EXT(name, key_type, value_type, key_desc, value_desc, prefix, key_at_op, value_at_op, key_flags, value_flags, entry_flags, version_accessor, collector_factory, NULL)


#define CMETA_CONTAINER2_RANGES_DEFINE(name, key_type, value_type, prefix, key_at_op, value_at_op, key_flags, value_flags, entry_flags, version_accessor, collector_factory) \
    CMETA_CONTAINER2_RANGES_DEFINE_WITH_TYPES(name, key_type, value_type, \
        CMETA_TYPEOF_OR(key_type, &name##_key_cmeta_type), \
        CMETA_TYPEOF_OR(value_type, &name##_value_cmeta_type), \
        prefix, key_at_op, value_at_op, key_flags, value_flags, entry_flags, version_accessor, collector_factory)

/* Ordered trees expose stable derived entry links. Their cursor follows one
 * link per next() call, avoiding repeated rank scans while retaining the
 * public allocation-free cmeta_range contract. */
#define CMETA_CONTAINER2_LINK_RANGES_DEFINE_WITH_TYPES_EXT(name, key_type, value_type, key_desc, value_desc, prefix, key_flags, value_flags, entry_flags, version_accessor, collector_factory, ext_expr) \
    CMETA_CONTAINER2_LINK_RANGES_DEFINE_WITH_TYPES_EXT_VALUE(name, key_type, value_type, key_desc, value_desc, prefix, key_flags, value_flags, entry_flags, version_accessor, collector_factory, ext_expr, NULL, NULL)
#define CMETA_CONTAINER2_LINK_RANGES_DEFINE_WITH_TYPES_EXT_VALUE(name, key_type, value_type, key_desc, value_desc, prefix, key_flags, value_flags, entry_flags, version_accessor, collector_factory, ext_expr, value_traits, value_identity) \
    CMETA_LOCAL const cmeta_type_desc name##_cmeta_type = { \
        CMETA_CONTAINER_STR(name), sizeof(name), _Alignof(name), CMETA_T_OBJECT, NULL, (value_traits), (value_identity) \
    }; \
    CMETA_LOCAL const cmeta_type_desc name##_key_cmeta_type = { \
        CMETA_CONTAINER_STR(key_type), sizeof(key_type), _Alignof(key_type), CMETA_T_OBJECT, NULL, NULL, NULL \
    }; \
    CMETA_LOCAL const cmeta_type_desc name##_value_cmeta_type = { \
        CMETA_CONTAINER_STR(value_type), sizeof(value_type), _Alignof(value_type), CMETA_T_OBJECT, NULL, NULL, NULL \
    }; \
    CMETA_CONTAINER2_ENTRY_TRAITS_DEFINE_WITH_TYPES(name, key_type, value_type, key_desc, value_desc, CMETA_TRAIT_COMPARE, NULL, NULL, name##_entry_cmeta_compare) \
    CMETA_LOCAL cmeta_type_desc name##_entry_cmeta_type = { \
        CMETA_CONTAINER_STR(name) "_entry", sizeof(name##_entry), _Alignof(name##_entry), CMETA_T_OBJECT, NULL, &name##_entry_cmeta_traits, NULL \
    }; \
    CMETA_INLINE size_t name##_cmeta_assoc_range_size(const void *object) { \
        const name *self = (const name *)object; \
        return self ? CMETA_CONTAINER_API(prefix, size)(&self->raw) : 0U; \
    } \
    CMETA_INLINE cmeta_gen_status name##_cmeta_link_next(const void *object, cmeta_range_cursor *cursor, void *out_value, int view) { \
        const name *self = (const name *)object; \
        const void *key = NULL; const void *value = NULL; \
        cmeta_range_cursor next_cursor; \
        if (!self || !cursor || !out_value) return CMETA_GEN_ERROR; \
        next_cursor = *cursor; \
        if (!CMETA_CONTAINER_API(prefix, range_next)(&self->raw, &next_cursor, &key, &value)) return CMETA_GEN_DONE; \
        if (view == 0) { \
            if (!cmeta_container_range_construct( \
                    (key_desc), \
                    out_value, key)) return CMETA_GEN_ERROR; \
        } else if (view == 1) { \
            if (!cmeta_container_range_construct( \
                    (value_desc), \
                    out_value, value)) return CMETA_GEN_ERROR; \
        } else { \
            name##_entry entry; \
            memcpy(&entry.key, key, sizeof(key_type)); \
            memcpy(&entry.value, value, sizeof(value_type)); \
            if (!cmeta_container_range_construct( \
                    &name##_entry_cmeta_type, out_value, &entry)) \
                return CMETA_GEN_ERROR; \
        } \
        *cursor = next_cursor; \
        return cursor->state[0] == NULL ? CMETA_GEN_VALUE_AND_DONE : CMETA_GEN_VALUE; \
    } \
    CMETA_INLINE cmeta_gen_status name##_cmeta_keys_next(const void *object, cmeta_range_cursor *cursor, void *out_value) { return name##_cmeta_link_next(object, cursor, out_value, 0); } \
    CMETA_INLINE cmeta_gen_status name##_cmeta_values_next(const void *object, cmeta_range_cursor *cursor, void *out_value) { return name##_cmeta_link_next(object, cursor, out_value, 1); } \
    CMETA_INLINE cmeta_gen_status name##_cmeta_entries_next(const void *object, cmeta_range_cursor *cursor, void *out_value) { return name##_cmeta_link_next(object, cursor, out_value, 2); } \
    CMETA_INLINE cmeta_range name##_keys_range(const name *self) { \
        cmeta_range range = { self, (key_desc), cmeta_container_range_flags_for((key_desc), (key_flags)), name##_cmeta_assoc_range_size, name##_cmeta_keys_next, cmeta_range_capture_version((version_accessor), self), (version_accessor) }; return range; \
    } \
    CMETA_INLINE cmeta_range name##_values_range(const name *self) { \
        cmeta_range range = { self, (value_desc), cmeta_container_range_flags_for((value_desc), (value_flags)), name##_cmeta_assoc_range_size, name##_cmeta_values_next, cmeta_range_capture_version((version_accessor), self), (version_accessor) }; return range; \
    } \
    CMETA_INLINE cmeta_range name##_entries_range(const name *self) { \
        cmeta_range range = { self, &name##_entry_cmeta_type, cmeta_container_range_flags_for(&name##_entry_cmeta_type, (entry_flags)), name##_cmeta_assoc_range_size, name##_cmeta_entries_next, cmeta_range_capture_version((version_accessor), self), (version_accessor) }; return range; \
    } \
    CMETA_INLINE cmeta_range name##_cmeta_erased_keys_range(const void *object) { return name##_keys_range((const name *)object); } \
    CMETA_INLINE cmeta_range name##_cmeta_erased_values_range(const void *object) { return name##_values_range((const name *)object); } \
    CMETA_INLINE cmeta_range name##_cmeta_erased_entries_range(const void *object) { return name##_entries_range((const name *)object); } \
    CMETA_LOCAL cmeta_container_desc name##_cmeta_container_desc = { \
        CMETA_CONTAINER_STR(name), &name##_cmeta_type, NULL, (key_desc), (value_desc), name##_cmeta_erased_entries_range, name##_cmeta_erased_keys_range, name##_cmeta_erased_values_range, name##_cmeta_erased_entries_range, (collector_factory), (ext_expr) \
    };

#define CMETA_CONTAINER2_LINK_RANGES_DEFINE_WITH_TYPES(name, key_type, value_type, key_desc, value_desc, prefix, key_flags, value_flags, entry_flags, version_accessor, collector_factory) \
    CMETA_CONTAINER2_LINK_RANGES_DEFINE_WITH_TYPES_EXT(name, key_type, value_type, key_desc, value_desc, prefix, key_flags, value_flags, entry_flags, version_accessor, collector_factory, NULL)


#define CMETA_CONTAINER2_LINK_RANGES_DEFINE(name, key_type, value_type, prefix, key_flags, value_flags, entry_flags, version_accessor, collector_factory) \
    CMETA_CONTAINER2_LINK_RANGES_DEFINE_WITH_TYPES(name, key_type, value_type, \
        CMETA_TYPEOF_OR(key_type, &name##_key_cmeta_type), \
        CMETA_TYPEOF_OR(value_type, &name##_value_cmeta_type), \
        prefix, key_flags, value_flags, entry_flags, version_accessor, collector_factory)

/* Descriptor-only capability for typed containers that intentionally expose no Range view. */
#define CMETA_CONTAINER2_OPAQUE_DESCRIPTOR_DEFINE(name, key_type, value_type) \
    CMETA_LOCAL const cmeta_type_desc name##_cmeta_type = { \
        CMETA_CONTAINER_STR(name), sizeof(name), _Alignof(name), CMETA_T_OBJECT, NULL, NULL, NULL \
    }; \
    CMETA_LOCAL cmeta_container_desc name##_cmeta_container_desc = { \
        CMETA_CONTAINER_STR(name), &name##_cmeta_type, NULL, NULL, NULL, \
        NULL, NULL, NULL, NULL, NULL, NULL \
    };

#endif /* CMETA_CONTAINER_H */
