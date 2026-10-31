#ifndef CMETA_DATA_H
#define CMETA_DATA_H

#include <cmeta/cmeta.h>
#include <cmeta/collector.h>
#include <cmeta/struct.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum cmeta_data_kind {
    CMETA_DATA_BOOL,
    CMETA_DATA_SINT,
    CMETA_DATA_UINT,
    CMETA_DATA_FLOAT,
    CMETA_DATA_STRING,
    CMETA_DATA_BYTES,
    CMETA_DATA_ENUM,
    CMETA_DATA_STRUCT,
    CMETA_DATA_VARIANT,
    CMETA_DATA_SEQUENCE,
    CMETA_DATA_SET,
    CMETA_DATA_MAP,
    CMETA_DATA_CUSTOM
} cmeta_data_kind;

enum {
    CMETA_DATA_DESC_ABI_VERSION = 1u,
    /* V2 is restricted to explicit native record reflection. V1 runtimes
     * reject it rather than silently deriving lifecycle authority. */
    CMETA_DATA_DESC_REFLECTION_ABI_VERSION = 2u
};

enum { CMETA_DATA_CONSTRUCT_OPS_ABI_VERSION = 1u };

typedef cmeta_status (*cmeta_data_construct_init_zero_fn)(void *object);
typedef void (*cmeta_data_construct_restore_zero_fn)(void *object);
typedef void (*cmeta_data_construct_move_fn)(void *destination, void *source);

typedef uint32_t cmeta_lifecycle_flags;
enum {
    CMETA_LIFECYCLE_INIT_NOFAIL = 1u << 0,
    CMETA_LIFECYCLE_TRIVIAL_ZERO = 1u << 1,
    CMETA_LIFECYCLE_TRIVIAL_CLEANUP = 1u << 2,
    CMETA_LIFECYCLE_MOVABLE = 1u << 3,
    CMETA_LIFECYCLE_FLAG_MASK = (1u << 4) - 1u
};

typedef struct cmeta_data_construct_ops {
    size_t struct_size;
    uint32_t abi_version;
    const cmeta_type_desc *storage_type;
    /** Initializes raw storage without inspecting an earlier value. A failed
     * initialization must leave storage acceptable to restore_zero. */
    cmeta_data_construct_init_zero_fn init_zero;
    /** Releases a live/partial value or semantic zero, then restores zero. */
    cmeta_data_construct_restore_zero_fn restore_zero;
    /** Transfers into a distinct initialized semantic-zero destination;
     * leaves the source live in semantic zero. Optional capability. */
    cmeta_data_construct_move_fn move;
    /** Optional size-versioned facts; zero means unspecified, never inferred
     * from callback identity. TRIVIAL_ZERO means native {0} is semantic zero;
     * TRIVIAL_CLEANUP means no release obligation (restore may reset storage). */
    cmeta_lifecycle_flags flags;
} cmeta_data_construct_ops;

static inline cmeta_lifecycle_flags cmeta_lifecycle_flags_of(
    const cmeta_data_construct_ops *ops) {
    return ops != NULL && ops->struct_size >=
        offsetof(cmeta_data_construct_ops, flags) + sizeof(ops->flags) ? ops->flags : 0u;
}
static inline bool cmeta_lifecycle_flags_valid(const cmeta_data_construct_ops *ops) {
    const cmeta_lifecycle_flags flags = cmeta_lifecycle_flags_of(ops);
    return (flags & ~CMETA_LIFECYCLE_FLAG_MASK) == 0u &&
        ((flags & CMETA_LIFECYCLE_MOVABLE) == 0u || ops->move != NULL) &&
        ((flags & CMETA_LIFECYCLE_TRIVIAL_ZERO) == 0u ||
            (flags & CMETA_LIFECYCLE_INIT_NOFAIL) != 0u);
}

typedef struct cmeta_data_buffer_ops cmeta_data_buffer_ops;
typedef struct cmeta_data_enum_ops cmeta_data_enum_ops;
typedef struct cmeta_data_enum_bits_ops cmeta_data_enum_bits_ops;
typedef struct cmeta_data_variant_ops cmeta_data_variant_ops;
typedef struct cmeta_data_fixed_ops cmeta_data_fixed_ops;
typedef struct cmeta_data_collection_ops cmeta_data_collection_ops;
typedef struct cmeta_data_map_ops cmeta_data_map_ops;

typedef struct cmeta_data_desc {
    size_t struct_size;
    uint32_t abi_version;
    const char *stable_id;
    const char *display_name;
    cmeta_data_kind kind;
    const cmeta_type_desc *storage_type;
    const void *shape;
    const cmeta_data_buffer_ops *buffer_ops;
    const cmeta_data_enum_ops *enum_ops;
    const cmeta_data_variant_ops *variant_ops;
    /** Optional exact native-value operations appended to the v1 prefix. */
    const cmeta_data_fixed_ops *fixed_ops;
    /** Canonical enum domain provider; mutually exclusive with shape/enum_ops. */
    const cmeta_data_enum_bits_ops *enum_bits_ops;
    /** Optional provider-neutral collection read adapter. */
    const cmeta_data_collection_ops *collection_ops;
    /** Optional provider-neutral map key/value adapter. */
    const cmeta_data_map_ops *map_ops;
    /** Optional transactional construction lifecycle. */
    const cmeta_data_construct_ops *construct_ops;
} cmeta_data_desc;

typedef struct cmeta_data_integer_shape {
    uint8_t bits;
} cmeta_data_integer_shape;

typedef struct cmeta_data_float_shape {
    uint8_t bits;
} cmeta_data_float_shape;

typedef enum cmeta_data_buffer_ownership {
    CMETA_DATA_BUFFER_OWNED,
    CMETA_DATA_BUFFER_BORROWED,
    CMETA_DATA_BUFFER_CUSTOM
} cmeta_data_buffer_ownership;

typedef struct cmeta_data_buffer_shape {
    cmeta_data_buffer_ownership ownership;
} cmeta_data_buffer_shape;

enum {
    CMETA_DATA_BUFFER_OPS_ABI_VERSION = 2u
};

typedef bool (*cmeta_data_buffer_is_zero_fn)(const void *object);
typedef cmeta_status (*cmeta_data_buffer_assign_fn)(
    void *object, const unsigned char *data, size_t size, size_t max_bytes);
typedef void (*cmeta_data_buffer_restore_zero_fn)(void *object);
/**
 * Return one provider-owned immutable byte span for the callback duration.
 *
 * Success initializes both outputs. A non-empty span has non-NULL data. The
 * provider does not transfer ownership or extend the span lifetime; callers
 * follow the enclosing object's documented mutation and destruction rules.
 */
