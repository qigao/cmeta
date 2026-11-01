#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
  #define _POSIX_C_SOURCE 200112L
#endif

#include <cmeta/data.h>

#include <limits.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
  #include <malloc.h>
#endif

#define CMETA_FIELD_END(type, member) (offsetof(type, member) + sizeof(((type *)0)->member))
#define CMETA_DATA_DESC_PREFIX_SIZE CMETA_FIELD_END(cmeta_data_desc, shape)
#define CMETA_DATA_DESC_BUFFER_OPS_SIZE CMETA_FIELD_END(cmeta_data_desc, buffer_ops)
#define CMETA_DATA_DESC_ENUM_OPS_SIZE CMETA_FIELD_END(cmeta_data_desc, enum_ops)
#define CMETA_DATA_DESC_VARIANT_OPS_SIZE CMETA_FIELD_END(cmeta_data_desc, variant_ops)
#define CMETA_DATA_DESC_FIXED_OPS_SIZE CMETA_FIELD_END(cmeta_data_desc, fixed_ops)
#define CMETA_DATA_DESC_ENUM_BITS_OPS_SIZE CMETA_FIELD_END(cmeta_data_desc, enum_bits_ops)
#define CMETA_DATA_ENUM_BITS_OPS_PREFIX_SIZE CMETA_FIELD_END(cmeta_data_enum_bits_ops, restore_zero)
#define CMETA_DATA_BUFFER_OPS_PREFIX_SIZE CMETA_FIELD_END(cmeta_data_buffer_ops, move)
#define CMETA_DATA_BUFFER_OPS_READ_SIZE CMETA_FIELD_END(cmeta_data_buffer_ops, read)
#define CMETA_DATA_ENUM_OPS_PREFIX_SIZE CMETA_FIELD_END(cmeta_data_enum_ops, restore_zero)
#define CMETA_DATA_VARIANT_OPS_PREFIX_SIZE CMETA_FIELD_END(cmeta_data_variant_ops, restore_zero)
#define CMETA_DATA_FIXED_OPS_PREFIX_SIZE CMETA_FIELD_END(cmeta_data_fixed_ops, restore_zero)

static bool cmeta_data_nonempty(const char *text) { return text != NULL && text[0] != '\0'; }

static bool cmeta_data_integer_bits_valid(uint8_t bits) {
  return bits == 8u || bits == 16u || bits == 32u || bits == 64u;
}

static bool cmeta_data_float_bits_valid(uint8_t bits) { return bits == 32u || bits == 64u; }

static bool cmeta_data_buffer_ownership_valid(cmeta_data_buffer_ownership ownership) {
  switch (ownership) {
  case CMETA_DATA_BUFFER_OWNED:
  case CMETA_DATA_BUFFER_BORROWED:
  case CMETA_DATA_BUFFER_CUSTOM:
    return true;
  }
  return false;
}

bool cmeta_data_kind_valid(cmeta_data_kind kind) {
  switch (kind) {
  case CMETA_DATA_BOOL:
  case CMETA_DATA_SINT:
  case CMETA_DATA_UINT:
  case CMETA_DATA_FLOAT:
  case CMETA_DATA_STRING:
  case CMETA_DATA_BYTES:
  case CMETA_DATA_ENUM:
  case CMETA_DATA_STRUCT:
  case CMETA_DATA_VARIANT:
  case CMETA_DATA_SEQUENCE:
  case CMETA_DATA_SET:
  case CMETA_DATA_MAP:
  case CMETA_DATA_CUSTOM:
    return true;
  }
  return false;
}

bool cmeta_data_kind_is_container(cmeta_data_kind kind) {
  return kind == CMETA_DATA_SEQUENCE || kind == CMETA_DATA_SET || kind == CMETA_DATA_MAP;
}

static bool cmeta_data_enum_shape_valid(const cmeta_data_enum_shape *shape) {
  const cmeta_enum_desc *meta;
  if (shape == NULL || shape->meta == NULL) return false;
  meta = shape->meta;
  return cmeta_data_nonempty(meta->name) && (meta->count == 0u || meta->items != NULL);
}

static uint64_t cmeta_data_enum_width_mask(uint8_t bits) {
  return bits == 64u ? UINT64_MAX : (UINT64_C(1) << bits) - 1u;
}

static bool cmeta_data_enum_bits_valid(const cmeta_enum_domain *domain, uint64_t bits) {
  size_t i;
  if ((bits & ~cmeta_data_enum_width_mask(domain->bits)) != 0u) return false;
  if (domain->kind == CMETA_ENUM_FLAGS) return (bits & ~domain->declared_mask) == 0u;
  for (i = 0u; i < domain->count; ++i)
    if (domain->items[i].bits == bits) return true;
  return false;
}

static bool cmeta_data_struct_shape_valid(const cmeta_data_struct_shape *shape) {
  const cmeta_struct_desc *layout;
  size_t i;

  if (shape == NULL || shape->layout == NULL) return false;
  layout = shape->layout;
  if (!cmeta_data_nonempty(layout->name) || (layout->field_count != 0u && layout->fields == NULL) ||
      (shape->field_count != 0u && shape->fields == NULL) ||
      shape->field_count > layout->field_count)
    return false;

  for (i = 0u; i < shape->field_count; ++i) {
    const cmeta_data_field_desc *field = &shape->fields[i];
    const cmeta_field_desc *layout_field;
    if (!cmeta_data_nonempty(field->stable_id) || !cmeta_data_nonempty(field->name) ||
        field->value == NULL)
      return false;
    layout_field = cmeta_struct_find_field(layout, field->name);
    if (layout_field == NULL || layout_field->offset != field->offset) return false;
  }
  return true;
}

static bool cmeta_data_reflection_field_valid(const cmeta_data_desc *owner,
                                             const cmeta_data_field_desc *field,
                                             const cmeta_field_desc *layout) {
  const cmeta_data_desc *value = field->value;
  const cmeta_type_desc *storage;
  if (!cmeta_data_nonempty(field->stable_id) || !cmeta_data_nonempty(field->name) ||
      !cmeta_data_nonempty(layout->name) || value == NULL ||
      value->struct_size < CMETA_DATA_DESC_PREFIX_SIZE ||
      !cmeta_type_desc_valid(layout->type) || !cmeta_type_desc_valid(value->storage_type))
    return false;
  storage = value->storage_type;
  return strcmp(field->name, layout->name) == 0 && field->offset == layout->offset &&
         layout->size == storage->size && layout->align == storage->align &&
         layout->type->size == storage->size && layout->type->align == storage->align &&
         cmeta_type_equal(layout->type, storage) &&
         field->offset <= owner->storage_type->size &&
         storage->size <= owner->storage_type->size - field->offset &&
         field->offset % storage->align == 0u &&
         owner->storage_type->align >= storage->align;
}

/* V2 is a bounded, positional schema. O(n^2) comparisons (n <= 16) check
 * identity uniqueness and disjoint value storage without a mutable registry.
 * Child shapes are validated by their own consumer, avoiding recursive schema
 * validation here; the storage contracts are checked before any object access. */
static bool cmeta_data_reflection_valid(const cmeta_data_desc *desc) {
  const cmeta_data_reflection_shape *reflection;
  const cmeta_data_struct_shape *shape;
  const cmeta_struct_desc *layout;
  size_t i, j;
  if (desc->kind != CMETA_DATA_STRUCT || desc->struct_size < sizeof(cmeta_data_desc) ||
      !cmeta_type_desc_valid(desc->storage_type) || desc->shape == NULL ||
      desc->buffer_ops != NULL || desc->enum_ops != NULL || desc->variant_ops != NULL ||
      desc->fixed_ops != NULL || desc->enum_bits_ops != NULL || desc->collection_ops != NULL ||
      desc->map_ops != NULL || desc->construct_ops != NULL)
    return false;
  reflection = (const cmeta_data_reflection_shape *)desc->shape;
  shape = &reflection->structure;
  layout = shape->layout;
  if (reflection->struct_size < sizeof(*reflection) ||
      (reflection->mode != CMETA_DATA_REFLECTION_VIEW &&
       reflection->mode != CMETA_DATA_REFLECTION_VALUE) ||
      layout == NULL || !cmeta_data_nonempty(layout->name) ||
      shape->field_count == 0u || shape->field_count > CMETA_DATA_REFLECTION_MAX_FIELDS ||
      shape->fields == NULL || layout->fields == NULL ||
      layout->field_count != shape->field_count || layout->size != desc->storage_type->size ||
      layout->align != desc->storage_type->align)
    return false;
  for (i = 0u; i < shape->field_count; ++i) {
    const cmeta_data_field_desc *field = &shape->fields[i];
    if (!cmeta_data_reflection_field_valid(desc, field, &layout->fields[i])) return false;
    for (j = 0u; j < i; ++j) {
      const cmeta_data_field_desc *previous = &shape->fields[j];
      if (strcmp(field->stable_id, previous->stable_id) == 0 ||
          strcmp(field->name, previous->name) == 0)
        return false;
      if (reflection->mode == CMETA_DATA_REFLECTION_VALUE &&
          field->offset < previous->offset + layout->fields[j].size &&
          previous->offset < field->offset + layout->fields[i].size)
        return false;
    }
  }
  return true;
}

/* Call only after descriptor validation. V1 retains its existing authority. */
static bool cmeta_data_reflection_is_view(const cmeta_data_desc *desc) {
  return desc->abi_version == CMETA_DATA_DESC_REFLECTION_ABI_VERSION &&
         ((const cmeta_data_reflection_shape *)desc->shape)->mode == CMETA_DATA_REFLECTION_VIEW;
}

static bool cmeta_data_variant_tag_kind_valid(const cmeta_data_desc *tag) {
  if (tag == NULL || (tag->kind != CMETA_DATA_SINT && tag->kind != CMETA_DATA_UINT &&
                      tag->kind != CMETA_DATA_ENUM))
    return false;
  /* Parent variant ops own tag access; tags still require legacy metadata. */
  if (tag->kind == CMETA_DATA_ENUM) return cmeta_data_desc_valid(tag) && tag->shape != NULL;
  return cmeta_data_desc_valid(tag);
}

static bool cmeta_data_variant_tag_value_valid(const cmeta_data_desc *tag, int64_t value) {
  if (tag == NULL) return false;

  if (tag->kind == CMETA_DATA_SINT) {
    const cmeta_data_integer_shape *shape = (const cmeta_data_integer_shape *)tag->shape;
    if (shape == NULL) return false;
    if (shape->bits == 64u) return true;
    {
      const int64_t limit = INT64_C(1) << (shape->bits - 1u);
      return value >= -limit && value < limit;
    }
  }

  if (tag->kind == CMETA_DATA_UINT) {
    const cmeta_data_integer_shape *shape = (const cmeta_data_integer_shape *)tag->shape;
    uint64_t max_value;
    if (shape == NULL || value < 0) return false;
    max_value = shape->bits == 64u ? UINT64_MAX : (UINT64_C(1) << shape->bits) - UINT64_C(1);
    return (uint64_t)value <= max_value;
  }

  if (tag->kind == CMETA_DATA_ENUM) {
    const cmeta_data_enum_shape *shape = (const cmeta_data_enum_shape *)tag->shape;
    return shape != NULL && shape->meta != NULL &&
           cmeta_enum_item_by_value(shape->meta, value) != NULL;
  }

  return false;
}

static bool cmeta_data_variant_member_fits(const cmeta_data_desc *owner, size_t offset,
                                           const cmeta_data_desc *member) {
  size_t extent;
  if (owner == NULL || owner->storage_type == NULL || member == NULL ||
      member->storage_type == NULL || !cmeta_type_desc_valid(member->storage_type) ||
      member->storage_type->align == 0u || offset % member->storage_type->align != 0u ||
      offset > owner->storage_type->size)
    return false;
  extent = owner->storage_type->size - offset;
  return member->storage_type->size <= extent;
}

static bool cmeta_data_variant_members_overlap(size_t left_offset, const cmeta_data_desc *left,
                                               size_t right_offset, const cmeta_data_desc *right) {
  size_t left_end;
  size_t right_end;
  if (left == NULL || left->storage_type == NULL || right == NULL || right->storage_type == NULL ||
      left_offset > SIZE_MAX - left->storage_type->size ||
      right_offset > SIZE_MAX - right->storage_type->size)
    return true;
  left_end = left_offset + left->storage_type->size;
  right_end = right_offset + right->storage_type->size;
  return left_offset < right_end && right_offset < left_end;
}

static bool cmeta_data_variant_shape_valid(const cmeta_data_desc *owner,
                                           const cmeta_data_variant_shape *shape) {
  size_t i;
  size_t j;

  if (owner == NULL || owner->storage_type == NULL || shape == NULL ||
      !cmeta_data_variant_tag_kind_valid(shape->tag) ||
      !cmeta_data_variant_member_fits(owner, shape->tag_offset, shape->tag) ||
      (shape->case_count != 0u && shape->cases == NULL))
    return false;

  for (i = 0u; i < shape->case_count; ++i) {
    const cmeta_data_variant_case *item = &shape->cases[i];
    if (!cmeta_data_nonempty(item->stable_id) || !cmeta_data_nonempty(item->name) ||
        item->value == NULL || !cmeta_data_variant_tag_value_valid(shape->tag, item->tag) ||
        !cmeta_data_variant_member_fits(owner, item->offset, item->value) ||
        cmeta_data_variant_members_overlap(shape->tag_offset, shape->tag, item->offset,
                                           item->value))
      return false;
    for (j = i + 1u; j < shape->case_count; ++j)
      if (shape->cases[j].tag == item->tag) return false;
  }
  return true;
}

