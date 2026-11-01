#ifndef CMETA_CORE_DATA_H
#define CMETA_CORE_DATA_H

#include <cmeta/core_fixed_width.h>
#include <tstr.h>
#include <cmeta_uuid.h>
#include <vstr.h>

#include <cmeta/data.h>

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef __cplusplus
#define CMETA_CORE_STATIC_ASSERT(condition_, message_) \
    static_assert((condition_), message_)
#else
#define CMETA_CORE_STATIC_ASSERT(condition_, message_) \
    _Static_assert((condition_), message_)
#endif

CMETA_CORE_STATIC_ASSERT(sizeof(cmeta_uuid_t) == SALTS_UUID_SIZE,
                          "cmeta_uuid_t must have exactly 16 bytes");

#undef CMETA_CORE_STATIC_ASSERT

/** Canonical process-wide UUID metadata exported by CMeta. */
extern const cmeta_type_desc cmeta_uuid_cmeta_type;
extern const cmeta_data_buffer_shape cmeta_uuid_cmeta_shape;
extern const cmeta_data_buffer_ops cmeta_uuid_cmeta_buffer_ops;
extern const cmeta_data_fixed_ops cmeta_uuid_cmeta_fixed_ops;
extern const cmeta_data_desc cmeta_uuid_cmeta_data;
/**
 * Admit a semantically equivalent UUID data provider.
 *
 * The stable data/type identities, native layout, ownership, operation ABI and
 * complete callback capabilities must match. Callback addresses are not
 * identities: an equivalent provider may originate in another TU or DSO.
 */
bool cmeta_uuid_cmeta_data_valid(
    const cmeta_data_desc *candidate);

/**
 * Canonical process-wide tstr metadata.
 *
 * tstr is unique-owned storage. Its semantic zero is NULL, assignment copies
 * exact bytes, read borrows the current byte span, move transfers ownership
 * without allocation, and restore_zero releases the owned allocation.
 */
extern const cmeta_type_desc cmeta_tstr_cmeta_type;
extern const cmeta_data_buffer_shape cmeta_tstr_cmeta_shape;
extern const cmeta_data_buffer_ops cmeta_tstr_cmeta_buffer_ops;
extern const cmeta_data_desc cmeta_tstr_cmeta_data;

/*
 * Import-safe semantic mirror for header-local static generic metadata.
 *
 * On Windows, addresses of __declspec(dllimport) data objects are not C static
 * initializer constants. These header-local descriptors preserve the same
 * stable type/data identities and lifecycle semantics while giving cmeta_type(...)
 * an address-constant reference. The process-wide exported objects above remain
 * the ABI and are semantically equal to these mirrors; descriptor address is
 * never semantic identity.
 */
static inline bool cmeta_tstr_header_cmeta_is_zero(const void *object) {
    return object != NULL && *(const tstr *)object == NULL;
}

static inline cmeta_status cmeta_tstr_header_cmeta_init_zero(void *object) {
    if (object == NULL)
        return CMETA_INVALID_ARGUMENT;
    *(tstr *)object = NULL;
    return CMETA_OK;
}

static inline cmeta_status cmeta_tstr_header_cmeta_read(
    const void *object, const unsigned char **out_data, size_t *out_size) {
    tstr value;
    if (object == NULL || out_data == NULL || out_size == NULL)
        return CMETA_INVALID_ARGUMENT;
    value = *(const tstr *)object;
    *out_data = (const unsigned char *)value;
    *out_size = tstr_len(value);
    return CMETA_OK;
}

static inline cmeta_status cmeta_tstr_header_cmeta_assign(
    void *object, const unsigned char *data, size_t size, size_t max_bytes) {
    tstr value;
    if (object == NULL || (size != 0u && data == NULL))
        return CMETA_INVALID_ARGUMENT;
    if (size > max_bytes)
        return CMETA_CAPACITY_EXCEEDED;
    if (*(tstr *)object != NULL)
        return CMETA_INVALID_ARGUMENT;
    if (size == 0u)
        return CMETA_OK;
    value = tstr_new_len(data, size);
    if (value == NULL)
        return CMETA_OUT_OF_MEMORY;
    *(tstr *)object = value;
    return CMETA_OK;
}

static inline void cmeta_tstr_header_cmeta_restore_zero(void *object) {
    if (object != NULL)
        tstr_freep((tstr *)object);
}

static inline void cmeta_tstr_header_cmeta_move(
    void *destination, void *source) {
    tstr *to = (tstr *)destination;
    tstr *from = (tstr *)source;
    if (to == NULL || from == NULL || to == from)
        return;
    *to = *from;
    *from = NULL;
}

static inline bool cmeta_tstr_header_cmeta_copy_construct(
    void *destination, const void *source) {
    const tstr value = source != NULL ? *(const tstr *)source : NULL;
    tstr copy;
    if (destination == NULL || source == NULL)
        return false;
    copy = tstr_clone(value);
    if (value != NULL && copy == NULL)
        return false;
    *(tstr *)destination = copy;
    return true;
}

static inline void cmeta_tstr_header_cmeta_move_construct(
    void *destination, void *source) {
    if (destination == NULL || source == NULL || destination == source)
        return;
    *(tstr *)destination = tstr_move((tstr *)source);
}

static inline void cmeta_tstr_header_cmeta_destroy(void *object) {
    if (object != NULL)
        tstr_freep((tstr *)object);
}

static inline int cmeta_tstr_header_cmeta_compare(
    const void *left, const void *right) {
    return tstr_cmp(*(const tstr *)left, *(const tstr *)right);
}

