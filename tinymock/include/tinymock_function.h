#ifndef TINYMOCK_FUNCTION_H
#define TINYMOCK_FUNCTION_H

/*
 * TinyMock free-function bridge for CMeta FunctionDecl.
 *
 * Define TINYMOCK_GENERATE_FUNCTION_OVERRIDES in the test source before
 * including this header and any production headers containing FunctionDecl.
 * The CMeta declaration extension hook replays those exact rows into external
 * replacement definitions in that test translation unit. No additional source
 * file or CMake generator is required; link the test with Salts::TinyMock.
 * Other translation units can include this header without the generation flag
 * to access the same mock state through TINYMOCk_FUNCTION_DECLARE.
 * In the generating test source, TINYMOCk_FUNCTION_DECLARE must follow the
 * production declarations and rejects functions without generated overrides.
 *
 *
 * Reflected wrappers use only CMeta typed history/actions/returns. Call-count
 * verification is derived directly from typed history; reflected functions do
 * not carry legacy mock state or its boxed expectation/script engine.
 * Literal-void and value-return functions are generated separately at
 * preprocessing time. Variadic declarations remain outside this backend.
 */

#include "tinymock_support.h"

#ifdef __cplusplus
#error "tinymock_function.h is the strict-C11 free-function mock bridge"
#endif

#define TINYMOCk_FUNCTION_RESET_NAME_I(name) tinymock_function_##name##_reset
#define TINYMOCk_FUNCTION_RESET_NAME(name) TINYMOCk_FUNCTION_RESET_NAME_I(name)

#define TINYMOCk_FUNCTION_META_NAME_I(name) tinymock_function_##name##_meta
#define TINYMOCk_FUNCTION_META_NAME(name) TINYMOCk_FUNCTION_META_NAME_I(name)

#define TINYMOCk_FUNCTION_ABI_META_NAME_I(name) tinymock_function_##name##_abi
#define TINYMOCk_FUNCTION_ABI_META_NAME(name) TINYMOCk_FUNCTION_ABI_META_NAME_I(name)

#define TINYMOCk_FUNCTION_HISTORY_NAME_I(name) tinymock_function_##name##_history
#define TINYMOCk_FUNCTION_HISTORY_NAME(name) TINYMOCk_FUNCTION_HISTORY_NAME_I(name)

#define TINYMOCk_FUNCTION_DESTROY_NAME_I(name) tinymock_function_##name##_destroy
#define TINYMOCk_FUNCTION_DESTROY_NAME(name) TINYMOCk_FUNCTION_DESTROY_NAME_I(name)

#define TINYMOCk_FUNCTION_ACTIONS_NAME_I(name) tinymock_function_##name##_actions
#define TINYMOCk_FUNCTION_ACTIONS_NAME(name) TINYMOCk_FUNCTION_ACTIONS_NAME_I(name)

#define TINYMOCk_FUNCTION_RETURN_STATE_NAME_I(name) tinymock_function_##name##_return
#define TINYMOCk_FUNCTION_RETURN_STATE_NAME(name) \
  TINYMOCk_FUNCTION_RETURN_STATE_NAME_I(name)