bool cmeta_data_desc_valid(const cmeta_data_desc *desc) {
  if (desc == NULL || desc->struct_size < CMETA_DATA_DESC_PREFIX_SIZE ||
      (desc->abi_version != CMETA_DATA_DESC_ABI_VERSION &&
       desc->abi_version != CMETA_DATA_DESC_REFLECTION_ABI_VERSION) ||
      !cmeta_data_nonempty(desc->stable_id) ||
      !cmeta_data_nonempty(desc->display_name) || !cmeta_data_kind_valid(desc->kind))
    return false;

  if (desc->abi_version == CMETA_DATA_DESC_REFLECTION_ABI_VERSION)
    return cmeta_data_reflection_valid(desc);
  if (cmeta_data_kind_is_container(desc->kind)) {
    if (desc->shape != NULL) return false;
    /* Kind-only semantic descriptors remain valid. A concrete native
     * provider may additionally select storage and collection_ops. */
    if (desc->storage_type == NULL)
      return desc->struct_size <
                 offsetof(cmeta_data_desc, collection_ops) + sizeof(desc->collection_ops) ||
             desc->collection_ops == NULL;
    if (!cmeta_type_desc_valid(desc->storage_type)) return false;
    if (desc->kind == CMETA_DATA_MAP)
      return desc->struct_size >= offsetof(cmeta_data_desc, map_ops) + sizeof(desc->map_ops) &&
             desc->map_ops != NULL;
    return desc->struct_size >=
               offsetof(cmeta_data_desc, collection_ops) + sizeof(desc->collection_ops) &&
           desc->collection_ops != NULL;
  }

  if (desc->storage_type == NULL || !cmeta_type_desc_valid(desc->storage_type)) return false;

  switch (desc->kind) {
  case CMETA_DATA_BOOL:
    return desc->shape == NULL;
  case CMETA_DATA_SINT:
  case CMETA_DATA_UINT:
    return desc->shape != NULL &&
           cmeta_data_integer_bits_valid(((const cmeta_data_integer_shape *)desc->shape)->bits);
  case CMETA_DATA_FLOAT:
    return desc->shape != NULL &&
           cmeta_data_float_bits_valid(((const cmeta_data_float_shape *)desc->shape)->bits);
  case CMETA_DATA_STRING:
  case CMETA_DATA_BYTES:
    return desc->shape != NULL && cmeta_data_buffer_ownership_valid(
                                      ((const cmeta_data_buffer_shape *)desc->shape)->ownership);
  case CMETA_DATA_ENUM:
    if (desc->struct_size >= CMETA_DATA_DESC_ENUM_BITS_OPS_SIZE && desc->enum_bits_ops != NULL) {
      const cmeta_data_enum_bits_ops *ops = desc->enum_bits_ops;
      return desc->shape == NULL && desc->enum_ops == NULL &&
             ops->struct_size >= CMETA_DATA_ENUM_BITS_OPS_PREFIX_SIZE &&
             ops->abi_version == CMETA_DATA_ENUM_BITS_OPS_ABI_VERSION &&
             cmeta_enum_domain_valid(ops->domain);
    }
    return cmeta_data_enum_shape_valid((const cmeta_data_enum_shape *)desc->shape);
  case CMETA_DATA_STRUCT:
    return cmeta_data_struct_shape_valid((const cmeta_data_struct_shape *)desc->shape);
  case CMETA_DATA_VARIANT:
    return cmeta_data_variant_shape_valid(desc, (const cmeta_data_variant_shape *)desc->shape);
  case CMETA_DATA_CUSTOM:
    return desc->shape != NULL;
  case CMETA_DATA_SEQUENCE:
  case CMETA_DATA_SET:
  case CMETA_DATA_MAP:
    return false;
  }
  return false;
}

static cmeta_status cmeta_data_buffer_ops_status(const cmeta_data_desc *desc,
                                                 const cmeta_data_buffer_ops **out) {
  const cmeta_data_buffer_shape *shape;
  const cmeta_data_buffer_ops *ops;

  if (out != NULL) *out = NULL;
  if (!cmeta_data_desc_valid(desc) ||
      (desc->kind != CMETA_DATA_STRING && desc->kind != CMETA_DATA_BYTES) ||
      desc->struct_size < CMETA_DATA_DESC_BUFFER_OPS_SIZE || desc->buffer_ops == NULL)
    return CMETA_INVALID_ARGUMENT;

  ops = desc->buffer_ops;
  if (ops->struct_size < CMETA_DATA_BUFFER_OPS_PREFIX_SIZE ||
      ops->abi_version != CMETA_DATA_BUFFER_OPS_ABI_VERSION || ops->storage_type == NULL ||
      !cmeta_type_desc_valid(ops->storage_type) ||
      !cmeta_data_buffer_ownership_valid(ops->ownership) || ops->is_zero == NULL ||
      ops->assign == NULL || ops->restore_zero == NULL || ops->init_zero == NULL ||
      ops->move == NULL)
    return CMETA_INVALID_ARGUMENT;

  shape = (const cmeta_data_buffer_shape *)desc->shape;
  if (!cmeta_type_equal(desc->storage_type, ops->storage_type) ||
      desc->storage_type->kind != ops->storage_type->kind ||
      desc->storage_type->size != ops->storage_type->size ||
      desc->storage_type->align != ops->storage_type->align || shape->ownership != ops->ownership)
    return CMETA_TYPE_MISMATCH;

  if (out != NULL) *out = ops;
  return CMETA_OK;
}

const cmeta_data_buffer_ops *cmeta_data_buffer_ops_of(const cmeta_data_desc *desc) {
  const cmeta_data_buffer_ops *ops = NULL;
  return cmeta_data_buffer_ops_status(desc, &ops) == CMETA_OK ? ops : NULL;
}

cmeta_status cmeta_data_buffer_init_zero(const cmeta_data_desc *desc, void *object) {
  const cmeta_data_buffer_ops *ops = NULL;
  cmeta_status status;

  if (object == NULL) return CMETA_INVALID_ARGUMENT;
  status = cmeta_data_buffer_ops_status(desc, &ops);
  if (status != CMETA_OK) return status;

  status = ops->init_zero(object);
  if (status != CMETA_OK) {
    ops->restore_zero(object);
    return ops->is_zero(object) ? status : CMETA_CALLBACK_ERROR;
  }
  if (!ops->is_zero(object)) {
    ops->restore_zero(object);
    return CMETA_CALLBACK_ERROR;
  }
  return CMETA_OK;
}

cmeta_status cmeta_data_buffer_move(const cmeta_data_desc *desc, void *destination, void *source) {
  const cmeta_data_buffer_ops *ops = NULL;
  bool source_was_zero;
  cmeta_status status;

  if (destination == NULL || source == NULL || destination == source) return CMETA_INVALID_ARGUMENT;
  status = cmeta_data_buffer_ops_status(desc, &ops);
  if (status != CMETA_OK) return status;
  if (!ops->is_zero(destination)) return CMETA_INVALID_ARGUMENT;

  source_was_zero = ops->is_zero(source);
  ops->move(destination, source);
  if (!ops->is_zero(source) || ops->is_zero(destination) != source_was_zero) {
    ops->restore_zero(destination);
    ops->restore_zero(source);
    return ops->is_zero(destination) && ops->is_zero(source) ? CMETA_CALLBACK_ERROR
                                                             : CMETA_CALLBACK_ERROR;
  }
  return CMETA_OK;
}

cmeta_status cmeta_data_buffer_is_zero(const cmeta_data_desc *desc, const void *object, bool *out) {
  const cmeta_data_buffer_ops *ops = NULL;
  cmeta_status status;

  if (object == NULL || out == NULL) return CMETA_INVALID_ARGUMENT;
  status = cmeta_data_buffer_ops_status(desc, &ops);
  if (status != CMETA_OK) return status;
  *out = ops->is_zero(object);
  return CMETA_OK;
}

cmeta_status cmeta_data_buffer_assign(const cmeta_data_desc *desc, void *object,
                                      const unsigned char *data, size_t size, size_t max_bytes) {
  const cmeta_data_buffer_ops *ops = NULL;
  cmeta_status status;

  if (object == NULL || (size != 0u && data == NULL)) return CMETA_INVALID_ARGUMENT;
  status = cmeta_data_buffer_ops_status(desc, &ops);
  if (status != CMETA_OK) return status;
  if (!ops->is_zero(object)) return CMETA_INVALID_ARGUMENT;
  if (size > max_bytes) return CMETA_CAPACITY_EXCEEDED;

  status = ops->assign(object, data, size, max_bytes);
  if (status != CMETA_OK) {
    ops->restore_zero(object);
    if (!ops->is_zero(object)) return CMETA_CALLBACK_ERROR;
  }
  return status;
}

cmeta_status cmeta_data_buffer_restore_zero(const cmeta_data_desc *desc, void *object) {
  const cmeta_data_buffer_ops *ops = NULL;
  cmeta_status status;

  if (object == NULL) return CMETA_INVALID_ARGUMENT;
  status = cmeta_data_buffer_ops_status(desc, &ops);
  if (status != CMETA_OK) return status;
  ops->restore_zero(object);
  return ops->is_zero(object) ? CMETA_OK : CMETA_CALLBACK_ERROR;
}

cmeta_status cmeta_data_buffer_read(const cmeta_data_desc *desc, const void *object,
                                    size_t max_bytes, const unsigned char **out_data,
                                    size_t *out_size) {
  const cmeta_data_buffer_ops *ops = NULL;
  const unsigned char *data = NULL;
  size_t size = 0u;
  cmeta_status status;

  if (object == NULL || out_data == NULL || out_size == NULL) return CMETA_INVALID_ARGUMENT;
  status = cmeta_data_buffer_ops_status(desc, &ops);
  if (status != CMETA_OK) return status;
  if (ops->struct_size < CMETA_DATA_BUFFER_OPS_READ_SIZE || ops->read == NULL)
    return CMETA_TRAIT_MISSING;

  status = ops->read(object, &data, &size);
  if (status != CMETA_OK) return status;
  if (size != 0u && data == NULL) return CMETA_CALLBACK_ERROR;
  if (size > max_bytes) return CMETA_CAPACITY_EXCEEDED;

  *out_data = data;
  *out_size = size;
  return CMETA_OK;
}

static cmeta_status cmeta_data_fixed_ops_status(const cmeta_data_desc *desc,
                                                const cmeta_data_fixed_ops **out) {
  const cmeta_data_fixed_ops *ops;

  if (out != NULL) *out = NULL;
  if (!cmeta_data_desc_valid(desc) || desc->struct_size < CMETA_DATA_DESC_FIXED_OPS_SIZE ||
      desc->fixed_ops == NULL)
    return CMETA_INVALID_ARGUMENT;

  ops = desc->fixed_ops;
  if (ops->struct_size < CMETA_DATA_FIXED_OPS_PREFIX_SIZE ||
      ops->abi_version != CMETA_DATA_FIXED_OPS_ABI_VERSION || ops->storage_type == NULL ||
      !cmeta_type_desc_valid(ops->storage_type) || ops->extent == 0u || ops->is_zero == NULL ||
      ops->copy == NULL || ops->restore_zero == NULL)
    return CMETA_INVALID_ARGUMENT;

  if (!cmeta_type_equal(desc->storage_type, ops->storage_type) ||
      desc->storage_type->kind != ops->storage_type->kind ||
      desc->storage_type->size != ops->storage_type->size ||
      desc->storage_type->align != ops->storage_type->align ||
      ops->extent != desc->storage_type->size)
    return CMETA_TYPE_MISMATCH;

  if (out != NULL) *out = ops;
  return CMETA_OK;
}

const cmeta_data_fixed_ops *cmeta_data_fixed_ops_of(const cmeta_data_desc *desc) {
  const cmeta_data_fixed_ops *ops = NULL;
  return cmeta_data_fixed_ops_status(desc, &ops) == CMETA_OK ? ops : NULL;
}

cmeta_status cmeta_data_fixed_extent(const cmeta_data_desc *desc, size_t *out) {
  const cmeta_data_fixed_ops *ops = NULL;
  cmeta_status status;

  if (out == NULL) return CMETA_INVALID_ARGUMENT;
  status = cmeta_data_fixed_ops_status(desc, &ops);
  if (status != CMETA_OK) return status;
  *out = ops->extent;
  return CMETA_OK;
}

cmeta_status cmeta_data_fixed_is_zero(const cmeta_data_desc *desc, const void *object, bool *out) {
  const cmeta_data_fixed_ops *ops = NULL;
  cmeta_status status;

  if (object == NULL || out == NULL) return CMETA_INVALID_ARGUMENT;
  status = cmeta_data_fixed_ops_status(desc, &ops);
  if (status != CMETA_OK) return status;
  *out = ops->is_zero(object);
  return CMETA_OK;
}

cmeta_status cmeta_data_fixed_copy(const cmeta_data_desc *desc, void *destination,
                                   const void *source, size_t source_extent) {
  const cmeta_data_fixed_ops *ops = NULL;
  cmeta_status status;

  if (destination == NULL || source == NULL || destination == source) return CMETA_INVALID_ARGUMENT;
  status = cmeta_data_fixed_ops_status(desc, &ops);
  if (status != CMETA_OK) return status;
  if (source_extent != ops->extent) return CMETA_TYPE_MISMATCH;
  if (!ops->is_zero(destination)) return CMETA_INVALID_ARGUMENT;

  status = ops->copy(destination, source);
  if (status != CMETA_OK) {
    ops->restore_zero(destination);
    if (!ops->is_zero(destination)) return CMETA_CALLBACK_ERROR;
  }
  return status;
}

cmeta_status cmeta_data_fixed_restore_zero(const cmeta_data_desc *desc, void *object) {
  const cmeta_data_fixed_ops *ops = NULL;
  cmeta_status status;

  if (object == NULL) return CMETA_INVALID_ARGUMENT;
  status = cmeta_data_fixed_ops_status(desc, &ops);
  if (status != CMETA_OK) return status;
  ops->restore_zero(object);
  return ops->is_zero(object) ? CMETA_OK : CMETA_CALLBACK_ERROR;
}

static cmeta_status cmeta_data_enum_ops_status(const cmeta_data_desc *desc,
                                               const cmeta_data_enum_ops **out) {
  const cmeta_data_enum_ops *ops;

  if (out != NULL) *out = NULL;
  if (!cmeta_data_desc_valid(desc) || desc->kind != CMETA_DATA_ENUM ||
      desc->struct_size < CMETA_DATA_DESC_ENUM_OPS_SIZE || desc->enum_ops == NULL)
    return CMETA_INVALID_ARGUMENT;

  ops = desc->enum_ops;
  if (ops->struct_size < CMETA_DATA_ENUM_OPS_PREFIX_SIZE ||
      ops->abi_version != CMETA_DATA_ENUM_OPS_ABI_VERSION || ops->storage_type == NULL ||
      !cmeta_type_desc_valid(ops->storage_type) || ops->is_zero == NULL || ops->read == NULL ||
      ops->assign == NULL || ops->restore_zero == NULL)
    return CMETA_INVALID_ARGUMENT;

  if (!cmeta_type_equal(desc->storage_type, ops->storage_type) ||
      desc->storage_type->kind != ops->storage_type->kind ||
      desc->storage_type->size != ops->storage_type->size ||
      desc->storage_type->align != ops->storage_type->align)
    return CMETA_TYPE_MISMATCH;

  if (out != NULL) *out = ops;
  return CMETA_OK;
}

const cmeta_data_enum_ops *cmeta_data_enum_ops_of(const cmeta_data_desc *desc) {
  const cmeta_data_enum_ops *ops = NULL;
  return cmeta_data_enum_ops_status(desc, &ops) == CMETA_OK ? ops : NULL;
}

cmeta_status cmeta_data_enum_is_zero(const cmeta_data_desc *desc, const void *object, bool *out) {
  const cmeta_data_enum_ops *ops = NULL;
  cmeta_status status;

  if (object == NULL || out == NULL) return CMETA_INVALID_ARGUMENT;
  status = cmeta_data_enum_ops_status(desc, &ops);
  if (status != CMETA_OK) return status;
  *out = ops->is_zero(object);
  return CMETA_OK;
}

