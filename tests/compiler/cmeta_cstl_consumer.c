/* Standalone CMeta + CSTL linked-library consumer. Never installs an SDK.
 * The exact CMeta/CSTL implementation TUs come from canonical CMake lists.
 */
#include <cmeta/abi.h>
#include <cmeta/type_identity.h>
#include <cstl/vec.h>
#include <stdint.h>
#include <stdio.h>

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "CMeta/CSTL consumer failed at %d: %s\n", __LINE__, #expr); \
        return 1; \
    } \
} while (0)

static int check_cmeta(void) {
    const cmeta_type_identity first = CMETA_TYPE_ID_ATOM_INIT("salts.tcc.int");
    const cmeta_type_identity equivalent = CMETA_TYPE_ID_ATOM_INIT("salts.tcc.int");
    const cmeta_type_identity different = CMETA_TYPE_ID_ATOM_INIT("salts.tcc.float");
    const cmeta_type_identity pointer = CMETA_TYPE_ID_POINTER_INIT(&first);
    const cmeta_type_identity *args[] = { &first };
    const cmeta_type_identity option =
        CMETA_TYPE_ID_APPLY_INIT(&cmeta_option_generic_desc, args);

    CHECK(cmeta_reflection_abi_version() == CMETA_REFLECTION_ABI_VERSION);
    CHECK(cmeta_abi_carrier_valid(CMETA_ABI_SCALAR));
    CHECK(!cmeta_abi_carrier_valid((cmeta_abi_carrier)99));
    CHECK(cmeta_generic_desc_valid(&cmeta_option_generic_desc));
    CHECK(cmeta_type_identity_valid(&first));
    CHECK(cmeta_type_identity_equal(&first, &equivalent));
    CHECK(!cmeta_type_identity_equal(&first, &different));
    CHECK(cmeta_type_identity_valid(&pointer));
    CHECK(cmeta_type_identity_valid(&option));
    CHECK(cmeta_type_identity_constructor(&option) ==
          &cmeta_option_generic_desc);
    CHECK(cmeta_type_identity_argument(&option, 0) == &first);
    return 0;
}

static int check_cstl(void) {
    vec_t v = {0};
    int value = 17;
    int out = 0;
    uint64_t generation;

    CHECK(vec_init_bytes(&v, sizeof(value), _Alignof(int), 3u) == STL_OK);
    CHECK(vec_push(&v, &value) == STL_OK);
    CHECK(vec_size(&v) == 1u);
    CHECK(*(const int *)vec_at_const(&v, 0u) == value);
    generation = vec_generation(&v);
    CHECK(vec_resize(&v, 3u) == STL_OK);
    CHECK(vec_generation(&v) > generation);
    CHECK(*(const int *)vec_at_const(&v, 1u) == 0);
    CHECK(*(const int *)vec_at_const(&v, 2u) == 0);
    generation = vec_generation(&v);
    CHECK(vec_push(&v, &value) == STL_CAPACITY_EXCEEDED);
    CHECK(vec_generation(&v) == generation);
    CHECK(vec_pop(&v, &out) == STL_OK && out == 0);
    CHECK(vec_size(&v) == 2u);
    vec_raw_destroy_storage(&v);
    CHECK(v.data == NULL && !v.initialized);
    return 0;
}

int main(void) {
    if (check_cmeta() != 0 || check_cstl() != 0) return 1;
    puts("CMeta+CSTL whole archive consumer: PASS (not Salts SDK qualification)");
    return 0;
}
