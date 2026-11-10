#define TINYTEST_NO_MAIN
#include "tinymock_return.h"

#include <string.h>
#include <stdlib.h>

static bool tinymock_cmeta_return_function_ok(
    const tinymock_cmeta_return *state,
    const cmeta_function_desc *function) {
  return state && function &&
         cmeta_function_desc_valid(function) &&
         function->return_type &&
         function->return_type->kind != CMETA_T_VOID &&
         (!state->function ||
          tinymock_cmeta_function_equal(state->function, function));
}

static bool tinymock_cmeta_return_semantics_supported(
    const cmeta_function_desc *function) {
  cmeta_result_cleanup cleanup;

  if (!function)
    return false;

  if (cmeta_result_cleanup_classify(function->result_flags, &cleanup) != CMETA_OK)
    return false;
  return cleanup == CMETA_RESULT_CLEANUP_VALUE ||
         (cleanup == CMETA_RESULT_CLEANUP_BORROW && function->return_type->kind == CMETA_T_POINTER);
}

void tinymock_cmeta_return_init(
    tinymock_cmeta_return *state,
    const cmeta_function_desc *function) {
  if (!state) return;
  memset(state, 0, sizeof(*state));
  state->function = cmeta_function_desc_valid(function) ? function : NULL;
  state->object = (cmeta_object_ref)CMETA_OBJECT_REF_INIT;
}

void tinymock_cmeta_return_destroy(tinymock_cmeta_return *state) {
  if (!state) return;
  tinymock_cmeta_return_clear(state);
  memset(state, 0, sizeof(*state));
}

void tinymock_cmeta_return_reset(
    tinymock_cmeta_return *state,
    const cmeta_function_desc *function) {
  if (!state) return;
  tinymock_cmeta_return_destroy(state);
  tinymock_cmeta_return_init(state, function);
}

bool tinymock_cmeta_return_set(
    tinymock_cmeta_return *state,
    const cmeta_function_desc *function,
    const void *value) {
  tinymock_cmeta_value prepared = {0};
  if (!value || !tinymock_cmeta_return_function_ok(state, function) ||
      !tinymock_cmeta_return_semantics_supported(function))
    return false;

  if (!tinymock_cmeta_value_copy_admitted(&prepared, function->return_type, value))
    return false;
  tinymock_cmeta_return_clear(state);
  state->value = prepared;
  state->function = function;
  state->enabled = true;
  return true;
}

void tinymock_cmeta_return_clear(tinymock_cmeta_return *state) {
  if (!state) return;
  if (state->object.lifetime != CMETA_OBJECT_LIFETIME_NONE) {
    cmeta_cleanup obligation = CMETA_CLEANUP_INIT;
    if (cmeta_cleanup_object_result(&obligation, state->function->result_flags,
                                    &state->object) != CMETA_OK) abort();
    cmeta_cleanup_run(&obligation);
  }
  tinymock_cmeta_value_reset(&state->value);
  state->enabled = false;
}

bool tinymock_cmeta_return_enabled(const tinymock_cmeta_return *state) {
  return state && state->enabled && state->value.constructed;
}

bool tinymock_cmeta_return_write(
    tinymock_cmeta_return *state,
    const cmeta_function_desc *function,
    void *destination) {
  if (!destination || !tinymock_cmeta_return_enabled(state) ||
      !tinymock_cmeta_return_function_ok(state, function) ||
      !cmeta_type_equal(state->value.type, function->return_type))
    return false;

  return tinymock_cmeta_return_write_admitted(state, destination);
}