static const cmeta_type_traits cmeta_tstr_header_cmeta_traits = {
    CMETA_TRAIT_COPY | CMETA_TRAIT_MOVE | CMETA_TRAIT_DESTROY |
        CMETA_TRAIT_COMPARE,
    NULL, NULL, cmeta_tstr_header_cmeta_compare,
    cmeta_tstr_header_cmeta_copy_construct,
    cmeta_tstr_header_cmeta_move_construct,
    cmeta_tstr_header_cmeta_destroy
};

static const cmeta_type_identity cmeta_tstr_header_cmeta_identity =
    CMETA_TYPE_ID_ATOM_INIT("salts.tstr");

static const cmeta_type_desc cmeta_tstr_header_cmeta_type = {
    "tstr", sizeof(tstr), CMETA_ALIGNOF(tstr), CMETA_T_OBJECT,
    NULL, &cmeta_tstr_header_cmeta_traits,
    &cmeta_tstr_header_cmeta_identity
};

static const cmeta_data_buffer_shape cmeta_tstr_header_cmeta_shape = {
    CMETA_DATA_BUFFER_OWNED
};

static const cmeta_data_buffer_ops cmeta_tstr_header_cmeta_buffer_ops = {
    sizeof(cmeta_data_buffer_ops), CMETA_DATA_BUFFER_OPS_ABI_VERSION,
    &cmeta_tstr_header_cmeta_type, CMETA_DATA_BUFFER_OWNED,
    cmeta_tstr_header_cmeta_is_zero, cmeta_tstr_header_cmeta_assign,
    cmeta_tstr_header_cmeta_restore_zero, cmeta_tstr_header_cmeta_read,
    cmeta_tstr_header_cmeta_init_zero, cmeta_tstr_header_cmeta_move
};

static const cmeta_data_desc cmeta_tstr_header_cmeta_data = {
    sizeof(cmeta_data_desc), CMETA_DATA_DESC_ABI_VERSION,
    "salts.tstr.data", "tstr", CMETA_DATA_STRING,
    &cmeta_tstr_header_cmeta_type, &cmeta_tstr_header_cmeta_shape,
    &cmeta_tstr_header_cmeta_buffer_ops,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL
};

#define CMETA_TSTR_TYPE_REF (&cmeta_tstr_header_cmeta_type)
#define CMETA_TSTR_DATA_REF (&cmeta_tstr_header_cmeta_data)

static const cmeta_type_identity cmeta_vstr_cmeta_identity =
    CMETA_TYPE_ID_ATOM_INIT("salts.vstr");

/** Header-local borrowed vstr metadata is compared semantically, never by address. */
static const cmeta_type_desc cmeta_vstr_cmeta_type = {
    "vstr", sizeof(vstr), CMETA_ALIGNOF(vstr), CMETA_T_OBJECT,
    NULL, NULL, &cmeta_vstr_cmeta_identity
};

static inline bool cmeta_vstr_cmeta_is_zero(const void *object) {
    const vstr *value = (const vstr *)object;
    return value != NULL && value->data == NULL && value->len == 0u;
}

static inline cmeta_status cmeta_vstr_cmeta_init_zero(void *object) {
    vstr *value = (vstr *)object;
    if (value == NULL)
        return CMETA_INVALID_ARGUMENT;
    value->data = NULL;
    value->len = 0u;
    return CMETA_OK;
}

static inline cmeta_status cmeta_vstr_cmeta_read(
    const void *object, const unsigned char **out_data, size_t *out_size) {
    const vstr *value = (const vstr *)object;
    if (value == NULL || out_data == NULL || out_size == NULL)
        return CMETA_INVALID_ARGUMENT;
    if (value->len != 0u && value->data == NULL)
        return CMETA_CALLBACK_ERROR;
    *out_data = (const unsigned char *)value->data;
    *out_size = value->len;
    return CMETA_OK;
}

static inline cmeta_status cmeta_vstr_cmeta_assign(
    void *object, const unsigned char *data, size_t size, size_t max_bytes) {
    vstr *value = (vstr *)object;

    if (value == NULL || (size != 0u && data == NULL))
        return CMETA_INVALID_ARGUMENT;
    if (size > max_bytes)
        return CMETA_CAPACITY_EXCEEDED;
    if (value->data != NULL || value->len != 0u)
        return CMETA_INVALID_ARGUMENT;

    value->data = size == 0u ? NULL : (const char *)data;
    value->len = size;
    return CMETA_OK;
}

static inline void cmeta_vstr_cmeta_restore_zero(void *object) {
    vstr *value = (vstr *)object;
    if (value != NULL) {
        value->data = NULL;
        value->len = 0u;
    }
}

static inline void cmeta_vstr_cmeta_move(void *destination, void *source) {
    vstr *to = (vstr *)destination;
    vstr *from = (vstr *)source;
    if (to == NULL || from == NULL)
        return;
    *to = *from;
    from->data = NULL;
    from->len = 0u;
}

/** Borrowed vstr storage adapter with the complete v2 lifecycle. */
static const cmeta_data_buffer_ops cmeta_vstr_cmeta_buffer_ops = {
    sizeof(cmeta_data_buffer_ops), CMETA_DATA_BUFFER_OPS_ABI_VERSION,
    &cmeta_vstr_cmeta_type, CMETA_DATA_BUFFER_BORROWED,
    cmeta_vstr_cmeta_is_zero, cmeta_vstr_cmeta_assign,
    cmeta_vstr_cmeta_restore_zero, cmeta_vstr_cmeta_read,
    cmeta_vstr_cmeta_init_zero, cmeta_vstr_cmeta_move
};

#ifdef __cplusplus
}
#endif

#endif /* CMETA_CORE_DATA_H */
