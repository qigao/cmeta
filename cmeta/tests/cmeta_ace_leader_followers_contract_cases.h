#ifndef CMETA_ACE_LEADER_FOLLOWERS_CONTRACT_CASES_H
#define CMETA_ACE_LEADER_FOLLOWERS_CONTRACT_CASES_H
/* #1064: test-local metadata only. Platform owns synchronization; CMeta
 * reflects an exact, borrowed CPU-event callback. No runtime or Plugin lease
 * is synthesized here; the external owner must outlive joined callbacks. */
#include <cmeta/function.h>
#include "platform_ace_leader_followers_types.h"

static const cmeta_type_desc lf_role_meta_type = {
    "lf_role", sizeof(enum lf_role), CMETA_ALIGNOF(enum lf_role),
    CMETA_T_INTEGER, NULL, NULL, NULL
};
static const cmeta_type_desc lf_stop_meta_type = {
    "lf_stop", sizeof(enum lf_stop), CMETA_ALIGNOF(enum lf_stop),
    CMETA_T_INTEGER, NULL, NULL, NULL
};
static const cmeta_type_desc lf_result_meta_type = {
    "lf_result", sizeof(enum lf_result), CMETA_ALIGNOF(enum lf_result),
    CMETA_T_INTEGER, NULL, NULL, NULL
};

CMETA_FUNCTION_METADATA_AS_ABI_RESULT(
    lf_ace_cpu_event_contract, "ace.leader_followers.cpu_event",
    io, &lf_result_meta_type, CMETA_ABI_ENUM, CMETA_RESULT_VALUE,
    (void *, context, CMETA_PARAM_IN | CMETA_PARAM_BORROWED,
     &cmeta_type_void_ptr, CMETA_ABI_OBJECT_POINTER),
    (int, event, CMETA_PARAM_IN, &cmeta_type_int, CMETA_ABI_SCALAR));

#ifdef __cplusplus
#define LF_META_CAST(type_, value_) static_cast<type_>(value_)
#else
#define LF_META_CAST(type_, value_) ((type_)(value_))
#endif
static enum lf_result lf_ace_cpu_handler(void *context, int event) {
    if (context == NULL || event < 0) return LF_ERROR;
    *LF_META_CAST(int *, context) += event;
    return LF_OK;
}
CMETA_STATIC_ASSERT(
    CMETA_TYPE_MATCHES(&lf_ace_cpu_handler, lf_handler_fn),
    "CMeta Leader/Followers reflection must match exact native callback type");

static void lf_ace_check_callable_metadata(void) {
    const cmeta_function_desc *fn =
        &lf_ace_cpu_event_contract__function_meta;
    const cmeta_function_abi_desc *abi =
        &lf_ace_cpu_event_contract__function_abi_meta;
    const cmeta_param_desc *context_param = cmeta_function_param(fn, 0u);
    const cmeta_param_desc *event_param = cmeta_function_param(fn, 1u);
    cmeta_function_abi_desc bad = *abi;
    const size_t expected_param_count = 2u;
    const cmeta_result_flags expected_result_flags = CMETA_RESULT_VALUE;
    const cmeta_param_flags expected_borrowed = CMETA_PARAM_IN | CMETA_PARAM_BORROWED;
    const cmeta_param_flags expected_input = CMETA_PARAM_IN;
    int sum = 0;
    lf_handler_fn callback = lf_ace_cpu_handler;

    check_true(cmeta_type_desc_valid(&lf_role_meta_type));
    check_true(cmeta_type_desc_valid(&lf_stop_meta_type));
    check_true(cmeta_type_desc_valid(&lf_result_meta_type));
    check_true(cmeta_function_desc_valid(fn));
    check_true(cmeta_function_abi_desc_valid(abi));
    check_true(context_param != NULL);
    check_true(event_param != NULL);
    check_equal(abi->return_carrier, CMETA_ABI_ENUM);
    check_equal(abi->param_count, expected_param_count);
    check_equal(abi->param_carriers[0], CMETA_ABI_OBJECT_POINTER);
    check_equal(abi->param_carriers[1], CMETA_ABI_SCALAR);
    check_equal(fn->result_flags, expected_result_flags);
    if (context_param != NULL && event_param != NULL) {
        check_equal(context_param->flags, expected_borrowed);
        check_equal(event_param->flags, expected_input);
    }
    bad.param_count = 1u;
    check_false(cmeta_function_abi_desc_valid(&bad));
    check_equal(callback(&sum, 3), LF_OK);
    check_equal(sum, 3);
    check_equal(callback(NULL, 4), LF_ERROR);
    check_equal(callback(&sum, -1), LF_ERROR);
    check_equal(sum, 3);
}
#undef LF_META_CAST
#endif
