#define TINYTEST_NO_MAIN
#include "tinymock_history.h"

#include <string.h>

static size_t tinymock_cmeta_recorded_limit(size_t count) {
  return count < TINYMOCk_MAX_CALLS ? count : TINYMOCk_MAX_CALLS;
}

bool tinymock_cmeta_function_equal(
    const cmeta_function_desc *left,
    const cmeta_function_desc *right) {
  return cmeta_function_desc_equal(left, right);
}

static bool tinymock_cmeta_history_copy_arg(
    tinymock_cmeta_value *value,
    const cmeta_type_desc *type,
    const tinymock_cmeta_arg_view *arg) {
  if (!arg || !arg->address)
    return false;

  if (!tinymock_cmeta_value_copy_admitted(value, type, arg->address)) return false;
  if (type->kind == CMETA_T_POINTER && arg->has_object_pointer_identity) {
    value->has_pointer_identity = true;
    value->pointer_identity = arg->object_pointer_identity;
  }
  return true;
}

static void tinymock_cmeta_call_clear(tinymock_cmeta_recorded_call *call) {
  size_t index;
  if (!call) return;
  for (index = 0; index < call->argc && index < TINYMOCk_MAX_ARGS; ++index)
    tinymock_cmeta_value_reset(&call->args[index]);
  memset(call, 0, sizeof(*call));
}

void tinymock_cmeta_history_init(tinymock_cmeta_history *history,
                                 const cmeta_function_desc *function) {
  if (!history) return;
  memset(history, 0, sizeof(*history));
  history->function = cmeta_function_desc_valid(function) ? function : NULL;
}

void tinymock_cmeta_history_destroy(tinymock_cmeta_history *history) {
  size_t index;
  size_t count;
  if (!history) return;

  count = tinymock_cmeta_recorded_limit(history->call_count);
  for (index = 0; index < count; ++index)
    tinymock_cmeta_call_clear(&history->calls[index]);
  memset(history, 0, sizeof(*history));
}

void tinymock_cmeta_history_reset(tinymock_cmeta_history *history,
                                  const cmeta_function_desc *function) {
  if (!history) return;
  tinymock_cmeta_history_destroy(history);
  tinymock_cmeta_history_init(history, function);
}

bool tinymock_cmeta_history_record(
    tinymock_cmeta_history *history,
    const cmeta_function_desc *function,
    size_t argc,
    const tinymock_cmeta_arg_view *args) {
  if (!history || !function || !cmeta_function_desc_valid(function) ||
      argc != function->param_count || argc > TINYMOCk_MAX_ARGS ||
      (argc != 0u && !args))
    return false;

  if (history->function &&
      !tinymock_cmeta_function_equal(history->function, function))
    return false;
  history->function = function;

  return tinymock_cmeta_history_record_admitted(history, argc, args);
}

bool tinymock_cmeta_history_record_admitted(tinymock_cmeta_history *history,
    size_t argc, const tinymock_cmeta_arg_view *args) {
  tinymock_cmeta_recorded_call *call;
  const cmeta_function_desc *function;
  size_t index;
  if (!history || !(function = history->function) || argc != function->param_count ||
      argc > TINYMOCk_MAX_ARGS || (argc && !args) || history->call_count == SIZE_MAX)
    return false;

  if (history->call_count >= TINYMOCk_MAX_CALLS) {
    ++history->call_count;
    return true;
  }

  call = &history->calls[history->call_count];
  memset(call, 0, sizeof(*call));
  call->argc = argc;

  for (index = 0; index < argc; ++index) {
    if (!tinymock_cmeta_history_copy_arg(
            &call->args[index], function->params[index].type,
            &args[index])) {
      tinymock_cmeta_call_clear(call);
      return false;
    }
  }

  ++history->call_count;
  return true;
}

size_t tinymock_cmeta_history_call_count(const tinymock_cmeta_history *history) {
  return history ? history->call_count : 0u;
}

const cmeta_param_desc *tinymock_cmeta_history_param(
    const tinymock_cmeta_history *history, size_t param_index) {
  if (!history || !history->function)
    return NULL;
  return cmeta_function_param(history->function, param_index);
}

const cmeta_param_desc *tinymock_cmeta_history_find_param(
    const tinymock_cmeta_history *history, const char *param_name) {
  if (!history || !history->function)
    return NULL;
  return cmeta_function_find_param(history->function, param_name);
}

const void *tinymock_cmeta_history_arg(
    const tinymock_cmeta_history *history,
    size_t call_index,
    size_t param_index,
    const cmeta_type_desc **out_type) {
  const tinymock_cmeta_recorded_call *call;
  const tinymock_cmeta_value *snapshot;

  if (out_type) *out_type = NULL;
  if (!history || call_index >= history->call_count ||
      call_index >= TINYMOCk_MAX_CALLS)
    return NULL;

  call = &history->calls[call_index];
  if (param_index >= call->argc)
    return NULL;

  snapshot = &call->args[param_index];
  if (!snapshot->constructed || !snapshot->data)
    return NULL;

  if (out_type) *out_type = snapshot->type;
  return snapshot->data;
}

static bool tinymock_cmeta_param_index_by_name(
    const tinymock_cmeta_history *history,
    const char *param_name,
    size_t *out_index) {
  const cmeta_param_desc *param;
  if (!history || !history->function || !param_name || !out_index)
    return false;

  param = cmeta_function_find_param(history->function, param_name);
  if (!param) return false;

  *out_index = (size_t)(param - history->function->params);
  return *out_index < history->function->param_count;
}