#if defined(TINYMOCK_GENERATE_FUNCTION_OVERRIDES)
#define TINYMOCk_FUNCTION_REQUIRE_DECL_(name) \
  _Static_assert( \
      sizeof(struct tinymock_required_FunctionDecl_##name) == sizeof(char), \
      "TinyMock function " #name " must be declared with FunctionDecl " \
      "and selected for generation");
#else
#define TINYMOCk_FUNCTION_REQUIRE_DECL_(name)
#endif

#define TINYMOCk_FUNCTION_DECLARE(name) \
  TINYMOCk_FUNCTION_REQUIRE_DECL_(name) \
  extern struct tinymock_cmeta_history TINYMOCk_FUNCTION_HISTORY_NAME(name); \
  extern struct tinymock_cmeta_actions TINYMOCk_FUNCTION_ACTIONS_NAME(name); \
  extern struct tinymock_cmeta_return TINYMOCk_FUNCTION_RETURN_STATE_NAME(name); \
  void TINYMOCk_FUNCTION_RESET_NAME(name)(void); \
  void TINYMOCk_FUNCTION_DESTROY_NAME(name)(void); \
  const struct cmeta_function_desc *TINYMOCk_FUNCTION_META_NAME(name)(void); \
  const struct cmeta_function_abi_desc *TINYMOCk_FUNCTION_ABI_META_NAME(name)(void)

#define TINYMOCk_FUNCTION_HISTORY(name) (&TINYMOCk_FUNCTION_HISTORY_NAME(name))
#define TINYMOCk_FUNCTION_ACTIONS(name) (&TINYMOCk_FUNCTION_ACTIONS_NAME(name))
#define TINYMOCk_FUNCTION_RETURN_STATE(name) \
  (&TINYMOCk_FUNCTION_RETURN_STATE_NAME(name))
#define TINYMOCk_FUNCTION_RESET(name) TINYMOCk_FUNCTION_RESET_NAME(name)()
#define TINYMOCk_FUNCTION_DESTROY(name) TINYMOCk_FUNCTION_DESTROY_NAME(name)()
#define TINYMOCk_FUNCTION_META(name) TINYMOCk_FUNCTION_META_NAME(name)()
#define TINYMOCk_FUNCTION_ABI(name) TINYMOCk_FUNCTION_ABI_META_NAME(name)()

#define TINYMOCk_FUNCTION_CALL_COUNT(name) \
  tinymock_cmeta_history_call_count(TINYMOCk_FUNCTION_HISTORY(name))

#define TINYMOCk_FUNCTION_VERIFY_TIMES(name, expected) \
  do { \
    size_t expected__ = (size_t)(expected); \
    size_t actual__ = TINYMOCk_FUNCTION_CALL_COUNT(name); \
    TINYMOCk_ASSERT(actual__ == expected__, \
        "tinymock reflected %s: expected exactly %zu calls, got %zu", \
        #name, expected__, actual__); \
  } while (0)
#define TINYMOCk_FUNCTION_VERIFY_NEVER(name) \
  TINYMOCk_FUNCTION_VERIFY_TIMES(name, 0u)
#define TINYMOCk_FUNCTION_VERIFY_AT_LEAST(name, minimum) \
  do { \
    size_t minimum__ = (size_t)(minimum); \
    size_t actual__ = TINYMOCk_FUNCTION_CALL_COUNT(name); \
    TINYMOCk_ASSERT(actual__ >= minimum__, \
        "tinymock reflected %s: expected at least %zu calls, got %zu", \
        #name, minimum__, actual__); \
  } while (0)
#define TINYMOCk_FUNCTION_VERIFY_AT_MOST(name, maximum) \
  do { \
    size_t maximum__ = (size_t)(maximum); \
    size_t actual__ = TINYMOCk_FUNCTION_CALL_COUNT(name); \
    TINYMOCk_ASSERT(actual__ <= maximum__, \
        "tinymock reflected %s: expected at most %zu calls, got %zu", \
        #name, maximum__, actual__); \
  } while (0)

#define TINYMOCk_FUNCTION_ARG_EQUAL_TYPED( \
    name, call_index, param_name, expected_lvalue) \
  tinymock_cmeta_history_arg_equal_typed_name( \
      TINYMOCk_FUNCTION_HISTORY(name), (call_index), (param_name), \
      &(expected_lvalue))

#define TINYMOCk_FUNCTION_COUNT_EQUAL_TYPED( \
    name, param_name, expected_lvalue) \
  tinymock_cmeta_history_count_equal_typed_name( \
      TINYMOCk_FUNCTION_HISTORY(name), (param_name), &(expected_lvalue))

#define TINYMOCk_FUNCTION_ARG_MATCH_DATA( \
    name, call_index, param_name, data, expected_lvalue, result) \
  tinymock_cmeta_history_arg_match_data_name( \
      TINYMOCk_FUNCTION_HISTORY(name), (call_index), (param_name), \
      (data), &(expected_lvalue), NULL, (result))

#define TINYMOCk_FUNCTION_ARG_POINTER_EQUAL( \
    name, call_index, param_name, expected_pointer) \
  tinymock_cmeta_history_arg_pointer_equal_name( \
      TINYMOCk_FUNCTION_HISTORY(name), (call_index), (param_name), \
      (const void *)(expected_pointer))

#define TINYMOCk_FUNCTION_CAPTURE(name, call_index, param_name, captor) \
  tinymock_cmeta_captor_capture_name( \
      (captor), TINYMOCk_FUNCTION_HISTORY(name), (call_index), (param_name))