bool tinymock_cmeta_return_set_object(tinymock_cmeta_return *state,
    const cmeta_function_abi_desc *abi, const tinymock_cmeta_arg_view *value,
    cmeta_object_ref *object) {
  tinymock_cmeta_value prepared = {0};
  cmeta_object_ref owner = CMETA_OBJECT_REF_INIT;
  cmeta_result_cleanup cleanup;
  if (!abi || !cmeta_function_abi_desc_valid(abi) ||
      abi->return_carrier != CMETA_ABI_OBJECT_POINTER ||
      !tinymock_cmeta_return_function_ok(state, abi->function) ||
      !value || !value->address || !value->has_object_pointer_identity ||
      !cmeta_object_ref_valid(object) || object == &state->object ||
      value->object_pointer_identity != object->object || !object->object ||
      !cmeta_type_equal(abi->function->return_type->pointee, object->data->storage_type) ||
      cmeta_result_cleanup_classify(abi->function->result_flags, &cleanup) != CMETA_OK)
    return false;
  if ((cleanup == CMETA_RESULT_CLEANUP_DESTROY && object->lifetime != CMETA_OBJECT_LIFETIME_OWNED) ||
      (cleanup == CMETA_RESULT_CLEANUP_RELEASE && object->lifetime != CMETA_OBJECT_LIFETIME_SHARED) ||
      (cleanup != CMETA_RESULT_CLEANUP_DESTROY && cleanup != CMETA_RESULT_CLEANUP_RELEASE))
    return false;
  if (!tinymock_cmeta_value_copy_admitted(&prepared, abi->function->return_type, value->address))
    return false;
  owner = *object;
  if (cleanup == CMETA_RESULT_CLEANUP_RELEASE) {
    owner.lifetime = CMETA_OBJECT_LIFETIME_BORROWED;
    owner.lifecycle = NULL;
    if (cmeta_object_share(&owner, object->lifecycle) != CMETA_OK) {
      tinymock_cmeta_value_reset(&prepared);
      return false;
    }
  }
  tinymock_cmeta_return_clear(state);
  state->function = abi->function;
  state->value = prepared;
  state->object = owner;
  state->enabled = true;
  if (cleanup == CMETA_RESULT_CLEANUP_DESTROY)
    *object = (cmeta_object_ref)CMETA_OBJECT_REF_INIT;
  return true;
}

bool tinymock_cmeta_return_write_admitted(tinymock_cmeta_return *state, void *destination) {
  if (!destination || !tinymock_cmeta_return_enabled(state) || !state->function ||
      destination == state->value.data) return false;
  if (state->value.lifecycle.ops) {
    if (cmeta_lifecycle_init(&state->value.lifecycle, destination) != CMETA_OK) return false;
    if (cmeta_lifecycle_move(&state->value.lifecycle, destination, state->value.data) != CMETA_OK)
      abort();
    tinymock_cmeta_return_clear(state);
    return true;
  }
  if (state->object.lifetime == CMETA_OBJECT_LIFETIME_SHARED) {
    /* The exact pointer bytes remain in the script. Only canonical retain may
     * create the caller's additional ownership obligation. */
    if (state->object.lifecycle->retain(state->object.lifecycle->context,
                                        state->object.object) != CMETA_OK) return false;
    memcpy(destination, state->value.data, state->value.type->size);
    return true;
  }
  if (!tinymock_cmeta_value_write(&state->value, destination, false)) return false;
  if (state->object.lifetime == CMETA_OBJECT_LIFETIME_OWNED) {
    state->object = (cmeta_object_ref)CMETA_OBJECT_REF_INIT;
    tinymock_cmeta_return_clear(state);
  }
  return true;
}

bool tinymock_cmeta_return_take_data(tinymock_cmeta_return *state,
    const cmeta_function_desc *function, const cmeta_lifecycle_binding *binding,
    void *source) {
  tinymock_cmeta_value prepared = {0};
  if (!binding || !binding->ops || !source ||
      !tinymock_cmeta_return_function_ok(state, function) ||
      (function->result_flags & CMETA_RESULT_CLASS_MASK) != CMETA_RESULT_VALUE ||
      !cmeta_type_equal(binding->ops->storage_type, function->return_type) ||
      !tinymock_cmeta_value_take_data(&prepared, binding, source)) return false;
  tinymock_cmeta_return_clear(state);
  state->function = function;
  state->value = prepared;
  state->enabled = true;
  return true;
}