cmeta_status cmeta_data_enum_read(const cmeta_data_desc *desc, const void *object, int64_t *out) {
  const cmeta_data_enum_shape *shape;
  const cmeta_data_enum_ops *ops = NULL;
  cmeta_status status;

  if (object == NULL || out == NULL) return CMETA_INVALID_ARGUMENT;
  status = cmeta_data_enum_ops_status(desc, &ops);
  if (status != CMETA_OK) return status;
  status = ops->read(object, out);
  if (status != CMETA_OK) return status;
  shape = (const cmeta_data_enum_shape *)desc->shape;
  return cmeta_enum_item_by_value(shape->meta, *out) != NULL ? CMETA_OK : CMETA_CALLBACK_ERROR;
}

cmeta_status cmeta_data_enum_assign(const cmeta_data_desc *desc, void *object, int64_t value) {
  const cmeta_data_enum_shape *shape;
  const cmeta_data_enum_ops *ops = NULL;
  int64_t actual = 0;
  cmeta_status status;

  if (object == NULL) return CMETA_INVALID_ARGUMENT;
  status = cmeta_data_enum_ops_status(desc, &ops);
  if (status != CMETA_OK) return status;
  shape = (const cmeta_data_enum_shape *)desc->shape;
  if (cmeta_enum_item_by_value(shape->meta, value) == NULL || !ops->is_zero(object))
    return CMETA_INVALID_ARGUMENT;

  status = ops->assign(object, value);
  if (status == CMETA_OK) {
    status = ops->read(object, &actual);
    if (status == CMETA_OK && actual != value) status = CMETA_CALLBACK_ERROR;
  }
  if (status != CMETA_OK) ops->restore_zero(object);
  return status;
}

cmeta_status cmeta_data_enum_restore_zero(const cmeta_data_desc *desc, void *object) {
  const cmeta_data_enum_ops *ops = NULL;
  cmeta_status status;

  if (object == NULL) return CMETA_INVALID_ARGUMENT;
  status = cmeta_data_enum_ops_status(desc, &ops);
  if (status != CMETA_OK) return status;
  ops->restore_zero(object);
  return ops->is_zero(object) ? CMETA_OK : CMETA_CALLBACK_ERROR;
}

static cmeta_status cmeta_data_enum_bits_ops_status(const cmeta_data_desc *desc,
                                                    const cmeta_data_enum_bits_ops **out) {
  const cmeta_data_enum_bits_ops *ops;
  if (!cmeta_data_desc_valid(desc) || desc->kind != CMETA_DATA_ENUM ||
      desc->struct_size < CMETA_DATA_DESC_ENUM_BITS_OPS_SIZE || desc->enum_bits_ops == NULL)
    return CMETA_INVALID_ARGUMENT;
  ops = desc->enum_bits_ops;
  if (!cmeta_type_desc_valid(ops->storage_type) || ops->is_zero == NULL || ops->read == NULL ||
      ops->assign == NULL || ops->restore_zero == NULL)
    return CMETA_INVALID_ARGUMENT;
  if (!cmeta_type_equal(desc->storage_type, ops->storage_type) ||
      desc->storage_type->kind != ops->storage_type->kind ||
      desc->storage_type->size != ops->storage_type->size ||
      desc->storage_type->align != ops->storage_type->align)
    return CMETA_TYPE_MISMATCH;
  *out = ops;
  return CMETA_OK;
}

const cmeta_data_enum_bits_ops *cmeta_data_enum_bits_ops_of(const cmeta_data_desc *desc) {
  const cmeta_data_enum_bits_ops *ops = NULL;
  return cmeta_data_enum_bits_ops_status(desc, &ops) == CMETA_OK ? ops : NULL;
}

cmeta_status cmeta_data_enum_bits_is_zero(const cmeta_data_desc *desc, const void *object,
                                          bool *out) {
  const cmeta_data_enum_bits_ops *ops = NULL;
  cmeta_status status;
  if (object == NULL || out == NULL) return CMETA_INVALID_ARGUMENT;
  status = cmeta_data_enum_bits_ops_status(desc, &ops);
  if (status != CMETA_OK) return status;
  *out = ops->is_zero(object);
  return CMETA_OK;
}

cmeta_status cmeta_data_enum_read_bits(const cmeta_data_desc *desc, const void *object,
                                       uint64_t *out) {
  const cmeta_data_enum_bits_ops *ops = NULL;
  uint64_t actual = 0u;
  cmeta_status status;
  if (object == NULL || out == NULL) return CMETA_INVALID_ARGUMENT;
  status = cmeta_data_enum_bits_ops_status(desc, &ops);
  if (status != CMETA_OK) return status;
  if (ops->read(object, &actual) != CMETA_OK || !cmeta_data_enum_bits_valid(ops->domain, actual))
    return CMETA_CALLBACK_ERROR;
  *out = actual;
  return CMETA_OK;
}

cmeta_status cmeta_data_enum_assign_bits(const cmeta_data_desc *desc, void *object, uint64_t bits) {
  const cmeta_data_enum_bits_ops *ops = NULL;
  uint64_t actual = 0u;
  cmeta_status status;
  if (object == NULL) return CMETA_INVALID_ARGUMENT;
  status = cmeta_data_enum_bits_ops_status(desc, &ops);
  if (status != CMETA_OK) return status;
  if (!cmeta_data_enum_bits_valid(ops->domain, bits) || !ops->is_zero(object))
    return CMETA_INVALID_ARGUMENT;
  if (ops->assign(object, bits) != CMETA_OK || ops->read(object, &actual) != CMETA_OK ||
      actual != bits) {
    ops->restore_zero(object);
    return CMETA_CALLBACK_ERROR;
  }
  return CMETA_OK;
}

cmeta_status cmeta_data_enum_bits_restore_zero(const cmeta_data_desc *desc, void *object) {
  const cmeta_data_enum_bits_ops *ops = NULL;
  cmeta_status status;
  if (object == NULL) return CMETA_INVALID_ARGUMENT;
  status = cmeta_data_enum_bits_ops_status(desc, &ops);
  if (status != CMETA_OK) return status;
  ops->restore_zero(object);
  return ops->is_zero(object) ? CMETA_OK : CMETA_CALLBACK_ERROR;
}

static cmeta_status cmeta_data_variant_ops_status(const cmeta_data_desc *desc,
                                                  const cmeta_data_variant_ops **out) {
  const cmeta_data_variant_ops *ops;

  if (out != NULL) *out = NULL;
  if (!cmeta_data_desc_valid(desc) || desc->kind != CMETA_DATA_VARIANT ||
      desc->struct_size < CMETA_DATA_DESC_VARIANT_OPS_SIZE || desc->variant_ops == NULL)
    return CMETA_INVALID_ARGUMENT;

  ops = desc->variant_ops;
  if (ops->struct_size < CMETA_DATA_VARIANT_OPS_PREFIX_SIZE ||
      ops->abi_version != CMETA_DATA_VARIANT_OPS_ABI_VERSION || ops->storage_type == NULL ||
      !cmeta_type_desc_valid(ops->storage_type) || ops->is_zero == NULL ||
      ops->active_tag == NULL || ops->select == NULL || ops->restore_zero == NULL)
    return CMETA_INVALID_ARGUMENT;

  if (!cmeta_type_equal(desc->storage_type, ops->storage_type) ||
      desc->storage_type->kind != ops->storage_type->kind ||
      desc->storage_type->size != ops->storage_type->size ||
      desc->storage_type->align != ops->storage_type->align)
    return CMETA_TYPE_MISMATCH;

  if (out != NULL) *out = ops;
  return CMETA_OK;
}

const cmeta_data_variant_ops *cmeta_data_variant_ops_of(const cmeta_data_desc *desc) {
  const cmeta_data_variant_ops *ops = NULL;
  return cmeta_data_variant_ops_status(desc, &ops) == CMETA_OK ? ops : NULL;
}

cmeta_status cmeta_data_variant_is_zero(const cmeta_data_desc *desc, const void *object,
                                        bool *out) {
  const cmeta_data_variant_ops *ops = NULL;
  cmeta_status status;

  if (object == NULL || out == NULL) return CMETA_INVALID_ARGUMENT;
  status = cmeta_data_variant_ops_status(desc, &ops);
  if (status != CMETA_OK) return status;
  *out = ops->is_zero(object);
  return CMETA_OK;
}

cmeta_status cmeta_data_variant_active_tag(const cmeta_data_desc *desc, const void *object,
                                           int64_t *out) {
  const cmeta_data_variant_shape *shape;
  const cmeta_data_variant_ops *ops = NULL;
  cmeta_status status;

  if (object == NULL || out == NULL) return CMETA_INVALID_ARGUMENT;
  status = cmeta_data_variant_ops_status(desc, &ops);
  if (status != CMETA_OK) return status;
  if (ops->is_zero(object)) return CMETA_INVALID_ARGUMENT;
  status = ops->active_tag(object, out);
  if (status != CMETA_OK) return status;
  shape = (const cmeta_data_variant_shape *)desc->shape;
  return cmeta_data_variant_case_by_tag(shape, *out) != NULL ? CMETA_OK : CMETA_CALLBACK_ERROR;
}

cmeta_status cmeta_data_variant_select(const cmeta_data_desc *desc, void *object, int64_t tag) {
  const cmeta_data_variant_ops *ops = NULL;
  const cmeta_data_variant_shape *shape;
  int64_t active = 0;
  cmeta_status status;

  if (object == NULL) return CMETA_INVALID_ARGUMENT;
  status = cmeta_data_variant_ops_status(desc, &ops);
  if (status != CMETA_OK) return status;
  shape = (const cmeta_data_variant_shape *)desc->shape;
  if (cmeta_data_variant_case_by_tag(shape, tag) == NULL || !ops->is_zero(object))
    return CMETA_INVALID_ARGUMENT;

  status = ops->select(object, tag);
  if (status == CMETA_OK) {
    if (ops->is_zero(object)) {
      status = CMETA_CALLBACK_ERROR;
    } else {
      status = ops->active_tag(object, &active);
      if (status == CMETA_OK && active != tag) status = CMETA_CALLBACK_ERROR;
    }
  }
  if (status != CMETA_OK) ops->restore_zero(object);
  return status;
}

cmeta_status cmeta_data_variant_restore_zero(const cmeta_data_desc *desc, void *object) {
  const cmeta_data_variant_ops *ops = NULL;
  cmeta_status status;

  if (object == NULL) return CMETA_INVALID_ARGUMENT;
  status = cmeta_data_variant_ops_status(desc, &ops);
  if (status != CMETA_OK) return status;
  ops->restore_zero(object);
  return ops->is_zero(object) ? CMETA_OK : CMETA_CALLBACK_ERROR;
}

const cmeta_data_field_desc *cmeta_data_struct_field(const cmeta_data_struct_shape *shape,
                                                     size_t index) {
  return shape != NULL && shape->fields != NULL && index < shape->field_count
             ? &shape->fields[index]
             : NULL;
}

const cmeta_data_field_desc *cmeta_data_struct_find_field(const cmeta_data_struct_shape *shape,
                                                          const char *name) {
  size_t i;
  if (shape == NULL || shape->fields == NULL || name == NULL) return NULL;
  for (i = 0u; i < shape->field_count; ++i)
    if (shape->fields[i].name != NULL && strcmp(shape->fields[i].name, name) == 0)
      return &shape->fields[i];
  return NULL;
}

const cmeta_data_variant_case *cmeta_data_variant_case_by_tag(const cmeta_data_variant_shape *shape,
                                                              int64_t tag) {
  size_t i;
  if (shape == NULL || shape->cases == NULL) return NULL;
  for (i = 0u; i < shape->case_count; ++i)
    if (shape->cases[i].tag == tag) return &shape->cases[i];
  return NULL;
}

static const cmeta_data_integer_shape cmeta_data_int8_shape = {8u};
static const cmeta_data_integer_shape cmeta_data_uint8_shape = {8u};
static const cmeta_data_integer_shape cmeta_data_int16_shape = {16u};
static const cmeta_data_integer_shape cmeta_data_uint16_shape = {16u};
static const cmeta_data_integer_shape cmeta_data_int32_shape = {32u};
static const cmeta_data_integer_shape cmeta_data_uint32_shape = {32u};
static const cmeta_data_integer_shape cmeta_data_int64_shape = {64u};
static const cmeta_data_integer_shape cmeta_data_uint64_shape = {64u};
static const cmeta_data_integer_shape cmeta_data_int_shape = {(uint8_t)(sizeof(int) * CHAR_BIT)};
static const cmeta_data_integer_shape cmeta_data_long_shape = {(uint8_t)(sizeof(long) * CHAR_BIT)};
static const cmeta_data_integer_shape cmeta_data_size_shape = {
    (uint8_t)(sizeof(size_t) * CHAR_BIT)};
static const cmeta_data_float_shape cmeta_data_float_shape_value = {
    (uint8_t)(sizeof(float) * CHAR_BIT)};
static const cmeta_data_float_shape cmeta_data_double_shape = {
    (uint8_t)(sizeof(double) * CHAR_BIT)};

#define CMETA_BUILTIN_DATA_DESC(stable_id_, display_name_, kind_, storage_, shape_)                \
  {.struct_size = CMETA_DATA_DESC_PREFIX_SIZE,                                                     \
   .abi_version = CMETA_DATA_DESC_ABI_VERSION,                                                     \
   .stable_id = (stable_id_),                                                                      \
   .display_name = (display_name_),                                                                \
   .kind = (kind_),                                                                                \
   .storage_type = (storage_),                                                                     \
   .shape = (shape_)}

const cmeta_data_desc cmeta_data_int8 = CMETA_BUILTIN_DATA_DESC(
    "cmeta.int8.data", "int8", CMETA_DATA_SINT, &cmeta_type_int8, &cmeta_data_int8_shape);
const cmeta_data_desc cmeta_data_uint8 = CMETA_BUILTIN_DATA_DESC(
    "cmeta.uint8.data", "uint8", CMETA_DATA_UINT, &cmeta_type_uint8, &cmeta_data_uint8_shape);
const cmeta_data_desc cmeta_data_int16 = CMETA_BUILTIN_DATA_DESC(
    "cmeta.int16.data", "int16", CMETA_DATA_SINT, &cmeta_type_int16, &cmeta_data_int16_shape);
const cmeta_data_desc cmeta_data_uint16 = CMETA_BUILTIN_DATA_DESC(
    "cmeta.uint16.data", "uint16", CMETA_DATA_UINT, &cmeta_type_uint16, &cmeta_data_uint16_shape);
const cmeta_data_desc cmeta_data_int32 = CMETA_BUILTIN_DATA_DESC(
    "cmeta.int32.data", "int32", CMETA_DATA_SINT, &cmeta_type_int32, &cmeta_data_int32_shape);
const cmeta_data_desc cmeta_data_uint32 = CMETA_BUILTIN_DATA_DESC(
    "cmeta.uint32.data", "uint32", CMETA_DATA_UINT, &cmeta_type_uint32, &cmeta_data_uint32_shape);
const cmeta_data_desc cmeta_data_int64 = CMETA_BUILTIN_DATA_DESC(
    "cmeta.int64.data", "int64", CMETA_DATA_SINT, &cmeta_type_int64, &cmeta_data_int64_shape);
