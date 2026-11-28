#ifndef CMETA_SAFETY_CONTRACT_CASES_H
#define CMETA_SAFETY_CONTRACT_CASES_H

#include <cmeta/component.h>
#include <cmeta/container.h>
#include <cmeta/core_fixed_width.h>
#include <cmeta/data_reflect.h>
#include <cmeta/fingerprint.h>
#include <cmeta/flags.h>
#include <cmeta/function.h>
#include <cmeta/lifecycle.h>
#include <cmeta/manifest.h>
#include <cmeta/manifest_view.h>
#include <cmeta/object.h>
#include <cmeta/variant.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define CMETA_CONTRACT_REQUIRE(condition_) \
    do { \
        if (!(condition_)) return __LINE__; \
    } while (0)

typedef struct SafetyLeaf {
    int a;
    int b;
} SafetyLeaf;

cmeta_reflect_value(SafetyLeaf, "test.safety.Leaf",
    cmeta_field(int, a)
    cmeta_field(int, b)
);

cmeta_flags(SafetyModeFlags, "test.safety.ModeFlags",
    cmeta_flag(Read, 1u, "read")
    cmeta_flag(Write, 2u, "write")
);

cmeta_variant(SafetyChoice, "test.safety.Choice",
    cmeta_case(Count, 1, int, &cmeta_data_int)
    cmeta_case(Flags, 2, SafetyModeFlags, &SafetyModeFlags__data)
);

CMETA_FUNCTION0_DECL_AS_ABI_RESULT(pure, void, &cmeta_type_void,
    CMETA_ABI_VOID, CMETA_RESULT_UNKNOWN, safety_void_fn);
CMETA_FUNCTION0_DECL_AS_ABI_RESULT(pure, int, &cmeta_type_int,
    CMETA_ABI_SCALAR, CMETA_RESULT_VALUE, safety_int_fn);

static inline void safety_void_fn_unused_guard(void) {
    safety_void_fn();
    (void)safety_int_fn();
}
void safety_void_fn(void) {}
int safety_int_fn(void) { return 42; }

typedef struct safety_raw_seq {
    int slot;
    size_t count;
    uint64_t generation;
} safety_raw_seq;

static inline int safety_raw_seq_init(safety_raw_seq *raw, const cmeta_type_desc *type, size_t limit) {
    (void)type;
    (void)limit;
    raw->slot = 0;
    raw->count = 0u;
    raw->generation = 1u;
    return CMETA_OK;
}
static inline void safety_raw_seq_destroy(safety_raw_seq *raw) {
    raw->count = 0u;
}
static inline void safety_raw_seq_clear(safety_raw_seq *raw) {
    raw->count = 0u;
}
static inline int safety_raw_seq_reserve(safety_raw_seq *raw, size_t cap) {
    (void)raw;
    (void)cap;
    return CMETA_OK;
}
static inline int safety_raw_seq_push(safety_raw_seq *raw, const void *value) {
    raw->slot = *CMETA_CONTAINER_CONST_CAST_(int, value);
    raw->count = 1u;
    return CMETA_OK;
}
static inline int safety_raw_seq_pop(safety_raw_seq *raw, void *out) {
    if (raw->count == 0u) return CMETA_CAPACITY_EXCEEDED;
    if (out != NULL) *CMETA_CONTAINER_CAST_(int, out) = raw->slot;
    raw->count = 0u;
    return CMETA_OK;
}
static inline void *safety_raw_seq_at(safety_raw_seq *raw, size_t index) {
    return index < raw->count ? &raw->slot : NULL;
}
static inline const void *safety_raw_seq_at_const(const safety_raw_seq *raw, size_t index) {
    return index < raw->count ? &raw->slot : NULL;
}
static inline void *safety_raw_seq_front(safety_raw_seq *raw) {
    return raw->count > 0u ? &raw->slot : NULL;
}
static inline const void *safety_raw_seq_front_const(const safety_raw_seq *raw) {
    return raw->count > 0u ? &raw->slot : NULL;
}
static inline void *safety_raw_seq_data_mut(safety_raw_seq *raw) {
    return raw->count > 0u ? &raw->slot : NULL;
}
static inline const void *safety_raw_seq_data(const safety_raw_seq *raw) {
    return raw->count > 0u ? &raw->slot : NULL;
}
static inline size_t safety_raw_seq_size(const safety_raw_seq *raw) {
    return raw->count;
}
static inline bool safety_raw_seq_empty(const safety_raw_seq *raw) {
    return raw->count == 0u;
}
static inline bool safety_raw_seq_contains(const safety_raw_seq *raw, const void *value) {
    return raw->count > 0u && raw->slot == *CMETA_CONTAINER_CONST_CAST_(int, value);
}

