#define TINYTEST_NO_MAIN
#include "tinymock_value.h"

#include <stdlib.h>
#include <string.h>

typedef union tinymock_cmeta_max_align {
  long double as_long_double;
  long long as_long_long;
  void *as_pointer;
} tinymock_cmeta_max_align;

void tinymock_cmeta_value_init(tinymock_cmeta_value *value) {
  if (!value) return;
  memset(value, 0, sizeof(*value));
}

static void tinymock_cmeta_traits_release(void *authority, void *resource) {
  const cmeta_type_traits *traits = (const cmeta_type_traits *)authority;
  traits->destroy(resource);
}

void tinymock_cmeta_value_reset(tinymock_cmeta_value *value) {
  const cmeta_type_traits *traits;
  cmeta_cleanup obligation = CMETA_CLEANUP_INIT;
  if (!value) return;

  traits = value->type ? value->type->traits : NULL;
  if (value->constructed && value->lifecycle.ops) {
    if (cmeta_cleanup_data(&obligation, &value->lifecycle, value->data) != CMETA_OK)
      abort();
  } else if (value->constructed && value->data && value->type &&
      value->type->kind != CMETA_T_POINTER && traits &&
      (traits->flags & CMETA_TRAIT_TRIVIAL_DESTROY) == 0u &&
      (traits->flags & CMETA_TRAIT_DESTROY) != 0u && traits->destroy) {
    if (cmeta_cleanup_arm(&obligation, tinymock_cmeta_traits_release,
                         (void *)traits, value->data) != CMETA_OK) abort();
  }
  cmeta_cleanup_run(&obligation);
  free(value->allocation);
  memset(value, 0, sizeof(*value));
}

static bool tinymock_cmeta_value_allocate(
    tinymock_cmeta_value *value,
    const cmeta_type_desc *type) {
  if (!value || value->allocation || value->constructed || !type ||
      type->size == 0u || type->size > TINYMOCk_MAX_VALUE_BYTES)
    return false;
  if (type->align == 0u ||
      type->align > _Alignof(tinymock_cmeta_max_align))
    return false;

  value->allocation = malloc(type->size);
  if (!value->allocation) return false;
  value->data = value->allocation;
  value->type = type;
  return true;
}

bool tinymock_cmeta_value_copy(
    tinymock_cmeta_value *value,
    const cmeta_type_desc *type,
    const void *source) {
  return cmeta_type_desc_valid(type) &&
         tinymock_cmeta_value_copy_admitted(value, type, source);
}

bool tinymock_cmeta_value_copy_admitted(tinymock_cmeta_value *value,
    const cmeta_type_desc *type, const void *source) {
  const cmeta_type_traits *traits;

  if (!value || !source || !tinymock_cmeta_value_allocate(value, type))
    return false;

  traits = type->traits;

  if (type->kind == CMETA_T_POINTER) {
    memcpy(value->data, source, type->size);
    value->constructed = true;
    return true;
  }

  if (traits &&
      (traits->flags & (CMETA_TRAIT_TRIVIAL_COPY |
                        CMETA_TRAIT_TRIVIAL_DESTROY)) ==
          (CMETA_TRAIT_TRIVIAL_COPY | CMETA_TRAIT_TRIVIAL_DESTROY)) {
    memcpy(value->data, source, type->size);
    value->constructed = true;
    return true;
  }

  if (cmeta_type_require_traits(
          type, CMETA_TRAIT_COPY | CMETA_TRAIT_DESTROY) == CMETA_OK &&
      traits && traits->copy_construct && traits->destroy &&
      traits->copy_construct(value->data, source)) {
    value->constructed = true;
    return true;
  }

  tinymock_cmeta_value_reset(value);
  return false;
}

bool tinymock_cmeta_value_copy_pointer(
    tinymock_cmeta_value *value,
    const cmeta_type_desc *type,
    const void *source,
    const void *pointer_identity) {
  if (!type || type->kind != CMETA_T_POINTER)
    return false;
  if (!tinymock_cmeta_value_copy(value, type, source))
    return false;

  value->has_pointer_identity = true;
  value->pointer_identity = pointer_identity;
  return true;
}