const cmeta_data_desc cmeta_data_uint64 = CMETA_BUILTIN_DATA_DESC(
    "cmeta.uint64.data", "uint64", CMETA_DATA_UINT, &cmeta_type_uint64, &cmeta_data_uint64_shape);

const cmeta_data_desc cmeta_data_bool =
    CMETA_BUILTIN_DATA_DESC("cmeta.bool.data", "bool", CMETA_DATA_BOOL, &cmeta_type_bool, NULL);
const cmeta_data_desc cmeta_data_int = CMETA_BUILTIN_DATA_DESC(
    "cmeta.int.data", "int", CMETA_DATA_SINT, &cmeta_type_int, &cmeta_data_int_shape);
const cmeta_data_desc cmeta_data_long = CMETA_BUILTIN_DATA_DESC(
    "cmeta.long.data", "long", CMETA_DATA_SINT, &cmeta_type_long, &cmeta_data_long_shape);
const cmeta_data_desc cmeta_data_size = CMETA_BUILTIN_DATA_DESC(
    "cmeta.size.data", "size_t", CMETA_DATA_UINT, &cmeta_type_size, &cmeta_data_size_shape);
const cmeta_data_desc cmeta_data_float =
    CMETA_BUILTIN_DATA_DESC("cmeta.float.data", "float", CMETA_DATA_FLOAT, &cmeta_type_float,
                            &cmeta_data_float_shape_value);
const cmeta_data_desc cmeta_data_double = CMETA_BUILTIN_DATA_DESC(
    "cmeta.double.data", "double", CMETA_DATA_FLOAT, &cmeta_type_double, &cmeta_data_double_shape);

const cmeta_data_desc cmeta_data_sequence =
    CMETA_BUILTIN_DATA_DESC("cmeta.data.sequence", "sequence", CMETA_DATA_SEQUENCE, NULL, NULL);
const cmeta_data_desc cmeta_data_set =
    CMETA_BUILTIN_DATA_DESC("cmeta.data.set", "set", CMETA_DATA_SET, NULL, NULL);
const cmeta_data_desc cmeta_data_map =
    CMETA_BUILTIN_DATA_DESC("cmeta.data.map", "map", CMETA_DATA_MAP, NULL, NULL);

#undef CMETA_BUILTIN_DATA_DESC

#undef CMETA_DATA_VARIANT_OPS_PREFIX_SIZE
#undef CMETA_DATA_FIXED_OPS_PREFIX_SIZE
#undef CMETA_DATA_ENUM_OPS_PREFIX_SIZE
#undef CMETA_DATA_ENUM_BITS_OPS_PREFIX_SIZE
#undef CMETA_ENUM_DOMAIN_PREFIX_SIZE
#undef CMETA_DATA_DESC_ENUM_BITS_OPS_SIZE
#undef CMETA_DATA_BUFFER_OPS_READ_SIZE
#undef CMETA_DATA_BUFFER_OPS_PREFIX_SIZE
#undef CMETA_DATA_DESC_VARIANT_OPS_SIZE
#undef CMETA_DATA_DESC_FIXED_OPS_SIZE
#undef CMETA_DATA_DESC_ENUM_OPS_SIZE
#undef CMETA_DATA_DESC_BUFFER_OPS_SIZE
#undef CMETA_DATA_DESC_PREFIX_SIZE
#undef CMETA_FIELD_END

static const cmeta_type_identity cmeta_collection_view_identity =
    CMETA_TYPE_ID_ATOM_INIT("cmeta.collection_view");

const cmeta_type_desc cmeta_type_collection_view = {.name = "cmeta_data_collection_view",
                                                    .size = sizeof(cmeta_data_collection_view),
                                                    .align = _Alignof(cmeta_data_collection_view),
                                                    .kind = CMETA_T_OBJECT,
                                                    .pointee = NULL,
                                                    .traits = NULL,
                                                    .identity = &cmeta_collection_view_identity};

static const cmeta_data_desc *cmeta_sequence_view_element(const void *object) {
  const cmeta_data_collection_view *view = (const cmeta_data_collection_view *)object;
  return view != NULL ? view->element : NULL;
}

static cmeta_status cmeta_sequence_view_read(const void *object, cmeta_data_collection_view *out) {
  const cmeta_data_collection_view *view = (const cmeta_data_collection_view *)object;
  if (view == NULL || out == NULL) return CMETA_INVALID_ARGUMENT;
  *out = *view;
  return CMETA_OK;
}

static size_t cmeta_sequence_view_borrow_size(const void *object) {
  const cmeta_data_collection_view *view = (const cmeta_data_collection_view *)object;
  return view != NULL ? view->count : 0u;
}

static cmeta_gen_status cmeta_sequence_view_borrow_next(const void *object,
                                                        cmeta_range_cursor *cursor,
                                                        const void **out_element) {
  const cmeta_data_collection_view *view = (const cmeta_data_collection_view *)object;
  size_t offset;
  if (view == NULL || cursor == NULL || out_element == NULL || view->element == NULL ||
      !cmeta_data_desc_valid(view->element))
    return CMETA_GEN_ERROR;
  *out_element = NULL;
  if (cursor->index >= view->count) return CMETA_GEN_DONE;
  if (view->data == NULL || view->stride == 0u || cursor->index > SIZE_MAX / view->stride)
    return CMETA_GEN_ERROR;
  offset = cursor->index * view->stride;
  *out_element = (const unsigned char *)view->data + offset;
  ++cursor->index;
  return cursor->index == view->count ? CMETA_GEN_VALUE_AND_DONE : CMETA_GEN_VALUE;
}

static const cmeta_data_collection_borrow_ops cmeta_sequence_view_borrow_ops = {
    .struct_size = sizeof(cmeta_data_collection_borrow_ops),
    .abi_version = CMETA_DATA_COLLECTION_BORROW_OPS_ABI_VERSION,
    .size = cmeta_sequence_view_borrow_size,
    .next = cmeta_sequence_view_borrow_next,
    .current_version = NULL};

static const cmeta_data_collection_ops cmeta_sequence_view_ops = {
    .struct_size = sizeof(cmeta_data_collection_ops),
    .abi_version = CMETA_DATA_COLLECTION_OPS_ABI_VERSION,
    .storage_type = &cmeta_type_collection_view,
    .flags = CMETA_DATA_COLLECTION_CONTIGUOUS | CMETA_DATA_COLLECTION_ORDERED |
             CMETA_DATA_COLLECTION_RANDOM_ACCESS,
    .element = cmeta_sequence_view_element,
    .read = cmeta_sequence_view_read,
    .foreach = NULL,
    .collector = NULL,
    .borrow = &cmeta_sequence_view_borrow_ops};

const cmeta_data_desc cmeta_data_sequence_view = {.struct_size = sizeof(cmeta_data_desc),
                                                  .abi_version = CMETA_DATA_DESC_ABI_VERSION,
                                                  .stable_id = "cmeta.data.sequence_view",
                                                  .display_name = "borrowed sequence view",
                                                  .kind = CMETA_DATA_SEQUENCE,
                                                  .storage_type = &cmeta_type_collection_view,
                                                  .shape = NULL,
                                                  .collection_ops = &cmeta_sequence_view_ops};

#define CMETA_COLLECTION_FIELD_END(type, member)                                                   \
  (offsetof(type, member) + sizeof(((type *)0)->member))
#define CMETA_COLLECTION_DESC_OPS_SIZE CMETA_COLLECTION_FIELD_END(cmeta_data_desc, collection_ops)
#define CMETA_COLLECTION_OPS_BASE_SIZE                                                             \
  CMETA_COLLECTION_FIELD_END(cmeta_data_collection_ops, foreach)
#define CMETA_COLLECTION_OPS_COLLECTOR_SIZE                                                        \
  CMETA_COLLECTION_FIELD_END(cmeta_data_collection_ops, collector)
#define CMETA_COLLECTION_OPS_ELEMENT_DATA_SIZE                                                     \
  CMETA_COLLECTION_FIELD_END(cmeta_data_collection_ops, element_data)

static cmeta_status cmeta_data_collection_ops_status(const cmeta_data_desc *desc,
                                                     const cmeta_data_collection_ops **out) {
  const cmeta_data_collection_ops *ops;

  if (out != NULL) *out = NULL;
  if (!cmeta_data_desc_valid(desc) ||
      (desc->kind != CMETA_DATA_SEQUENCE && desc->kind != CMETA_DATA_SET) ||
      desc->struct_size < CMETA_COLLECTION_DESC_OPS_SIZE || desc->collection_ops == NULL)
    return CMETA_INVALID_ARGUMENT;

  ops = desc->collection_ops;
  if (ops->struct_size < CMETA_COLLECTION_OPS_BASE_SIZE ||
      ops->abi_version != CMETA_DATA_COLLECTION_OPS_ABI_VERSION || ops->storage_type == NULL ||
      !cmeta_type_desc_valid(ops->storage_type) ||
      (ops->flags & ~CMETA_DATA_COLLECTION_FLAGS_MASK) != 0u || ops->element == NULL ||
      (ops->read == NULL && ops->foreach == NULL))
    return CMETA_INVALID_ARGUMENT;

  if (desc->storage_type == NULL || !cmeta_type_equal(desc->storage_type, ops->storage_type) ||
      desc->storage_type->kind != ops->storage_type->kind ||
      desc->storage_type->size != ops->storage_type->size ||
      desc->storage_type->align != ops->storage_type->align)
    return CMETA_TYPE_MISMATCH;

  if (out != NULL) *out = ops;
  return CMETA_OK;
}

const cmeta_data_collection_ops *cmeta_data_collection_ops_of(const cmeta_data_desc *desc) {
  const cmeta_data_collection_ops *ops = NULL;
  return cmeta_data_collection_ops_status(desc, &ops) == CMETA_OK ? ops : NULL;
}

const cmeta_data_desc *cmeta_data_collection_element_data(const cmeta_data_desc *desc) {
  const cmeta_data_collection_ops *ops = NULL;
  if (cmeta_data_collection_ops_status(desc, &ops) != CMETA_OK ||
      ops->struct_size < CMETA_COLLECTION_OPS_ELEMENT_DATA_SIZE || ops->element_data == NULL ||
      !cmeta_data_desc_valid(ops->element_data))
    return NULL;
  return ops->element_data;
}

cmeta_status cmeta_data_collection_read(const cmeta_data_desc *desc, const void *object,
                                        cmeta_data_collection_view *out) {
  const cmeta_data_collection_ops *ops = NULL;
  const cmeta_data_desc *expected;
  cmeta_data_collection_view view = {0};
  cmeta_status status;

  if (object == NULL || out == NULL) return CMETA_INVALID_ARGUMENT;
  status = cmeta_data_collection_ops_status(desc, &ops);
  if (status != CMETA_OK) return status;
  if (ops->read == NULL) return CMETA_TRAIT_MISSING;

  expected = ops->element(object);
  if (expected != NULL && !cmeta_data_desc_valid(expected)) return CMETA_TRAIT_MISSING;
  status = ops->read(object, &view);
  if (status != CMETA_OK) return status;

  if (view.count != 0u) {
    if (view.data == NULL || view.stride == 0u) return CMETA_CALLBACK_ERROR;
    if (view.element == NULL) view.element = expected;
    if (view.element == NULL || !cmeta_data_desc_valid(view.element)) return CMETA_TRAIT_MISSING;
    if (expected != NULL && !cmeta_data_desc_equal(expected, view.element))
      return CMETA_CALLBACK_ERROR;
  } else {
    if (view.data != NULL && view.stride == 0u) return CMETA_CALLBACK_ERROR;
    if (view.element == NULL) view.element = expected;
    if (view.element != NULL && !cmeta_data_desc_valid(view.element)) return CMETA_CALLBACK_ERROR;
  }

  *out = view;
  return CMETA_OK;
}

#define CMETA_COLLECTION_OPS_BORROW_SIZE                                                           \
  CMETA_COLLECTION_FIELD_END(cmeta_data_collection_ops, borrow)
#define CMETA_COLLECTION_BORROW_OPS_SIZE                                                           \
  CMETA_COLLECTION_FIELD_END(cmeta_data_collection_borrow_ops, current_version)

static cmeta_status
cmeta_data_collection_borrow_ops_status(const cmeta_data_desc *desc,
                                        const cmeta_data_collection_borrow_ops **out) {
  const cmeta_data_collection_ops *ops = NULL;
  const cmeta_data_collection_borrow_ops *borrow;
  cmeta_status status;

  if (out != NULL) *out = NULL;
  status = cmeta_data_collection_ops_status(desc, &ops);
  if (status != CMETA_OK) return status;
  if (ops->struct_size < CMETA_COLLECTION_OPS_BORROW_SIZE || ops->borrow == NULL)
    return CMETA_TRAIT_MISSING;
  borrow = ops->borrow;
  if (borrow->struct_size < CMETA_COLLECTION_BORROW_OPS_SIZE ||
      borrow->abi_version != CMETA_DATA_COLLECTION_BORROW_OPS_ABI_VERSION || borrow->next == NULL)
    return CMETA_INVALID_ARGUMENT;
  if (out != NULL) *out = borrow;
  return CMETA_OK;
}

cmeta_status cmeta_data_collection_borrow_begin(const cmeta_data_desc *desc, const void *object,
                                                cmeta_data_collection_borrow_cursor *out) {
  const cmeta_data_collection_ops *ops = NULL;
  const cmeta_data_collection_borrow_ops *borrow = NULL;
  const cmeta_data_desc *element;
  cmeta_status status;

  if (object == NULL || out == NULL) return CMETA_INVALID_ARGUMENT;
  *out = (cmeta_data_collection_borrow_cursor){0};
  status = cmeta_data_collection_ops_status(desc, &ops);
  if (status != CMETA_OK) return status;
  status = cmeta_data_collection_borrow_ops_status(desc, &borrow);
  if (status != CMETA_OK) return status;
  element = ops->element(object);
  if (element == NULL || !cmeta_data_desc_valid(element)) return CMETA_TRAIT_MISSING;

  out->data = desc;
  out->element = element;
  out->object = object;
  out->ops = borrow;
  out->version = borrow->current_version != NULL ? borrow->current_version(object) : UINT64_C(0);
  return CMETA_OK;
}

cmeta_status cmeta_data_collection_borrow_size(const cmeta_data_collection_borrow_cursor *cursor,
                                               size_t *out_size) {
  if (cursor == NULL || out_size == NULL || cursor->ops == NULL || cursor->object == NULL)
    return CMETA_INVALID_ARGUMENT;
  if (cursor->ops->size == NULL) return CMETA_TRAIT_MISSING;
  if (cursor->ops->current_version != NULL &&
      cursor->ops->current_version(cursor->object) != cursor->version)
    return CMETA_CALLBACK_ERROR;
  *out_size = cursor->ops->size(cursor->object);
  return CMETA_OK;
}

