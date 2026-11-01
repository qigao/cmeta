#include <cmeta/core_data.h>

#include <string.h>

static int cmeta_uuid_cmeta_hex_value(unsigned char value) {
  if (value >= (unsigned char)'0' && value <= (unsigned char)'9')
    return (int)(value - (unsigned char)'0');
  if (value >= (unsigned char)'a' && value <= (unsigned char)'f')
    return (int)(value - (unsigned char)'a') + 10;
  if (value >= (unsigned char)'A' && value <= (unsigned char)'F')
    return (int)(value - (unsigned char)'A') + 10;
  return -1;
}

static bool cmeta_uuid_cmeta_is_zero(const void *object) {
  static const cmeta_uuid_t zero = {{0}};
  return object != NULL &&
         memcmp(((const cmeta_uuid_t *)object)->bytes, zero.bytes, SALTS_UUID_SIZE) == 0;
}

static cmeta_status cmeta_uuid_cmeta_init_zero(void *object) {
  if (object == NULL) return CMETA_INVALID_ARGUMENT;
  memset(object, 0, sizeof(cmeta_uuid_t));
  return CMETA_OK;
}

static cmeta_status cmeta_uuid_cmeta_assign(void *object, const unsigned char *data, size_t size,
                                            size_t max_bytes) {
  static const size_t group_ends[] = {4u, 6u, 8u, 10u};
  cmeta_uuid_t parsed = {{0}};
  size_t text_index = 0u;
  size_t group_index = 0u;
  size_t byte_index;

  if (object == NULL || (size != 0u && data == NULL)) return CMETA_INVALID_ARGUMENT;
  if (size > max_bytes) return CMETA_CAPACITY_EXCEEDED;
  if (!cmeta_uuid_cmeta_is_zero(object)) return CMETA_INVALID_ARGUMENT;
  if (size != SALTS_UUID_STRING_LENGTH) return CMETA_INVALID_ARGUMENT;

  for (byte_index = 0u; byte_index < SALTS_UUID_SIZE; ++byte_index) {
    int high;
    int low;

    if (group_index < sizeof(group_ends) / sizeof(group_ends[0]) &&
        byte_index == group_ends[group_index]) {
      if (data[text_index] != (unsigned char)'-') return CMETA_INVALID_ARGUMENT;
      ++text_index;
      ++group_index;
    }

    high = cmeta_uuid_cmeta_hex_value(data[text_index++]);
    low = cmeta_uuid_cmeta_hex_value(data[text_index++]);
    if (high < 0 || low < 0) return CMETA_INVALID_ARGUMENT;
    parsed.bytes[byte_index] = (uint8_t)((high << 4) | low);
  }

  memcpy(object, &parsed, sizeof(parsed));
  return CMETA_OK;
}

static void cmeta_uuid_cmeta_restore_zero(void *object) {
  if (object != NULL) memset(object, 0, sizeof(cmeta_uuid_t));
}

static void cmeta_uuid_cmeta_move(void *destination, void *source) {
  if (destination == NULL || source == NULL) return;
  memcpy(destination, source, sizeof(cmeta_uuid_t));
  memset(source, 0, sizeof(cmeta_uuid_t));
}

static cmeta_status cmeta_uuid_cmeta_copy(void *destination, const void *source) {
  if (destination == NULL || source == NULL) return CMETA_INVALID_ARGUMENT;
  memcpy(destination, source, sizeof(cmeta_uuid_t));
  return CMETA_OK;
}

static const cmeta_type_identity cmeta_uuid_cmeta_identity = CMETA_TYPE_ID_ATOM_INIT("salts.uuid");

const cmeta_type_desc cmeta_uuid_cmeta_type = {
    "cmeta_uuid_t", sizeof(cmeta_uuid_t),      CMETA_ALIGNOF(cmeta_uuid_t), CMETA_T_OBJECT, NULL,
    NULL,           &cmeta_uuid_cmeta_identity};