bool tinymock_cmeta_value_clone(
    tinymock_cmeta_value *value,
    const tinymock_cmeta_value *source) {
  if (!value || !source || !source->constructed ||
      !source->type || !source->data)
    return false;

  if (source->lifecycle.ops) return false; /* A moved Data slot has no copy authority. */
  if (!tinymock_cmeta_value_copy_admitted(value, source->type, source->data)) return false;
  value->has_pointer_identity = source->has_pointer_identity;
  value->pointer_identity = source->pointer_identity;
  return true;
}

bool tinymock_cmeta_value_take_data(tinymock_cmeta_value *value,
    const cmeta_lifecycle_binding *binding, void *source) {
  if (!binding || !binding->ops || !binding->ops->move || !source ||
      !tinymock_cmeta_value_allocate(value, binding->ops->storage_type)) return false;
  value->lifecycle = *binding;
  if (cmeta_lifecycle_init(binding, value->data) != CMETA_OK) {
    tinymock_cmeta_value_reset(value);
    return false;
  }
  value->constructed = true;
  if (cmeta_lifecycle_move(binding, value->data, source) != CMETA_OK) {
    tinymock_cmeta_value_reset(value);
    return false;
  }
  return true;
}

bool tinymock_cmeta_value_can_move(const tinymock_cmeta_value *value) {
  const cmeta_type_traits *traits;
  if (!value || !value->constructed || !value->type || value->lifecycle.ops) return false;
  traits = value->type->traits;
  return value->type->kind == CMETA_T_POINTER || (traits &&
      (((traits->flags & (CMETA_TRAIT_TRIVIAL_COPY | CMETA_TRAIT_TRIVIAL_DESTROY)) ==
        (CMETA_TRAIT_TRIVIAL_COPY | CMETA_TRAIT_TRIVIAL_DESTROY)) ||
       ((traits->flags & CMETA_TRAIT_MOVE) && traits->move_construct)));
}

bool tinymock_cmeta_value_move(tinymock_cmeta_value *value, void *destination) {
  const cmeta_type_traits *traits;
  if (!destination || !tinymock_cmeta_value_can_move(value) || destination == value->data) return false;
  traits = value->type->traits;
  if (value->type->kind == CMETA_T_POINTER ||
      (traits->flags & (CMETA_TRAIT_TRIVIAL_COPY | CMETA_TRAIT_TRIVIAL_DESTROY)) ==
        (CMETA_TRAIT_TRIVIAL_COPY | CMETA_TRAIT_TRIVIAL_DESTROY)) {
    memcpy(destination, value->data, value->type->size);
    value->constructed = false;
  } else {
    traits->move_construct(destination, value->data);
  }
  tinymock_cmeta_value_reset(value);
  return true;
}

bool tinymock_cmeta_value_write(
    const tinymock_cmeta_value *value,
    void *destination,
    bool replace_existing) {
  const cmeta_type_traits *traits;
  const cmeta_type_desc *type;

  if (!value || value->lifecycle.ops || !value->constructed || !value->data ||
      !destination || destination == value->data || !value->type)
    return false;

  type = value->type;
  traits = type->traits;

  if (type->kind == CMETA_T_POINTER) {
    memcpy(destination, value->data, type->size);
    return true;
  }

  if (traits &&
      (traits->flags & (CMETA_TRAIT_TRIVIAL_COPY |
                        CMETA_TRAIT_TRIVIAL_DESTROY)) ==
          (CMETA_TRAIT_TRIVIAL_COPY | CMETA_TRAIT_TRIVIAL_DESTROY)) {
    memcpy(destination, value->data, type->size);
    return true;
  }

  if (cmeta_type_require_traits(
          type, CMETA_TRAIT_COPY | CMETA_TRAIT_DESTROY) != CMETA_OK ||
      !traits || !traits->copy_construct || !traits->destroy)
    return false;

  if (replace_existing) {
    tinymock_cmeta_value prepared = {0};
    if (!tinymock_cmeta_value_can_move(value) ||
        !tinymock_cmeta_value_clone(&prepared, value)) return false;
    traits->destroy(destination);
    if (!tinymock_cmeta_value_move(&prepared, destination)) abort();
    return true;
  }

  return traits->copy_construct(destination, value->data);
}
