#ifndef CMETA_CORE_FIXED_WIDTH_H
#define CMETA_CORE_FIXED_WIDTH_H

#include <cmeta/data.h>

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#ifdef __cplusplus
  #define CMETA_CORE_FIXED_WIDTH_STATIC_ASSERT(condition_, message_)                              \
    static_assert((condition_), message_)
#else
  #define CMETA_CORE_FIXED_WIDTH_STATIC_ASSERT(condition_, message_)                              \
    _Static_assert((condition_), message_)
#endif

CMETA_CORE_FIXED_WIDTH_STATIC_ASSERT(CHAR_BIT == 8,
                                      "fixed-width CMeta metadata requires 8-bit bytes");
CMETA_CORE_FIXED_WIDTH_STATIC_ASSERT(sizeof(int8_t) * CHAR_BIT == 8u,
                                      "int8_t must have exactly 8 bits");
CMETA_CORE_FIXED_WIDTH_STATIC_ASSERT(sizeof(uint8_t) * CHAR_BIT == 8u,
                                      "uint8_t must have exactly 8 bits");
CMETA_CORE_FIXED_WIDTH_STATIC_ASSERT(sizeof(int16_t) * CHAR_BIT == 16u,
                                      "int16_t must have exactly 16 bits");
CMETA_CORE_FIXED_WIDTH_STATIC_ASSERT(sizeof(uint16_t) * CHAR_BIT == 16u,
                                      "uint16_t must have exactly 16 bits");
CMETA_CORE_FIXED_WIDTH_STATIC_ASSERT(sizeof(int32_t) * CHAR_BIT == 32u,
                                      "int32_t must have exactly 32 bits");
CMETA_CORE_FIXED_WIDTH_STATIC_ASSERT(sizeof(uint32_t) * CHAR_BIT == 32u,
                                      "uint32_t must have exactly 32 bits");
CMETA_CORE_FIXED_WIDTH_STATIC_ASSERT(sizeof(int64_t) * CHAR_BIT == 64u,
                                      "int64_t must have exactly 64 bits");
CMETA_CORE_FIXED_WIDTH_STATIC_ASSERT(sizeof(uint64_t) * CHAR_BIT == 64u,
                                      "uint64_t must have exactly 64 bits");

#undef CMETA_CORE_FIXED_WIDTH_STATIC_ASSERT

static const cmeta_type_identity cmeta_bool8_cmeta_identity =
    CMETA_TYPE_ID_ATOM_INIT("salts.bool8");
static inline bool cmeta_bool8_cmeta_copy_construct(void *, const void *);
static inline void cmeta_bool8_cmeta_move(void *, void *);
static inline void cmeta_bool8_cmeta_restore_zero(void *);
static const cmeta_type_traits cmeta_bool8_cmeta_traits = {
    CMETA_TRAIT_COPY | CMETA_TRAIT_MOVE | CMETA_TRAIT_DESTROY,
    NULL, NULL, NULL, cmeta_bool8_cmeta_copy_construct,
    cmeta_bool8_cmeta_move, cmeta_bool8_cmeta_restore_zero
};
static const cmeta_type_desc cmeta_bool8_cmeta_type = {
    "uint8_t", sizeof(uint8_t), CMETA_ALIGNOF(uint8_t), CMETA_T_INTEGER,
    NULL, &cmeta_bool8_cmeta_traits, &cmeta_bool8_cmeta_identity
};

static inline bool cmeta_bool8_cmeta_is_zero(const void *object) {
  return object != NULL && *(const uint8_t *)object == 0u;
}

static inline cmeta_status cmeta_bool8_cmeta_copy(void *destination,
                                                  const void *source) {
  uint8_t value;
  if (destination == NULL || source == NULL)
    return CMETA_INVALID_ARGUMENT;
  value = *(const uint8_t *)source;
  if (value > 1u) return CMETA_INVALID_ARGUMENT;
  *(uint8_t *)destination = value;
  return CMETA_OK;
}

static inline void cmeta_bool8_cmeta_restore_zero(void *object) {
  if (object != NULL)
    *(uint8_t *)object = 0u;
}

static inline bool cmeta_bool8_cmeta_copy_construct(void *destination,
                                                    const void *source) {
  return cmeta_bool8_cmeta_copy(destination, source) == CMETA_OK;
}

static inline void cmeta_bool8_cmeta_move(void *destination, void *source) {
  if (destination == source) return;
  /* Move is no-fail only for a valid Bool value; invalid octets violate the
   * provider contract instead of being copied into another owner. */
  if (cmeta_bool8_cmeta_copy(destination, source) != CMETA_OK) abort();
  cmeta_bool8_cmeta_restore_zero(source);
}

/** Canonical octet-backed Bool provider for schemas with an 8-bit native ABI. */
static const cmeta_data_fixed_ops cmeta_bool8_cmeta_fixed_ops = {
    sizeof(cmeta_data_fixed_ops), CMETA_DATA_FIXED_OPS_ABI_VERSION,
    &cmeta_bool8_cmeta_type, sizeof(uint8_t), cmeta_bool8_cmeta_is_zero,
    cmeta_bool8_cmeta_copy, cmeta_bool8_cmeta_restore_zero
};

static const cmeta_data_desc cmeta_bool8_cmeta_data = {
    sizeof(cmeta_data_desc), CMETA_DATA_DESC_ABI_VERSION,
    "salts.bool8.data", "uint8_t Bool", CMETA_DATA_BOOL,
    &cmeta_bool8_cmeta_type, NULL, NULL, NULL, NULL,
    &cmeta_bool8_cmeta_fixed_ops, NULL, NULL, NULL, NULL
};

/*
 * Exact-width integer authority lives in CMeta core. Keep no Salts-private
 * duplicate descriptors here: consumers use cmeta_type_int32 /
 * cmeta_data_int32 (and the corresponding widths/signs) directly.
 */


#endif /* CMETA_CORE_FIXED_WIDTH_H */
