#ifndef CMETA_VARIANT_H
#define CMETA_VARIANT_H

#include <cmeta/data.h>
#include <limits.h>
#include <stdlib.h>

#ifdef __cplusplus
#define CMETA_VARIANT_ASSERT_(condition_, message_) static_assert(condition_, message_)
#define CMETA_VARIANT_CAST_(type_, value_) static_cast<type_ *>(value_)
#define CMETA_VARIANT_INTEGER_(type_, value_) static_cast<type_>(value_)
#else
#define CMETA_VARIANT_ASSERT_(condition_, message_) _Static_assert(condition_, message_)
#define CMETA_VARIANT_CAST_(type_, value_) ((type_ *)(value_))
#define CMETA_VARIANT_INTEGER_(type_, value_) ((type_)(value_))
#endif

#define CMETA_VARIANT_ROW_(mapper_, owner_, row_) \
    CMETA_VARIANT_ROW_E_(mapper_, owner_, CMETA_PP_UNPAREN row_)
#define CMETA_VARIANT_ROW_E_(mapper_, owner_, ...) mapper_(owner_, __VA_ARGS__)
#define CMETA_VARIANT_TAG_(row_, owner_) CMETA_VARIANT_ROW_(CMETA_VARIANT_TAG_I_, owner_, row_)
#define CMETA_VARIANT_TAG_I_(owner_, case_, tag_, type_, data_) owner_##_##case_ = (tag_),
#define CMETA_VARIANT_STORAGE_(row_, owner_) CMETA_VARIANT_ROW_(CMETA_VARIANT_STORAGE_I_, owner_, row_)
#define CMETA_VARIANT_STORAGE_I_(owner_, case_, tag_, type_, data_) type_ case_;
#define CMETA_VARIANT_CHECK_(row_, owner_) CMETA_VARIANT_ROW_(CMETA_VARIANT_CHECK_I_, owner_, row_)
/* Unsigned literals must not convert INT_MIN to an unsigned lower bound. */
#define CMETA_VARIANT_CHECK_I_(owner_, case_, tag_, type_, data_) \
    CMETA_VARIANT_ASSERT_((tag_) != 0 && ((tag_) > 0 \
        ? CMETA_VARIANT_INTEGER_(uintmax_t, tag_) <= INT_MAX \
        : CMETA_VARIANT_INTEGER_(intmax_t, tag_) >= INT_MIN), \
                         "CMeta variant tag must be nonzero and fit int");
#define CMETA_VARIANT_META_(row_, context_) \
    CMETA_VARIANT_META_E_(CMETA_PP_UNPAREN context_, CMETA_PP_UNPAREN row_)
#define CMETA_VARIANT_META_E_(...) CMETA_VARIANT_META_I_(__VA_ARGS__)
#define CMETA_VARIANT_META_I_(owner_, id_, case_, tag_, type_, data_) \
    {(tag_), id_ "." #case_, #case_, offsetof(owner_, as.case_), (data_)},

#define CMETA_VARIANT_VALIDATE_(row_, owner_) CMETA_VARIANT_ROW_(CMETA_VARIANT_VALIDATE_I_, owner_, row_)
#define CMETA_VARIANT_VALIDATE_I_(owner_, case_, tag_, type_, data_) \
    if (!cmeta_data_desc_valid(data_) || (data_)->storage_type == NULL || \
        (data_)->storage_type->size != sizeof(type_) || \
        (data_)->storage_type->align != CMETA_ALIGNOF(type_)) \
        return CMETA_TYPE_MISMATCH; \
    if (!cmeta_data_value_traits_supported(data_)) return CMETA_TRAIT_MISSING;

#define CMETA_VARIANT_SELECT_(row_, owner_) CMETA_VARIANT_ROW_(CMETA_VARIANT_SELECT_I_, owner_, row_)
#define CMETA_VARIANT_SELECT_I_(owner_, case_, tag_, type_, data_) \
    case owner_##_##case_: \
        value_->tag = (tag_); \
        return cmeta_data_value_init_zero(data_, &value_->as.case_);
#define CMETA_VARIANT_RESTORE_(row_, owner_) CMETA_VARIANT_ROW_(CMETA_VARIANT_RESTORE_I_, owner_, row_)
#define CMETA_VARIANT_RESTORE_I_(owner_, case_, tag_, type_, data_) \
    case owner_##_##case_: \
        cmeta_data_value_destroy(data_, &value_->as.case_); \
        break;

#define CMETA_VARIANT_ACCESS_(row_, owner_) CMETA_VARIANT_ROW_(CMETA_VARIANT_ACCESS_I_, owner_, row_)
#define CMETA_VARIANT_ACCESS_I_(owner_, case_, tag_, type_, data_) \
    CMETA_INLINE const type_ *owner_##_get_##case_(const owner_ *value_) { \
        return value_ != NULL && value_->tag == (tag_) ? &value_->as.case_ : NULL; \
    } \
    CMETA_INLINE type_ *owner_##_mut_##case_(owner_ *value_) { \
        return value_ != NULL && value_->tag == (tag_) ? &value_->as.case_ : NULL; \
    } \
    CMETA_INLINE cmeta_status owner_##_copy_##case_(owner_ *out_, const type_ *source_) { \
        cmeta_status status_; \
        if (out_ == NULL || source_ == NULL || \
            &out_->as.case_ == source_) \
            return CMETA_INVALID_ARGUMENT; \
        status_ = owner_##_select(out_, (tag_)); \
        if (status_ != CMETA_OK) return status_; \
        status_ = cmeta_data_value_copy(data_, &out_->as.case_, source_); \
        if (status_ != CMETA_OK) owner_##_destroy(out_); \
        return status_; \
    } \
    CMETA_INLINE cmeta_status owner_##_move_##case_(owner_ *out_, type_ *source_) { \
        cmeta_status status_; \
        if (out_ == NULL || source_ == NULL || \
            &out_->as.case_ == source_) \
            return CMETA_INVALID_ARGUMENT; \
        status_ = owner_##_select(out_, (tag_)); \
        if (status_ != CMETA_OK) return status_; \
        status_ = cmeta_data_value_move(data_, &out_->as.case_, source_); \
        if (status_ != CMETA_OK) owner_##_destroy(out_); \
        return status_; \
    }