cmeta_gen_status cmeta_data_collection_borrow_next(cmeta_data_collection_borrow_cursor *cursor,
                                                   const void **out_element) {
  cmeta_gen_status status;
  if (cursor == NULL || out_element == NULL || cursor->ops == NULL || cursor->object == NULL ||
      cursor->element == NULL)
    return CMETA_GEN_ERROR;
  *out_element = NULL;
  if (cursor->ops->current_version != NULL &&
      cursor->ops->current_version(cursor->object) != cursor->version)
    return CMETA_GEN_MUTATED;

  status = cursor->ops->next(cursor->object, &cursor->cursor, out_element);
  switch (status) {
  case CMETA_GEN_VALUE:
  case CMETA_GEN_VALUE_AND_DONE:
    return *out_element != NULL ? status : CMETA_GEN_ERROR;
  case CMETA_GEN_DONE:
  case CMETA_GEN_ERROR:
  case CMETA_GEN_MUTATED:
    *out_element = NULL;
    return status;
  default:
    *out_element = NULL;
    return CMETA_GEN_ERROR;
  }
}

#undef CMETA_COLLECTION_BORROW_OPS_SIZE
#undef CMETA_COLLECTION_OPS_BORROW_SIZE
#undef CMETA_COLLECTION_OPS_ELEMENT_DATA_SIZE
#undef CMETA_COLLECTION_OPS_COLLECTOR_SIZE
#undef CMETA_COLLECTION_OPS_BASE_SIZE
#undef CMETA_COLLECTION_DESC_OPS_SIZE
#undef CMETA_COLLECTION_FIELD_END

const cmeta_data_desc *cmeta_data_integer_width(bool is_signed, uint8_t bits) {
  switch (bits) {
  case 8u:
    return is_signed ? &cmeta_data_int8 : &cmeta_data_uint8;
  case 16u:
    return is_signed ? &cmeta_data_int16 : &cmeta_data_uint16;
  case 32u:
    return is_signed ? &cmeta_data_int32 : &cmeta_data_uint32;
  case 64u:
    return is_signed ? &cmeta_data_int64 : &cmeta_data_uint64;
  default:
    return NULL;
  }
}

typedef struct cmeta_data_collection_foreach_context {
  cmeta_data_collection_visit_fn visit;
  void *context;
  size_t remaining;
} cmeta_data_collection_foreach_context;

static cmeta_status cmeta_data_collection_bounded_visit(void *context, const void *element) {
  cmeta_data_collection_foreach_context *bounded = (cmeta_data_collection_foreach_context *)context;
  if (bounded == NULL || bounded->visit == NULL || element == NULL) return CMETA_INVALID_ARGUMENT;
  if (bounded->remaining == 0u) return CMETA_CAPACITY_EXCEEDED;
  --bounded->remaining;
  return bounded->visit(bounded->context, element);
}

cmeta_status cmeta_data_collection_foreach(const cmeta_data_desc *desc, const void *object,
                                           cmeta_data_collection_visit_fn visit, void *context,
                                           size_t max_items) {
  const cmeta_data_collection_ops *ops = NULL;
  cmeta_data_collection_foreach_context bounded;
  cmeta_status status;
  size_t i;

  if (object == NULL || visit == NULL) return CMETA_INVALID_ARGUMENT;
  status = cmeta_data_collection_ops_status(desc, &ops);
  if (status != CMETA_OK) return status;

  if (ops->foreach != NULL) {
    bounded.visit = visit;
    bounded.context = context;
    bounded.remaining = max_items;
    return ops->foreach (object, cmeta_data_collection_bounded_visit, &bounded, max_items);
  }

  if (ops->read != NULL) {
    cmeta_data_collection_view view = {0};
    status = cmeta_data_collection_read(desc, object, &view);
    if (status != CMETA_OK) return status;
    if (view.count > max_items) return CMETA_CAPACITY_EXCEEDED;
    for (i = 0u; i < view.count; ++i) {
      status = visit(context, (const unsigned char *)view.data + i * view.stride);
      if (status != CMETA_OK) return status;
    }
    return CMETA_OK;
  }
  return CMETA_TRAIT_MISSING;
}

#define CMETA_MAP_FIELD_END(type, member) (offsetof(type, member) + sizeof(((type *)0)->member))
#define CMETA_MAP_DESC_OPS_SIZE CMETA_MAP_FIELD_END(cmeta_data_desc, map_ops)
#define CMETA_MAP_OPS_BASE_SIZE CMETA_MAP_FIELD_END(cmeta_data_map_ops, foreach)
#define CMETA_MAP_OPS_COLLECTOR_SIZE CMETA_MAP_FIELD_END(cmeta_data_map_ops, collector)
#define CMETA_MAP_OPS_ACCEPT_SIZE CMETA_MAP_FIELD_END(cmeta_data_map_ops, accept)
#define CMETA_MAP_OPS_BORROW_SIZE CMETA_MAP_FIELD_END(cmeta_data_map_ops, borrow)
#define CMETA_MAP_OPS_KEY_DATA_SIZE CMETA_MAP_FIELD_END(cmeta_data_map_ops, key_data)
#define CMETA_MAP_OPS_VALUE_DATA_SIZE CMETA_MAP_FIELD_END(cmeta_data_map_ops, value_data)
#define CMETA_MAP_BORROW_OPS_SIZE CMETA_MAP_FIELD_END(cmeta_data_map_borrow_ops, current_version)

static cmeta_status cmeta_data_map_ops_status(const cmeta_data_desc *desc,
                                              const cmeta_data_map_ops **out) {
  const cmeta_data_map_ops *ops;
  if (out != NULL) *out = NULL;
  if (!cmeta_data_desc_valid(desc) || desc->kind != CMETA_DATA_MAP ||
      desc->struct_size < CMETA_MAP_DESC_OPS_SIZE || desc->map_ops == NULL)
    return CMETA_INVALID_ARGUMENT;
  ops = desc->map_ops;
  if (ops->struct_size < CMETA_MAP_OPS_BASE_SIZE ||
      ops->abi_version != CMETA_DATA_MAP_OPS_ABI_VERSION || ops->storage_type == NULL ||
      !cmeta_type_desc_valid(ops->storage_type) ||
      (ops->flags & ~CMETA_DATA_MAP_FLAGS_MASK) != 0u ||
      ((ops->flags & CMETA_DATA_MAP_UNIQUE_KEYS) != 0u &&
       (ops->flags & CMETA_DATA_MAP_REPEATED_KEYS) != 0u) ||
      ((ops->flags & (CMETA_DATA_MAP_UNIQUE_KEYS | CMETA_DATA_MAP_REPEATED_KEYS)) == 0u) ||
      ops->key == NULL || ops->value == NULL || ops->foreach == NULL)
    return CMETA_INVALID_ARGUMENT;
  if (desc->storage_type == NULL || !cmeta_type_equal(desc->storage_type, ops->storage_type) ||
      desc->storage_type->size != ops->storage_type->size ||
      desc->storage_type->align != ops->storage_type->align)
    return CMETA_TYPE_MISMATCH;
  if (out != NULL) *out = ops;
  return CMETA_OK;
}

const cmeta_data_map_ops *cmeta_data_map_ops_of(const cmeta_data_desc *desc) {
  const cmeta_data_map_ops *ops = NULL;
  return cmeta_data_map_ops_status(desc, &ops) == CMETA_OK ? ops : NULL;
}

const cmeta_data_desc *cmeta_data_map_key_data(const cmeta_data_desc *desc) {
  const cmeta_data_map_ops *ops = NULL;
  if (cmeta_data_map_ops_status(desc, &ops) != CMETA_OK ||
      ops->struct_size < CMETA_MAP_OPS_KEY_DATA_SIZE || ops->key_data == NULL ||
      !cmeta_data_desc_valid(ops->key_data))
    return NULL;
  return ops->key_data;
}

const cmeta_data_desc *cmeta_data_map_value_data(const cmeta_data_desc *desc) {
  const cmeta_data_map_ops *ops = NULL;
  if (cmeta_data_map_ops_status(desc, &ops) != CMETA_OK ||
      ops->struct_size < CMETA_MAP_OPS_VALUE_DATA_SIZE || ops->value_data == NULL ||
      !cmeta_data_desc_valid(ops->value_data))
    return NULL;
  return ops->value_data;
}

typedef struct cmeta_data_map_bounded_context {
  cmeta_data_map_visit_fn visit;
  void *context;
  size_t remaining;
} cmeta_data_map_bounded_context;

static cmeta_status cmeta_data_map_bounded_visit(void *context, const void *key,
                                                 const void *value) {
  cmeta_data_map_bounded_context *bounded = (cmeta_data_map_bounded_context *)context;
  if (bounded == NULL || bounded->visit == NULL || key == NULL || value == NULL)
    return CMETA_INVALID_ARGUMENT;
  if (bounded->remaining == 0u) return CMETA_CAPACITY_EXCEEDED;
  --bounded->remaining;
  return bounded->visit(bounded->context, key, value);
}

cmeta_status cmeta_data_map_foreach(const cmeta_data_desc *desc, const void *object,
                                    cmeta_data_map_visit_fn visit, void *context,
                                    size_t max_items) {
  const cmeta_data_map_ops *ops = NULL;
  const cmeta_data_desc *key_data;
  const cmeta_data_desc *value_data;
  cmeta_data_map_bounded_context bounded;
  cmeta_status status;
  if (object == NULL || visit == NULL) return CMETA_INVALID_ARGUMENT;
  status = cmeta_data_map_ops_status(desc, &ops);
  if (status != CMETA_OK) return status;
  key_data = ops->key(object);
  value_data = ops->value(object);
  if (key_data == NULL || value_data == NULL || !cmeta_data_desc_valid(key_data) ||
      !cmeta_data_desc_valid(value_data))
    return CMETA_TRAIT_MISSING;
  bounded.visit = visit;
  bounded.context = context;
  bounded.remaining = max_items;
  return ops->foreach (object, cmeta_data_map_bounded_visit, &bounded, max_items);
}

static cmeta_status cmeta_data_map_borrow_ops_status(const cmeta_data_desc *desc,
                                                     const cmeta_data_map_borrow_ops **out) {
  const cmeta_data_map_ops *ops = NULL;
  const cmeta_data_map_borrow_ops *borrow;
  cmeta_status status;

  if (out != NULL) *out = NULL;
  status = cmeta_data_map_ops_status(desc, &ops);
  if (status != CMETA_OK) return status;
  if (ops->struct_size < CMETA_MAP_OPS_BORROW_SIZE || ops->borrow == NULL)
    return CMETA_TRAIT_MISSING;
  borrow = ops->borrow;
  if (borrow->struct_size < CMETA_MAP_BORROW_OPS_SIZE ||
      borrow->abi_version != CMETA_DATA_MAP_BORROW_OPS_ABI_VERSION || borrow->next == NULL)
    return CMETA_INVALID_ARGUMENT;
  if (out != NULL) *out = borrow;
  return CMETA_OK;
}

cmeta_status cmeta_data_map_borrow_begin(const cmeta_data_desc *desc, const void *object,
                                         cmeta_data_map_borrow_cursor *out) {
  const cmeta_data_map_ops *ops = NULL;
  const cmeta_data_map_borrow_ops *borrow = NULL;
  const cmeta_data_desc *key;
  const cmeta_data_desc *value;
  cmeta_status status;

  if (object == NULL || out == NULL) return CMETA_INVALID_ARGUMENT;
  *out = (cmeta_data_map_borrow_cursor){0};
  status = cmeta_data_map_ops_status(desc, &ops);
  if (status != CMETA_OK) return status;
  status = cmeta_data_map_borrow_ops_status(desc, &borrow);
  if (status != CMETA_OK) return status;
  key = ops->key(object);
  value = ops->value(object);
  if (key == NULL || value == NULL || !cmeta_data_desc_valid(key) || !cmeta_data_desc_valid(value))
    return CMETA_TRAIT_MISSING;

  out->data = desc;
  out->key = key;
  out->value = value;
  out->object = object;
  out->ops = borrow;
  out->version = borrow->current_version != NULL ? borrow->current_version(object) : UINT64_C(0);
  return CMETA_OK;
}

cmeta_status cmeta_data_map_borrow_size(const cmeta_data_map_borrow_cursor *cursor,
                                        size_t *out_size) {
  if (cursor == NULL || out_size == NULL || cursor->ops == NULL || cursor->object == NULL)
    return CMETA_INVALID_ARGUMENT;
  if (cursor->ops->size == NULL) return CMETA_TRAIT_MISSING;
  if (cursor->ops->current_version != NULL &&
      cursor->ops->current_version(cursor->object) != cursor->version)
    return CMETA_CALLBACK_ERROR;
  *out_size = cursor->ops->size(cursor->object);
  return CMETA_OK;
}

cmeta_gen_status cmeta_data_map_borrow_next(cmeta_data_map_borrow_cursor *cursor,
                                            const void **out_key, const void **out_value) {
  cmeta_gen_status status;
  if (cursor == NULL || out_key == NULL || out_value == NULL || cursor->ops == NULL ||
      cursor->object == NULL || cursor->key == NULL || cursor->value == NULL)
    return CMETA_GEN_ERROR;
  *out_key = NULL;
  *out_value = NULL;
  if (cursor->ops->current_version != NULL &&
      cursor->ops->current_version(cursor->object) != cursor->version)
    return CMETA_GEN_MUTATED;

  status = cursor->ops->next(cursor->object, &cursor->cursor, out_key, out_value);
  switch (status) {
  case CMETA_GEN_VALUE:
  case CMETA_GEN_VALUE_AND_DONE:
    if (*out_key != NULL && *out_value != NULL) return status;
    break;
  case CMETA_GEN_DONE:
  case CMETA_GEN_ERROR:
  case CMETA_GEN_MUTATED:
    *out_key = NULL;
    *out_value = NULL;
    return status;
  default:
    break;
  }
  *out_key = NULL;
  *out_value = NULL;
  return CMETA_GEN_ERROR;
}

#undef CMETA_MAP_BORROW_OPS_SIZE
#undef CMETA_MAP_OPS_VALUE_DATA_SIZE
#undef CMETA_MAP_OPS_KEY_DATA_SIZE
#undef CMETA_MAP_OPS_BORROW_SIZE
#undef CMETA_MAP_OPS_ACCEPT_SIZE
#undef CMETA_MAP_OPS_COLLECTOR_SIZE
#undef CMETA_MAP_OPS_BASE_SIZE
#undef CMETA_MAP_DESC_OPS_SIZE
#undef CMETA_MAP_FIELD_END

#define CMETA_CONSTRUCT_FIELD_END(type, member)                                                    \
  (offsetof(type, member) + sizeof(((type *)0)->member))
#define CMETA_CONSTRUCT_DESC_SIZE CMETA_CONSTRUCT_FIELD_END(cmeta_data_desc, construct_ops)
#define CMETA_CONSTRUCT_OPS_SIZE CMETA_CONSTRUCT_FIELD_END(cmeta_data_construct_ops, move)

