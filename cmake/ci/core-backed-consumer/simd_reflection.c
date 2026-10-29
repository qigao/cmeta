/* Independent installed C11 reflection-to-native-SIMD consumer. */
#include <cmeta/vector.h>
#include <salts/simd.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define SIMD_CHECK(expr) do { if (!(expr)) { \
  fprintf(stderr, "Installed SIMD reflection C11 line %d: %s\n", __LINE__, #expr); \
  return 1; \
} } while (0)

typedef struct vector_case {
  const cmeta_vector_desc *reflected;
  const salts_simd_desc *native;
  const char *name;
  const char *stable_id;
} vector_case;

static bool same_shape(const salts_simd_desc *a, const salts_simd_desc *b) {
  return a != NULL && b != NULL && a->lane_kind == b->lane_kind &&
         a->lane_bits == b->lane_bits && a->lane_count == b->lane_count &&
         a->is_mask == b->is_mask;
}

int main(void) {
  const vector_case cases[] = {
    {&cmeta_vector_i8x16, &salts_simd_i8x16, "i8x16", "cmeta.vector.i8x16"},
    {&cmeta_vector_u8x16, &salts_simd_u8x16, "u8x16", "cmeta.vector.u8x16"},
    {&cmeta_vector_i16x8, &salts_simd_i16x8, "i16x8", "cmeta.vector.i16x8"},
    {&cmeta_vector_u16x8, &salts_simd_u16x8, "u16x8", "cmeta.vector.u16x8"},
    {&cmeta_vector_i32x4, &salts_simd_i32x4, "i32x4", "cmeta.vector.i32x4"},
    {&cmeta_vector_u32x4, &salts_simd_u32x4, "u32x4", "cmeta.vector.u32x4"},
    {&cmeta_vector_i64x2, &salts_simd_i64x2, "i64x2", "cmeta.vector.i64x2"},
    {&cmeta_vector_u64x2, &salts_simd_u64x2, "u64x2", "cmeta.vector.u64x2"},
    {&cmeta_vector_f32x4, &salts_simd_f32x4, "f32x4", "cmeta.vector.f32x4"},
    {&cmeta_vector_f64x2, &salts_simd_f64x2, "f64x2", "cmeta.vector.f64x2"},
    {&cmeta_vector_b8x16, &salts_simd_b8x16, "b8x16", "cmeta.mask.b8x16"},
    {&cmeta_vector_b16x8, &salts_simd_b16x8, "b16x8", "cmeta.mask.b16x8"},
    {&cmeta_vector_b32x4, &salts_simd_b32x4, "b32x4", "cmeta.mask.b32x4"},
    {&cmeta_vector_b64x2, &salts_simd_b64x2, "b64x2", "cmeta.mask.b64x2"}
  };
  const cmeta_type_traits *traits;
  salts_simd_desc shape;
  cmeta_type_desc type;
  cmeta_vector_desc reflected;
  cmeta_v128 left = {{0}}, right = {{0}}, result = {{0}}, sentinel = {{0}};
  int32_t lanes[4] = {0};
  size_t i;

  SIMD_CHECK(sizeof(salts_simd_v128) == 16u && _Alignof(salts_simd_v128) == 16u);
  for (i = 0u; i < sizeof(cases) / sizeof(cases[0]); ++i) {
    const vector_case *row = &cases[i];
    const cmeta_type_desc *found = cmeta_builtin_type_find(row->name);
    const cmeta_vector_desc *mapped;
    cmeta_type_identity identity;
    char stable_id[64];
    SIMD_CHECK(cmeta_vector_desc_valid(row->reflected));
    SIMD_CHECK(same_shape(row->reflected->native_shape, row->native));
    SIMD_CHECK(cmeta_type_equal(found, row->reflected->type));
    SIMD_CHECK(strcmp(row->reflected->type->name, row->name) == 0);
    SIMD_CHECK(row->reflected->type->identity != NULL);
    SIMD_CHECK(strcmp(row->reflected->type->identity->stable_atom_id,
                      row->stable_id) == 0);
    SIMD_CHECK(row->reflected->type->size == sizeof(salts_simd_v128));
    SIMD_CHECK(row->reflected->type->align == _Alignof(salts_simd_v128));
    traits = row->reflected->type->traits;
    SIMD_CHECK(traits != NULL && traits->flags ==
               (CMETA_TRAIT_TRIVIAL_COPY | CMETA_TRAIT_TRIVIAL_DESTROY));
    SIMD_CHECK(cmeta_type_is_mask(found) == row->native->is_mask);
    SIMD_CHECK(cmeta_type_is_vector(found) == !row->native->is_mask);

    /* The type, identity, stable ID bytes and native shape have different
     * addresses. Equality and the native mapping must remain semantic. */
    SIMD_CHECK(strlen(row->stable_id) < sizeof(stable_id));
    memcpy(stable_id, row->stable_id, strlen(row->stable_id) + 1u);
    identity = *row->reflected->type->identity;
    identity.stable_atom_id = stable_id;
    type = *row->reflected->type;
    type.identity = &identity;
    shape = *row->native;
    reflected = (cmeta_vector_desc){&type, &shape};
    SIMD_CHECK(cmeta_vector_desc_valid(&reflected));
    SIMD_CHECK(cmeta_type_equal(&type, row->reflected->type));
    mapped = cmeta_vector_desc_for_type(&type);
    SIMD_CHECK(mapped != NULL && same_shape(mapped->native_shape, &shape));
  }

  SIMD_CHECK(!cmeta_vector_desc_valid(NULL));
  SIMD_CHECK(cmeta_vector_desc_for_type(NULL) == NULL);
  SIMD_CHECK(!cmeta_type_is_vector(NULL) && !cmeta_type_is_mask(NULL));
  type = cmeta_type_i32x4;
  shape = salts_simd_i32x4;
  reflected = (cmeta_vector_desc){NULL, &shape};
  SIMD_CHECK(!cmeta_vector_desc_valid(&reflected));
  reflected = (cmeta_vector_desc){&type, NULL};
  SIMD_CHECK(!cmeta_vector_desc_valid(&reflected));
  reflected.native_shape = &shape;
  type.name = NULL;
  SIMD_CHECK(!cmeta_vector_desc_valid(&reflected));
  type = cmeta_type_i32x4;
  type.kind = CMETA_T_INTEGER;
  SIMD_CHECK(!cmeta_vector_desc_valid(&reflected));
  type = cmeta_type_i32x4;
  shape.lane_count = 3u;
  SIMD_CHECK(!cmeta_vector_desc_valid(&reflected));
  shape = salts_simd_i32x4;
  shape.is_mask = true;
  SIMD_CHECK(!cmeta_vector_desc_valid(&reflected));

  shape = salts_simd_i32x4;
  SIMD_CHECK(cmeta_vector_desc_valid(&reflected));
  cmeta_simd_i32x4_splat(&left, 7);
  cmeta_simd_i32x4_splat(&right, 3);
  SIMD_CHECK(cmeta_simd_binary(reflected.native_shape, SALTS_SIMD_BINARY_ADD,
                               &result, &left, &right));
  cmeta_simd_v128_store(lanes, &result);
  for (i = 0u; i < 4u; ++i) SIMD_CHECK(lanes[i] == 10);
  memset(sentinel.bytes, 0xa5, sizeof(sentinel.bytes));
  result = sentinel;
  SIMD_CHECK(!cmeta_simd_binary(reflected.native_shape, SALTS_SIMD_BINARY_DIV,
                                &result, &left, &right));
  SIMD_CHECK(memcmp(&result, &sentinel, sizeof(result)) == 0);
  return 0;
}
