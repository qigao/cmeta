/* Reversed include order also exercises the installed C++17 boundary. */
#include <salts/simd.h>
#include <cmeta/vector.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <type_traits>

static_assert(sizeof(salts_simd_v128) == 16u && alignof(salts_simd_v128) == 16u);
static_assert(std::is_standard_layout<cmeta_vector_desc>::value);
static_assert(std::is_same<decltype(cmeta_vector_i32x4.native_shape),
                           const salts_simd_desc *>::value);
static_assert(std::is_same<decltype(&cmeta_simd_binary),
                           bool (*)(const salts_simd_desc *, cmeta_simd_binary_op,
                                    cmeta_v128 *, const cmeta_v128 *,
                                    const cmeta_v128 *)>::value);

#define SIMD_CHECK(expr) do { if (!(expr)) { \
  std::fprintf(stderr, "Installed SIMD reflection C++17 line %d: %s\n", __LINE__, #expr); \
  return 1; \
} } while (0)

int main() {
  const cmeta_vector_desc *const reflected[] = {
    &cmeta_vector_i8x16, &cmeta_vector_u8x16,
    &cmeta_vector_i16x8, &cmeta_vector_u16x8,
    &cmeta_vector_i32x4, &cmeta_vector_u32x4,
    &cmeta_vector_i64x2, &cmeta_vector_u64x2,
    &cmeta_vector_f32x4, &cmeta_vector_f64x2,
    &cmeta_vector_b8x16, &cmeta_vector_b16x8,
    &cmeta_vector_b32x4, &cmeta_vector_b64x2
  };
  for (const auto *entry : reflected) {
    SIMD_CHECK(cmeta_vector_desc_valid(entry));
    const cmeta_type_desc type = *entry->type;
    const salts_simd_desc shape = *entry->native_shape;
    const cmeta_vector_desc copy{&type, &shape};
    SIMD_CHECK(cmeta_vector_desc_valid(&copy));
    SIMD_CHECK(cmeta_type_equal(&type, entry->type));
    const auto *mapped = cmeta_vector_desc_for_type(&type);
    SIMD_CHECK(mapped != nullptr && mapped->native_shape != nullptr);
    SIMD_CHECK(mapped->native_shape->lane_kind == shape.lane_kind);
    SIMD_CHECK(mapped->native_shape->lane_bits == shape.lane_bits);
    SIMD_CHECK(mapped->native_shape->lane_count == shape.lane_count);
    SIMD_CHECK(mapped->native_shape->is_mask == shape.is_mask);
    SIMD_CHECK(cmeta_type_is_mask(&type) == shape.is_mask);
    SIMD_CHECK(cmeta_type_is_vector(&type) == !shape.is_mask);
  }

  salts_simd_desc shape = salts_simd_i32x4;
  cmeta_type_desc type = cmeta_type_i32x4;
  cmeta_vector_desc copy{&type, &shape};
  cmeta_v128 left{}, right{}, result{}, sentinel{};
  std::int32_t lanes[4]{};
  cmeta_simd_i32x4_splat(&left, 6);
  cmeta_simd_i32x4_splat(&right, 7);
  SIMD_CHECK(cmeta_vector_desc_valid(&copy));
  SIMD_CHECK(cmeta_simd_binary(copy.native_shape, SALTS_SIMD_BINARY_MUL,
                               &result, &left, &right));
  cmeta_simd_v128_store(lanes, &result);
  for (const auto lane : lanes) SIMD_CHECK(lane == 42);

  copy.native_shape = nullptr;
  SIMD_CHECK(!cmeta_vector_desc_valid(&copy));
  copy.native_shape = &shape;
  type.align = 0u;
  SIMD_CHECK(!cmeta_vector_desc_valid(&copy));
  type = cmeta_type_i32x4;
  shape.lane_bits = 0u;
  SIMD_CHECK(!cmeta_vector_desc_valid(&copy));
  std::memset(sentinel.bytes, 0x5a, sizeof(sentinel.bytes));
  result = sentinel;
  SIMD_CHECK(!cmeta_simd_binary(copy.native_shape, SALTS_SIMD_BINARY_ADD,
                                &result, &left, &right));
  SIMD_CHECK(std::memcmp(&result, &sentinel, sizeof(result)) == 0);
  SIMD_CHECK(cmeta_vector_desc_for_type(nullptr) == nullptr);
  return 0;
}
