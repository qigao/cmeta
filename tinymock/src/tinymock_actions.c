#define TINYTEST_NO_MAIN
#include "tinymock_actions.h"

#include <string.h>
#include <stdlib.h>

static bool tinymock_cmeta_actions_function_ok(
    const tinymock_cmeta_actions *actions,
    const cmeta_function_desc *function) {
  return actions && function &&
         cmeta_function_desc_valid(function) &&
         (!actions->function ||
          tinymock_cmeta_function_equal(actions->function, function));
}

static bool tinymock_cmeta_output_param_valid(
    const cmeta_param_desc *param) {
  return param &&
         cmeta_param_desc_valid(param) &&
         cmeta_param_direction_known(param) &&
         (param->flags & CMETA_PARAM_OUT) != 0u &&
         param->type &&
         param->type->kind == CMETA_T_POINTER &&
         param->type->pointee &&
         cmeta_type_desc_valid(param->type->pointee) &&
         param->type->pointee->size != 0u;
}

static void tinymock_cmeta_output_action_reset(
    tinymock_cmeta_output_action *action) {
  if (!action) return;
  tinymock_cmeta_value_reset(&action->value);
  memset(action, 0, sizeof(*action));
}

void tinymock_cmeta_actions_init(
    tinymock_cmeta_actions *actions,
    const cmeta_function_desc *function) {
  if (!actions) return;
  memset(actions, 0, sizeof(*actions));
  actions->function = cmeta_function_desc_valid(function) ? function : NULL;
}

void tinymock_cmeta_actions_destroy(tinymock_cmeta_actions *actions) {
  size_t index;
  if (!actions) return;
  for (index = 0u; index < TINYMOCk_MAX_ARGS; ++index)
    tinymock_cmeta_output_action_reset(&actions->outputs[index]);
  memset(actions, 0, sizeof(*actions));
}

void tinymock_cmeta_actions_reset(
    tinymock_cmeta_actions *actions,
    const cmeta_function_desc *function) {
  if (!actions) return;
  tinymock_cmeta_actions_destroy(actions);
  tinymock_cmeta_actions_init(actions, function);
}

bool tinymock_cmeta_actions_set_output(
    tinymock_cmeta_actions *actions,
    const cmeta_function_desc *function,
    size_t param_index,
    const void *value) {
  const cmeta_param_desc *param;
  tinymock_cmeta_output_action *action;

  if (!value ||
      !tinymock_cmeta_actions_function_ok(actions, function) ||
      param_index >= function->param_count ||
      param_index >= TINYMOCk_MAX_ARGS)
    return false;

  param = cmeta_function_param(function, param_index);
  if (!tinymock_cmeta_output_param_valid(param))
    return false;

  action = &actions->outputs[param_index];
  tinymock_cmeta_output_action_reset(action);

  if (!tinymock_cmeta_value_copy(
          &action->value, param->type->pointee, value))
    return false;

  action->enabled = true;
  action->param_index = param_index;
  actions->function = function;
  return true;
}

bool tinymock_cmeta_actions_set_output_name(
    tinymock_cmeta_actions *actions,
    const cmeta_function_desc *function,
    const char *param_name,
    const void *value) {
  const cmeta_param_desc *param;
  size_t index;

  if (!function || !param_name) return false;
  param = cmeta_function_find_param(function, param_name);
  if (!param) return false;
  index = (size_t)(param - function->params);
  return tinymock_cmeta_actions_set_output(
      actions, function, index, value);
}

bool tinymock_cmeta_actions_clear_output(
    tinymock_cmeta_actions *actions,
    size_t param_index) {
  if (!actions || param_index >= TINYMOCk_MAX_ARGS)
    return false;
  tinymock_cmeta_output_action_reset(&actions->outputs[param_index]);
  return true;
}

