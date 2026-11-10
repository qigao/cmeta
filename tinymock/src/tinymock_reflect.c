#define TINYTEST_NO_MAIN
#include "tinymock_reflect.h"
#include "tinymock_history.h"

#include <string.h>
#include <stdint.h>

typedef struct tinymock_reflect_walk {
  tinymock_reflect_limits limits;
  size_t nodes;
  size_t bytes;
  size_t path_size;
  tinymock_reflect_result *result;
} tinymock_reflect_walk;

static void tinymock_reflect_clear(tinymock_reflect_result *result) {
  if (result) {
    result->equal = false;
    result->path[0] = '\0';
  }
}

static cmeta_status tinymock_reflect_path(tinymock_reflect_walk *walk, const char *name) {
  const size_t separator = walk->path_size != 0u;
  const size_t available = sizeof(walk->result->path) - walk->path_size - separator;
  size_t length = 0u;
  if (name == NULL || name[0] == '\0') return CMETA_INVALID_ARGUMENT;
  while (length < available && name[length] != '\0') ++length;
  if (length == available) return CMETA_CAPACITY_EXCEEDED;
  if (separator) walk->result->path[walk->path_size++] = '.';
  memcpy(walk->result->path + walk->path_size, name, length + 1u);
  walk->path_size += length;
  return CMETA_OK;
}

static cmeta_status tinymock_reflect_bytes(tinymock_reflect_walk *walk, size_t bytes) {
  if (bytes > walk->limits.max_bytes - walk->bytes) return CMETA_CAPACITY_EXCEEDED;
  walk->bytes += bytes;
  return CMETA_OK;
}

static cmeta_status tinymock_reflect_visit(tinymock_reflect_walk *walk,
    const cmeta_data_desc *data, const void *actual, const void *expected, size_t depth);

static cmeta_status tinymock_reflect_struct(tinymock_reflect_walk *walk,
    const cmeta_data_desc *data, const void *actual, const void *expected, size_t depth) {
  const cmeta_data_struct_shape *shape = (const cmeta_data_struct_shape *)data->shape;
  size_t index;
  if (shape->field_count > walk->limits.max_nodes - walk->nodes)
    return CMETA_CAPACITY_EXCEEDED;
  for (index = 0u; index < shape->field_count; ++index) {
    const cmeta_data_field_desc *field = &shape->fields[index];
    const cmeta_field_desc *layout = cmeta_struct_find_field(shape->layout, field->name);
    const cmeta_type_desc *type;
    const size_t parent_path = walk->path_size;
    cmeta_status status;
    if (field->offset == CMETA_FIELD_DYNAMIC_OFFSET) return CMETA_TRAIT_MISSING;
    if (field->value->struct_size < offsetof(cmeta_data_desc, shape) + sizeof(field->value->shape))
      return CMETA_INVALID_ARGUMENT;
    type = field->value->storage_type;
    /* Legacy shapes permit omitted fields too, but no descriptor may project
     * outside its actual native member or reinterpret a different storage type. */
    if (!cmeta_type_desc_valid(type) || layout == NULL || layout->size != type->size ||
        layout->align != type->align || (layout->type && !cmeta_type_equal(layout->type, type)) ||
        field->offset > data->storage_type->size ||
        type->size > data->storage_type->size - field->offset ||
        field->offset % type->align != 0u || data->storage_type->align < type->align)
      return CMETA_TYPE_MISMATCH;
    status = tinymock_reflect_path(walk, field->name);
    if (status != CMETA_OK) return status;
    status = tinymock_reflect_visit(walk, field->value,
        (const unsigned char *)actual + field->offset,
        (const unsigned char *)expected + field->offset, depth + 1u);
    if (status != CMETA_OK || !walk->result->equal) return status;
    walk->path_size = parent_path;
    walk->result->path[parent_path] = '\0';
  }
  return CMETA_OK;
}