static cmeta_status cmeta_data_construct_ops_status(const cmeta_data_desc *desc,
                                                    const cmeta_data_construct_ops **out) {
  const cmeta_data_construct_ops *ops;
  if (out != NULL) *out = NULL;
  if (!cmeta_data_desc_valid(desc)) return CMETA_INVALID_ARGUMENT;
  if (desc->struct_size < CMETA_CONSTRUCT_DESC_SIZE || desc->construct_ops == NULL)
    return CMETA_TRAIT_MISSING;
  ops = desc->construct_ops;
  if (ops->struct_size < CMETA_CONSTRUCT_OPS_SIZE ||
      ops->abi_version != CMETA_DATA_CONSTRUCT_OPS_ABI_VERSION || ops->storage_type == NULL ||
      !cmeta_type_desc_valid(ops->storage_type) || ops->init_zero == NULL ||
      ops->restore_zero == NULL || ops->move == NULL || !cmeta_lifecycle_flags_valid(ops))
    return CMETA_INVALID_ARGUMENT;
  if (desc->storage_type == NULL || !cmeta_type_equal(desc->storage_type, ops->storage_type) ||
      desc->storage_type->size != ops->storage_type->size ||
      desc->storage_type->align != ops->storage_type->align)
    return CMETA_TYPE_MISMATCH;
  if (out != NULL) *out = ops;
  return CMETA_OK;
}

const cmeta_data_construct_ops *cmeta_data_construct_ops_of(const cmeta_data_desc *desc) {
  const cmeta_data_construct_ops *ops = NULL;
  return cmeta_data_construct_ops_status(desc, &ops) == CMETA_OK ? ops : NULL;
}

cmeta_status cmeta_data_construct_init_zero(const cmeta_data_desc *desc, void *object) {
  const cmeta_data_construct_ops *ops = NULL;
  cmeta_status status;
  if (object == NULL) return CMETA_INVALID_ARGUMENT;
  status = cmeta_data_construct_ops_status(desc, &ops);
  if (status != CMETA_OK) return status;
  status = ops->init_zero(object);
  if (status != CMETA_OK) ops->restore_zero(object);
  return status;
}

cmeta_status cmeta_data_construct_restore_zero(const cmeta_data_desc *desc, void *object) {
  const cmeta_data_construct_ops *ops = NULL;
  cmeta_status status;
  if (object == NULL) return CMETA_INVALID_ARGUMENT;
  status = cmeta_data_construct_ops_status(desc, &ops);
  if (status != CMETA_OK) return status;
  ops->restore_zero(object);
  return CMETA_OK;
}

cmeta_status cmeta_data_construct_move(const cmeta_data_desc *desc, void *destination,
                                       void *source) {
  const cmeta_data_construct_ops *ops = NULL;
  cmeta_status status;
  if (destination == NULL || source == NULL || destination == source) return CMETA_INVALID_ARGUMENT;
  status = cmeta_data_construct_ops_status(desc, &ops);
  if (status != CMETA_OK) return status;
  ops->move(destination, source);
  return CMETA_OK;
}

#undef CMETA_CONSTRUCT_OPS_SIZE
#undef CMETA_CONSTRUCT_DESC_SIZE
#undef CMETA_CONSTRUCT_FIELD_END

cmeta_status cmeta_data_collection_collector(const cmeta_data_desc *desc, void *zero_output,
                                             size_t limit, cmeta_collector *out) {
  const cmeta_data_collection_ops *ops = NULL;
  cmeta_collector collector;
  cmeta_status status;
  if (zero_output == NULL || out == NULL) return CMETA_INVALID_ARGUMENT;
  status = cmeta_data_collection_ops_status(desc, &ops);
  if (status != CMETA_OK) return status;
  if (ops->struct_size >=
          offsetof(cmeta_data_collection_ops, collector_init) + sizeof(ops->collector_init) &&
      ops->collector_init != NULL) {
    memset(out, 0, sizeof(*out));
    status = ops->collector_init(zero_output, limit, out);
    if (status != CMETA_OK) return status;
    if (!cmeta_collector_ops_valid(out->ops) || out->zero_output != zero_output ||
        out->limit != limit || out->input_type == NULL || out->state != CMETA_COLLECTOR_ZERO ||
        out->count != 0u || out->status != CMETA_OK)
      return CMETA_CALLBACK_ERROR;
    return CMETA_OK;
  }
  if (ops->struct_size < offsetof(cmeta_data_collection_ops, collector) +
                             sizeof(((cmeta_data_collection_ops *)0)->collector) ||
      ops->collector == NULL)
    return CMETA_TRAIT_MISSING;
  collector = ops->collector(zero_output, limit);
  if (!cmeta_collector_ops_valid(collector.ops) || collector.zero_output != zero_output ||
      collector.limit != limit || collector.input_type == NULL)
    return CMETA_CALLBACK_ERROR;
  *out = collector;
  return CMETA_OK;
}

cmeta_status cmeta_data_map_collector(const cmeta_data_desc *desc, void *zero_output, size_t limit,
                                      cmeta_collector *out) {
  const cmeta_data_map_ops *ops = NULL;
  cmeta_collector collector;
  cmeta_status status;
  if (zero_output == NULL || out == NULL) return CMETA_INVALID_ARGUMENT;
  status = cmeta_data_map_ops_status(desc, &ops);
  if (status != CMETA_OK) return status;
  if (ops->struct_size <
          offsetof(cmeta_data_map_ops, collector) + sizeof(((cmeta_data_map_ops *)0)->collector) ||
      ops->collector == NULL)
    return CMETA_TRAIT_MISSING;
  collector = ops->collector(zero_output, limit);
  if (!cmeta_collector_ops_valid(collector.ops) || collector.zero_output != zero_output ||
      collector.limit != limit || collector.input_type == NULL)
    return CMETA_CALLBACK_ERROR;
  *out = collector;
  return CMETA_OK;
}

bool cmeta_data_desc_equal(const cmeta_data_desc *left, const cmeta_data_desc *right) {
  if (left == right) return left != NULL && cmeta_data_desc_valid(left);
  if (!cmeta_data_desc_valid(left) || !cmeta_data_desc_valid(right) || left->kind != right->kind ||
      left->stable_id == NULL || right->stable_id == NULL ||
      strcmp(left->stable_id, right->stable_id) != 0)
    return false;
  if (left->storage_type == NULL || right->storage_type == NULL)
    return left->storage_type == right->storage_type;
  return cmeta_type_equal(left->storage_type, right->storage_type);
}

cmeta_status cmeta_data_collection_accept(const cmeta_data_desc *desc, cmeta_collector *collector,
                                          const cmeta_data_desc *element_data,
                                          const void *element) {
  const cmeta_data_collection_ops *ops = NULL;
  const cmeta_data_desc *expected;
  cmeta_status status;
  if (collector == NULL || element_data == NULL || element == NULL) return CMETA_INVALID_ARGUMENT;
  status = cmeta_data_collection_ops_status(desc, &ops);
  if (status != CMETA_OK) return status;
  expected = ops->element(collector->zero_output);
  if (expected == NULL || !cmeta_data_desc_valid(expected) || !cmeta_data_desc_valid(element_data))
    return CMETA_TRAIT_MISSING;
  if (!cmeta_data_desc_equal(expected, element_data)) return CMETA_TYPE_MISMATCH;
  if (element_data->storage_type == NULL ||
      !cmeta_type_equal(element_data->storage_type, collector->input_type))
    return CMETA_TYPE_MISMATCH;
  return cmeta_collector_accept(collector, element_data->storage_type, element);
}

static cmeta_status cmeta_data_struct_init_zero(const cmeta_data_desc *desc, void *object);
static cmeta_status cmeta_data_struct_restore_zero(const cmeta_data_desc *desc, void *object);
static cmeta_status cmeta_data_struct_move(const cmeta_data_desc *desc, void *destination,
                                           void *source);

cmeta_status cmeta_data_value_init_zero(const cmeta_data_desc *desc, void *object) {
  if (!cmeta_data_desc_valid(desc) || object == NULL || desc->storage_type == NULL)
    return CMETA_INVALID_ARGUMENT;
  if (cmeta_data_reflection_is_view(desc)) return CMETA_TRAIT_MISSING;
  if (desc->fixed_ops != NULL) return cmeta_data_fixed_restore_zero(desc, object);
  if (desc->variant_ops != NULL) {
    /* A tagged provider must initialize raw storage without inspecting an
     * indeterminate tag. Existing providers without construction ops keep
     * their established zero-storage contract. */
    if (desc->struct_size >=
            offsetof(cmeta_data_desc, construct_ops) + sizeof(desc->construct_ops) &&
        desc->construct_ops != NULL)
      return cmeta_data_construct_init_zero(desc, object);
    return cmeta_data_variant_restore_zero(desc, object);
  }
  switch (desc->kind) {
  case CMETA_DATA_BOOL:
  case CMETA_DATA_SINT:
  case CMETA_DATA_UINT:
  case CMETA_DATA_FLOAT:
    memset(object, 0, desc->storage_type->size);
    return CMETA_OK;
  case CMETA_DATA_STRING:
  case CMETA_DATA_BYTES:
    return cmeta_data_buffer_init_zero(desc, object);
  case CMETA_DATA_ENUM:
    if (desc->enum_bits_ops != NULL) return cmeta_data_enum_bits_restore_zero(desc, object);
    return cmeta_data_enum_restore_zero(desc, object);
  case CMETA_DATA_STRUCT:
    return cmeta_data_struct_init_zero(desc, object);
  default:
    break;
  }
  return cmeta_data_construct_init_zero(desc, object);
}

cmeta_status cmeta_data_value_restore_zero(const cmeta_data_desc *desc, void *object) {
  if (!cmeta_data_desc_valid(desc) || object == NULL || desc->storage_type == NULL)
    return CMETA_INVALID_ARGUMENT;
  if (cmeta_data_reflection_is_view(desc)) return CMETA_TRAIT_MISSING;
  if (desc->fixed_ops != NULL) return cmeta_data_fixed_restore_zero(desc, object);
  if (desc->variant_ops != NULL) return cmeta_data_variant_restore_zero(desc, object);
  switch (desc->kind) {
  case CMETA_DATA_BOOL:
  case CMETA_DATA_SINT:
  case CMETA_DATA_UINT:
  case CMETA_DATA_FLOAT:
    memset(object, 0, desc->storage_type->size);
    return CMETA_OK;
  case CMETA_DATA_STRING:
  case CMETA_DATA_BYTES:
    return cmeta_data_buffer_restore_zero(desc, object);
  case CMETA_DATA_ENUM:
    if (desc->enum_bits_ops != NULL) return cmeta_data_enum_bits_restore_zero(desc, object);
    return cmeta_data_enum_restore_zero(desc, object);
  case CMETA_DATA_STRUCT:
    return cmeta_data_struct_restore_zero(desc, object);
  default:
    break;
  }
  return cmeta_data_construct_restore_zero(desc, object);
}

void cmeta_data_value_destroy(const cmeta_data_desc *desc, void *object) {
  if (!cmeta_data_desc_valid(desc) || object == NULL ||
      cmeta_data_value_restore_zero(desc, object) != CMETA_OK)
    abort();
}

static bool cmeta_data_struct_field_bounds_valid(const cmeta_data_desc *owner,
                                                 const cmeta_data_field_desc *field) {
  const cmeta_data_desc *value;
  size_t owner_size;
  if (owner == NULL || field == NULL || field->value == NULL || owner->storage_type == NULL)
    return false;
  value = field->value;
  if (!cmeta_data_desc_valid(value) || value->storage_type == NULL) return false;
  owner_size = owner->storage_type->size;
  if (field->offset > owner_size || value->storage_type->size > owner_size - field->offset)
    return false;
  if (value->storage_type->align != 0u && (field->offset % value->storage_type->align) != 0u)
    return false;
  return true;
}

enum { CMETA_DATA_ZERO_MAX_DEPTH = 64u };

static cmeta_status cmeta_data_value_is_zero_depth(const cmeta_data_desc *desc, const void *object,
                                                   bool *out, unsigned depth) {
  size_t i;
  if (out == NULL) return CMETA_INVALID_ARGUMENT;
  *out = false;
  if (depth > CMETA_DATA_ZERO_MAX_DEPTH || !cmeta_data_desc_valid(desc) ||
      desc->storage_type == NULL || object == NULL)
    return CMETA_INVALID_ARGUMENT;
  if (cmeta_data_reflection_is_view(desc)) return CMETA_TRAIT_MISSING;
  if (desc->struct_size >= offsetof(cmeta_data_desc, fixed_ops) + sizeof(desc->fixed_ops) &&
      desc->fixed_ops != NULL)
    return cmeta_data_fixed_is_zero(desc, object, out);
  if (desc->struct_size >= offsetof(cmeta_data_desc, variant_ops) + sizeof(desc->variant_ops) &&
      desc->variant_ops != NULL)
    return cmeta_data_variant_is_zero(desc, object, out);
  switch (desc->kind) {
  case CMETA_DATA_BOOL:
  case CMETA_DATA_SINT:
  case CMETA_DATA_UINT:
    for (i = 0u; i < desc->storage_type->size; ++i)
      if (((const unsigned char *)object)[i] != 0u) return CMETA_OK;
    *out = true;
    return CMETA_OK;
  case CMETA_DATA_FLOAT:
    if (desc->storage_type->kind != CMETA_T_FLOAT) return CMETA_TYPE_MISMATCH;
    if (((const cmeta_data_float_shape *)desc->shape)->bits == 32u &&
        desc->storage_type->size == sizeof(float)) {
      float value;
      memcpy(&value, object, sizeof(value));
      *out = value == 0.0f;
    } else if (((const cmeta_data_float_shape *)desc->shape)->bits == 64u &&
               desc->storage_type->size == sizeof(double)) {
      double value;
      memcpy(&value, object, sizeof(value));
      *out = value == 0.0;
    } else return CMETA_TYPE_MISMATCH;
    return CMETA_OK;
  case CMETA_DATA_STRING:
  case CMETA_DATA_BYTES:
    return cmeta_data_buffer_is_zero(desc, object, out);
  case CMETA_DATA_ENUM:
    if (cmeta_data_enum_bits_ops_of(desc) != NULL)
      return cmeta_data_enum_bits_is_zero(desc, object, out);
    return cmeta_data_enum_is_zero(desc, object, out);
  case CMETA_DATA_STRUCT: {
    const cmeta_data_struct_shape *shape = (const cmeta_data_struct_shape *)desc->shape;
    for (i = 0u; i < shape->field_count; ++i) {
      const cmeta_data_field_desc *field = &shape->fields[i];
      bool zero = false;
      cmeta_status status;
      if (!cmeta_data_struct_field_bounds_valid(desc, field)) return CMETA_TYPE_MISMATCH;
      status = cmeta_data_value_is_zero_depth(
          field->value, (const unsigned char *)object + field->offset, &zero, depth + 1u);
      if (status != CMETA_OK || !zero) return status;
    }
    *out = true;
    return CMETA_OK;
  }
  case CMETA_DATA_SEQUENCE:
  case CMETA_DATA_SET: {
    const cmeta_data_collection_ops *ops = cmeta_data_collection_ops_of(desc);
    cmeta_data_collection_borrow_cursor cursor = {0};
    size_t count = 0u;
    cmeta_status status;
    if (ops == NULL) return CMETA_TRAIT_MISSING;
    if (ops->struct_size >= offsetof(cmeta_data_collection_ops, is_zero) + sizeof(ops->is_zero) &&
        ops->is_zero != NULL) {
      *out = ops->is_zero(object);
      return CMETA_OK;
    }
    status = cmeta_data_collection_borrow_begin(desc, object, &cursor);
    if (status == CMETA_OK) status = cmeta_data_collection_borrow_size(&cursor, &count);
    if (status == CMETA_OK) *out = count == 0u;
    return status;
  }
  case CMETA_DATA_MAP: {
    cmeta_data_map_borrow_cursor cursor = {0};
    size_t count = 0u;
    cmeta_status status = cmeta_data_map_borrow_begin(desc, object, &cursor);
    if (status == CMETA_OK) status = cmeta_data_map_borrow_size(&cursor, &count);
    if (status == CMETA_OK) *out = count == 0u;
    return status;
  }
  default:
    return CMETA_TRAIT_MISSING;
  }
}