typedef cmeta_status (*cmeta_data_buffer_read_fn)(
    const void *object, const unsigned char **out_data, size_t *out_size);
/** Initialize one raw storage slot to the provider-defined semantic zero. */
typedef cmeta_status (*cmeta_data_buffer_init_zero_fn)(void *object);
/**
 * Move one complete provider value into a semantic-zero destination.
 *
 * This operation is no-fail. It transfers provider-owned state without
 * allocation and leaves source in semantic zero. Both objects remain valid for
 * restore_zero after the call.
 */
typedef void (*cmeta_data_buffer_move_fn)(void *destination, void *source);

/**
 * Version-2 native buffer lifecycle provider.
 *
 * v2 is intentionally strict: init_zero and move are mandatory and providers
 * compiled against v1 are not admitted. read remains optional because some
 * exact fixed/native values use buffer assignment only and expose their read
 * representation through a separate canonical authority. There is no v1
 * fallback path.
 */
struct cmeta_data_buffer_ops {
    size_t struct_size;
    uint32_t abi_version;
    const cmeta_type_desc *storage_type;
    cmeta_data_buffer_ownership ownership;
    cmeta_data_buffer_is_zero_fn is_zero;
    cmeta_data_buffer_assign_fn assign;
    cmeta_data_buffer_restore_zero_fn restore_zero;
    cmeta_data_buffer_read_fn read;
    cmeta_data_buffer_init_zero_fn init_zero;
    cmeta_data_buffer_move_fn move;
};

enum {
    CMETA_DATA_FIXED_OPS_ABI_VERSION = 1u
};

/**
 * Exact native-value provider contract.
 *
 * extent is the complete native object extent and must exactly match the
 * associated storage type. copy receives one source object of that extent and
 * a semantic-zero destination. A failed copy may partially mutate the
 * destination; the checked facade restores it to semantic zero before
 * returning. restore_zero is no-fail, idempotent, and accepts partially
 * initialized objects.
 */
typedef bool (*cmeta_data_fixed_is_zero_fn)(const void *object);
typedef cmeta_status (*cmeta_data_fixed_copy_fn)(void *destination,
                                                 const void *source);
typedef void (*cmeta_data_fixed_restore_zero_fn)(void *object);

struct cmeta_data_fixed_ops {
    size_t struct_size;
    uint32_t abi_version;
    const cmeta_type_desc *storage_type;
    size_t extent;
    cmeta_data_fixed_is_zero_fn is_zero;
    cmeta_data_fixed_copy_fn copy;
    cmeta_data_fixed_restore_zero_fn restore_zero;
};

/**
 * Define header-local canonical metadata for one bounded inline byte value.
 *
 * storage_type must be a complete native type, including an array typedef when
 * the native value is an array. extent is explicit and compilation fails when
 * it differs from sizeof(storage_type). The generated semantic zero is the
 * all-zero byte representation. The exact-value and buffer-v2 providers share
 * that representation: assign requires exactly extent bytes, read borrows the
 * complete inline value until mutation or destruction, and move clears source.
 * Copy, move, construction, and restore neither allocate nor retain pointers.
 * Access is single-threaded unless the owner supplies synchronization; work is
 * O(extent) with O(1) additional storage. Repeated declarations compare by
 * the supplied stable type identity rather than by descriptor address.
 */
#ifdef __cplusplus
#define CMETA_DATA_BYTES_MUTABLE(object_) \
    reinterpret_cast<unsigned char *>(object_)
#define CMETA_DATA_BYTES_CONST(object_) \
    reinterpret_cast<const unsigned char *>(object_)
#else
#define CMETA_DATA_BYTES_MUTABLE(object_) ((unsigned char *)(object_))
#define CMETA_DATA_BYTES_CONST(object_) ((const unsigned char *)(object_))
#endif