bool tinymock_cmeta_history_arg_equal_typed(
    const tinymock_cmeta_history *history,
    size_t call_index,
    size_t param_index,
    const void *expected) {
  const tinymock_cmeta_value *snapshot;
  const cmeta_type_traits *traits;

  if (!history || !expected || call_index >= history->call_count ||
      call_index >= TINYMOCk_MAX_CALLS ||
      param_index >= history->calls[call_index].argc)
    return false;

  snapshot = &history->calls[call_index].args[param_index];
  if (!snapshot->constructed || !snapshot->type || !snapshot->data ||
      snapshot->type->kind == CMETA_T_POINTER)
    return false;

  traits = snapshot->type->traits;
  return traits && (traits->flags & CMETA_TRAIT_EQUAL) != 0u &&
         traits->equal &&
         traits->equal(snapshot->data, expected);
}

bool tinymock_cmeta_history_arg_equal_typed_name(
    const tinymock_cmeta_history *history,
    size_t call_index,
    const char *param_name,
    const void *expected) {
  size_t index;
  if (!tinymock_cmeta_param_index_by_name(history, param_name, &index))
    return false;
  return tinymock_cmeta_history_arg_equal_typed(
      history, call_index, index, expected);
}

bool tinymock_cmeta_history_arg_pointer_equal(
    const tinymock_cmeta_history *history,
    size_t call_index,
    size_t param_index,
    const void *expected_identity) {
  const tinymock_cmeta_value *snapshot;

  if (!history || call_index >= history->call_count ||
      call_index >= TINYMOCk_MAX_CALLS ||
      param_index >= history->calls[call_index].argc)
    return false;

  snapshot = &history->calls[call_index].args[param_index];
  return snapshot->constructed &&
         snapshot->type &&
         snapshot->type->kind == CMETA_T_POINTER &&
         snapshot->has_pointer_identity &&
         snapshot->pointer_identity == expected_identity;
}

bool tinymock_cmeta_history_arg_pointer_equal_name(
    const tinymock_cmeta_history *history,
    size_t call_index,
    const char *param_name,
    const void *expected_identity) {
  size_t index;
  if (!tinymock_cmeta_param_index_by_name(history, param_name, &index))
    return false;
  return tinymock_cmeta_history_arg_pointer_equal(
      history, call_index, index, expected_identity);
}

size_t tinymock_cmeta_history_count_equal_typed(
    const tinymock_cmeta_history *history,
    size_t param_index,
    const void *expected) {
  size_t call_index;
  size_t count = 0u;
  size_t limit;

  if (!history) return 0u;
  limit = tinymock_cmeta_recorded_limit(history->call_count);
  for (call_index = 0u; call_index < limit; ++call_index)
    if (tinymock_cmeta_history_arg_equal_typed(
            history, call_index, param_index, expected))
      ++count;
  return count;
}

size_t tinymock_cmeta_history_count_equal_typed_name(
    const tinymock_cmeta_history *history,
    const char *param_name,
    const void *expected) {
  size_t index;
  if (!tinymock_cmeta_param_index_by_name(history, param_name, &index))
    return 0u;
  return tinymock_cmeta_history_count_equal_typed(
      history, index, expected);
}

void tinymock_cmeta_captor_init(tinymock_cmeta_captor *captor) {
  if (!captor) return;
  memset(captor, 0, sizeof(*captor));
}

void tinymock_cmeta_captor_reset(tinymock_cmeta_captor *captor) {
  if (!captor) return;
  tinymock_cmeta_value_reset(&captor->value);
  captor->capture_count = 0u;
}

void tinymock_cmeta_captor_destroy(tinymock_cmeta_captor *captor) {
  tinymock_cmeta_captor_reset(captor);
}

bool tinymock_cmeta_captor_capture(
    tinymock_cmeta_captor *captor,
    const tinymock_cmeta_history *history,
    size_t call_index,
    size_t param_index) {
  const tinymock_cmeta_value *snapshot;

  if (!captor || !history || call_index >= history->call_count ||
      call_index >= TINYMOCk_MAX_CALLS ||
      param_index >= history->calls[call_index].argc)
    return false;

  snapshot = &history->calls[call_index].args[param_index];
  if (!snapshot->constructed || !snapshot->type || !snapshot->data)
    return false;

  tinymock_cmeta_value_reset(&captor->value);
  if (!tinymock_cmeta_value_clone(&captor->value, snapshot))
    return false;

  ++captor->capture_count;
  return true;
}

bool tinymock_cmeta_captor_capture_name(
    tinymock_cmeta_captor *captor,
    const tinymock_cmeta_history *history,
    size_t call_index,
    const char *param_name) {
  size_t index;
  if (!tinymock_cmeta_param_index_by_name(history, param_name, &index))
    return false;
  return tinymock_cmeta_captor_capture(
      captor, history, call_index, index);
}

const cmeta_type_desc *tinymock_cmeta_captor_type(
    const tinymock_cmeta_captor *captor) {
  return captor && captor->value.constructed ? captor->value.type : NULL;
}

const void *tinymock_cmeta_captor_value(
    const tinymock_cmeta_captor *captor) {
  return captor && captor->value.constructed ? captor->value.data : NULL;
}

size_t tinymock_cmeta_captor_count(
    const tinymock_cmeta_captor *captor) {
  return captor ? captor->capture_count : 0u;
}