static cmeta_status tinymock_reflect_buffer(tinymock_reflect_walk *walk,
    const cmeta_data_desc *data, const void *actual, const void *expected) {
  const unsigned char *left, *right;
  size_t left_size, right_size;
  cmeta_status status;
  if (data->struct_size < offsetof(cmeta_data_desc, buffer_ops) + sizeof(data->buffer_ops) ||
      data->buffer_ops == NULL) return CMETA_TRAIT_MISSING;
  status = cmeta_data_buffer_read(data, actual, walk->limits.max_bytes - walk->bytes,
      &left, &left_size);
  if (status != CMETA_OK) return status;
  status = tinymock_reflect_bytes(walk, left_size);
  if (status != CMETA_OK) return status;
  status = cmeta_data_buffer_read(data, expected, walk->limits.max_bytes - walk->bytes,
      &right, &right_size);
  if (status != CMETA_OK) return status;
  status = tinymock_reflect_bytes(walk, right_size);
  if (status != CMETA_OK) return status;
  walk->result->equal = left_size == right_size &&
      (left_size == 0u || memcmp(left, right, left_size) == 0);
  return CMETA_OK;
}

static cmeta_status tinymock_reflect_enum(tinymock_reflect_walk *walk,
    const cmeta_data_desc *data, const void *actual, const void *expected) {
  cmeta_status status;
  if (cmeta_data_enum_bits_ops_of(data) != NULL) {
    uint64_t left, right;
    status = cmeta_data_enum_read_bits(data, actual, &left);
    if (status != CMETA_OK) return status;
    status = cmeta_data_enum_read_bits(data, expected, &right);
    if (status != CMETA_OK) return status;
    walk->result->equal = left == right;
  } else {
    int64_t left, right;
    status = cmeta_data_enum_read(data, actual, &left);
    if (status != CMETA_OK) return status;
    status = cmeta_data_enum_read(data, expected, &right);
    if (status != CMETA_OK) return status;
    walk->result->equal = left == right;
  }
  return CMETA_OK;
}

static cmeta_status tinymock_reflect_visit(tinymock_reflect_walk *walk,
    const cmeta_data_desc *data, const void *actual, const void *expected, size_t depth) {
  cmeta_status status;
  if (depth > walk->limits.max_depth || walk->nodes == walk->limits.max_nodes)
    return CMETA_CAPACITY_EXCEEDED;
  ++walk->nodes;
  if (data != NULL && data->struct_size >= offsetof(cmeta_data_desc, shape) + sizeof(data->shape) &&
      data->kind == CMETA_DATA_STRUCT && data->shape != NULL) {
    const cmeta_data_struct_shape *shape = (const cmeta_data_struct_shape *)data->shape;
    if (shape->field_count > walk->limits.max_nodes - walk->nodes ||
        (shape->layout && shape->layout->field_count > walk->limits.max_nodes))
      return CMETA_CAPACITY_EXCEEDED;
  }
  if (!cmeta_data_desc_valid(data) || !cmeta_type_desc_valid(data->storage_type))
    return CMETA_INVALID_ARGUMENT;
  if ((uintptr_t)actual % data->storage_type->align != 0u ||
      (uintptr_t)expected % data->storage_type->align != 0u)
    return CMETA_INVALID_ARGUMENT;
  if (data->kind == CMETA_DATA_STRING || data->kind == CMETA_DATA_BYTES)
    return tinymock_reflect_buffer(walk, data, actual, expected);
  if (data->storage_type->kind == CMETA_T_POINTER) return CMETA_TRAIT_MISSING;
  if (data->kind == CMETA_DATA_STRUCT)
    return tinymock_reflect_struct(walk, data, actual, expected, depth);
  status = tinymock_reflect_bytes(walk, data->storage_type->size);
  if (status != CMETA_OK) return status;
  status = tinymock_reflect_bytes(walk, data->storage_type->size);
  if (status != CMETA_OK) return status;
  if (data->kind == CMETA_DATA_ENUM)
    return tinymock_reflect_enum(walk, data, actual, expected);
  switch (data->kind) {
  case CMETA_DATA_BOOL:
  case CMETA_DATA_SINT:
  case CMETA_DATA_UINT:
  case CMETA_DATA_FLOAT:
  case CMETA_DATA_CUSTOM:
    status = cmeta_type_require_traits(data->storage_type, CMETA_TRAIT_EQUAL);
    if (status != CMETA_OK) return status;
    walk->result->equal = data->storage_type->traits->equal(actual, expected);
    return CMETA_OK;
  default:
    return CMETA_TRAIT_MISSING;
  }
}