#define SAFETY_SEQ_METHODS(M, CTX) \
    M(INIT_SIZE, init, init, 0, CTX) \
    M(DESTROY, destroy, destroy, 0, CTX) \
    M(CLEAR, clear, clear, 0, CTX) \
    M(RESERVE, reserve, reserve, 0, CTX) \
    M(PUSH_VALUE, push, push, 0, CTX) \
    M(POP_BOOL, pop, pop, 0, CTX) \
    M(PTR_INDEX, at, at, 0, CTX) \
    M(CONST_PTR_INDEX, at_const, at_const, 0, CTX) \
    M(PTR, front, front, 0, CTX) \
    M(CONST_PTR, front_const, front_const, 0, CTX) \
    M(SIZE, size, size, 0, CTX) \
    M(BOOL, empty, empty, 0, CTX) \
    M(KEY_CONTAINS, contains, contains, 0, CTX)

CMETA_CONTAINER1_DECLARE(SafetyIntSeq, safety_raw_seq);
CMETA_CONTAINER1_INDEX_RANGE_DEFINE_WITH_TYPE(SafetyIntSeq, int, &cmeta_type_int,
    safety_raw_seq, CMETA_RANGE_ORDERED, NULL, NULL)
CMETA_CONTAINER1_METHODS_WITH_DESCRIPTOR(SafetyIntSeq, int, &cmeta_type_int,
    safety_raw_seq, safety_raw_seq, CMETA_OK, &SafetyIntSeq_cmeta_container_desc, SAFETY_SEQ_METHODS)

typedef struct safety_raw_map {
    int key;
    int value;
    size_t count;
    uint64_t generation;
} safety_raw_map;

