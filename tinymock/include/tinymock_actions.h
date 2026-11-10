#ifndef TINYMOCK_ACTIONS_H
#define TINYMOCK_ACTIONS_H

#include "tinymock_history.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct tinymock_cmeta_output_action {
  bool enabled;
  size_t param_index;
  tinymock_cmeta_value value;
} tinymock_cmeta_output_action;

typedef struct tinymock_cmeta_actions {
  const cmeta_function_desc *function;
  tinymock_cmeta_output_action outputs[TINYMOCk_MAX_ARGS];
} tinymock_cmeta_actions;

void tinymock_cmeta_actions_init(
    tinymock_cmeta_actions *actions,
    const cmeta_function_desc *function);
void tinymock_cmeta_actions_reset(
    tinymock_cmeta_actions *actions,
    const cmeta_function_desc *function);
void tinymock_cmeta_actions_destroy(tinymock_cmeta_actions *actions);

bool tinymock_cmeta_actions_set_output(
    tinymock_cmeta_actions *actions,
    const cmeta_function_desc *function,
    size_t param_index,
    const void *value);

bool tinymock_cmeta_actions_set_output_name(
    tinymock_cmeta_actions *actions,
    const cmeta_function_desc *function,
    const char *param_name,
    const void *value);

bool tinymock_cmeta_actions_clear_output(
    tinymock_cmeta_actions *actions,
    size_t param_index);

bool tinymock_cmeta_actions_clear_output_name(
    tinymock_cmeta_actions *actions,
    const char *param_name);

bool tinymock_cmeta_actions_apply(
    tinymock_cmeta_actions *actions,
    const cmeta_function_desc *function,
    size_t argc,
    const tinymock_cmeta_arg_view *args);

/* Uses only the immutable function admitted by init/reset. Prepares every
 * fallible copy before any output mutation; commit uses no-fail native moves. */
bool tinymock_cmeta_actions_apply_admitted(tinymock_cmeta_actions *actions,
    size_t argc, const tinymock_cmeta_arg_view *args);

#ifdef __cplusplus
}
#endif

#endif /* TINYMOCK_ACTIONS_H */