const cmeta_data_buffer_shape cmeta_uuid_cmeta_shape = {CMETA_DATA_BUFFER_OWNED};

const cmeta_data_buffer_ops cmeta_uuid_cmeta_buffer_ops = {
    sizeof(cmeta_data_buffer_ops), CMETA_DATA_BUFFER_OPS_ABI_VERSION, &cmeta_uuid_cmeta_type,
    CMETA_DATA_BUFFER_OWNED,       cmeta_uuid_cmeta_is_zero,          cmeta_uuid_cmeta_assign,
    cmeta_uuid_cmeta_restore_zero, NULL, cmeta_uuid_cmeta_init_zero, cmeta_uuid_cmeta_move};

const cmeta_data_fixed_ops cmeta_uuid_cmeta_fixed_ops = {
    sizeof(cmeta_data_fixed_ops), CMETA_DATA_FIXED_OPS_ABI_VERSION, &cmeta_uuid_cmeta_type,
    sizeof(cmeta_uuid_t), cmeta_uuid_cmeta_is_zero, cmeta_uuid_cmeta_copy,
    cmeta_uuid_cmeta_restore_zero};

const cmeta_data_desc cmeta_uuid_cmeta_data = {sizeof(cmeta_data_desc),
                                                         CMETA_DATA_DESC_ABI_VERSION,
                                                         "salts.uuid.data",
                                                         "cmeta_uuid_t",
                                                         CMETA_DATA_STRING,
                                                         &cmeta_uuid_cmeta_type,
                                                         &cmeta_uuid_cmeta_shape,
                                                         &cmeta_uuid_cmeta_buffer_ops,
                                                         NULL,
                                                         NULL,
                                                         &cmeta_uuid_cmeta_fixed_ops,
                                                         NULL};

bool cmeta_uuid_cmeta_data_valid(const cmeta_data_desc *candidate) {
  const cmeta_data_buffer_ops *ops = cmeta_data_buffer_ops_of(candidate);
  const cmeta_data_fixed_ops *fixed_ops = cmeta_data_fixed_ops_of(candidate);

  if (ops == NULL || fixed_ops == NULL) return false;

  return cmeta_data_desc_equal(candidate, &cmeta_uuid_cmeta_data) &&
         candidate->storage_type->kind == cmeta_uuid_cmeta_type.kind &&
         candidate->storage_type->size == sizeof(cmeta_uuid_t) &&
         candidate->storage_type->align == CMETA_ALIGNOF(cmeta_uuid_t) &&
         ((const cmeta_data_buffer_shape *)candidate->shape)->ownership ==
             CMETA_DATA_BUFFER_OWNED &&
         ops->abi_version == CMETA_DATA_BUFFER_OPS_ABI_VERSION &&
         ops->ownership == CMETA_DATA_BUFFER_OWNED &&
         fixed_ops->extent == sizeof(cmeta_uuid_t);
}

static bool cmeta_tstr_cmeta_is_zero(const void *object) {
  return object != NULL && *(const tstr *)object == NULL;
}

static cmeta_status cmeta_tstr_cmeta_init_zero(void *object) {
  if (object == NULL) return CMETA_INVALID_ARGUMENT;
  *(tstr *)object = NULL;
  return CMETA_OK;
}

static cmeta_status cmeta_tstr_cmeta_read(
    const void *object, const unsigned char **out_data, size_t *out_size) {
  const tstr value = object != NULL ? *(const tstr *)object : NULL;
  if (object == NULL || out_data == NULL || out_size == NULL)
    return CMETA_INVALID_ARGUMENT;
  *out_data = (const unsigned char *)value;
  *out_size = tstr_len(value);
  return CMETA_OK;
}

