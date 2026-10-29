#ifndef CMETA_VECTOR_H
#define CMETA_VECTOR_H

#include <cmeta/cmeta.h>
#include <salts/simd_types.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Reflection belongs to CMeta; the carrier and execution shape belong to
 * Salts::SIMD. Both pointers borrow immutable metadata and must remain valid
 * while the descriptor is used. Native shape equality is by fields, never by
 * descriptor address across static archives or DSOs. */
typedef struct cmeta_vector_desc {
    const cmeta_type_desc *type;
    const salts_simd_desc *native_shape;
} cmeta_vector_desc;

extern const cmeta_type_desc cmeta_type_i8x16;
extern const cmeta_type_desc cmeta_type_u8x16;
extern const cmeta_type_desc cmeta_type_i16x8;
extern const cmeta_type_desc cmeta_type_u16x8;
extern const cmeta_type_desc cmeta_type_i32x4;
extern const cmeta_type_desc cmeta_type_u32x4;
extern const cmeta_type_desc cmeta_type_i64x2;
extern const cmeta_type_desc cmeta_type_u64x2;
extern const cmeta_type_desc cmeta_type_f32x4;
extern const cmeta_type_desc cmeta_type_f64x2;
extern const cmeta_type_desc cmeta_type_b8x16;
extern const cmeta_type_desc cmeta_type_b16x8;
extern const cmeta_type_desc cmeta_type_b32x4;
extern const cmeta_type_desc cmeta_type_b64x2;

extern const cmeta_vector_desc cmeta_vector_i8x16;
extern const cmeta_vector_desc cmeta_vector_u8x16;
extern const cmeta_vector_desc cmeta_vector_i16x8;
extern const cmeta_vector_desc cmeta_vector_u16x8;
extern const cmeta_vector_desc cmeta_vector_i32x4;
extern const cmeta_vector_desc cmeta_vector_u32x4;
extern const cmeta_vector_desc cmeta_vector_i64x2;
extern const cmeta_vector_desc cmeta_vector_u64x2;
extern const cmeta_vector_desc cmeta_vector_f32x4;
extern const cmeta_vector_desc cmeta_vector_f64x2;
extern const cmeta_vector_desc cmeta_vector_b8x16;
extern const cmeta_vector_desc cmeta_vector_b16x8;
extern const cmeta_vector_desc cmeta_vector_b32x4;
extern const cmeta_vector_desc cmeta_vector_b64x2;

bool cmeta_vector_desc_valid(const cmeta_vector_desc *desc);
const cmeta_vector_desc *cmeta_vector_desc_for_type(const cmeta_type_desc *type);
bool cmeta_type_is_vector(const cmeta_type_desc *type);
bool cmeta_type_is_mask(const cmeta_type_desc *type);

#ifdef __cplusplus
}
#endif

#endif /* CMETA_VECTOR_H */
