#ifndef TINYMOCK_RETURN_H
#define TINYMOCK_RETURN_H

#include "tinymock_history.h"
#include <cmeta/object_scope.h>

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct tinymock_cmeta_return {
  const cmeta_function_desc *function;
  tinymock_cmeta_value value;
  bool enabled;
  cmeta_object_ref object;
} tinymock_cmeta_return;

void tinymock_cmeta_return_init(
    tinymock_cmeta_return *state,
    const cmeta_function_desc *function);
void tinymock_cmeta_return_reset(
    tinymock_cmeta_return *state,
    const cmeta_function_desc *function);
void tinymock_cmeta_return_destroy(tinymock_cmeta_return *state);

bool tinymock_cmeta_return_set(
    tinymock_cmeta_return *state,
    const cmeta_function_desc *function,
    const void *value);

void tinymock_cmeta_return_clear(tinymock_cmeta_return *state);
bool tinymock_cmeta_return_enabled(const tinymock_cmeta_return *state);

bool tinymock_cmeta_return_write(
    tinymock_cmeta_return *state,
    const cmeta_function_desc *function,
    void *destination);

/* Exact object-pointer carrier plus an explicit canonical ownership authority.
 * OWNED consumes object on success; SHARED retains its authority on success.
 * Rejection preserves source and existing script. Views/providers stay borrowed. */
bool tinymock_cmeta_return_set_object(tinymock_cmeta_return *state,
    const cmeta_function_abi_desc *abi, const tinymock_cmeta_arg_view *value,
    cmeta_object_ref *object);

/* Explicit one-shot VALUE script for an admitted movable Data lifecycle.
 * Success transfers source payload and leaves source semantic zero. */
bool tinymock_cmeta_return_take_data(tinymock_cmeta_return *state,
    const cmeta_function_desc *function, const cmeta_lifecycle_binding *binding,
    void *source);

/* Consumes the immutable function admitted by init/reset and scripted values.
 * OWNED writes once; SHARED transfers one retained reference to the caller. */
bool tinymock_cmeta_return_write_admitted(tinymock_cmeta_return *state, void *destination);

#ifdef __cplusplus
}
#endif

#endif /* TINYMOCK_RETURN_H */
