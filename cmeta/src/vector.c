#include <cmeta/vector.h>

#include <stddef.h>

static const cmeta_type_traits cmeta_vector_storage_traits = {
    .flags = CMETA_TRAIT_TRIVIAL_COPY | CMETA_TRAIT_TRIVIAL_DESTROY
};

#define CMETA_DEFINE_VECTOR(symbol, stable_id, display_name) \
    static const cmeta_type_identity cmeta_id_##symbol = \
        CMETA_TYPE_ID_ATOM_INIT(stable_id); \
    const cmeta_type_desc cmeta_type_##symbol = { \
        .name = display_name, \
        .size = sizeof(salts_simd_v128), \
        .align = _Alignof(salts_simd_v128), \
        .kind = CMETA_T_OBJECT, \
        .pointee = NULL, \
        .traits = &cmeta_vector_storage_traits, \
        .identity = &cmeta_id_##symbol \
    }; \
    const cmeta_vector_desc cmeta_vector_##symbol = { \
        .type = &cmeta_type_##symbol, \
        .native_shape = &salts_simd_##symbol \
    }

CMETA_DEFINE_VECTOR(i8x16,  "cmeta.vector.i8x16",  "i8x16");
CMETA_DEFINE_VECTOR(u8x16,  "cmeta.vector.u8x16",  "u8x16");
CMETA_DEFINE_VECTOR(i16x8,  "cmeta.vector.i16x8",  "i16x8");
CMETA_DEFINE_VECTOR(u16x8,  "cmeta.vector.u16x8",  "u16x8");
CMETA_DEFINE_VECTOR(i32x4,  "cmeta.vector.i32x4",  "i32x4");
CMETA_DEFINE_VECTOR(u32x4,  "cmeta.vector.u32x4",  "u32x4");
CMETA_DEFINE_VECTOR(i64x2,  "cmeta.vector.i64x2",  "i64x2");
CMETA_DEFINE_VECTOR(u64x2,  "cmeta.vector.u64x2",  "u64x2");
CMETA_DEFINE_VECTOR(f32x4,  "cmeta.vector.f32x4",  "f32x4");
CMETA_DEFINE_VECTOR(f64x2,  "cmeta.vector.f64x2",  "f64x2");
CMETA_DEFINE_VECTOR(b8x16,  "cmeta.mask.b8x16",    "b8x16");
CMETA_DEFINE_VECTOR(b16x8,  "cmeta.mask.b16x8",    "b16x8");
CMETA_DEFINE_VECTOR(b32x4,  "cmeta.mask.b32x4",    "b32x4");
CMETA_DEFINE_VECTOR(b64x2,  "cmeta.mask.b64x2",    "b64x2");

#undef CMETA_DEFINE_VECTOR

static const cmeta_vector_desc *const cmeta_vectors[] = {
    &cmeta_vector_i8x16, &cmeta_vector_u8x16,
    &cmeta_vector_i16x8, &cmeta_vector_u16x8,
    &cmeta_vector_i32x4, &cmeta_vector_u32x4,
    &cmeta_vector_i64x2, &cmeta_vector_u64x2,
    &cmeta_vector_f32x4, &cmeta_vector_f64x2,
    &cmeta_vector_b8x16, &cmeta_vector_b16x8,
    &cmeta_vector_b32x4, &cmeta_vector_b64x2
};

bool cmeta_vector_desc_valid(const cmeta_vector_desc *desc) {
    if (desc == NULL || desc->type == NULL || !cmeta_type_desc_valid(desc->type))
        return false;
    if (desc->type->kind != CMETA_T_OBJECT)
        return false;
    return salts_simd_desc_valid(desc->native_shape);
}

const cmeta_vector_desc *cmeta_vector_desc_for_type(const cmeta_type_desc *type) {
    size_t index;
    if (type == NULL) return NULL;
    for (index = 0u; index < sizeof(cmeta_vectors) / sizeof(cmeta_vectors[0]); ++index)
        if (cmeta_type_equal(type, cmeta_vectors[index]->type))
            return cmeta_vectors[index];
    return NULL;
}

bool cmeta_type_is_vector(const cmeta_type_desc *type) {
    const cmeta_vector_desc *desc = cmeta_vector_desc_for_type(type);
    return desc != NULL && !desc->native_shape->is_mask;
}

bool cmeta_type_is_mask(const cmeta_type_desc *type) {
    const cmeta_vector_desc *desc = cmeta_vector_desc_for_type(type);
    return desc != NULL && desc->native_shape->is_mask;
}