#define TINYMOCk_FUNCTION_SET_OUT(name, param_name, value_lvalue) \
  tinymock_cmeta_actions_set_output_name( \
      TINYMOCk_FUNCTION_ACTIONS(name), TINYMOCk_FUNCTION_META(name), \
      (param_name), &(value_lvalue))

#define TINYMOCk_FUNCTION_CLEAR_OUT(name, param_name) \
  tinymock_cmeta_actions_clear_output_name( \
      TINYMOCk_FUNCTION_ACTIONS(name), (param_name))

#define TINYMOCk_FUNCTION_SET_RETURN(name, value_lvalue) \
  tinymock_cmeta_return_set( \
      TINYMOCk_FUNCTION_RETURN_STATE(name), TINYMOCk_FUNCTION_META(name), \
      &(value_lvalue))

#define TINYMOCk_FUNCTION_CLEAR_RETURN(name) \
  tinymock_cmeta_return_clear(TINYMOCk_FUNCTION_RETURN_STATE(name))

#if defined(TINYMOCK_GENERATE_FUNCTION_OVERRIDES)

#define TINYMOCk_PP_SECOND_(a, b, ...) b
#define TINYMOCk_PP_PROBE_() ~, 1
#define TINYMOCk_PP_IS_PROBE_(...) TINYMOCk_PP_SECOND_(__VA_ARGS__, 0, 0)
#define TINYMOCk_RETURN_VOID_MARK_void TINYMOCk_PP_PROBE_()
#define TINYMOCk_RETURN_IS_VOID_(type) \
  TINYMOCk_PP_IS_PROBE_(TINYMOCk_CAT(TINYMOCk_RETURN_VOID_MARK_, type))
#define TINYMOCk_RETURN_SELECT_(prefix, type) \
  TINYMOCk_CAT(prefix, TINYMOCk_RETURN_IS_VOID_(type))

#define TINYMOCk_FUNCTION_SELECTED_MARK_(name) \
  TINYMOCk_CAT(TINYMOCK_SELECTED_FUNCTION_, name)
#define TINYMOCk_FUNCTION_IS_SELECTED_(name) \
  TINYMOCk_PP_IS_PROBE_(TINYMOCk_FUNCTION_SELECTED_MARK_(name))

#define TINYMOCk_FUNCTION_WITNESS_TAG_I(name) \
  tinymock_required_FunctionDecl_##name
#define TINYMOCk_FUNCTION_WITNESS_TAG(name) \
  TINYMOCk_FUNCTION_WITNESS_TAG_I(name)

#if defined(TINYMOCK_SELECTIVE_FUNCTION_OVERRIDES)
#define TINYMOCk_FUNCTION_SHOULD_GENERATE_(name) \
  TINYMOCk_FUNCTION_IS_SELECTED_(name)
#else
#define TINYMOCk_FUNCTION_SHOULD_GENERATE_(name) 1
#endif

#define TINYMOCk_FUNCTION_SELECTION_WITNESS(name) \
  struct TINYMOCk_FUNCTION_WITNESS_TAG(name) { char seen; };

#ifdef CMETA_FUNCTION_DECL_EXTENSION
#undef CMETA_FUNCTION_DECL_EXTENSION
#endif
#ifdef CMETA_FUNCTION0_DECL_EXTENSION
#undef CMETA_FUNCTION0_DECL_EXTENSION
#endif
#ifdef CMETA_FUNCTION_DECL_ABI_EXTENSION
#undef CMETA_FUNCTION_DECL_ABI_EXTENSION
#endif
#ifdef CMETA_FUNCTION0_DECL_ABI_EXTENSION
#undef CMETA_FUNCTION0_DECL_ABI_EXTENSION
#endif

#define TINYMOCk_FUNCTION_PARAM_NAME(row) \
  CMETA_FUNCTION_PARAM_NAME(row)

#define TINYMOCk_FUNCTION_VIEW_CMETA_ABI_SCALAR(value) \
  { (const void *)&(value), false, NULL }
#define TINYMOCk_FUNCTION_VIEW_CMETA_ABI_OBJECT_POINTER(value) \
  { (const void *)&(value), true, (const void *)(value) }
#define TINYMOCk_FUNCTION_VIEW_CMETA_ABI_AGGREGATE(value) \
  { (const void *)&(value), false, NULL }
#define TINYMOCk_FUNCTION_VIEW_CMETA_ABI_FUNCTION_POINTER(value) \
  { (const void *)&(value), false, NULL }