static cmeta_status cmeta_tstr_cmeta_assign(
    void *object, const unsigned char *data, size_t size, size_t max_bytes) {
  tstr value;

  if (object == NULL || (size != 0u && data == NULL))
    return CMETA_INVALID_ARGUMENT;
  if (size > max_bytes)
    return CMETA_CAPACITY_EXCEEDED;
  if (*(tstr *)object != NULL)
    return CMETA_INVALID_ARGUMENT;
  if (size == 0u)
    return CMETA_OK;

  value = tstr_new_len(data, size);
  if (value == NULL)
    return CMETA_OUT_OF_MEMORY;
  *(tstr *)object = value;
  return CMETA_OK;
}

static void cmeta_tstr_cmeta_restore_zero(void *object) {
  if (object != NULL)
    tstr_freep((tstr *)object);
}

static void cmeta_tstr_cmeta_move(void *destination, void *source) {
  tstr *to = (tstr *)destination;
  tstr *from = (tstr *)source;
  if (to == NULL || from == NULL)
    return;
  *to = *from;
  *from = NULL;
}

static const cmeta_type_identity cmeta_tstr_cmeta_identity =
    CMETA_TYPE_ID_ATOM_INIT("salts.tstr");

CMETA_INLINE bool cmeta_tstr_cmeta_data_copy_construct(
    void *destination, const void *source) {
  return cmeta_data_trait_copy_construct(
      &cmeta_tstr_cmeta_data, destination, source);
}

CMETA_INLINE void cmeta_tstr_cmeta_data_move_construct(
    void *destination, void *source) {
  cmeta_data_trait_move_construct(
      &cmeta_tstr_cmeta_data, destination, source);
}

CMETA_INLINE void cmeta_tstr_cmeta_data_destroy(void *object) {
  cmeta_data_trait_destroy(&cmeta_tstr_cmeta_data, object);
}

static int cmeta_tstr_cmeta_compare(
    const void *left, const void *right) {
  return tstr_cmp(*(const tstr *)left, *(const tstr *)right);
}

CMETA_LOCAL const cmeta_type_traits cmeta_traits_cmeta_tstr = {
    .flags = CMETA_TRAIT_COPY | CMETA_TRAIT_MOVE | CMETA_TRAIT_DESTROY |
             CMETA_TRAIT_COMPARE,
    .equal = NULL,
    .hash = NULL,
    .compare = cmeta_tstr_cmeta_compare,
    .copy_construct = cmeta_tstr_cmeta_data_copy_construct,
    .move_construct = cmeta_tstr_cmeta_data_move_construct,
    .destroy = cmeta_tstr_cmeta_data_destroy
};

const cmeta_type_desc cmeta_tstr_cmeta_type = {
    "tstr", sizeof(tstr), CMETA_ALIGNOF(tstr), CMETA_T_OBJECT,
    NULL, &cmeta_traits_cmeta_tstr, &cmeta_tstr_cmeta_identity
};

const cmeta_data_buffer_shape cmeta_tstr_cmeta_shape = {
    CMETA_DATA_BUFFER_OWNED
};

const cmeta_data_buffer_ops cmeta_tstr_cmeta_buffer_ops = {
    sizeof(cmeta_data_buffer_ops), CMETA_DATA_BUFFER_OPS_ABI_VERSION,
    &cmeta_tstr_cmeta_type, CMETA_DATA_BUFFER_OWNED,
    cmeta_tstr_cmeta_is_zero, cmeta_tstr_cmeta_assign,
    cmeta_tstr_cmeta_restore_zero, cmeta_tstr_cmeta_read,
    cmeta_tstr_cmeta_init_zero, cmeta_tstr_cmeta_move
};

const cmeta_data_desc cmeta_tstr_cmeta_data = {
    sizeof(cmeta_data_desc), CMETA_DATA_DESC_ABI_VERSION,
    "salts.tstr.data", "tstr", CMETA_DATA_STRING,
    &cmeta_tstr_cmeta_type, &cmeta_tstr_cmeta_shape,
    &cmeta_tstr_cmeta_buffer_ops, NULL, NULL, NULL, NULL
};