#ifdef __cplusplus
#define CMETA_DATA_FIXED_DESC_INIT_(name_, stable_id_, display_name_)         \
    {sizeof(cmeta_data_desc), CMETA_DATA_DESC_ABI_VERSION,                   \
     stable_id_ ".data", display_name_, CMETA_DATA_BYTES,                   \
     &name_##_cmeta_type, &name_##_cmeta_shape,                             \
     &name_##_cmeta_buffer_ops, NULL, NULL,                                \
     &name_##_cmeta_fixed_ops, NULL, NULL, NULL,                            \
     &name_##_cmeta_construct_ops}
#else
#define CMETA_DATA_FIXED_DESC_INIT_(name_, stable_id_, display_name_)         \
    {.struct_size = sizeof(cmeta_data_desc),                                 \
     .abi_version = CMETA_DATA_DESC_ABI_VERSION,                             \
     .stable_id = stable_id_ ".data",                                       \
     .display_name = display_name_,                                          \
     .kind = CMETA_DATA_BYTES,                                               \
     .storage_type = &name_##_cmeta_type,                                    \
     .shape = &name_##_cmeta_shape,                                          \
     .buffer_ops = &name_##_cmeta_buffer_ops,                              \
     .enum_ops = NULL,                                                       \
     .variant_ops = NULL,                                                    \
     .fixed_ops = &name_##_cmeta_fixed_ops,                                  \
     .enum_bits_ops = NULL,                                                  \
     .collection_ops = NULL,                                                 \
     .map_ops = NULL,                                                        \
     .construct_ops = &name_##_cmeta_construct_ops}
#endif

#define CMETA_DEFINE_FIXED_BYTES(name_, storage_type_, extent_, stable_id_,  \
                                 display_name_)                              \
    typedef char name_##_cmeta_extent_must_match_storage[                    \
        ((extent_) > 0u && sizeof(storage_type_) == (extent_)) ? 1 : -1];    \
    static inline bool name_##_cmeta_is_zero(const void *object_) {          \
        const unsigned char *bytes_;                                         \
        size_t index_;                                                       \
        if (object_ == NULL)                                                 \
            return false;                                                    \
        bytes_ = CMETA_DATA_BYTES_CONST(object_);                            \
        for (index_ = 0u; index_ < (extent_); ++index_)                      \
            if (bytes_[index_] != 0u)                                        \
                return false;                                                \
        return true;                                                         \
    }                                                                        \
    static inline cmeta_status name_##_cmeta_copy(                           \
        void *destination_, const void *source_) {                           \
        unsigned char *destination_bytes_;                                   \
        const unsigned char *source_bytes_;                                  \
        size_t index_;                                                       \
        if (destination_ == NULL || source_ == NULL)                         \
            return CMETA_INVALID_ARGUMENT;                                  \
        destination_bytes_ = CMETA_DATA_BYTES_MUTABLE(destination_);         \
        source_bytes_ = CMETA_DATA_BYTES_CONST(source_);                     \
        for (index_ = 0u; index_ < (extent_); ++index_)                      \
            destination_bytes_[index_] = source_bytes_[index_];              \
        return CMETA_OK;                                                     \
    }                                                                        \
    static inline void name_##_cmeta_restore_zero(void *object_) {           \
        unsigned char *bytes_;                                               \
        size_t index_;                                                       \
        if (object_ == NULL)                                                 \
            return;                                                          \
        bytes_ = CMETA_DATA_BYTES_MUTABLE(object_);                          \
        for (index_ = 0u; index_ < (extent_); ++index_)                      \
            bytes_[index_] = 0u;                                             \
    }                                                                        \
    static inline cmeta_status name_##_cmeta_init_zero(void *object_) {     \
        if (object_ == NULL)                                                 \
            return CMETA_INVALID_ARGUMENT;                                  \
        name_##_cmeta_restore_zero(object_);                                \
        return CMETA_OK;                                                     \
    }                                                                        \
    static inline void name_##_cmeta_move(                                  \
        void *destination_, void *source_) {                                \
        (void)name_##_cmeta_copy(destination_, source_);                    \
        name_##_cmeta_restore_zero(source_);                                \
    }                                                                        \
    static inline bool name_##_cmeta_copy_construct(                        \
        void *destination_, const void *source_) {                          \
        return name_##_cmeta_copy(destination_, source_) == CMETA_OK;       \
    }                                                                        \
    static inline cmeta_status name_##_cmeta_assign(                        \
        void *object_, const unsigned char *data_,                          \
        size_t size_, size_t max_bytes_) {                                  \
        if (object_ == NULL || data_ == NULL)                               \
            return CMETA_INVALID_ARGUMENT;                                  \
        if (size_ != (extent_))                                              \
            return CMETA_TYPE_MISMATCH;                                      \
        if (size_ > max_bytes_)                                              \
            return CMETA_CAPACITY_EXCEEDED;                                  \
        return name_##_cmeta_copy(object_, data_);                          \
    }                                                                        \
    static inline cmeta_status name_##_cmeta_read(                          \
        const void *object_, const unsigned char **out_data_,               \
        size_t *out_size_) {                                                 \
        if (object_ == NULL || out_data_ == NULL || out_size_ == NULL)       \
            return CMETA_INVALID_ARGUMENT;                                  \
        *out_data_ = CMETA_DATA_BYTES_CONST(object_);                       \
        *out_size_ = (extent_);                                              \
        return CMETA_OK;                                                     \
    }                                                                        \
    static const cmeta_type_traits name_##_cmeta_traits = {                 \
        CMETA_TRAIT_COPY | CMETA_TRAIT_MOVE | CMETA_TRAIT_DESTROY,          \
        NULL, NULL, NULL, name_##_cmeta_copy_construct,                     \
        name_##_cmeta_move, name_##_cmeta_restore_zero};                    \
    static const cmeta_type_identity name_##_cmeta_identity =                \
        CMETA_TYPE_ID_ATOM_INIT(stable_id_);                                 \
    static const cmeta_type_desc name_##_cmeta_type = {                      \
        #storage_type_, sizeof(storage_type_), CMETA_ALIGNOF(storage_type_), \
        CMETA_T_OBJECT, NULL, &name_##_cmeta_traits,                        \
        &name_##_cmeta_identity};                                           \
    static const cmeta_data_buffer_shape name_##_cmeta_shape = {             \
        CMETA_DATA_BUFFER_OWNED};                                            \
    static const cmeta_data_fixed_ops name_##_cmeta_fixed_ops = {            \
        sizeof(cmeta_data_fixed_ops), CMETA_DATA_FIXED_OPS_ABI_VERSION,      \
        &name_##_cmeta_type, (extent_), name_##_cmeta_is_zero,               \
        name_##_cmeta_copy, name_##_cmeta_restore_zero};                     \
    static const cmeta_data_buffer_ops name_##_cmeta_buffer_ops = {         \
        sizeof(cmeta_data_buffer_ops), CMETA_DATA_BUFFER_OPS_ABI_VERSION,  \
        &name_##_cmeta_type, CMETA_DATA_BUFFER_OWNED,                       \
        name_##_cmeta_is_zero, name_##_cmeta_assign,                        \
        name_##_cmeta_restore_zero, name_##_cmeta_read,                      \
        name_##_cmeta_init_zero, name_##_cmeta_move};                       \
    static const cmeta_data_construct_ops name_##_cmeta_construct_ops = {   \
        sizeof(cmeta_data_construct_ops),                                  \
        CMETA_DATA_CONSTRUCT_OPS_ABI_VERSION, &name_##_cmeta_type,          \
        name_##_cmeta_init_zero, name_##_cmeta_restore_zero,                \
        name_##_cmeta_move, 0};                                             \
    static const cmeta_data_desc name_##_cmeta_data =                        \
        CMETA_DATA_FIXED_DESC_INIT_(name_, stable_id_, display_name_)

enum {
    CMETA_DATA_ENUM_OPS_ABI_VERSION = 1u
};

/**
 * Enum adapter callback contract.
 *
 * Every object points to one correctly aligned instance of storage_type.
 * is_zero and read do not mutate it. On successful read, out must receive a
 * value declared by the descriptor's enum metadata. assign is called only for
 * a semantic-zero object and a declared value; success must make read return
 * that exact value. assign may partially mutate on failure because the checked
 * facade always follows failure with restore_zero.
 *
 * restore_zero is a no-fail cleanup callback. It must release provider-owned
 * resources, be safe for both zero and partially/fully initialized objects,
 * be idempotent, and leave is_zero(object) true.
 */
typedef bool (*cmeta_data_enum_is_zero_fn)(const void *object);
typedef cmeta_status (*cmeta_data_enum_read_fn)(const void *object,
                                                int64_t *out);
typedef cmeta_status (*cmeta_data_enum_assign_fn)(void *object,
                                                  int64_t value);
typedef void (*cmeta_data_enum_restore_zero_fn)(void *object);

struct cmeta_data_enum_ops {
    size_t struct_size;
    uint32_t abi_version;
    const cmeta_type_desc *storage_type;
    cmeta_data_enum_is_zero_fn is_zero;
    cmeta_data_enum_read_fn read;
    cmeta_data_enum_assign_fn assign;
    cmeta_data_enum_restore_zero_fn restore_zero;
};