/**
 * Generate one inline tagged union and canonical DataDesc provider from 1-16
 * cases. IDs are string literals; tags are explicit nonzero int constants.
 * Payload DataDesc pointers are static expressions matching native size/align
 * and must provide canonical copy/move/restore. init accepts raw storage;
 * copy/move/select require a live empty destination. Accessors borrow until
 * mutation, move or destroy. Mutable access is single-owner or synchronized.
 * Payload allocation and limits remain with its canonical provider.
 */
#define CMETA_VARIANT_(name_, id_, ...) \
    CMETA_PP_FOR_EACH(CMETA_VARIANT_CHECK_, name_, __VA_ARGS__) \
    typedef enum name_##_tag { \
        name_##_None = 0, \
        CMETA_PP_FOR_EACH(CMETA_VARIANT_TAG_, name_, __VA_ARGS__) \
    } name_##_tag; \
    typedef struct name_ { \
        int64_t tag; \
        union { CMETA_PP_FOR_EACH(CMETA_VARIANT_STORAGE_, name_, __VA_ARGS__) } as; \
    } name_; \
    CMETA_INLINE const cmeta_data_desc *name_##_cmeta_data(void); \
    CMETA_INLINE cmeta_status name_##_validate(void); \
    CMETA_INLINE cmeta_status name_##__init_zero(void *object_) { \
        if (object_ == NULL) return CMETA_INVALID_ARGUMENT; \
        CMETA_VARIANT_CAST_(name_, object_)->tag = 0; \
        return name_##_validate(); \
    } \
    CMETA_INLINE bool name_##__is_zero(const void *object_) { \
        return object_ != NULL && CMETA_VARIANT_CAST_(const name_, object_)->tag == 0; \
    } \
    CMETA_INLINE cmeta_status name_##__active_tag(const void *object_, int64_t *out_) { \
        if (object_ == NULL || out_ == NULL) return CMETA_INVALID_ARGUMENT; \
        *out_ = CMETA_VARIANT_CAST_(const name_, object_)->tag; \
        return CMETA_OK; \
    } \
    CMETA_INLINE cmeta_status name_##__select(void *object_, int64_t tag_) { \
        name_ *value_ = CMETA_VARIANT_CAST_(name_, object_); \
        if (value_ == NULL || value_->tag != 0) return CMETA_INVALID_ARGUMENT; \
        switch (tag_) { \
            CMETA_PP_FOR_EACH(CMETA_VARIANT_SELECT_, name_, __VA_ARGS__) \
            default: return CMETA_INVALID_ARGUMENT; \
        } \
    } \
    CMETA_INLINE void name_##__restore(void *object_) { \
        name_ *value_ = CMETA_VARIANT_CAST_(name_, object_); \
        if (value_ == NULL) return; \
        switch (value_->tag) { \
            case 0: return; \
            CMETA_PP_FOR_EACH(CMETA_VARIANT_RESTORE_, name_, __VA_ARGS__) \
            default: abort(); \
        } \
        value_->tag = 0; \
    } \
    CMETA_INLINE void name_##__construct_move(void *out_, void *source_) { \
        if (cmeta_data_value_move(name_##_cmeta_data(), out_, source_) != CMETA_OK) \
            abort(); \
    } \
    CMETA_INLINE bool name_##__trait_copy(void *out_, const void *source_) { \
        return cmeta_data_trait_copy_construct(name_##_cmeta_data(), out_, source_); \
    } \
    CMETA_INLINE void name_##__trait_move(void *out_, void *source_) { \
        cmeta_data_trait_move_construct(name_##_cmeta_data(), out_, source_); \
    } \
    CMETA_INLINE void name_##__trait_destroy(void *value_) { \
        cmeta_data_value_destroy(name_##_cmeta_data(), value_); \
    } \
    enum { name_##__cmeta_traits_flags = \
        CMETA_TRAIT_COPY | CMETA_TRAIT_MOVE | CMETA_TRAIT_DESTROY }; \
    CMETA_LOCAL const cmeta_type_traits cmeta_traits_##name_ = { \
        name_##__cmeta_traits_flags, NULL, NULL, NULL, \
        name_##__trait_copy, name_##__trait_move, name_##__trait_destroy }; \
    CMETA_LOCAL const cmeta_type_identity name_##__identity = CMETA_TYPE_ID_ATOM_INIT(id_); \
    CMETA_LOCAL const cmeta_type_desc name_##_cmeta_type = { \
        #name_, sizeof(name_), CMETA_ALIGNOF(name_), CMETA_T_OBJECT, \
        NULL, &cmeta_traits_##name_, &name_##__identity }; \
    CMETA_LOCAL const cmeta_data_variant_case name_##__cases[] = { \
        CMETA_PP_FOR_EACH(CMETA_VARIANT_META_, (name_, id_), __VA_ARGS__) \
    }; \
    CMETA_LOCAL const cmeta_data_variant_shape name_##__shape = { \
        offsetof(name_, tag), &cmeta_data_int64, name_##__cases, \
        sizeof(name_##__cases) / sizeof(name_##__cases[0]) }; \
    CMETA_LOCAL const cmeta_data_variant_ops name_##__variant_ops = { \
        sizeof(cmeta_data_variant_ops), CMETA_DATA_VARIANT_OPS_ABI_VERSION, \
        &name_##_cmeta_type, name_##__is_zero, name_##__active_tag, \
        name_##__select, name_##__restore }; \
    CMETA_LOCAL const cmeta_data_construct_ops name_##__construct_ops = { \
        sizeof(cmeta_data_construct_ops), CMETA_DATA_CONSTRUCT_OPS_ABI_VERSION, \
        &name_##_cmeta_type, name_##__init_zero, name_##__restore, name_##__construct_move, 0 }; \
    CMETA_LOCAL const cmeta_data_desc name_##__data = { \
        sizeof(cmeta_data_desc), CMETA_DATA_DESC_ABI_VERSION, id_ ".data", #name_, \
        CMETA_DATA_VARIANT, &name_##_cmeta_type, &name_##__shape, NULL, NULL, \
        &name_##__variant_ops, NULL, NULL, NULL, NULL, &name_##__construct_ops }; \
    CMETA_INLINE const cmeta_data_desc *name_##_cmeta_data(void) { return &name_##__data; } \
    CMETA_INLINE cmeta_status name_##_validate(void) { \
        CMETA_PP_FOR_EACH(CMETA_VARIANT_VALIDATE_, name_, __VA_ARGS__) \
        return cmeta_data_desc_valid(&name_##__data) ? CMETA_OK : CMETA_TYPE_MISMATCH; \
    } \
    CMETA_INLINE cmeta_status name_##_init(name_ *out_) { return name_##__init_zero(out_); } \
    CMETA_INLINE void name_##_destroy(name_ *value_) { name_##__trait_destroy(value_); } \
    CMETA_INLINE cmeta_status name_##_select(name_ *value_, int64_t tag_) { \
        cmeta_status status_ = name_##_validate(); \
        return status_ == CMETA_OK ? cmeta_data_variant_select(&name_##__data, value_, tag_) : status_; \
    } \
    CMETA_INLINE cmeta_status name_##_copy(name_ *out_, const name_ *source_) { \
        if (out_ == NULL || source_ == NULL || out_ == source_ || out_->tag != 0) \
            return CMETA_INVALID_ARGUMENT; \
        cmeta_status status_ = name_##_validate(); \
        return status_ == CMETA_OK ? cmeta_data_value_copy(&name_##__data, out_, source_) : status_; \
    } \
    CMETA_INLINE cmeta_status name_##_move(name_ *out_, name_ *source_) { \
        cmeta_status status_ = name_##_validate(); \
        return status_ == CMETA_OK ? cmeta_data_value_move(&name_##__data, out_, source_) : status_; \
    } \
    CMETA_PP_FOR_EACH(CMETA_VARIANT_ACCESS_, name_, __VA_ARGS__) \
    typedef name_ name_##_value_type

#define cmeta_case(case_, tag_, type_, data_) , (case_, tag_, type_, data_)
#define CMETA_VARIANT_PUBLIC_E_(...) CMETA_VARIANT_PUBLIC_I_(__VA_ARGS__)
#define CMETA_VARIANT_PUBLIC_I_(name_, id_, sentinel_, ...) CMETA_VARIANT_(name_, id_, __VA_ARGS__)
#define cmeta_variant(name_, id_, ...) CMETA_VARIANT_PUBLIC_E_(name_, id_, ~ __VA_ARGS__)
/** Ordinary switch; use generated Name_Case labels and checked typed accessors. */
#define cmeta_match(value_) switch ((value_)->tag)

#endif
