#ifndef TINYMOCK_VALUE_H
#define TINYMOCK_VALUE_H

#include <cmeta/function.h>
#include <cmeta/cleanup.h>

#include <stdbool.h>

#ifndef TINYMOCk_MAX_VALUE_BYTES
#define TINYMOCk_MAX_VALUE_BYTES (64u * 1024u)
#endif

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Canonical reflected value storage used by TinyMock behavior state.
 *
 * CMeta owns type and lifecycle semantics. TinyMock only owns the bounded
 * storage needed by history/capture/return/output behavior. Pointer identity
 * is an optional exact-ABI observation supplied at the reflected dispatch
 * boundary; it is not inferred from raw bytes or pointer spelling.
 */
typedef struct tinymock_cmeta_value {
  const cmeta_type_desc *type;
  void *allocation;
  void *data;
  bool constructed;
  bool has_pointer_identity;
  const void *pointer_identity;
  cmeta_lifecycle_binding lifecycle;
} tinymock_cmeta_value;

typedef struct tinymock_cmeta_arg_view {
  const void *address;
  bool has_object_pointer_identity;
  const void *object_pointer_identity;
} tinymock_cmeta_arg_view;

void tinymock_cmeta_value_init(tinymock_cmeta_value *value);
void tinymock_cmeta_value_reset(tinymock_cmeta_value *value);

bool tinymock_cmeta_value_copy(
    tinymock_cmeta_value *value,
    const cmeta_type_desc *type,
    const void *source);

/* Only a previously validated immutable type may enter this path. The slot
 * must be empty. Metadata and callbacks remain borrowed until reset. */
bool tinymock_cmeta_value_copy_admitted(tinymock_cmeta_value *value,
    const cmeta_type_desc *type, const void *source);

/* Move an admitted Data value into an empty bounded slot. Failure preserves
 * source ownership; success leaves it semantic zero. No provider is retained. */
bool tinymock_cmeta_value_take_data(tinymock_cmeta_value *value,
    const cmeta_lifecycle_binding *binding, void *source);

/* Prepare before mutating a destination. Commit requires an admitted no-fail
 * move/trivial copy and uninitialized destination storage after any teardown. */
bool tinymock_cmeta_value_can_move(const tinymock_cmeta_value *value);
bool tinymock_cmeta_value_move(tinymock_cmeta_value *value, void *destination);

bool tinymock_cmeta_value_copy_pointer(
    tinymock_cmeta_value *value,
    const cmeta_type_desc *type,
    const void *source,
    const void *pointer_identity);

bool tinymock_cmeta_value_clone(
    tinymock_cmeta_value *value,
    const tinymock_cmeta_value *source);

bool tinymock_cmeta_value_write(
    const tinymock_cmeta_value *value,
    void *destination,
    bool replace_existing);

#ifdef __cplusplus
}
#endif

#endif /* TINYMOCK_VALUE_H */