static inline int safety_raw_map_init(safety_raw_map *raw, const cmeta_type_desc *kd,
                                      const cmeta_type_desc *vd, size_t limit) {
    (void)kd;
    (void)vd;
    (void)limit;
    raw->key = 0;
    raw->value = 0;
    raw->count = 0u;
    raw->generation = 1u;
    return CMETA_OK;
}
static inline void safety_raw_map_destroy(safety_raw_map *raw) { raw->count = 0u; }
static inline void safety_raw_map_clear(safety_raw_map *raw) { raw->count = 0u; }
static inline int safety_raw_map_reserve(safety_raw_map *raw, size_t cap) {
    (void)raw;
    (void)cap;
    return CMETA_OK;
}
static inline uint64_t safety_raw_map_generation(const safety_raw_map *raw) {
    return raw->generation;
}
static inline int safety_raw_map_put(safety_raw_map *raw, const void *k, const void *v) {
    raw->key = *CMETA_CONTAINER_CONST_CAST_(int, k);
    raw->value = *CMETA_CONTAINER_CONST_CAST_(int, v);
    raw->count = 1u;
    return CMETA_OK;
}
static inline void *safety_raw_map_get_mut(safety_raw_map *raw, const void *k) {
    return raw->count > 0u && raw->key == *CMETA_CONTAINER_CONST_CAST_(int, k) ? &raw->value : NULL;
}
static inline const void *safety_raw_map_get(const safety_raw_map *raw, const void *k) {
    return raw->count > 0u && raw->key == *CMETA_CONTAINER_CONST_CAST_(int, k) ? &raw->value : NULL;
}
static inline bool safety_raw_map_contains(const safety_raw_map *raw, const void *k) {
    return safety_raw_map_get(raw, k) != NULL;
}
static inline int safety_raw_map_remove_status(safety_raw_map *raw, const void *k, void *out) {
    if (!safety_raw_map_contains(raw, k)) return CMETA_CAPACITY_EXCEEDED;
    if (out != NULL) *CMETA_CONTAINER_CAST_(int, out) = raw->value;
    raw->count = 0u;
    return CMETA_OK;
}
static inline bool safety_raw_map_remove_bool(safety_raw_map *raw, const void *k, void *out) {
    return safety_raw_map_remove_status(raw, k, out) == CMETA_OK;
}
static inline size_t safety_raw_map_size(const safety_raw_map *raw) { return raw->count; }
static inline size_t safety_raw_map_capacity(const safety_raw_map *raw) {
    return raw->count > 0u ? 1u : 0u;
}
static inline bool safety_raw_map_empty(const safety_raw_map *raw) { return raw->count == 0u; }
static inline void *safety_raw_map_key_at_mut(safety_raw_map *raw, size_t idx) {
    return idx < raw->count ? &raw->key : NULL;
}
static inline const void *safety_raw_map_key_at(const safety_raw_map *raw, size_t idx) {
    return idx < raw->count ? &raw->key : NULL;
}
static inline void *safety_raw_map_value_at_mut(safety_raw_map *raw, size_t idx) {
    return idx < raw->count ? &raw->value : NULL;
}
static inline const void *safety_raw_map_value_at(const safety_raw_map *raw, size_t idx) {
    return idx < raw->count ? &raw->value : NULL;
}
static inline size_t safety_raw_map_find_slot(safety_raw_map *raw, const void *k, bool *found) {
    bool hit = safety_raw_map_contains(raw, k);
    if (found != NULL) *found = hit;
    return 0u;
}
static inline size_t safety_raw_map_count_key(const safety_raw_map *raw, const void *k) {
    return safety_raw_map_contains(raw, k) ? 1u : 0u;
}
static inline size_t safety_raw_map_erase_key(safety_raw_map *raw, const void *k) {
    return safety_raw_map_remove_bool(raw, k, NULL) ? 1u : 0u;
}

#define SAFETY_MAP_METHODS(M, CTX) \
    M(INIT_KV_HASH, init, init, 0, CTX) \
    M(FROM_ENTRIES, from_entries, init, CMETA_INVALID_ARGUMENT, CTX) \
    M(DESTROY, destroy, destroy, 0, CTX) \
    M(CLEAR, clear, clear, 0, CTX) \
    M(RESERVE, reserve, reserve, 0, CTX) \
    M(PUT, put, put, 0, CTX) \
    M(GET, get_mut, get_mut, 0, CTX) \
    M(GET_CONST, get, get, 0, CTX) \
    M(CONTAINS, contains, contains, 0, CTX) \
    M(REMOVE_STATUS_BOOL, remove, remove_status, 0, CTX) \
    M(REMOVE_BOOL, remove_ok, remove_bool, 0, CTX) \
    M(SIZE, size, size, 0, CTX) \
    M(CAPACITY, capacity, capacity, 0, CTX) \
    M(EMPTY, empty, empty, 0, CTX) \
    M(KEY_AT, key_at_mut, key_at_mut, 0, CTX) \
    M(KEY_AT_CONST, key_at, key_at, 0, CTX) \
    M(VALUE_AT, value_at_mut, value_at_mut, 0, CTX) \
    M(VALUE_AT_CONST, value_at, value_at, 0, CTX) \
    M(FIND_SLOT, find_slot, find_slot, 0, CTX) \
    M(COUNT_KEY, count, count_key, 0, CTX) \
    M(ERASE_KEY, erase, erase_key, 0, CTX)

CMETA_CONTAINER2_DECLARE(SafetyIntMap, int, int, safety_raw_map);
CMETA_CONTAINER2_RANGES_DEFINE_WITH_TYPES(SafetyIntMap, int, int,
    &cmeta_type_int, &cmeta_type_int, safety_raw_map, key_at, value_at,
    CMETA_RANGE_NONE, CMETA_RANGE_NONE, CMETA_RANGE_NONE, NULL, NULL)
CMETA_CONTAINER2_METHODS_WITH_DESCRIPTOR(SafetyIntMap, int, int, &cmeta_type_int, &cmeta_type_int,
    safety_raw_map, safety_raw_map, CMETA_OK, &SafetyIntMap_cmeta_container_desc, SAFETY_MAP_METHODS)