enum { CMETA_DATA_ENUM_BITS_OPS_ABI_VERSION = 1u };
typedef cmeta_status (*cmeta_data_enum_read_bits_fn)(const void *object,
                                                    uint64_t *out);
typedef cmeta_status (*cmeta_data_enum_assign_bits_fn)(void *object,
                                                      uint64_t bits);

/**
 * Canonical enum provider, never routed through the legacy int64_t adapter.
 * The descriptor sets shape and enum_ops to NULL and appends enum_bits_ops.
 * domain is the sole membership/width/flags authority, not storage_type.
 * Object alignment, ownership and lifetime follow storage_type. Metadata is
 * immutable and borrowed; callbacks retain no arguments. Callers synchronize
 * mutable objects; the facade provides no concurrent atomicity.
 *
 * is_zero/read never mutate the object. read initializes out on success.
 * assign receives a semantic-zero destination and validated canonical bits;
 * success must read back exactly those bits. Failure may partially mutate.
 * restore_zero is no-fail, idempotent, handles partial objects and releases
 * provider-owned resources. Failure rollback guarantees semantic zero, not
 * byte-for-byte restoration. A broken cleanup callback cannot be repaired by
 * the facade and is reported as CMETA_CALLBACK_ERROR.
 */
struct cmeta_data_enum_bits_ops {
    size_t struct_size;
    uint32_t abi_version;
    const cmeta_type_desc *storage_type;
    const cmeta_enum_domain *domain;
    cmeta_data_enum_is_zero_fn is_zero;
    cmeta_data_enum_read_bits_fn read;
    cmeta_data_enum_assign_bits_fn assign;
    cmeta_data_enum_restore_zero_fn restore_zero;
};

enum {
    CMETA_DATA_VARIANT_OPS_ABI_VERSION = 1u
};

/**
 * Variant adapter callback contract.
 *
 * Every object points to one correctly aligned instance of storage_type.
 * is_zero and active_tag do not mutate it. active_tag is called only for an
 * active object and, on success, must return a declared case tag. select is
 * called only for a semantic-zero object and a declared tag. On success it
 * must engage exactly that tag and initialize the selected payload to the
 * payload descriptor's semantic-zero state. It may partially mutate on
 * failure because the checked facade always follows failure with restore_zero.
 *
 * restore_zero is the single no-fail destruction path. It must release the
 * active payload and all provider-owned resources, accept zero or partially
 * initialized objects, be idempotent, and leave is_zero(object) true. Callback
 * implementations must not retain object or out pointers after returning.
 */
typedef bool (*cmeta_data_variant_is_zero_fn)(const void *object);
typedef cmeta_status (*cmeta_data_variant_active_tag_fn)(
    const void *object, int64_t *out);
typedef cmeta_status (*cmeta_data_variant_select_fn)(void *object,
                                                     int64_t tag);
typedef void (*cmeta_data_variant_restore_zero_fn)(void *object);

struct cmeta_data_variant_ops {
    size_t struct_size;
    uint32_t abi_version;
    const cmeta_type_desc *storage_type;
    cmeta_data_variant_is_zero_fn is_zero;
    cmeta_data_variant_active_tag_fn active_tag;
    cmeta_data_variant_select_fn select;
    cmeta_data_variant_restore_zero_fn restore_zero;
};

typedef struct cmeta_data_enum_shape {
    const cmeta_enum_desc *meta;
} cmeta_data_enum_shape;

typedef struct cmeta_data_field_desc {
    const char *stable_id;
    const char *name;
    size_t offset;
    const cmeta_data_desc *value;
} cmeta_data_field_desc;

typedef struct cmeta_data_struct_shape {
    const cmeta_struct_desc *layout;
    const cmeta_data_field_desc *fields;
    size_t field_count;
} cmeta_data_struct_shape;

typedef enum cmeta_data_reflection_mode {
    CMETA_DATA_REFLECTION_VIEW,
    CMETA_DATA_REFLECTION_VALUE
} cmeta_data_reflection_mode;

enum { CMETA_DATA_REFLECTION_MAX_FIELDS = 16u };

/* Only V2 STRUCT descriptors use this shape. The first member preserves
 * read-only struct inspection. VALUE attests that every native value field
 * is listed and that fieldwise zero/copy/move/cleanup is the native contract.
 * VIEW grants no semantic lifecycle, even for an otherwise trivial record. */
typedef struct cmeta_data_reflection_shape {
    cmeta_data_struct_shape structure;
    size_t struct_size;
    cmeta_data_reflection_mode mode;
} cmeta_data_reflection_shape;

/*
 * Provider-neutral collection reflection.
 *
 * A view borrows immutable element/key/value storage for the duration of one
 * consumer operation. It does not imply ownership, mutability, contiguity of
 * the provider's native container, or any CSTL/VM-specific representation.
 */
enum { CMETA_DATA_COLLECTION_OPS_ABI_VERSION = 1u };

typedef uint32_t cmeta_data_collection_flags;
enum {
    CMETA_DATA_COLLECTION_CONTIGUOUS = 1u << 0,
    CMETA_DATA_COLLECTION_ORDERED = 1u << 1,
    CMETA_DATA_COLLECTION_SORTED = 1u << 2,
    CMETA_DATA_COLLECTION_UNIQUE = 1u << 3,
    CMETA_DATA_COLLECTION_RANDOM_ACCESS = 1u << 4,
    CMETA_DATA_COLLECTION_FLAGS_MASK =
        CMETA_DATA_COLLECTION_CONTIGUOUS |
        CMETA_DATA_COLLECTION_ORDERED |
        CMETA_DATA_COLLECTION_SORTED |
        CMETA_DATA_COLLECTION_UNIQUE |
        CMETA_DATA_COLLECTION_RANDOM_ACCESS
};

typedef struct cmeta_data_collection_view {
    const void *data;
    size_t count;
    size_t stride;
    const cmeta_data_desc *element;
} cmeta_data_collection_view;

/* Canonical native storage descriptor for a borrowed contiguous sequence view.
 * The view owns nothing and remains valid only while its data/element provider
 * remain alive and immutable. */
extern const cmeta_type_desc cmeta_type_collection_view;
extern const cmeta_data_desc cmeta_data_sequence_view;

typedef const cmeta_data_desc *(*cmeta_data_collection_element_fn)(
    const void *object);
typedef cmeta_status (*cmeta_data_collection_read_fn)(
    const void *object, cmeta_data_collection_view *out);
typedef cmeta_status (*cmeta_data_collection_visit_fn)(
    void *context, const void *element);