static cmeta_status tinymock_reflect_match(const cmeta_data_desc *data,
    const void *actual, const void *expected, const char *root,
    const tinymock_reflect_limits *limits, tinymock_reflect_result *result) {
  const tinymock_reflect_limits defaults = {TINYMOCK_REFLECT_MAX_DEPTH,
      TINYMOCK_REFLECT_DEFAULT_NODES, TINYMOCK_REFLECT_DEFAULT_BYTES};
  tinymock_reflect_walk walk = {0};
  cmeta_status status;
  tinymock_reflect_clear(result);
  if (!result || !data || !actual || !expected) return CMETA_INVALID_ARGUMENT;
  walk.limits = limits ? *limits : defaults;
  walk.result = result;
  if (walk.limits.max_depth == 0u || walk.limits.max_depth > TINYMOCK_REFLECT_MAX_DEPTH ||
      walk.limits.max_nodes == 0u || walk.limits.max_bytes == 0u)
    return CMETA_INVALID_ARGUMENT;
  status = tinymock_reflect_path(&walk, root);
  if (status != CMETA_OK) return status;
  result->equal = true;
  status = tinymock_reflect_visit(&walk, data, actual, expected, 1u);
  if (status != CMETA_OK) tinymock_reflect_clear(result);
  else if (result->equal) result->path[0] = '\0';
  return status;
}

cmeta_status tinymock_cmeta_data_match(const cmeta_data_desc *data,
    const void *actual, const void *expected, const tinymock_reflect_limits *limits,
    tinymock_reflect_result *result) {
  return tinymock_reflect_match(data, actual, expected, "$", limits, result);
}

cmeta_status tinymock_cmeta_history_arg_match_data(const tinymock_cmeta_history *history,
    size_t call_index, size_t param_index, const cmeta_data_desc *data, const void *expected,
    const tinymock_reflect_limits *limits, tinymock_reflect_result *result) {
  const cmeta_type_desc *type = NULL;
  const void *actual = tinymock_cmeta_history_arg(history, call_index, param_index, &type);
  const cmeta_param_desc *param = tinymock_cmeta_history_param(history, param_index);
  tinymock_reflect_clear(result);
  if (!result || !actual || !param || !cmeta_data_desc_valid(data)) return CMETA_INVALID_ARGUMENT;
  if (type->kind == CMETA_T_POINTER || !cmeta_type_equal(type, data->storage_type) ||
      type->size != data->storage_type->size || type->align != data->storage_type->align)
    return CMETA_TYPE_MISMATCH;
  return tinymock_reflect_match(data, actual, expected, param->name, limits, result);
}

cmeta_status tinymock_cmeta_history_arg_match_data_name(const tinymock_cmeta_history *history,
    size_t call_index, const char *param_name, const cmeta_data_desc *data, const void *expected,
    const tinymock_reflect_limits *limits, tinymock_reflect_result *result) {
  const cmeta_param_desc *param = tinymock_cmeta_history_find_param(history, param_name);
  tinymock_reflect_clear(result);
  if (!param) return CMETA_INVALID_ARGUMENT;
  return tinymock_cmeta_history_arg_match_data(history, call_index,
      (size_t)(param - history->function->params), data, expected, limits, result);
}