#define TINYMOCk_FUNCTION_VIEW_CMETA_ABI_ENUM(value) \
  { (const void *)&(value), false, NULL }
#define TINYMOCk_FUNCTION_VIEW_CMETA_ABI_UNSPECIFIED(value) \
  { (const void *)&(value), false, NULL }
#define TINYMOCk_FUNCTION_VIEW_CMETA_ABI_OPAQUE(value) \
  { (const void *)&(value), false, NULL }
#define TINYMOCk_FUNCTION_VIEW_CMETA_ABI_VOID(value) \
  { (const void *)&(value), false, NULL }
#define TINYMOCk_FUNCTION_VIEW_(carrier, value) \
  CMETA_PP_CAT(TINYMOCk_FUNCTION_VIEW_, carrier)(value)

#define TINYMOCk_FUNCTION_ARG_VIEW_APPLY(row) \
  TINYMOCk_FUNCTION_VIEW_(CMETA_FUNCTION_PARAM_CARRIER(row),CMETA_FUNCTION_PARAM_NAME(row))

#define TINYMOCk_FUNCTION_TYPED_ARG_ROW(index, row, ignored) \
  CMETA_PP_SEP_COMMA(index) \
  TINYMOCk_FUNCTION_ARG_VIEW_APPLY(row)

#define TINYMOCk_FUNCTION_PARAM_ADMIT_3(function_name, type, name, flags) \
  _Static_assert(1, "TinyMock scalar ABI admission")
#define TINYMOCk_FUNCTION_PARAM_ADMIT_4( \
    function_name, type, name, flags, descriptor) \
  _Static_assert(0, \
      "TinyMock auto-mock parameter " #function_name "." #name \
      " has unspecified ABI carrier; use the 5-field FunctionDecl row")
#define TINYMOCk_FUNCTION_PARAM_ADMIT_5( \
    function_name, type, name, flags, descriptor, abi_carrier) \
  _Static_assert( \
      (abi_carrier) == CMETA_ABI_SCALAR || \
      (abi_carrier) == CMETA_ABI_OBJECT_POINTER || \
      (abi_carrier) == CMETA_ABI_AGGREGATE || \
      (abi_carrier) == CMETA_ABI_FUNCTION_POINTER || \
      (abi_carrier) == CMETA_ABI_ENUM, \
      "TinyMock auto-mock parameter " #function_name "." #name \
      " uses an unsupported ABI carrier")
#define TINYMOCk_FUNCTION_PARAM_ADMIT_APPLY_I(function_name, ...) \
  CMETA_PP_CAT(TINYMOCk_FUNCTION_PARAM_ADMIT_, \
               CMETA_PP_NARG(__VA_ARGS__))(function_name, __VA_ARGS__)
#define TINYMOCk_FUNCTION_PARAM_ADMIT_APPLY(function_name, row) \
  TINYMOCk_FUNCTION_PARAM_ADMIT_APPLY_I(function_name, \
                                         TINYMOCk_FUNCTION_PARAM_ROW_EXPAND row)
#define TINYMOCk_FUNCTION_PARAM_ROW_EXPAND(...) __VA_ARGS__
#define TINYMOCk_FUNCTION_PARAM_ADMIT_ROW(row, function_name) \
  TINYMOCk_FUNCTION_PARAM_ADMIT_APPLY(function_name, row);

#define TINYMOCk_FUNCTION_ADMIT_PARAMS(function_name, ...) \
  CMETA_PP_FOR_EACH_A( \
      TINYMOCk_FUNCTION_PARAM_ADMIT_ROW, function_name, __VA_ARGS__)

#define TINYMOCk_FUNCTION_VALUE_RETURN_ADMIT(return_abi_carrier, name) \
  _Static_assert( \
      (return_abi_carrier) != CMETA_ABI_VOID, \
      "TinyMock auto-mock return for " #name \
      " uses CMETA_ABI_VOID with a non-literal void spelling; use literal void"); \
  _Static_assert( \
      (return_abi_carrier) == CMETA_ABI_SCALAR || \
      (return_abi_carrier) == CMETA_ABI_OBJECT_POINTER || \
      (return_abi_carrier) == CMETA_ABI_AGGREGATE || \
      (return_abi_carrier) == CMETA_ABI_FUNCTION_POINTER || \
      (return_abi_carrier) == CMETA_ABI_ENUM || \
      (return_abi_carrier) == CMETA_ABI_VOID, \
      "TinyMock auto-mock return for " #name \
      " uses an unsupported or unspecified ABI carrier")