cmeta_status cmeta_data_value_is_zero(const cmeta_data_desc *desc, const void *object, bool *out) {
  return cmeta_data_value_is_zero_depth(desc, object, out, 0u);
}

static bool cmeta_data_value_move_supported_depth(const cmeta_data_desc *desc, unsigned depth) {
  const cmeta_data_struct_shape *shape;
  size_t i;
  if (depth > 64u || !cmeta_data_desc_valid(desc) || desc->storage_type == NULL) return false;
  if (cmeta_data_reflection_is_view(desc)) return false;
  switch (desc->kind) {
  case CMETA_DATA_BOOL:
  case CMETA_DATA_SINT:
  case CMETA_DATA_UINT:
  case CMETA_DATA_FLOAT:
    return true;
  case CMETA_DATA_STRING:
  case CMETA_DATA_BYTES:
    return cmeta_data_buffer_ops_of(desc) != NULL;
  case CMETA_DATA_STRUCT:
    shape = (const cmeta_data_struct_shape *)desc->shape;
    if (shape == NULL) return false;
    for (i = 0u; i < shape->field_count; ++i) {
      const cmeta_data_field_desc *field = &shape->fields[i];
      if (!cmeta_data_struct_field_bounds_valid(desc, field) ||
          !cmeta_data_value_move_supported_depth(field->value, depth + 1u))
        return false;
    }
    return true;
  case CMETA_DATA_VARIANT: {
    const cmeta_data_variant_shape *variant = (const cmeta_data_variant_shape *)desc->shape;
    if (cmeta_data_variant_ops_of(desc) == NULL || variant == NULL ||
        (variant->case_count != 0u && variant->cases == NULL))
      return false;
    for (i = 0u; i < variant->case_count; ++i) {
      const cmeta_data_variant_case *item = &variant->cases[i];
      if (item->value == NULL || !cmeta_data_value_move_supported_depth(item->value, depth + 1u))
        return false;
    }
    return true;
  }
  default:
    return cmeta_data_construct_ops_of(desc) != NULL;
  }
}

bool cmeta_data_struct_constructible(const cmeta_data_desc *desc) {
  return desc != NULL && desc->kind == CMETA_DATA_STRUCT &&
         cmeta_data_value_move_supported_depth(desc, 0u);
}

static cmeta_status cmeta_data_struct_init_zero(const cmeta_data_desc *desc, void *object) {
  const cmeta_data_struct_shape *shape;
  size_t i;
  if (!cmeta_data_struct_constructible(desc) || object == NULL) return CMETA_TRAIT_MISSING;
  shape = (const cmeta_data_struct_shape *)desc->shape;
  for (i = 0u; i < shape->field_count; ++i) {
    const cmeta_data_field_desc *field = &shape->fields[i];
    cmeta_status status =
        cmeta_data_value_init_zero(field->value, (unsigned char *)object + field->offset);
    if (status != CMETA_OK) {
      while (i != 0u) {
        --i;
        field = &shape->fields[i];
        (void)cmeta_data_value_restore_zero(field->value, (unsigned char *)object + field->offset);
      }
      return status;
    }
  }
  return CMETA_OK;
}

static cmeta_status cmeta_data_struct_restore_zero(const cmeta_data_desc *desc, void *object) {
  const cmeta_data_struct_shape *shape;
  size_t i;
  cmeta_status result = CMETA_OK;
  if (!cmeta_data_struct_constructible(desc) || object == NULL) return CMETA_TRAIT_MISSING;
  shape = (const cmeta_data_struct_shape *)desc->shape;
  i = shape->field_count;
  while (i != 0u) {
    const cmeta_data_field_desc *field = &shape->fields[--i];
    cmeta_status status =
        cmeta_data_value_restore_zero(field->value, (unsigned char *)object + field->offset);
    if (result == CMETA_OK && status != CMETA_OK) result = status;
  }
  return result;
}

static cmeta_status cmeta_data_struct_move(const cmeta_data_desc *desc, void *destination,
                                           void *source) {
  const cmeta_data_struct_shape *shape;
  size_t i;
  if (!cmeta_data_struct_constructible(desc) || destination == NULL || source == NULL ||
      destination == source)
    return CMETA_TRAIT_MISSING;
  shape = (const cmeta_data_struct_shape *)desc->shape;
  for (i = 0u; i < shape->field_count; ++i) {
    const cmeta_data_field_desc *field = &shape->fields[i];
    cmeta_status status =
        cmeta_data_value_move(field->value, (unsigned char *)destination + field->offset,
                              (unsigned char *)source + field->offset);
    if (status != CMETA_OK) {
      cmeta_status rollback_status = CMETA_OK;
      (void)cmeta_data_value_restore_zero(field->value,
                                          (unsigned char *)destination + field->offset);
      while (i != 0u) {
        const cmeta_data_field_desc *moved = &shape->fields[--i];
        cmeta_status rollback =
            cmeta_data_value_move(moved->value, (unsigned char *)source + moved->offset,
                                  (unsigned char *)destination + moved->offset);
        if (rollback != CMETA_OK) {
          (void)cmeta_data_value_restore_zero(moved->value,
                                              (unsigned char *)destination + moved->offset);
          rollback_status = CMETA_CALLBACK_ERROR;
        }
      }
      return rollback_status == CMETA_OK ? status : rollback_status;
    }
  }
  return CMETA_OK;
}

static cmeta_status cmeta_data_variant_move(const cmeta_data_desc *desc, void *destination,
                                            void *source) {
  const cmeta_data_variant_shape *shape;
  const cmeta_data_variant_case *active_case;
  bool source_zero = false;
  bool destination_zero = false;
  int64_t tag = 0;
  cmeta_status status;

  if (desc == NULL || destination == NULL || source == NULL || destination == source ||
      !cmeta_data_value_move_supported_depth(desc, 0u))
    return CMETA_TRAIT_MISSING;

  status = cmeta_data_variant_is_zero(desc, source, &source_zero);
  if (status != CMETA_OK) return status;
  status = cmeta_data_variant_is_zero(desc, destination, &destination_zero);
  if (status != CMETA_OK) return status;
  if (!destination_zero) return CMETA_INVALID_ARGUMENT;
  if (source_zero) return CMETA_OK;

  status = cmeta_data_variant_active_tag(desc, source, &tag);
  if (status != CMETA_OK) return status;
  shape = (const cmeta_data_variant_shape *)desc->shape;
  active_case = cmeta_data_variant_case_by_tag(shape, tag);
  if (active_case == NULL || active_case->value == NULL) return CMETA_CALLBACK_ERROR;

  status = cmeta_data_variant_select(desc, destination, tag);
  if (status != CMETA_OK) return status;

  status =
      cmeta_data_value_move(active_case->value, (unsigned char *)destination + active_case->offset,
                            (unsigned char *)source + active_case->offset);
  if (status != CMETA_OK) {
    (void)cmeta_data_variant_restore_zero(desc, destination);
    return status;
  }

  status = cmeta_data_variant_restore_zero(desc, source);
  if (status != CMETA_OK) {
    (void)cmeta_data_variant_restore_zero(desc, destination);
    return CMETA_CALLBACK_ERROR;
  }
  return CMETA_OK;
}

static bool cmeta_data_value_copy_supported_depth(const cmeta_data_desc *desc, unsigned depth) {
  const cmeta_data_struct_shape *shape;
  size_t i;
  if (depth > 64u || !cmeta_data_desc_valid(desc) || desc->storage_type == NULL) return false;
  if (cmeta_data_reflection_is_view(desc)) return false;
  if (cmeta_data_fixed_ops_of(desc) != NULL) return true;
  switch (desc->kind) {
  case CMETA_DATA_BOOL:
  case CMETA_DATA_SINT:
  case CMETA_DATA_UINT:
  case CMETA_DATA_FLOAT:
    return true;
  case CMETA_DATA_STRING:
  case CMETA_DATA_BYTES: {
    const cmeta_data_buffer_ops *ops = cmeta_data_buffer_ops_of(desc);
    return ops != NULL &&
           ops->struct_size >=
               offsetof(cmeta_data_buffer_ops, read) + sizeof(((cmeta_data_buffer_ops *)0)->read) &&
           ops->read != NULL;
  }
  case CMETA_DATA_ENUM:
    return cmeta_data_enum_bits_ops_of(desc) != NULL || cmeta_data_enum_ops_of(desc) != NULL;
  case CMETA_DATA_STRUCT:
    shape = (const cmeta_data_struct_shape *)desc->shape;
    if (shape == NULL) return false;
    for (i = 0u; i < shape->field_count; ++i) {
      const cmeta_data_field_desc *field = &shape->fields[i];
      if (!cmeta_data_struct_field_bounds_valid(desc, field) ||
          !cmeta_data_value_copy_supported_depth(field->value, depth + 1u))
        return false;
    }
    return true;
  case CMETA_DATA_VARIANT: {
    const cmeta_data_variant_shape *variant = (const cmeta_data_variant_shape *)desc->shape;
    if (cmeta_data_variant_ops_of(desc) == NULL || variant == NULL || variant->cases == NULL)
      return false;
    for (i = 0u; i < variant->case_count; ++i)
      if (!cmeta_data_value_copy_supported_depth(variant->cases[i].value, depth + 1u)) return false;
    return true;
  }
  case CMETA_DATA_SEQUENCE:
  case CMETA_DATA_SET: {
    const cmeta_data_collection_ops *ops = cmeta_data_collection_ops_of(desc);
    const cmeta_data_desc *element = cmeta_data_collection_element_data(desc);
    return ops != NULL &&
           ops->struct_size >= offsetof(cmeta_data_collection_ops, borrow) +
                                   sizeof(((cmeta_data_collection_ops *)0)->borrow) &&
           (ops->collector != NULL ||
            (ops->struct_size >= offsetof(cmeta_data_collection_ops, collector_init) +
                                     sizeof(ops->collector_init) &&
             ops->collector_init != NULL)) &&
           ops->borrow != NULL &&
           ops->borrow->struct_size >= offsetof(cmeta_data_collection_borrow_ops, next) +
                                           sizeof(((cmeta_data_collection_borrow_ops *)0)->next) &&
           ops->borrow->abi_version == CMETA_DATA_COLLECTION_BORROW_OPS_ABI_VERSION &&
           ops->borrow->next != NULL && element != NULL && element->storage_type != NULL &&
           cmeta_data_value_copy_supported_depth(element, depth + 1u) &&
           cmeta_type_require_traits(element->storage_type, CMETA_TRAIT_COPY | CMETA_TRAIT_MOVE |
                                                                CMETA_TRAIT_DESTROY) == CMETA_OK;
  }
  case CMETA_DATA_MAP: {
    const cmeta_data_map_ops *ops = cmeta_data_map_ops_of(desc);
    const cmeta_data_desc *key = cmeta_data_map_key_data(desc);
    const cmeta_data_desc *value = cmeta_data_map_value_data(desc);
    return ops != NULL &&
           ops->struct_size >=
               offsetof(cmeta_data_map_ops, borrow) + sizeof(((cmeta_data_map_ops *)0)->borrow) &&
           ops->collector != NULL && ops->accept != NULL && ops->borrow != NULL &&
           ops->borrow->struct_size >= offsetof(cmeta_data_map_borrow_ops, next) +
                                           sizeof(((cmeta_data_map_borrow_ops *)0)->next) &&
           ops->borrow->abi_version == CMETA_DATA_MAP_BORROW_OPS_ABI_VERSION &&
           ops->borrow->next != NULL && key != NULL && value != NULL && key->storage_type != NULL &&
           value->storage_type != NULL && cmeta_data_value_copy_supported_depth(key, depth + 1u) &&
           cmeta_data_value_copy_supported_depth(value, depth + 1u) &&
           cmeta_type_require_traits(key->storage_type, CMETA_TRAIT_COPY | CMETA_TRAIT_MOVE |
                                                            CMETA_TRAIT_DESTROY) == CMETA_OK &&
           cmeta_type_require_traits(value->storage_type, CMETA_TRAIT_COPY | CMETA_TRAIT_MOVE |
                                                              CMETA_TRAIT_DESTROY) == CMETA_OK;
  }
  default:
    return false;
  }
}

bool cmeta_data_value_copy_supported(const cmeta_data_desc *desc) {
  return cmeta_data_value_copy_supported_depth(desc, 0u);
}

static cmeta_status cmeta_data_struct_copy(const cmeta_data_desc *desc, void *destination,
                                           const void *source) {
  const cmeta_data_struct_shape *shape = (const cmeta_data_struct_shape *)desc->shape;
  size_t i;
  for (i = 0u; i < shape->field_count; ++i) {
    const cmeta_data_field_desc *field = &shape->fields[i];
    cmeta_status status =
        cmeta_data_value_copy(field->value, (unsigned char *)destination + field->offset,
                              (const unsigned char *)source + field->offset);
    if (status != CMETA_OK) {
      while (i != 0u) {
        const cmeta_data_field_desc *copied = &shape->fields[--i];
        (void)cmeta_data_value_restore_zero(copied->value,
                                            (unsigned char *)destination + copied->offset);
      }
      return status;
    }
  }
  return CMETA_OK;
}

static cmeta_status cmeta_data_variant_copy(const cmeta_data_desc *desc, void *destination,
                                            const void *source) {
  const cmeta_data_variant_shape *shape = (const cmeta_data_variant_shape *)desc->shape;
  const cmeta_data_variant_case *active_case;
  bool source_zero = false;
  int64_t tag = 0;
  cmeta_status status = cmeta_data_variant_is_zero(desc, source, &source_zero);
  if (status != CMETA_OK) return status;
  if (source_zero) return CMETA_OK;
  status = cmeta_data_variant_active_tag(desc, source, &tag);
  if (status != CMETA_OK) return status;
  active_case = cmeta_data_variant_case_by_tag(shape, tag);
  if (active_case == NULL) return CMETA_CALLBACK_ERROR;
  status = cmeta_data_variant_select(desc, destination, tag);
  if (status != CMETA_OK) return status;
  status =
      cmeta_data_value_copy(active_case->value, (unsigned char *)destination + active_case->offset,
                            (const unsigned char *)source + active_case->offset);
  if (status != CMETA_OK) (void)cmeta_data_variant_restore_zero(desc, destination);
  return status;
}

