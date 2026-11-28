#ifndef CMETA_FLAGS_H
#define CMETA_FLAGS_H

#include <cmeta/data.h>
#include <stdlib.h>

#ifdef __cplusplus
#define CMETA_FLAGS_CAST_(type_, value_) static_cast<type_ *>(value_)
#define CMETA_FLAGS_BITS_(value_) static_cast<uint64_t>(value_)
#else
#define CMETA_FLAGS_CAST_(type_, value_) ((type_ *)(value_))
#define CMETA_FLAGS_BITS_(value_) ((uint64_t)(value_))
#endif
#define CMETA_FLAGS_ROW_(mapper_, owner_, row_) \
    CMETA_FLAGS_ROW_E_(mapper_, owner_, CMETA_PP_UNPAREN row_)
#define CMETA_FLAGS_ROW_E_(mapper_, owner_, ...) mapper_(owner_, __VA_ARGS__)
#define CMETA_FLAGS_VALUE_(row_, owner_) CMETA_FLAGS_ROW_(CMETA_FLAGS_VALUE_I_, owner_, row_)
#define CMETA_FLAGS_VALUE_I_(owner_, symbol_, bits_, text_) \
    typedef char owner_##_##symbol_##__nonempty_text[ \
        sizeof(#symbol_) > 1u && sizeof("" text_) > 1u ? 1 : -1]; \
    CMETA_LOCAL const owner_ owner_##_##symbol_ = {CMETA_FLAGS_BITS_(bits_)};
#define CMETA_FLAGS_ITEM_(row_, owner_) CMETA_FLAGS_ROW_(CMETA_FLAGS_ITEM_I_, owner_, row_)
#define CMETA_FLAGS_ITEM_I_(owner_, symbol_, bits_, text_) \
    {CMETA_FLAGS_BITS_(bits_), #symbol_, text_},
#define CMETA_FLAGS_MASK_(row_, owner_) CMETA_FLAGS_ROW_(CMETA_FLAGS_MASK_I_, owner_, row_)
#define CMETA_FLAGS_MASK_I_(owner_, symbol_, bits_, text_) | CMETA_FLAGS_BITS_(bits_)

/**
 * Define a distinct uint64_t flags value and one canonical unsigned64 domain.
 * Rows are (symbol, canonical bits, display text); zero, aliases and composite
 * masks are permitted. IDs/text are static string literals. Metadata, named
 * constants, membership checks and DataDesc all derive from these same rows.
 * Values own no resources; access requires a single owner or synchronization.
 */
#define CMETA_FLAGS_(name_, id_, ...) \
    typedef struct name_ { uint64_t bits; } name_; \
    CMETA_PP_FOR_EACH(CMETA_FLAGS_VALUE_, name_, __VA_ARGS__) \
    CMETA_LOCAL const cmeta_enum_bits_item name_##__items[] = { \
        CMETA_PP_FOR_EACH(CMETA_FLAGS_ITEM_, name_, __VA_ARGS__) }; \
    CMETA_LOCAL const cmeta_enum_domain name_##__domain = { \
        sizeof(cmeta_enum_domain), CMETA_ENUM_DOMAIN_ABI_VERSION, \
        CMETA_ENUM_UNSIGNED, 64u, CMETA_ENUM_FLAGS, name_##__items, \
        sizeof(name_##__items) / sizeof(name_##__items[0]), \
        UINT64_C(0) CMETA_PP_FOR_EACH(CMETA_FLAGS_MASK_, name_, __VA_ARGS__) }; \
    CMETA_INLINE const cmeta_enum_domain *name_##_meta(void) { return &name_##__domain; } \
    CMETA_INLINE bool name_##_valid(name_ value_) { \
        return (value_.bits & ~name_##__domain.declared_mask) == 0u; \
    } \
    CMETA_INLINE cmeta_status name_##_from_bits(uint64_t bits_, name_ *out_) { \
        if (out_ == NULL || (bits_ & ~name_##__domain.declared_mask) != 0u) \
            return CMETA_INVALID_ARGUMENT; \
        out_->bits = bits_; \
        return CMETA_OK; \
    } \
    CMETA_INLINE cmeta_status name_##_or(name_ left_, name_ right_, name_ *out_) { \
        return name_##_valid(left_) && name_##_valid(right_) \
            ? name_##_from_bits(left_.bits | right_.bits, out_) : CMETA_INVALID_ARGUMENT; \
    } \
    CMETA_INLINE bool name_##_contains(name_ value_, name_ mask_) { \
        return name_##_valid(value_) && name_##_valid(mask_) && \
               (value_.bits & mask_.bits) == mask_.bits; \
    } \
    CMETA_INLINE cmeta_status name_##__init(void *object_) { \
        if (object_ == NULL) return CMETA_INVALID_ARGUMENT; \
        CMETA_FLAGS_CAST_(name_, object_)->bits = 0u; \
        return CMETA_OK; \
    } \
    CMETA_INLINE bool name_##__is_zero(const void *object_) { \
        return object_ != NULL && CMETA_FLAGS_CAST_(const name_, object_)->bits == 0u; \
    } \
    CMETA_INLINE cmeta_status name_##__read(const void *object_, uint64_t *out_) { \
        const name_ *value_ = CMETA_FLAGS_CAST_(const name_, object_); \
        if (value_ == NULL || out_ == NULL || !name_##_valid(*value_)) \
            return CMETA_INVALID_ARGUMENT; \
        *out_ = value_->bits; \
        return CMETA_OK; \
    } \
    CMETA_INLINE cmeta_status name_##__assign(void *object_, uint64_t bits_) { \
        return name_##_from_bits(bits_, CMETA_FLAGS_CAST_(name_, object_)); \
    } \
    CMETA_INLINE void name_##__restore(void *object_) { \
        if (object_ != NULL) CMETA_FLAGS_CAST_(name_, object_)->bits = 0u; \
    } \
    CMETA_INLINE bool name_##__copy(void *out_, const void *source_) { \
        if (out_ == NULL || source_ == NULL || out_ == source_) return false; \
        name_ *destination_ = CMETA_FLAGS_CAST_(name_, out_); \
        const name_ *value_ = CMETA_FLAGS_CAST_(const name_, source_); \
        destination_->bits = 0u; \
        if (!name_##_valid(*value_)) return false; \
        destination_->bits = value_->bits; \
        return true; \
    } \
    CMETA_INLINE void name_##__move(void *out_, void *source_) { \
        name_ *destination_ = CMETA_FLAGS_CAST_(name_, out_); \
        name_ *value_ = CMETA_FLAGS_CAST_(name_, source_); \
        if (destination_ == NULL || value_ == NULL || \
            destination_ == value_ || !name_##_valid(*value_)) abort(); \
        destination_->bits = value_->bits; \
        value_->bits = 0u; \
    } \
    enum { name_##__cmeta_traits_flags = CMETA_TRAIT_COPY | CMETA_TRAIT_MOVE | CMETA_TRAIT_DESTROY }; \
    CMETA_LOCAL const cmeta_type_traits cmeta_traits_##name_ = { \
        name_##__cmeta_traits_flags, NULL, NULL, NULL, name_##__copy, name_##__move, name_##__restore }; \
    CMETA_LOCAL const cmeta_type_identity name_##__identity = CMETA_TYPE_ID_ATOM_INIT(id_); \
    CMETA_LOCAL const cmeta_type_desc name_##_cmeta_type = { \
        #name_, sizeof(name_), CMETA_ALIGNOF(name_), CMETA_T_OBJECT, \
        NULL, &cmeta_traits_##name_, &name_##__identity }; \
    CMETA_LOCAL const cmeta_data_enum_bits_ops name_##__bits_ops = { \
        sizeof(cmeta_data_enum_bits_ops), CMETA_DATA_ENUM_BITS_OPS_ABI_VERSION, \
        &name_##_cmeta_type, &name_##__domain, name_##__is_zero, name_##__read, \
        name_##__assign, name_##__restore }; \
    CMETA_LOCAL const cmeta_data_construct_ops name_##__construct_ops = { \
        sizeof(cmeta_data_construct_ops), CMETA_DATA_CONSTRUCT_OPS_ABI_VERSION, \
        &name_##_cmeta_type, name_##__init, name_##__restore, name_##__move, 0 }; \
    CMETA_LOCAL const cmeta_data_desc name_##__data = { \
        sizeof(cmeta_data_desc), CMETA_DATA_DESC_ABI_VERSION, id_ ".data", #name_, \
        CMETA_DATA_ENUM, &name_##_cmeta_type, NULL, NULL, NULL, NULL, NULL, \
        &name_##__bits_ops, NULL, NULL, &name_##__construct_ops }; \
    CMETA_INLINE const cmeta_data_desc *name_##_cmeta_data(void) { return &name_##__data; } \
    typedef name_ name_##_value_type

#define cmeta_flag(symbol_, bits_, text_) , (symbol_, bits_, text_)
#define CMETA_FLAGS_PUBLIC_E_(...) CMETA_FLAGS_PUBLIC_I_(__VA_ARGS__)
#define CMETA_FLAGS_PUBLIC_I_(name_, id_, sentinel_, ...) CMETA_FLAGS_(name_, id_, __VA_ARGS__)
#define cmeta_flags(name_, id_, ...) CMETA_FLAGS_PUBLIC_E_(name_, id_, ~ __VA_ARGS__)

#endif