static cmeta_status safety_dummy_assign(void *context, void *object,
                                        const cmeta_data_field_desc *field,
                                        const void *value) {
    (void)context;
    (void)object;
    (void)field;
    (void)value;
    return CMETA_OK;
}

static inline int cmeta_run_safety_contract_cases(void) {
    /* 1. manifest_view & fingerprint component config budget enforcement */
    {
        cmeta_component_desc component = {
            sizeof(cmeta_component_desc), CMETA_COMPONENT_DECLARATION_VERSION,
            "safety.component", cmeta_reflected_data(SafetyLeaf),
            NULL, 0u
        };
        const cmeta_manifest_entry entries[] = {
            { "safety-component", CMETA_MANIFEST_COMPONENT, &component, UINT64_C(0), UINT32_C(0) }
        };
        const cmeta_manifest manifest = {
            "safety.module", entries, 1u, CMETA_MANIFEST_FORMAT_VERSION
        };
        const cmeta_manifest_limits tight_manifest = {
            CMETA_MANIFEST_DEFAULT_ITEMS, CMETA_MANIFEST_DEFAULT_DEPTH, 1u
        };
        const cmeta_manifest_limits ok_manifest = {
            CMETA_MANIFEST_DEFAULT_ITEMS, CMETA_MANIFEST_DEFAULT_DEPTH, CMETA_MANIFEST_DEFAULT_NODES
        };
        const cmeta_fingerprint_limits tight_fp = {
            CMETA_FINGERPRINT_DEFAULT_DEPTH, 1u,
            CMETA_FINGERPRINT_DEFAULT_ROWS, CMETA_FINGERPRINT_DEFAULT_STRING_BYTES
        };
        const cmeta_fingerprint_limits ok_fp = {
            CMETA_FINGERPRINT_DEFAULT_DEPTH, CMETA_FINGERPRINT_DEFAULT_NODES,
            CMETA_FINGERPRINT_DEFAULT_ROWS, CMETA_FINGERPRINT_DEFAULT_STRING_BYTES
        };
        const cmeta_fingerprint_limits generous_fp = {
            CMETA_FINGERPRINT_DEFAULT_DEPTH, 128u,
            CMETA_FINGERPRINT_DEFAULT_ROWS, CMETA_FINGERPRINT_DEFAULT_STRING_BYTES
        };
        const cmeta_component_desc *found = NULL;
        uint64_t digest_ok = UINT64_C(0);
        uint64_t digest_generous = UINT64_C(0);
        uint64_t digest_tight = UINT64_C(0x55aa);

        CMETA_CONTRACT_REQUIRE(cmeta_component_desc_valid(&component));
        CMETA_CONTRACT_REQUIRE(
            cmeta_manifest_get_component(&manifest, 0u, &ok_manifest, &found) == CMETA_OK);
        CMETA_CONTRACT_REQUIRE(found == &component);
        found = NULL;
        CMETA_CONTRACT_REQUIRE(
            cmeta_manifest_get_component(&manifest, 0u, &tight_manifest, &found) ==
            CMETA_CAPACITY_EXCEEDED);
        CMETA_CONTRACT_REQUIRE(found == NULL);

        CMETA_CONTRACT_REQUIRE(
            cmeta_contract_fingerprint_component(&component, &ok_fp, &digest_ok) == CMETA_OK);
        CMETA_CONTRACT_REQUIRE(digest_ok != UINT64_C(0));
        CMETA_CONTRACT_REQUIRE(
            cmeta_contract_fingerprint_component(&component, &generous_fp, &digest_generous) ==
            CMETA_OK);
        CMETA_CONTRACT_REQUIRE(digest_ok == digest_generous);
        CMETA_CONTRACT_REQUIRE(
            cmeta_contract_fingerprint_component(&component, &tight_fp, &digest_tight) ==
            CMETA_CAPACITY_EXCEEDED);
        CMETA_CONTRACT_REQUIRE(digest_tight == UINT64_C(0x55aa));
    }

    /* 2. cmeta_abi_fingerprint_type cyclic & malformed identity termination */
    {
        cmeta_type_identity cycle = CMETA_TYPE_ID_POINTER_INIT(NULL);
        cmeta_type_identity missing_args = {
            CMETA_TYPE_APPLY, NULL, &cmeta_option_generic_desc, NULL, NULL, 1u
        };
        cmeta_type_desc bad_type = cmeta_type_int;
        cycle.base = &cycle;

        bad_type.identity = &cycle;
        CMETA_CONTRACT_REQUIRE(cmeta_abi_fingerprint_type(&bad_type) != UINT64_C(0));
        bad_type.identity = &missing_args;
        CMETA_CONTRACT_REQUIRE(cmeta_abi_fingerprint_type(&bad_type) != UINT64_C(0));
        CMETA_CONTRACT_REQUIRE(cmeta_abi_fingerprint_type(&cmeta_type_int) != UINT64_C(0));
    }

    /* 3. Kind-only map descriptor with non-NULL map_ops rejected */
    {
        cmeta_data_map_ops dummy_map_ops = {
            sizeof(cmeta_data_map_ops), CMETA_DATA_MAP_OPS_ABI_VERSION,
            &cmeta_type_int, CMETA_DATA_MAP_UNIQUE_KEYS,
            NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL
        };
        cmeta_data_desc bad_kind_only_map = cmeta_data_map;
        bad_kind_only_map.struct_size = sizeof(cmeta_data_desc);
        bad_kind_only_map.map_ops = &dummy_map_ops;
        CMETA_CONTRACT_REQUIRE(!cmeta_data_desc_valid(&bad_kind_only_map));
    }

    /* 4. fixed_ops, flags, variant lifecycle + NULL callback safety */
    {
        uint8_t bool_src = 1u, bool_dst = 0u, bool_bad = 2u;
        SafetyModeFlags flags_src = SafetyModeFlags_Write, flags_dst = {0u};
        uint64_t raw_bits = 0u;
        int64_t raw_tag = 0;
        SafetyChoice choice_src, choice_dst, choice_empty;
        cmeta_lifecycle_binding choice_binding = CMETA_LIFECYCLE_BINDING_INIT;

        CMETA_CONTRACT_REQUIRE(cmeta_data_value_move_supported(&cmeta_bool8_cmeta_data));
        CMETA_CONTRACT_REQUIRE(
            cmeta_data_value_move(&cmeta_bool8_cmeta_data, &bool_dst, &bool_src) == CMETA_OK);
        CMETA_CONTRACT_REQUIRE(bool_dst == 1u && bool_src == 0u);
        bool_src = 1u;
        CMETA_CONTRACT_REQUIRE(
            cmeta_data_value_move(&cmeta_bool8_cmeta_data, &bool_dst, &bool_src) ==
            CMETA_INVALID_ARGUMENT);
        CMETA_CONTRACT_REQUIRE(bool_dst == 1u && bool_src == 1u);
        bool_dst = 0u;
        CMETA_CONTRACT_REQUIRE(
            cmeta_data_value_move(&cmeta_bool8_cmeta_data, &bool_dst, &bool_bad) ==
            CMETA_INVALID_ARGUMENT);
        CMETA_CONTRACT_REQUIRE(bool_dst == 0u);

        CMETA_CONTRACT_REQUIRE(!SafetyModeFlags__is_zero(NULL));
        CMETA_CONTRACT_REQUIRE(SafetyModeFlags__read(NULL, &raw_bits) == CMETA_INVALID_ARGUMENT);
        SafetyModeFlags__restore(NULL);
        CMETA_CONTRACT_REQUIRE(cmeta_data_value_move_supported(SafetyModeFlags_cmeta_data()));
        CMETA_CONTRACT_REQUIRE(
            cmeta_data_value_move(SafetyModeFlags_cmeta_data(), &flags_dst, &flags_src) == CMETA_OK);
        CMETA_CONTRACT_REQUIRE(flags_dst.bits == SafetyModeFlags_Write.bits);
        CMETA_CONTRACT_REQUIRE(flags_src.bits == UINT64_C(0));

        CMETA_CONTRACT_REQUIRE(!SafetyChoice__is_zero(NULL));
        CMETA_CONTRACT_REQUIRE(SafetyChoice__active_tag(NULL, &raw_tag) == CMETA_INVALID_ARGUMENT);
        CMETA_CONTRACT_REQUIRE(SafetyChoice__select(NULL, 1) == CMETA_INVALID_ARGUMENT);
        SafetyChoice__restore(NULL);

        CMETA_CONTRACT_REQUIRE(cmeta_data_value_move_supported(SafetyChoice_cmeta_data()));
        CMETA_CONTRACT_REQUIRE(
            cmeta_lifecycle_admit(SafetyChoice_cmeta_data(), sizeof(SafetyChoice),
                                  CMETA_ALIGNOF(SafetyChoice), &choice_binding) == CMETA_OK);
        CMETA_CONTRACT_REQUIRE(cmeta_lifecycle_init(&choice_binding, &choice_src) == CMETA_OK);
        CMETA_CONTRACT_REQUIRE(cmeta_lifecycle_init(&choice_binding, &choice_dst) == CMETA_OK);
        CMETA_CONTRACT_REQUIRE(SafetyChoice_init(&choice_empty) == CMETA_OK);
        CMETA_CONTRACT_REQUIRE(SafetyChoice_copy_Flags(&choice_src, &flags_dst) == CMETA_OK);
        CMETA_CONTRACT_REQUIRE(
            cmeta_lifecycle_move(&choice_binding, &choice_dst, &choice_src) == CMETA_OK);
        CMETA_CONTRACT_REQUIRE(choice_src.tag == INT64_C(0));
        CMETA_CONTRACT_REQUIRE(SafetyChoice_get_Flags(&choice_dst) != NULL);
        CMETA_CONTRACT_REQUIRE(
            SafetyChoice_get_Flags(&choice_dst)->bits == SafetyModeFlags_Write.bits);
        /* Copying an empty source into an active destination must fail closed */
        CMETA_CONTRACT_REQUIRE(
            cmeta_data_value_copy(SafetyChoice_cmeta_data(), &choice_dst, &choice_empty) ==
            CMETA_INVALID_ARGUMENT);
        CMETA_CONTRACT_REQUIRE(
            choice_dst.tag == CMETA_VARIANT_INTEGER_(int64_t, SafetyChoice_Flags));
        CMETA_CONTRACT_REQUIRE(cmeta_lifecycle_restore(&choice_binding, &choice_dst) == CMETA_OK);
        CMETA_CONTRACT_REQUIRE(cmeta_lifecycle_restore(&choice_binding, &choice_src) == CMETA_OK);
        SafetyChoice_destroy(&choice_empty);
    }

    /* 5. Struct move preserves non-zero destination and source on rejection */
    {
        SafetyLeaf src = {10, 20};
        SafetyLeaf dst = {0, 99};
        CMETA_CONTRACT_REQUIRE(
            cmeta_data_value_move(cmeta_reflected_data(SafetyLeaf), &dst, &src) ==
            CMETA_INVALID_ARGUMENT);
        CMETA_CONTRACT_REQUIRE(dst.a == 0 && dst.b == 99);
        CMETA_CONTRACT_REQUIRE(src.a == 10 && src.b == 20);
    }

    /* 6. Collection/map accept zero_output guard, collection read overflow, temp_open budget */
    {
        int item = 3;
        const unsigned char dummy_bytes[4] = {1u, 2u, 3u, 4u};
        cmeta_data_collection_view overflow_view = {
            dummy_bytes, (SIZE_MAX / sizeof(int)) + 2u, sizeof(int), &cmeta_data_int
        };
        cmeta_data_collection_view out_view = {NULL, 0u, 0u, NULL};
        cmeta_collector broken_collector;
        cmeta_data_temp temp = {NULL, NULL, 0u, 0u, CMETA_DATA_TEMP_NONE};

        memset(&broken_collector, 0, sizeof(broken_collector));
        CMETA_CONTRACT_REQUIRE(
            cmeta_data_collection_read(&cmeta_data_sequence_view, &overflow_view, &out_view) ==
            CMETA_CALLBACK_ERROR);
        CMETA_CONTRACT_REQUIRE(
            cmeta_data_collection_accept(&cmeta_data_sequence_view, &broken_collector,
                                         &cmeta_data_int, &item) ==
            CMETA_INVALID_ARGUMENT);
        CMETA_CONTRACT_REQUIRE(
            cmeta_data_map_accept(&cmeta_data_map, &broken_collector,
                                  &cmeta_data_int, &item, &cmeta_data_int, &item) ==
            CMETA_INVALID_ARGUMENT);

        CMETA_CONTRACT_REQUIRE(
            cmeta_data_temp_open(cmeta_reflected_data(SafetyLeaf), sizeof(SafetyLeaf) - 1u, &temp) ==
            CMETA_CAPACITY_EXCEEDED);
        CMETA_CONTRACT_REQUIRE(
            cmeta_data_temp_open(&cmeta_bool8_cmeta_data, 1u, &temp) == CMETA_OK);
        CMETA_CONTRACT_REQUIRE(cmeta_data_temp_try_close(&temp) == CMETA_OK);
    }

    /* 7. Object field provider capability & struct-size checks */
    {
        SafetyLeaf leaf = {5, 6};
        cmeta_object_ref object = CMETA_OBJECT_REF_INIT;
        cmeta_object_field_binding binding = CMETA_OBJECT_FIELD_BINDING_INIT;
        cmeta_object_field_provider truncated_provider = {
            offsetof(cmeta_object_field_provider, assign),
            cmeta_reflected_data(SafetyLeaf), NULL, safety_dummy_assign, NULL
        };
        const void *read_ptr = NULL;
        int new_val = 11;

        CMETA_CONTRACT_REQUIRE(
            cmeta_object_borrow(&object, &leaf, cmeta_reflected_data(SafetyLeaf), NULL) == CMETA_OK);
        CMETA_CONTRACT_REQUIRE(
            cmeta_object_field_bind(&object, "a", &binding) == CMETA_OK);
        CMETA_CONTRACT_REQUIRE(
            cmeta_object_field_read_admitted(&binding, &read_ptr) == CMETA_OK);
        CMETA_CONTRACT_REQUIRE(*CMETA_CONTAINER_CONST_CAST_(int, read_ptr) == 5);

        binding.fixed_value = NULL;
        binding.provider = NULL;
        CMETA_CONTRACT_REQUIRE(
            cmeta_object_field_read_admitted(&binding, &read_ptr) == CMETA_TRAIT_MISSING);

        binding.provider = &truncated_provider;
        CMETA_CONTRACT_REQUIRE(
            cmeta_object_field_assign_admitted(&binding, &new_val) == CMETA_TRAIT_MISSING);
        cmeta_object_release(&object);
    }

    /* 8. Function0 ABI result macro descriptors & container NULL-receiver safety */
    {
        int popped = 0;
        bool found = true;
        SafetyIntSeq seq = CMETA_CONTAINER_ZERO_INIT_;
        SafetyIntMap map = CMETA_CONTAINER_ZERO_INIT_;

        safety_void_fn_unused_guard();
        CMETA_CONTRACT_REQUIRE(cmeta_function_abi_desc_valid(safety_void_fn_function_abi()));
        CMETA_CONTRACT_REQUIRE(cmeta_function_abi_desc_valid(safety_int_fn_function_abi()));

        SafetyIntSeq_destroy(NULL);
        SafetyIntSeq_clear(NULL);
        CMETA_CONTRACT_REQUIRE(SafetyIntSeq_init(NULL, 4u) == CMETA_INVALID_ARGUMENT);
        CMETA_CONTRACT_REQUIRE(SafetyIntSeq_reserve(NULL, 4u) == CMETA_INVALID_ARGUMENT);
        CMETA_CONTRACT_REQUIRE(SafetyIntSeq_push(NULL, 7) == CMETA_INVALID_ARGUMENT);
        CMETA_CONTRACT_REQUIRE(SafetyIntSeq_pop(NULL, &popped) == CMETA_INVALID_ARGUMENT);
        CMETA_CONTRACT_REQUIRE(SafetyIntSeq_at(NULL, 0u) == NULL);
        CMETA_CONTRACT_REQUIRE(SafetyIntSeq_at_const(NULL, 0u) == NULL);
        CMETA_CONTRACT_REQUIRE(SafetyIntSeq_front(NULL) == NULL);
        CMETA_CONTRACT_REQUIRE(SafetyIntSeq_front_const(NULL) == NULL);
        CMETA_CONTRACT_REQUIRE(SafetyIntSeq_size(NULL) == 0u);
        CMETA_CONTRACT_REQUIRE(SafetyIntSeq_empty(NULL));
        CMETA_CONTRACT_REQUIRE(!SafetyIntSeq_contains(NULL, 7));

        CMETA_CONTRACT_REQUIRE(SafetyIntSeq_init(&seq, 4u) == CMETA_OK);
        CMETA_CONTRACT_REQUIRE(SafetyIntSeq_push(&seq, 7) == CMETA_OK);
        CMETA_CONTRACT_REQUIRE(!SafetyIntSeq_empty(&seq));
        CMETA_CONTRACT_REQUIRE(SafetyIntSeq_contains(&seq, 7));
        SafetyIntSeq_destroy(&seq);

        SafetyIntMap_destroy(NULL);
        SafetyIntMap_clear(NULL);
        CMETA_CONTRACT_REQUIRE(SafetyIntMap_init(NULL, 4u) == CMETA_INVALID_ARGUMENT);
        CMETA_CONTRACT_REQUIRE(SafetyIntMap_from_entries(NULL, NULL, 0u, 4u) == CMETA_INVALID_ARGUMENT);
        CMETA_CONTRACT_REQUIRE(SafetyIntMap_reserve(NULL, 4u) == CMETA_INVALID_ARGUMENT);
        CMETA_CONTRACT_REQUIRE(SafetyIntMap_put(NULL, 1, 2) == CMETA_INVALID_ARGUMENT);
        CMETA_CONTRACT_REQUIRE(SafetyIntMap_get_mut(NULL, 1) == NULL);
        CMETA_CONTRACT_REQUIRE(SafetyIntMap_get(NULL, 1) == NULL);
        CMETA_CONTRACT_REQUIRE(!SafetyIntMap_contains(NULL, 1));
        CMETA_CONTRACT_REQUIRE(SafetyIntMap_remove(NULL, 1, &popped) == CMETA_INVALID_ARGUMENT);
        CMETA_CONTRACT_REQUIRE(!SafetyIntMap_remove_ok(NULL, 1, &popped));
        CMETA_CONTRACT_REQUIRE(SafetyIntMap_size(NULL) == 0u);
        CMETA_CONTRACT_REQUIRE(SafetyIntMap_capacity(NULL) == 0u);
        CMETA_CONTRACT_REQUIRE(SafetyIntMap_empty(NULL));
        CMETA_CONTRACT_REQUIRE(SafetyIntMap_key_at_mut(NULL, 0u) == NULL);
        CMETA_CONTRACT_REQUIRE(SafetyIntMap_key_at(NULL, 0u) == NULL);
        CMETA_CONTRACT_REQUIRE(SafetyIntMap_value_at_mut(NULL, 0u) == NULL);
        CMETA_CONTRACT_REQUIRE(SafetyIntMap_value_at(NULL, 0u) == NULL);
        CMETA_CONTRACT_REQUIRE(SafetyIntMap_find_slot(NULL, 1, &found) == 0u);
        CMETA_CONTRACT_REQUIRE(!found);
        CMETA_CONTRACT_REQUIRE(SafetyIntMap_count(NULL, 1) == 0u);
        CMETA_CONTRACT_REQUIRE(SafetyIntMap_erase(NULL, 1) == 0u);

        CMETA_CONTRACT_REQUIRE(SafetyIntMap_init(&map, 4u) == CMETA_OK);
        CMETA_CONTRACT_REQUIRE(SafetyIntMap_put(&map, 1, 2) == CMETA_OK);
        CMETA_CONTRACT_REQUIRE(SafetyIntMap_contains(&map, 1));
        CMETA_CONTRACT_REQUIRE(!SafetyIntMap_empty(&map));
        SafetyIntMap_destroy(&map);
    }

    return 0;
}

#endif
