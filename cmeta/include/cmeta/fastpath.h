#ifndef CMETA_FASTPATH_H
#define CMETA_FASTPATH_H

#include <cmeta/function.h>
#include <fastpath.h>

#ifndef __cplusplus
#include <stdatomic.h>

/*
 * Typed static-call projection over C11 atomic target storage.
 *
 * CMeta owns declaration/signature/ABI validation. Invocation acquires the
 * target without a Reflection lookup.
 */
#define cmeta_static_call(name_, default_) \
    typedef default_##_function_type name_##_target_type; \
    typedef struct name_##_slot_type { \
        _Atomic(name_##_target_type) target; \
    } name_##_slot_type; \
    CMETA_INLINE name_##_target_type name_##_load( \
        const name_##_slot_type *slot_) { \
        return atomic_load_explicit(&slot_->target, memory_order_acquire); \
    } \
    CMETA_INLINE cmeta_status name_##_set( \
        name_##_slot_type *slot_, name_##_target_type target_, \
        const cmeta_function_abi_desc *abi_) { \
        const cmeta_function_abi_desc *expected_ = default_##_function_abi(); \
        if (slot_ == NULL || target_ == NULL || \
            !cmeta_function_abi_desc_valid(expected_) || \
            !cmeta_function_abi_desc_valid(abi_)) \
            return CMETA_INVALID_ARGUMENT; \
        if (!cmeta_function_abi_contract_compatible(expected_, abi_)) \
            return CMETA_TYPE_MISMATCH; \
        atomic_store_explicit(&slot_->target, target_, memory_order_release); \
        return CMETA_OK; \
    } \
    name_##_slot_type name_ = {default_}

#define cmeta_static_update(name_, function_) \
    name_##_set(&(name_), \
        _Generic((function_), name_##_target_type: (function_)), \
        function_##_function_abi())

#define cmeta_static_invoke(name_, ...) name_##_load(&(name_))(__VA_ARGS__)
#define cmeta_static_invoke0(name_) name_##_load(&(name_))()

#endif /* !__cplusplus */

#endif