typedef cmeta_status (*cmeta_data_collection_foreach_fn)(
    const void *object, cmeta_data_collection_visit_fn visit, void *context,
    size_t max_items);

enum { CMETA_DATA_COLLECTION_BORROW_OPS_ABI_VERSION = 1u };

typedef size_t (*cmeta_data_collection_borrow_size_fn)(const void *object);
typedef cmeta_gen_status (*cmeta_data_collection_borrow_next_fn)(
    const void *object, cmeta_range_cursor *cursor, const void **out_element);
typedef uint64_t (*cmeta_data_collection_borrow_version_fn)(
    const void *object);

typedef struct cmeta_data_collection_borrow_ops {
    size_t struct_size;
    uint32_t abi_version;
    cmeta_data_collection_borrow_size_fn size;
    cmeta_data_collection_borrow_next_fn next;
    cmeta_data_collection_borrow_version_fn current_version;
} cmeta_data_collection_borrow_ops;

typedef struct cmeta_data_collection_borrow_cursor {
    const cmeta_data_desc *data;
    const cmeta_data_desc *element;
    const void *object;
    const cmeta_data_collection_borrow_ops *ops;
    cmeta_range_cursor cursor;
    uint64_t version;
} cmeta_data_collection_borrow_cursor;

typedef cmeta_collector (*cmeta_data_collection_collector_fn)(
    void *zero_output, size_t limit);

/** Initialize a collector in its final caller-owned slot. Providers may borrow
 * that slot as callback context; it must not be copied before termination. */
typedef cmeta_status (*cmeta_data_collection_collector_init_fn)(
    void *zero_output, size_t limit, cmeta_collector *out);
typedef bool (*cmeta_data_collection_is_zero_fn)(const void *object);

typedef struct cmeta_data_collection_ops {
    size_t struct_size;
    uint32_t abi_version;
    const cmeta_type_desc *storage_type;
    cmeta_data_collection_flags flags;
    cmeta_data_collection_element_fn element;
    cmeta_data_collection_read_fn read;
    cmeta_data_collection_foreach_fn foreach;
    cmeta_data_collection_collector_fn collector;
    const cmeta_data_collection_borrow_ops *borrow;
    /** Optional descriptor-only element semantics; no object callback required. */
    const cmeta_data_desc *element_data;
    /** Optional allocation-free collector construction, preferred when present. */
    cmeta_data_collection_collector_init_fn collector_init;
    /** Optional semantic zero for collections whose zero is not an empty range. */
    cmeta_data_collection_is_zero_fn is_zero;
} cmeta_data_collection_ops;

enum { CMETA_DATA_MAP_OPS_ABI_VERSION = 1u };

typedef uint32_t cmeta_data_map_flags;
enum {
    CMETA_DATA_MAP_UNIQUE_KEYS = 1u << 0,
    CMETA_DATA_MAP_REPEATED_KEYS = 1u << 1,
    CMETA_DATA_MAP_ORDERED = 1u << 2,
    CMETA_DATA_MAP_SORTED = 1u << 3,
    CMETA_DATA_MAP_FLAGS_MASK =
        CMETA_DATA_MAP_UNIQUE_KEYS | CMETA_DATA_MAP_REPEATED_KEYS |
        CMETA_DATA_MAP_ORDERED | CMETA_DATA_MAP_SORTED
};

typedef const cmeta_data_desc *(*cmeta_data_map_member_fn)(
    const void *object);
typedef cmeta_status (*cmeta_data_map_visit_fn)(
    void *context, const void *key, const void *value);
typedef cmeta_status (*cmeta_data_map_foreach_fn)(
    const void *object, cmeta_data_map_visit_fn visit, void *context,
    size_t max_items);

enum { CMETA_DATA_MAP_BORROW_OPS_ABI_VERSION = 1u };

typedef size_t (*cmeta_data_map_borrow_size_fn)(const void *object);
typedef cmeta_gen_status (*cmeta_data_map_borrow_next_fn)(
    const void *object, cmeta_range_cursor *cursor,
    const void **out_key, const void **out_value);
typedef uint64_t (*cmeta_data_map_borrow_version_fn)(const void *object);

typedef struct cmeta_data_map_borrow_ops {
    size_t struct_size;
    uint32_t abi_version;
    cmeta_data_map_borrow_size_fn size;
    cmeta_data_map_borrow_next_fn next;
    cmeta_data_map_borrow_version_fn current_version;
} cmeta_data_map_borrow_ops;

typedef struct cmeta_data_map_borrow_cursor {
    const cmeta_data_desc *data;
    const cmeta_data_desc *key;
    const cmeta_data_desc *value;
    const void *object;
    const cmeta_data_map_borrow_ops *ops;
    cmeta_range_cursor cursor;
    uint64_t version;
} cmeta_data_map_borrow_cursor;

typedef cmeta_collector (*cmeta_data_map_collector_fn)(
    void *zero_output, size_t limit);
typedef cmeta_status (*cmeta_data_map_accept_fn)(
    cmeta_collector *collector, const cmeta_data_desc *key_data,
    const void *key, const cmeta_data_desc *value_data, const void *value);

typedef struct cmeta_data_map_ops {
    size_t struct_size;
    uint32_t abi_version;
    const cmeta_type_desc *storage_type;
    cmeta_data_map_flags flags;
    cmeta_data_map_member_fn key;
    cmeta_data_map_member_fn value;
    cmeta_data_map_foreach_fn foreach;
    cmeta_data_map_collector_fn collector;
    cmeta_data_map_accept_fn accept;
    const cmeta_data_map_borrow_ops *borrow;
    /** Optional descriptor-only key/value semantics; no object callback required. */
    const cmeta_data_desc *key_data;
    const cmeta_data_desc *value_data;
} cmeta_data_map_ops;

typedef struct cmeta_data_variant_case {
    int64_t tag;
    const char *stable_id;
    const char *name;
    size_t offset;
    const cmeta_data_desc *value;
} cmeta_data_variant_case;

typedef struct cmeta_data_variant_shape {
    size_t tag_offset;
    const cmeta_data_desc *tag;
    const cmeta_data_variant_case *cases;
    size_t case_count;
} cmeta_data_variant_shape;

bool cmeta_data_kind_valid(cmeta_data_kind kind);
bool cmeta_data_kind_is_container(cmeta_data_kind kind);
bool cmeta_data_desc_valid(const cmeta_data_desc *desc);
/** Semantic equality uses stable data identity plus canonical native type
 * identity; descriptor addresses are never semantic identity. */
bool cmeta_data_desc_equal(
    const cmeta_data_desc *left, const cmeta_data_desc *right);