#define TINYMOCk_FUNCTION_VOID_RETURN_ADMIT(return_abi_carrier, name) \
  _Static_assert( \
      (return_abi_carrier) == CMETA_ABI_VOID, \
      "TinyMock auto-mock void return for " #name \
      " must use CMETA_ABI_VOID")

#define TINYMOCk_FUNCTION_RETURN_TYPED_ONLY(name, type) \
  do { \
    type typed_result__; \
    bool typed_ok__ = tinymock_cmeta_return_write_admitted( \
        TINYMOCk_FUNCTION_RETURN_STATE(name), \
        &typed_result__); \
    TINYMOCk_ASSERT(typed_ok__, \
                    "tinymock reflected return for %s requires a typed return", \
                    #name); \
    if (typed_ok__) return typed_result__; \
    return (type){0}; \
  } while (0)

#define TINYMOCk_FUNCTION_RETURN_CMETA_ABI_SCALAR(name, type) \
  TINYMOCk_FUNCTION_RETURN_TYPED_ONLY(name, type)
#define TINYMOCk_FUNCTION_RETURN_CMETA_ABI_OBJECT_POINTER(name, type) \
  TINYMOCk_FUNCTION_RETURN_TYPED_ONLY(name, type)
#define TINYMOCk_FUNCTION_RETURN_CMETA_ABI_AGGREGATE(name, type) \
  TINYMOCk_FUNCTION_RETURN_TYPED_ONLY(name, type)
#define TINYMOCk_FUNCTION_RETURN_CMETA_ABI_FUNCTION_POINTER(name, type) \
  TINYMOCk_FUNCTION_RETURN_TYPED_ONLY(name, type)
#define TINYMOCk_FUNCTION_RETURN_CMETA_ABI_ENUM(name, type) \
  TINYMOCk_FUNCTION_RETURN_TYPED_ONLY(name, type)
#define TINYMOCk_FUNCTION_RETURN_CMETA_ABI_UNSPECIFIED(name, type) \
  return *(type *)0
#define TINYMOCk_FUNCTION_RETURN_CMETA_ABI_OPAQUE(name, type) \
  return *(type *)0
#define TINYMOCk_FUNCTION_RETURN_CMETA_ABI_VOID(name, type) \
  return
#define TINYMOCk_FUNCTION_RETURN_VALUE(carrier, name, type) \
  CMETA_PP_CAT(TINYMOCk_FUNCTION_RETURN_, carrier)(name, type)

#define TINYMOCk_FUNCTION_DEFINE_STATE(fn_name) \
  tinymock_cmeta_history TINYMOCk_FUNCTION_HISTORY_NAME(fn_name); \
  tinymock_cmeta_actions TINYMOCk_FUNCTION_ACTIONS_NAME(fn_name); \
  tinymock_cmeta_return TINYMOCk_FUNCTION_RETURN_STATE_NAME(fn_name); \
  void TINYMOCk_FUNCTION_RESET_NAME(fn_name)(void) { \
    const cmeta_function_desc *meta__ = FunctionMeta(fn_name); \
    const cmeta_function_abi_desc *abi__ = FunctionAbi(fn_name); \
    TINYMOCk_ASSERT(cmeta_function_desc_valid(meta__), \
                    "tinymock reflected function metadata is invalid: %s", #fn_name); \
    TINYMOCk_ASSERT(cmeta_function_abi_desc_valid(abi__), \
                    "tinymock reflected ABI metadata is invalid: %s", #fn_name); \
    tinymock_cmeta_history_reset(&TINYMOCk_FUNCTION_HISTORY_NAME(fn_name), meta__); \
    tinymock_cmeta_actions_reset(&TINYMOCk_FUNCTION_ACTIONS_NAME(fn_name), meta__); \
    tinymock_cmeta_return_reset(&TINYMOCk_FUNCTION_RETURN_STATE_NAME(fn_name), meta__); \
  } \
  void TINYMOCk_FUNCTION_DESTROY_NAME(fn_name)(void) { \
    tinymock_cmeta_history_destroy(&TINYMOCk_FUNCTION_HISTORY_NAME(fn_name)); \
    tinymock_cmeta_actions_destroy(&TINYMOCk_FUNCTION_ACTIONS_NAME(fn_name)); \
    tinymock_cmeta_return_destroy(&TINYMOCk_FUNCTION_RETURN_STATE_NAME(fn_name)); \
  } \
  const cmeta_function_desc *TINYMOCk_FUNCTION_META_NAME(fn_name)(void) { \
    return FunctionMeta(fn_name); \
  } \
  const cmeta_function_abi_desc *TINYMOCk_FUNCTION_ABI_META_NAME(fn_name)(void) { \
    return FunctionAbi(fn_name); \
  }

#define TINYMOCk_FUNCTION_DECL_ABI_EXTENSION_0( \
    contract, return_type, return_desc, return_abi_carrier, name, ...) \
  TINYMOCk_FUNCTION_VALUE_RETURN_ADMIT(return_abi_carrier, name); \
  TINYMOCk_FUNCTION_ADMIT_PARAMS(name, __VA_ARGS__) \
  TINYMOCk_FUNCTION_DEFINE_STATE(name) \
  return_type name( \
      CMETA_PP_FOR_EACH_I(CMETA_FUNCTION_PARAM_DECL, ~, __VA_ARGS__)) { \
    tinymock_cmeta_arg_view typed_args__[] = { \
      CMETA_PP_FOR_EACH_I(TINYMOCk_FUNCTION_TYPED_ARG_ROW, ~, __VA_ARGS__) \
    }; \
    TINYMOCk_ASSERT( \
        tinymock_cmeta_history_record_admitted( \
            &TINYMOCk_FUNCTION_HISTORY_NAME(name), \
            CMETA_PP_NARG(__VA_ARGS__), typed_args__), \
        "tinymock cannot snapshot reflected arguments for %s", #name); \
    TINYMOCk_ASSERT( \
        tinymock_cmeta_actions_apply_admitted( \
            &TINYMOCk_FUNCTION_ACTIONS_NAME(name), \
            CMETA_PP_NARG(__VA_ARGS__), typed_args__), \
        "tinymock cannot apply reflected output actions for %s", #name); \
    TINYMOCk_FUNCTION_RETURN_VALUE(return_abi_carrier, name, return_type); \
  }

#define TINYMOCk_FUNCTION_DECL_ABI_EXTENSION_1( \
    contract, return_type, return_desc, return_abi_carrier, name, ...) \
  TINYMOCk_FUNCTION_VOID_RETURN_ADMIT(return_abi_carrier, name); \
  TINYMOCk_FUNCTION_ADMIT_PARAMS(name, __VA_ARGS__) \
  TINYMOCk_FUNCTION_DEFINE_STATE(name) \
  return_type name( \
      CMETA_PP_FOR_EACH_I(CMETA_FUNCTION_PARAM_DECL, ~, __VA_ARGS__)) { \
    tinymock_cmeta_arg_view typed_args__[] = { \
      CMETA_PP_FOR_EACH_I(TINYMOCk_FUNCTION_TYPED_ARG_ROW, ~, __VA_ARGS__) \
    }; \
    TINYMOCk_ASSERT( \
        tinymock_cmeta_history_record_admitted( \
            &TINYMOCk_FUNCTION_HISTORY_NAME(name), \
            CMETA_PP_NARG(__VA_ARGS__), typed_args__), \
        "tinymock cannot snapshot reflected arguments for %s", #name); \
    TINYMOCk_ASSERT( \
        tinymock_cmeta_actions_apply_admitted( \
            &TINYMOCk_FUNCTION_ACTIONS_NAME(name), \
            CMETA_PP_NARG(__VA_ARGS__), typed_args__), \
        "tinymock cannot apply reflected output actions for %s", #name); \
    return; \
  }

#define TINYMOCk_FUNCTION_DECL_ABI_SELECTED_0( \
    contract, return_type, return_desc, return_abi_carrier, name, ...)
#define TINYMOCk_FUNCTION_DECL_ABI_SELECTED_1( \
    contract, return_type, return_desc, return_abi_carrier, name, ...) \
  TINYMOCk_FUNCTION_SELECTION_WITNESS(name) \
  TINYMOCk_RETURN_SELECT_(TINYMOCk_FUNCTION_DECL_ABI_EXTENSION_, return_type)( \
      contract, return_type, return_desc, return_abi_carrier, name, __VA_ARGS__)

#define TINYMOCk_FUNCTION_DECL_ABI_EXTENSION( \
    contract, return_type, return_desc, return_abi_carrier, name, ...) \
  TINYMOCk_CAT(TINYMOCk_FUNCTION_DECL_ABI_SELECTED_, \
               TINYMOCk_FUNCTION_SHOULD_GENERATE_(name))( \
      contract, return_type, return_desc, return_abi_carrier, name, __VA_ARGS__)

#define TINYMOCk_FUNCTION0_DECL_ABI_EXTENSION_0( \
    contract, return_type, return_desc, return_abi_carrier, name) \
  TINYMOCk_FUNCTION_VALUE_RETURN_ADMIT(return_abi_carrier, name); \
  TINYMOCk_FUNCTION_DEFINE_STATE(name) \
  return_type name(void) { \
    TINYMOCk_ASSERT( \
        tinymock_cmeta_history_record_admitted( \
            &TINYMOCk_FUNCTION_HISTORY_NAME(name), \
            0u, NULL), \
        "tinymock cannot snapshot reflected arguments for %s", #name); \
    TINYMOCk_ASSERT( \
        tinymock_cmeta_actions_apply_admitted( \
            &TINYMOCk_FUNCTION_ACTIONS_NAME(name), \
            0u, NULL), \
        "tinymock cannot apply reflected output actions for %s", #name); \
    TINYMOCk_FUNCTION_RETURN_VALUE(return_abi_carrier, name, return_type); \
  }

#define TINYMOCk_FUNCTION0_DECL_ABI_EXTENSION_1( \
    contract, return_type, return_desc, return_abi_carrier, name) \
  TINYMOCk_FUNCTION_VOID_RETURN_ADMIT(return_abi_carrier, name); \
  TINYMOCk_FUNCTION_DEFINE_STATE(name) \
  return_type name(void) { \
    TINYMOCk_ASSERT( \
        tinymock_cmeta_history_record_admitted( \
            &TINYMOCk_FUNCTION_HISTORY_NAME(name), \
            0u, NULL), \
        "tinymock cannot snapshot reflected arguments for %s", #name); \
    TINYMOCk_ASSERT( \
        tinymock_cmeta_actions_apply_admitted( \
            &TINYMOCk_FUNCTION_ACTIONS_NAME(name), \
            0u, NULL), \
        "tinymock cannot apply reflected output actions for %s", #name); \
    return; \
  }

#define TINYMOCk_FUNCTION0_DECL_ABI_SELECTED_0( \
    contract, return_type, return_desc, return_abi_carrier, name)
#define TINYMOCk_FUNCTION0_DECL_ABI_SELECTED_1( \
    contract, return_type, return_desc, return_abi_carrier, name) \
  TINYMOCk_FUNCTION_SELECTION_WITNESS(name) \
  TINYMOCk_RETURN_SELECT_(TINYMOCk_FUNCTION0_DECL_ABI_EXTENSION_, return_type)( \
      contract, return_type, return_desc, return_abi_carrier, name)

#define TINYMOCk_FUNCTION0_DECL_ABI_EXTENSION( \
    contract, return_type, return_desc, return_abi_carrier, name) \
  TINYMOCk_CAT(TINYMOCk_FUNCTION0_DECL_ABI_SELECTED_, \
               TINYMOCk_FUNCTION_SHOULD_GENERATE_(name))( \
      contract, return_type, return_desc, return_abi_carrier, name)

#define CMETA_FUNCTION_DECL_EXTENSION(contract, return_type, return_desc, name, ...)
#define CMETA_FUNCTION0_DECL_EXTENSION(contract, return_type, return_desc, name)

#define CMETA_FUNCTION_DECL_ABI_EXTENSION( \
    contract, return_type, return_desc, return_abi_carrier, name, ...) \
  TINYMOCk_FUNCTION_DECL_ABI_EXTENSION( \
      contract, return_type, return_desc, return_abi_carrier, name, __VA_ARGS__)

#define CMETA_FUNCTION0_DECL_ABI_EXTENSION( \
    contract, return_type, return_desc, return_abi_carrier, name) \
  TINYMOCk_FUNCTION0_DECL_ABI_EXTENSION( \
      contract, return_type, return_desc, return_abi_carrier, name)

#endif /* TINYMOCK_GENERATE_FUNCTION_OVERRIDES */

#include <cmeta/function.h>
#include "tinymock_history.h"
#include "tinymock_actions.h"
#include "tinymock_return.h"

#endif /* TINYMOCK_FUNCTION_H */