static cmeta_status cmeta_data_collection_copy(const cmeta_data_desc *desc, void *destination,
                                               const void *source) {
  const cmeta_data_desc *static_element = cmeta_data_collection_element_data(desc);
  cmeta_data_collection_borrow_cursor cursor = {0};
  cmeta_collector collector = {0};
  cmeta_status status;
  bool begun = false;

  status = cmeta_data_collection_borrow_begin(desc, source, &cursor);
  if (status != CMETA_OK) return status;
  if (static_element == NULL || !cmeta_data_desc_equal(static_element, cursor.element))
    return CMETA_CALLBACK_ERROR;

  status = cmeta_data_collection_collector(desc, destination, SIZE_MAX, &collector);
  if (status != CMETA_OK) return status;
  status = cmeta_collector_begin(&collector);
  if (status != CMETA_OK) return status;
  begun = true;

  for (;;) {
    const void *element = NULL;
    cmeta_gen_status generated = cmeta_data_collection_borrow_next(&cursor, &element);
    if (generated == CMETA_GEN_DONE) break;
    if (generated != CMETA_GEN_VALUE && generated != CMETA_GEN_VALUE_AND_DONE) {
      status = CMETA_CALLBACK_ERROR;
      goto done;
    }
    status = cmeta_data_collection_accept(desc, &collector, cursor.element, element);
    if (status != CMETA_OK) goto done;
    if (generated == CMETA_GEN_VALUE_AND_DONE) break;
  }

  status = cmeta_collector_finish(&collector);
  begun = false;

done:
  if (begun) cmeta_collector_abort(&collector);
  return status;
}

static cmeta_status cmeta_data_map_copy(const cmeta_data_desc *desc, void *destination,
                                        const void *source) {
  const cmeta_data_desc *static_key = cmeta_data_map_key_data(desc);
  const cmeta_data_desc *static_value = cmeta_data_map_value_data(desc);
  cmeta_data_map_borrow_cursor cursor = {0};
  cmeta_collector collector = {0};
  cmeta_status status;
  bool begun = false;

  status = cmeta_data_map_borrow_begin(desc, source, &cursor);
  if (status != CMETA_OK) return status;
  if (static_key == NULL || static_value == NULL ||
      !cmeta_data_desc_equal(static_key, cursor.key) ||
      !cmeta_data_desc_equal(static_value, cursor.value))
    return CMETA_CALLBACK_ERROR;

  status = cmeta_data_map_collector(desc, destination, SIZE_MAX, &collector);
  if (status != CMETA_OK) return status;
  status = cmeta_collector_begin(&collector);
  if (status != CMETA_OK) return status;
  begun = true;

  for (;;) {
    const void *key = NULL;
    const void *value = NULL;
    cmeta_gen_status generated = cmeta_data_map_borrow_next(&cursor, &key, &value);
    if (generated == CMETA_GEN_DONE) break;
    if (generated != CMETA_GEN_VALUE && generated != CMETA_GEN_VALUE_AND_DONE) {
      status = CMETA_CALLBACK_ERROR;
      goto done;
    }
    status = cmeta_data_map_accept(desc, &collector, cursor.key, key, cursor.value, value);
    if (status != CMETA_OK) goto done;
    if (generated == CMETA_GEN_VALUE_AND_DONE) break;
  }

  status = cmeta_collector_finish(&collector);
  begun = false;

done:
  if (begun) cmeta_collector_abort(&collector);
  return status;
}

cmeta_status cmeta_data_value_copy(const cmeta_data_desc *desc, void *destination,
                                   const void *source) {
  if (!cmeta_data_desc_valid(desc) || destination == NULL || source == NULL ||
      destination == source || desc->storage_type == NULL)
    return CMETA_INVALID_ARGUMENT;

  if (cmeta_data_fixed_ops_of(desc) != NULL)
    return cmeta_data_fixed_copy(desc, destination, source, desc->storage_type->size);

  switch (desc->kind) {
  case CMETA_DATA_BOOL:
  case CMETA_DATA_SINT:
  case CMETA_DATA_UINT:
  case CMETA_DATA_FLOAT:
    memcpy(destination, source, desc->storage_type->size);
    return CMETA_OK;
  case CMETA_DATA_STRING:
  case CMETA_DATA_BYTES: {
    const unsigned char *bytes = NULL;
    size_t size = 0u;
    cmeta_status status = cmeta_data_buffer_read(desc, source, SIZE_MAX, &bytes, &size);
    if (status != CMETA_OK) return status;
    return cmeta_data_buffer_assign(desc, destination, bytes, size, SIZE_MAX);
  }
  case CMETA_DATA_ENUM:
    if (cmeta_data_enum_bits_ops_of(desc) != NULL) {
      uint64_t bits = 0u;
      cmeta_status status = cmeta_data_enum_read_bits(desc, source, &bits);
      return status == CMETA_OK ? cmeta_data_enum_assign_bits(desc, destination, bits) : status;
    } else {
      int64_t value = 0;
      cmeta_status status = cmeta_data_enum_read(desc, source, &value);
      return status == CMETA_OK ? cmeta_data_enum_assign(desc, destination, value) : status;
    }
  case CMETA_DATA_STRUCT:
    if (!cmeta_data_value_copy_supported(desc)) return CMETA_TRAIT_MISSING;
    return cmeta_data_struct_copy(desc, destination, source);
  case CMETA_DATA_VARIANT:
    if (!cmeta_data_value_copy_supported(desc)) return CMETA_TRAIT_MISSING;
    return cmeta_data_variant_copy(desc, destination, source);
  case CMETA_DATA_SEQUENCE:
  case CMETA_DATA_SET:
    if (!cmeta_data_value_copy_supported(desc)) return CMETA_TRAIT_MISSING;
    return cmeta_data_collection_copy(desc, destination, source);
  case CMETA_DATA_MAP:
    if (!cmeta_data_value_copy_supported(desc)) return CMETA_TRAIT_MISSING;
    return cmeta_data_map_copy(desc, destination, source);
  default:
    return CMETA_TRAIT_MISSING;
  }
}

bool cmeta_data_value_traits_supported(const cmeta_data_desc *desc) {
  return cmeta_data_value_copy_supported(desc) && cmeta_data_value_move_supported(desc);
}

bool cmeta_data_trait_copy_construct(const cmeta_data_desc *desc, void *destination,
                                     const void *source) {
  cmeta_status status;
  if (!cmeta_data_value_traits_supported(desc) || destination == NULL || source == NULL ||
      destination == source)
    return false;
  status = cmeta_data_value_init_zero(desc, destination);
  if (status != CMETA_OK) return false;
  status = cmeta_data_value_copy(desc, destination, source);
  if (status == CMETA_OK) return true;
  (void)cmeta_data_value_restore_zero(desc, destination);
  return false;
}

void cmeta_data_trait_move_construct(const cmeta_data_desc *desc, void *destination, void *source) {
  cmeta_status status;
  if (!cmeta_data_value_traits_supported(desc) || destination == NULL || source == NULL ||
      destination == source)
    abort();
  status = cmeta_data_value_init_zero(desc, destination);
  if (status != CMETA_OK) abort();
  status = cmeta_data_value_move(desc, destination, source);
  if (status != CMETA_OK) {
    (void)cmeta_data_value_restore_zero(desc, destination);
    abort();
  }
}

void cmeta_data_trait_destroy(const cmeta_data_desc *desc, void *object) {
  if (!cmeta_data_value_traits_supported(desc) || object == NULL ||
      cmeta_data_value_restore_zero(desc, object) != CMETA_OK)
    abort();
}

bool cmeta_data_value_move_supported(const cmeta_data_desc *desc) {
  return cmeta_data_value_move_supported_depth(desc, 0u);
}

cmeta_status cmeta_data_value_move(const cmeta_data_desc *desc, void *destination, void *source) {
  cmeta_status status;
  if (!cmeta_data_desc_valid(desc) || destination == NULL || source == NULL ||
      destination == source || desc->storage_type == NULL)
    return CMETA_INVALID_ARGUMENT;
  switch (desc->kind) {
  case CMETA_DATA_BOOL:
  case CMETA_DATA_SINT:
  case CMETA_DATA_UINT:
  case CMETA_DATA_FLOAT:
    memcpy(destination, source, desc->storage_type->size);
    memset(source, 0, desc->storage_type->size);
    return CMETA_OK;
  case CMETA_DATA_STRING:
  case CMETA_DATA_BYTES:
    return cmeta_data_buffer_move(desc, destination, source);
  case CMETA_DATA_STRUCT:
    return cmeta_data_struct_move(desc, destination, source);
  case CMETA_DATA_VARIANT:
    return cmeta_data_variant_move(desc, destination, source);
  default:
    break;
  }
  status = cmeta_data_construct_move(desc, destination, source);
  return status;
}

static cmeta_status cmeta_data_temp_init(const cmeta_data_desc *desc, void *storage,
                                         cmeta_data_temp_lifecycle *lifecycle) {
  cmeta_status status;
  if (desc == NULL || storage == NULL || lifecycle == NULL) return CMETA_INVALID_ARGUMENT;
  status = cmeta_data_value_init_zero(desc, storage);
  if (status != CMETA_OK) return status;
  if (desc->fixed_ops != NULL) {
    *lifecycle = CMETA_DATA_TEMP_FIXED;
    return CMETA_OK;
  }
  if (desc->variant_ops != NULL) {
    *lifecycle = CMETA_DATA_TEMP_VARIANT;
    return CMETA_OK;
  }
  switch (desc->kind) {
  case CMETA_DATA_BOOL:
  case CMETA_DATA_SINT:
  case CMETA_DATA_UINT:
  case CMETA_DATA_FLOAT:
    *lifecycle = CMETA_DATA_TEMP_TRIVIAL;
    break;
  case CMETA_DATA_STRING:
  case CMETA_DATA_BYTES:
    *lifecycle = CMETA_DATA_TEMP_BUFFER;
    break;
  case CMETA_DATA_ENUM:
    *lifecycle = desc->enum_bits_ops != NULL ? CMETA_DATA_TEMP_ENUM_BITS : CMETA_DATA_TEMP_ENUM;
    break;
  default:
    *lifecycle = CMETA_DATA_TEMP_CONSTRUCT;
    break;
  }
  return CMETA_OK;
}

cmeta_status cmeta_data_temp_open(const cmeta_data_desc *desc, size_t max_bytes,
                                  cmeta_data_temp *out) {
  void *storage;
  size_t extent;
  size_t alignment;
  size_t padded;
  cmeta_status status;
  cmeta_data_temp_lifecycle lifecycle = CMETA_DATA_TEMP_NONE;

  if (out == NULL || !cmeta_data_desc_valid(desc) || desc->storage_type == NULL)
    return CMETA_INVALID_ARGUMENT;
  *out = (cmeta_data_temp){0};
  if (cmeta_data_reflection_is_view(desc)) return CMETA_TRAIT_MISSING;
  extent = desc->storage_type->size;
  alignment = desc->storage_type->align;
  if (extent == 0u || alignment == 0u || extent > max_bytes)
    return extent > max_bytes ? CMETA_CAPACITY_EXCEEDED : CMETA_INVALID_ARGUMENT;
  if ((alignment & (alignment - 1u)) != 0u) return CMETA_INVALID_ARGUMENT;
  if (alignment <= CMETA_ALIGNOF(cmeta_capture_storage)) {
    padded = extent;
    storage = malloc(padded);
  } else {
    if (extent > SIZE_MAX - (alignment - 1u)) return CMETA_CAPACITY_EXCEEDED;
    padded = (extent + alignment - 1u) & ~(alignment - 1u);
#if defined(_WIN32)
    storage = _aligned_malloc(padded, alignment);
#elif defined(__wasm__)
    /* A Wasm C11 libc need not expose POSIX. padded is an alignment multiple,
     * and the result has the ordinary free() lifetime used below. */
    storage = aligned_alloc(alignment, padded);
#else
    storage = NULL;
    if (posix_memalign(&storage, alignment, padded) != 0) storage = NULL;
#endif
  }
  if (storage == NULL) return CMETA_OUT_OF_MEMORY;
  memset(storage, 0, padded);
  status = cmeta_data_temp_init(desc, storage, &lifecycle);
  if (status != CMETA_OK) {
#if defined(_WIN32)
    if (alignment > CMETA_ALIGNOF(cmeta_capture_storage)) _aligned_free(storage);
    else free(storage);
#else
    free(storage);
#endif
    return status;
  }
  out->data = desc;
  out->storage = storage;
  out->extent = extent;
  out->alignment = alignment;
  out->lifecycle = lifecycle;
  return CMETA_OK;
}

void cmeta_data_temp_close(cmeta_data_temp *temp) {
  if (temp == NULL || temp->storage == NULL) return;
  if (temp->data != NULL) (void)cmeta_data_value_restore_zero(temp->data, temp->storage);
#if defined(_WIN32)
  if (temp->alignment > CMETA_ALIGNOF(cmeta_capture_storage)) _aligned_free(temp->storage);
  else free(temp->storage);
#else
  free(temp->storage);
#endif
  *temp = (cmeta_data_temp){0};
}

cmeta_status cmeta_data_map_accept(const cmeta_data_desc *desc, cmeta_collector *collector,
                                   const cmeta_data_desc *key_data, const void *key,
                                   const cmeta_data_desc *value_data, const void *value) {
  const cmeta_data_map_ops *ops = NULL;
  const cmeta_data_desc *expected_key;
  const cmeta_data_desc *expected_value;
  cmeta_status status;
  if (collector == NULL || key_data == NULL || key == NULL || value_data == NULL || value == NULL)
    return CMETA_INVALID_ARGUMENT;
  status = cmeta_data_map_ops_status(desc, &ops);
  if (status != CMETA_OK) return status;
  expected_key = ops->key(collector->zero_output);
  expected_value = ops->value(collector->zero_output);
  if (expected_key == NULL || expected_value == NULL || !cmeta_data_desc_valid(expected_key) ||
      !cmeta_data_desc_valid(expected_value))
    return CMETA_TRAIT_MISSING;
  if (!cmeta_data_desc_equal(expected_key, key_data) ||
      !cmeta_data_desc_equal(expected_value, value_data))
    return CMETA_TYPE_MISMATCH;
  if (ops->struct_size <
          offsetof(cmeta_data_map_ops, accept) + sizeof(((cmeta_data_map_ops *)0)->accept) ||
      ops->accept == NULL)
    return CMETA_TRAIT_MISSING;
  return ops->accept(collector, key_data, key, value_data, value);
}