/** Return the canonical exact-width integer descriptor, or NULL. */
const cmeta_data_desc *cmeta_data_integer_width(bool is_signed, uint8_t bits);

/**
 * Return a validated STRING/BYTES adapter, or NULL when the descriptor does
 * not expose a complete, matching v2 adapter. Descriptor and adapter addresses
 * are not type identities; storage types are compared semantically and then
 * checked for exact kind, size, and alignment. v1 providers are rejected;
 * there is no compatibility fallback.
 */
const cmeta_data_buffer_ops *cmeta_data_buffer_ops_of(
    const cmeta_data_desc *desc);

/** Return a validated SEQUENCE/SET collection adapter, or NULL. */
const cmeta_data_collection_ops *cmeta_data_collection_ops_of(
    const cmeta_data_desc *desc);

/**
 * Return provider-declared static element data without invoking an object
 * callback. Providers compiled without the optional tail return NULL.
 */
const cmeta_data_desc *cmeta_data_collection_element_data(
    const cmeta_data_desc *desc);

/** Borrow one immutable collection view for the provider callback lifetime. */
cmeta_status cmeta_data_collection_read(
    const cmeta_data_desc *desc, const void *object,
    cmeta_data_collection_view *out);

/**
 * Visit collection elements without exposing native iterator representation.
 * The facade enforces max_items before invoking a visitor beyond the bound.
 */
cmeta_status cmeta_data_collection_foreach(
    const cmeta_data_desc *desc, const void *object,
    cmeta_data_collection_visit_fn visit, void *context, size_t max_items);

cmeta_status cmeta_data_collection_collector(
    const cmeta_data_desc *desc, void *zero_output, size_t limit,
    cmeta_collector *out);

cmeta_status cmeta_data_collection_borrow_begin(
    const cmeta_data_desc *desc, const void *object,
    cmeta_data_collection_borrow_cursor *out);
cmeta_status cmeta_data_collection_borrow_size(
    const cmeta_data_collection_borrow_cursor *cursor, size_t *out_size);
cmeta_gen_status cmeta_data_collection_borrow_next(
    cmeta_data_collection_borrow_cursor *cursor, const void **out_element);

/**
 * Accept one already-constructed canonical semantic element. The facade checks
 * that the semantic descriptor's native storage type matches the collector's
 * declared input type before borrowing the value for the callback.
 */
cmeta_status cmeta_data_collection_accept(
    const cmeta_data_desc *desc, cmeta_collector *collector,
    const cmeta_data_desc *element_data, const void *element);

const cmeta_data_construct_ops *cmeta_data_construct_ops_of(
    const cmeta_data_desc *desc);
cmeta_status cmeta_data_construct_init_zero(
    const cmeta_data_desc *desc, void *object);
cmeta_status cmeta_data_construct_restore_zero(
    const cmeta_data_desc *desc, void *object);
cmeta_status cmeta_data_construct_move(
    const cmeta_data_desc *desc, void *destination, void *source);

/**
 * Generic semantic lifecycle dispatch used by derived aggregate providers.
 * These operations select the canonical authority for the data kind.
 */
bool cmeta_data_struct_constructible(const cmeta_data_desc *desc);
/** Query canonical semantic zero without mutating or retaining the object.
 * Unsupported providers return CMETA_TRAIT_MISSING and leave *out false. */
cmeta_status cmeta_data_value_is_zero(
    const cmeta_data_desc *desc, const void *object, bool *out);
cmeta_status cmeta_data_value_init_zero(
    const cmeta_data_desc *desc, void *object);
cmeta_status cmeta_data_value_restore_zero(
    const cmeta_data_desc *desc, void *object);
/**
 * Destroy one canonical value through its semantic restore-to-zero lifecycle.
 *
 * Unlike cmeta_data_trait_destroy(), this destroy-only facade does not require
 * copy or move support. Invalid arguments or a failed restore contract are
 * fatal provider/compile-contract violations.
 */
void cmeta_data_value_destroy(
    const cmeta_data_desc *desc, void *object);
bool cmeta_data_value_copy_supported(const cmeta_data_desc *desc);
/**
 * Copy one canonical value into semantic-zero destination storage.
 *
 * Source remains unchanged. On failure the destination is restored to
 * semantic zero whenever the descriptor exposes the required lifecycle.
 */
cmeta_status cmeta_data_value_copy(
    const cmeta_data_desc *desc, void *destination, const void *source);

bool cmeta_data_value_traits_supported(const cmeta_data_desc *desc);

/**
 * cmeta_type_traits bridge over canonical data lifecycle.
 *
 * copy_construct returns false on ordinary construction/copy failure.
 * move_construct and destroy cannot report errors in cmeta_type_traits; they
 * terminate the process if a descriptor that declared support violates its
 * no-fail move/restore contract.
 */
bool cmeta_data_trait_copy_construct(
    const cmeta_data_desc *desc, void *destination, const void *source);
void cmeta_data_trait_move_construct(
    const cmeta_data_desc *desc, void *destination, void *source);
void cmeta_data_trait_destroy(
    const cmeta_data_desc *desc, void *object);

#define CMETA_DEFINE_DATA_TRAITS(name_, data_expr_)                           \
    CMETA_INLINE bool name_##_cmeta_data_copy_construct(                      \
        void *destination_, const void *source_) {                            \
        return cmeta_data_trait_copy_construct(                               \
            (data_expr_), destination_, source_);                             \
    }                                                                         \
    CMETA_INLINE void name_##_cmeta_data_move_construct(                      \
        void *destination_, void *source_) {                                  \
        cmeta_data_trait_move_construct(                                      \
            (data_expr_), destination_, source_);                             \
    }                                                                         \
    CMETA_INLINE void name_##_cmeta_data_destroy(void *object_) {             \
        cmeta_data_trait_destroy((data_expr_), object_);                       \
    }                                                                         \
    CMETA_LOCAL const cmeta_type_traits cmeta_traits_##name_ = {              \
        .flags = CMETA_TRAIT_COPY | CMETA_TRAIT_MOVE | CMETA_TRAIT_DESTROY,  \
        .copy_construct = name_##_cmeta_data_copy_construct,                  \
        .move_construct = name_##_cmeta_data_move_construct,                  \
        .destroy = name_##_cmeta_data_destroy                                 \
    }

bool cmeta_data_value_move_supported(const cmeta_data_desc *desc);
cmeta_status cmeta_data_value_move(
    const cmeta_data_desc *desc, void *destination, void *source);