bool tinymock_cmeta_actions_clear_output_name(
    tinymock_cmeta_actions *actions,
    const char *param_name) {
  const cmeta_param_desc *param;
  size_t index;

  if (!actions || !actions->function || !param_name)
    return false;
  param = cmeta_function_find_param(actions->function, param_name);
  if (!param) return false;
  index = (size_t)(param - actions->function->params);
  return tinymock_cmeta_actions_clear_output(actions, index);
}

static bool tinymock_cmeta_output_destination(
    const tinymock_cmeta_output_action *action,
    const cmeta_param_desc *param,
    const tinymock_cmeta_arg_view *arg,
    void **out_destination) {
  if (!action || !action->enabled || !param || !arg || !out_destination ||
      !arg->has_object_pointer_identity)
    return false;

  *out_destination = (void *)arg->object_pointer_identity;
  if (*out_destination != NULL)
    return true;

  return (param->flags & CMETA_PARAM_NULLABLE) != 0u;
}

bool tinymock_cmeta_actions_apply(
    tinymock_cmeta_actions *actions,
    const cmeta_function_desc *function,
    size_t argc,
    const tinymock_cmeta_arg_view *args) {
  if (!tinymock_cmeta_actions_function_ok(actions, function) ||
      argc != function->param_count ||
      argc > TINYMOCk_MAX_ARGS ||
      (argc != 0u && !args))
    return false;

  actions->function = function;
  return tinymock_cmeta_actions_apply_admitted(actions, argc, args);
}

bool tinymock_cmeta_actions_apply_admitted(tinymock_cmeta_actions *actions,
    size_t argc, const tinymock_cmeta_arg_view *args) {
  void *destinations[TINYMOCk_MAX_ARGS] = {0};
  tinymock_cmeta_value prepared[TINYMOCk_MAX_ARGS] = {0};
  const cmeta_function_desc *function;
  size_t index, previous;
  bool success = false;
  if (!actions || !(function = actions->function) || argc != function->param_count ||
      argc > TINYMOCk_MAX_ARGS || (argc && !args)) return false;

  /* Admission pass: validate every enabled output before mutating any target. */
  for (index = 0u; index < argc; ++index) {
    const tinymock_cmeta_output_action *action = &actions->outputs[index];
    const cmeta_param_desc *param;

    if (!action->enabled) continue;
    param = &function->params[index];
    if (!tinymock_cmeta_output_destination(
            action, param, &args[index], &destinations[index]))
      goto cleanup;
    if (!destinations[index]) continue;
    /* Multiple outputs to the same object have no independent commit order. */
    for (previous = 0; previous < index; ++previous)
      if (destinations[previous] == destinations[index]) goto cleanup;
    if (!tinymock_cmeta_value_can_move(&action->value) ||
        !tinymock_cmeta_value_clone(&prepared[index], &action->value) ||
        !tinymock_cmeta_value_can_move(&prepared[index])) goto cleanup;
  }

  for (index = 0u; index < argc; ++index) {
    tinymock_cmeta_output_action *action = &actions->outputs[index];
    const cmeta_param_desc *param;
    bool replace_existing;

    if (!action->enabled) continue;
    param = &function->params[index];

    if (destinations[index] == NULL) {
      /* Nullable null output: explicit safe no-op. */
      continue;
    }

    replace_existing = (param->flags & CMETA_PARAM_IN) != 0u;
    if (replace_existing && action->value.type->kind != CMETA_T_POINTER) {
      const cmeta_type_traits *traits = action->value.type->traits;
      if (!(traits->flags & CMETA_TRAIT_TRIVIAL_DESTROY)) traits->destroy(destinations[index]);
    }
    /* Capability was checked during prepare; a failure here is an invariant
     * violation, not permission to publish only a prefix of the outputs. */
    if (!tinymock_cmeta_value_move(&prepared[index], destinations[index])) abort();
  }
  success = true;
cleanup:
  for (index = 0; index < argc; ++index) tinymock_cmeta_value_reset(&prepared[index]);
  return success;
}