/* Ordinary-C move facades.
 *
 * cmeta_move_data() is the descriptor-explicit form for arbitrary providers.
 * cmeta_move(Type, ...) is the typed convenience for concrete types exposing
 * the canonical Type_cmeta_data() accessor. A successful move must leave the
 * source in provider-defined semantic zero, so source remains safe to destroy
 * or reinitialize without compiler-private ownership state.
 */
#define CMETA_DATA_ACCESSOR_I_(type_) type_##_cmeta_data
#define CMETA_DATA_ACCESSOR_(type_) CMETA_DATA_ACCESSOR_I_(type_)
#ifndef cmeta_move_data
#define cmeta_move_data(data_, destination_, source_) \
    cmeta_data_value_move((data_), (destination_), (source_))
#endif
#ifndef cmeta_move
#define cmeta_move(type_, destination_, source_) \
    cmeta_move_data(CMETA_DATA_ACCESSOR_(type_)(), (destination_), (source_))
#endif

typedef enum cmeta_data_temp_lifecycle {
    CMETA_DATA_TEMP_NONE = 0,
    CMETA_DATA_TEMP_TRIVIAL,
    CMETA_DATA_TEMP_BUFFER,
    CMETA_DATA_TEMP_ENUM,
    CMETA_DATA_TEMP_ENUM_BITS,
    CMETA_DATA_TEMP_FIXED,
    CMETA_DATA_TEMP_VARIANT,
    CMETA_DATA_TEMP_CONSTRUCT
} cmeta_data_temp_lifecycle;

typedef struct cmeta_data_temp {
    const cmeta_data_desc *data;
    void *storage;
    size_t extent;
    size_t alignment;
    cmeta_data_temp_lifecycle lifecycle;
} cmeta_data_temp;

/**
 * Allocate correctly aligned temporary native storage and initialize it to the
 * descriptor's semantic zero. The descriptor must expose a safe lifecycle.
 */
cmeta_status cmeta_data_temp_open(
    const cmeta_data_desc *desc, size_t max_bytes, cmeta_data_temp *out);

/**
 * Checked temporary close, backed by the SAME canonical no-fail
 * restore_zero provider (not a second lifecycle implementation).
 *
 * A provider that violates its no-fail semantic-zero contract can return
 * CMETA_CALLBACK_ERROR. In that case, temp retains its original storage,
 * descriptor and module-lifetime obligation; it MUST NOT be reused, moved,
 * freed or have its provider unloaded. The caller must isolate the owner
 * and treat the violation as fatal unless a higher owner has an explicit
 * safe repair strategy. A failed close is NOT an ordinary retryable fault.
 *
 * A successful close clears temp; NULL/already-empty inputs are idempotent.
 */
cmeta_status cmeta_data_temp_try_close(cmeta_data_temp *temp);

/**
 * Strict no-fail destruction of temporary storage. This legacy void API
 * aborts on any restore contract violation INSTEAD of freeing storage that
 * might still own resources. Use try_close only when the calling owner can
 * preserve the failed storage and original provider/module lease.
 */
void cmeta_data_temp_close(cmeta_data_temp *temp);


const cmeta_data_map_ops *cmeta_data_map_ops_of(
    const cmeta_data_desc *desc);

/** Descriptor-only static map member metadata; never calls key/value callbacks. */
const cmeta_data_desc *cmeta_data_map_key_data(
    const cmeta_data_desc *desc);
const cmeta_data_desc *cmeta_data_map_value_data(
    const cmeta_data_desc *desc);
cmeta_status cmeta_data_map_foreach(
    const cmeta_data_desc *desc, const void *object,
    cmeta_data_map_visit_fn visit, void *context, size_t max_items);
cmeta_status cmeta_data_map_borrow_begin(
    const cmeta_data_desc *desc, const void *object,
    cmeta_data_map_borrow_cursor *out);
cmeta_status cmeta_data_map_borrow_size(
    const cmeta_data_map_borrow_cursor *cursor, size_t *out_size);
cmeta_gen_status cmeta_data_map_borrow_next(
    cmeta_data_map_borrow_cursor *cursor,
    const void **out_key, const void **out_value);

cmeta_status cmeta_data_map_collector(
    const cmeta_data_desc *desc, void *zero_output, size_t limit,
    cmeta_collector *out);

cmeta_status cmeta_data_map_accept(
    const cmeta_data_desc *desc, cmeta_collector *collector,
    const cmeta_data_desc *key_data, const void *key,
    const cmeta_data_desc *value_data, const void *value);

/** Initialize one raw storage slot to provider semantic zero. */
cmeta_status cmeta_data_buffer_init_zero(
    const cmeta_data_desc *desc, void *object);

/**
 * Move source into an already semantic-zero destination and restore source to
 * semantic zero. A violated provider postcondition returns
 * CMETA_CALLBACK_ERROR after best-effort zero restoration.
 */
cmeta_status cmeta_data_buffer_move(
    const cmeta_data_desc *desc, void *destination, void *source);

/**
 * Query the provider-defined semantic zero state.
 *
 * @param desc STRING/BYTES descriptor with an accessible matching adapter.
 * @param object Address of one object of desc->storage_type.
 * @param out Receives true only when object is in semantic zero state.
 * @return CMETA_OK, CMETA_INVALID_ARGUMENT, or CMETA_TYPE_MISMATCH.
 */
cmeta_status cmeta_data_buffer_is_zero(
    const cmeta_data_desc *desc, const void *object, bool *out);

/**
 * Assign one bounded byte slice to a semantic-zero destination.
 *
 * The provider chooses copy or borrow semantics from its ownership contract.
 * On failure the checked facade invokes restore_zero before returning.
 *
 * @param data Borrowed input for this call; may be NULL only when size is zero.
 * @param size Input byte count.
 * @param max_bytes Hard per-value byte limit; larger input is rejected before
 *        the provider callback.
 * @return The exact provider status, CMETA_CAPACITY_EXCEEDED for the bound,
 *         or a validation error.
 *
 * Example: `cmeta_data_buffer_assign(desc, &value, bytes, count, 4096u)`.
 */
cmeta_status cmeta_data_buffer_assign(
    const cmeta_data_desc *desc, void *object,
    const unsigned char *data, size_t size, size_t max_bytes);

/**
 * Release owned state or clear borrowed state, then verify semantic zero.
 *
 * @return CMETA_OK, a descriptor validation error, or CMETA_CALLBACK_ERROR
 *         when the provider did not establish its declared zero state.
 */
cmeta_status cmeta_data_buffer_restore_zero(
    const cmeta_data_desc *desc, void *object);

/**
 * Read one bounded borrowed byte view without mutating the source object.
 *
 * The returned view remains provider-owned. It expires when the object is
 * assigned, restored, moved, destroyed, concurrently mutated, or when the
 * provider otherwise invalidates its storage. The caller must not retain it
 * across any such boundary. A zero-length view may have a NULL data pointer.
 *
 * read remains an optional capability within an otherwise complete v2
 * lifecycle provider. A provider without read returns CMETA_TRAIT_MISSING.
 */
cmeta_status cmeta_data_buffer_read(
    const cmeta_data_desc *desc, const void *object, size_t max_bytes,
    const unsigned char **out_data, size_t *out_size);

/** Return a complete, storage-matching exact native-value provider, or NULL. */
const cmeta_data_fixed_ops *cmeta_data_fixed_ops_of(
    const cmeta_data_desc *desc);

/** Return the provider-declared exact native object extent. */
cmeta_status cmeta_data_fixed_extent(
    const cmeta_data_desc *desc, size_t *out);

/** Query the provider-defined semantic-zero state of a native object. */
cmeta_status cmeta_data_fixed_is_zero(
    const cmeta_data_desc *desc, const void *object, bool *out);

/**
 * Copy one exact native value into a semantic-zero destination.
 *
 * source_extent must equal the provider extent. Provider failure restores the
 * destination to semantic zero before returning.
 */
cmeta_status cmeta_data_fixed_copy(
    const cmeta_data_desc *desc, void *destination, const void *source,
    size_t source_extent);

/** Restore and verify the provider-defined semantic-zero state. */
cmeta_status cmeta_data_fixed_restore_zero(
    const cmeta_data_desc *desc, void *object);

/** Return a complete, storage-matching enum adapter, or NULL. */
const cmeta_data_enum_ops *cmeta_data_enum_ops_of(
    const cmeta_data_desc *desc);

/** Query the provider-defined semantic-zero state of an enum object. */
cmeta_status cmeta_data_enum_is_zero(
    const cmeta_data_desc *desc, const void *object, bool *out);

/** Read one enum object as its reflected signed 64-bit value. */
cmeta_status cmeta_data_enum_read(
    const cmeta_data_desc *desc, const void *object, int64_t *out);

/**
 * Assign a declared enum value to a semantic-zero destination.
 *
 * Provider failure or a violated read-back postcondition restores zero before
 * returning.
 */
cmeta_status cmeta_data_enum_assign(
    const cmeta_data_desc *desc, void *object, int64_t value);

/** Restore and verify the provider-defined enum semantic-zero state. */
cmeta_status cmeta_data_enum_restore_zero(
    const cmeta_data_desc *desc, void *object);

/** Return a complete canonical domain provider, or NULL; no legacy fallback. */
const cmeta_data_enum_bits_ops *cmeta_data_enum_bits_ops_of(
    const cmeta_data_desc *desc);

/** Query canonical enum semantic zero; output changes only on success. */
cmeta_status cmeta_data_enum_bits_is_zero(
    const cmeta_data_desc *desc, const void *object, bool *out);

/**
 * Read validated canonical bits. Every failure leaves out unchanged.
 * Malformed arguments/descriptors return CMETA_INVALID_ARGUMENT; mismatched
 * provider storage returns CMETA_TYPE_MISMATCH. Any provider failure or
 * invalid provider result returns CMETA_CALLBACK_ERROR (not the callback's
 * status), keeping provider execution failures distinct from malformed input.
 */
cmeta_status cmeta_data_enum_read_bits(
    const cmeta_data_desc *desc, const void *object, uint64_t *out);

/**
 * Assign canonical bits to semantic zero, checking width and membership before
 * mutation. Invalid values/nonzero destinations are unchanged and return
 * CMETA_INVALID_ARGUMENT. Provider/read-back failure restores semantic zero
 * and returns CMETA_CALLBACK_ERROR. Descriptor errors match read_bits.
 */
cmeta_status cmeta_data_enum_assign_bits(
    const cmeta_data_desc *desc, void *object, uint64_t bits);

/** Restore semantic zero and verify cleanup; errors match read_bits. */
cmeta_status cmeta_data_enum_bits_restore_zero(
    const cmeta_data_desc *desc, void *object);

/** Return a complete, storage-matching variant adapter, or NULL. */
const cmeta_data_variant_ops *cmeta_data_variant_ops_of(
    const cmeta_data_desc *desc);

/** Query the provider-defined unselected semantic-zero state. */
cmeta_status cmeta_data_variant_is_zero(
    const cmeta_data_desc *desc, const void *object, bool *out);

/** Read the active tag; an unselected object is not an active variant. */
cmeta_status cmeta_data_variant_active_tag(
    const cmeta_data_desc *desc, const void *object, int64_t *out);

/**
 * Select a declared case in a semantic-zero object and verify engagement.
 *
 * On provider failure or postcondition violation the object is restored to
 * semantic zero before returning.
 */
cmeta_status cmeta_data_variant_select(
    const cmeta_data_desc *desc, void *object, int64_t tag);

/** Destroy any active payload, restore semantic zero, and verify the result. */
cmeta_status cmeta_data_variant_restore_zero(
    const cmeta_data_desc *desc, void *object);

const cmeta_data_field_desc *cmeta_data_struct_field(
    const cmeta_data_struct_shape *shape, size_t index);
const cmeta_data_field_desc *cmeta_data_struct_find_field(
    const cmeta_data_struct_shape *shape, const char *name);
const cmeta_data_variant_case *cmeta_data_variant_case_by_tag(
    const cmeta_data_variant_shape *shape, int64_t tag);

extern const cmeta_data_desc cmeta_data_int8;
extern const cmeta_data_desc cmeta_data_uint8;
extern const cmeta_data_desc cmeta_data_int16;
extern const cmeta_data_desc cmeta_data_uint16;
extern const cmeta_data_desc cmeta_data_int32;
extern const cmeta_data_desc cmeta_data_uint32;
extern const cmeta_data_desc cmeta_data_int64;
extern const cmeta_data_desc cmeta_data_uint64;
extern const cmeta_data_desc cmeta_data_bool;
extern const cmeta_data_desc cmeta_data_int;
extern const cmeta_data_desc cmeta_data_long;
extern const cmeta_data_desc cmeta_data_size;
extern const cmeta_data_desc cmeta_data_float;
extern const cmeta_data_desc cmeta_data_double;

extern const cmeta_data_desc cmeta_data_sequence;
extern const cmeta_data_desc cmeta_data_set;
extern const cmeta_data_desc cmeta_data_map;

#ifdef __cplusplus
}
#endif

#endif /* CMETA_DATA_H */
