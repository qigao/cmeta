#include <cstl/typed.h>
#include "tinytest.h"

#include <stdlib.h>
#include <string.h>

typedef struct compare_value {
    int value;
} compare_value;

static int compare_value_compare(const void *left_, const void *right_) {
    const compare_value *left = (const compare_value *)left_;
    const compare_value *right = (const compare_value *)right_;
    return (left->value > right->value) - (left->value < right->value);
}

static bool compare_value_copy(void *destination, const void *source) {
    *(compare_value *)destination = *(const compare_value *)source;
    return true;
}

static void compare_value_move(void *destination, void *source) {
    *(compare_value *)destination = *(compare_value *)source;
}

static void compare_value_destroy(void *value) {
    (void)value;
}

static const cmeta_type_traits compare_value_traits = {
    CMETA_TRAIT_COMPARE | CMETA_TRAIT_COPY | CMETA_TRAIT_MOVE |
        CMETA_TRAIT_DESTROY,
    NULL, NULL, compare_value_compare, compare_value_copy,
    compare_value_move, compare_value_destroy
};

static const cmeta_type_traits missing_compare_traits = {
    CMETA_TRAIT_COPY | CMETA_TRAIT_MOVE | CMETA_TRAIT_DESTROY,
    NULL, NULL, NULL, compare_value_copy, compare_value_move,
    compare_value_destroy
};

static const cmeta_type_desc compare_value_type = {
    .name = "compare_value",
    .size = sizeof(compare_value),
    .align = _Alignof(compare_value),
    .kind = CMETA_T_OBJECT,
    .pointee = NULL,
    .traits = &compare_value_traits,
    .identity = NULL
};

static const cmeta_type_desc missing_compare_type = {
    .name = "missing_compare",
    .size = sizeof(compare_value),
    .align = _Alignof(compare_value),
    .kind = CMETA_T_OBJECT,
    .pointee = NULL,
    .traits = &missing_compare_traits,
    .identity = NULL
};

typedef struct map_owned_value {
    int *value;
} map_owned_value;

static size_t map_owned_live;
static bool map_owned_fail_copy;

static map_owned_value map_owned_make(int value) {
    map_owned_value result;
    result.value = (int *)malloc(sizeof(*result.value));
    if (result.value != NULL) {
        *result.value = value;
        ++map_owned_live;
    }
    return result;
}

static int map_owned_compare(const void *left_, const void *right_) {
    const map_owned_value *left = (const map_owned_value *)left_;
    const map_owned_value *right = (const map_owned_value *)right_;
    return (*left->value > *right->value) - (*left->value < *right->value);
}

static bool map_owned_copy(void *destination_, const void *source_) {
    map_owned_value *destination = (map_owned_value *)destination_;
    const map_owned_value *source = (const map_owned_value *)source_;
    if (map_owned_fail_copy) return false;
    *destination = map_owned_make(*source->value);
    return destination->value != NULL;
}

static void map_owned_move(void *destination_, void *source_) {
    map_owned_value *destination = (map_owned_value *)destination_;
    map_owned_value *source = (map_owned_value *)source_;
    destination->value = source->value;
    source->value = NULL;
}

static void map_owned_destroy(void *value_) {
    map_owned_value *value = (map_owned_value *)value_;
    if (value != NULL && value->value != NULL) {
        free(value->value);
        value->value = NULL;
        --map_owned_live;
    }
}

static const cmeta_type_traits map_owned_traits = {
    CMETA_TRAIT_COMPARE | CMETA_TRAIT_COPY | CMETA_TRAIT_MOVE |
        CMETA_TRAIT_DESTROY,
    NULL, NULL, map_owned_compare, map_owned_copy, map_owned_move,
    map_owned_destroy
};

static const cmeta_type_desc map_owned_type = {
    .name = "map_owned",
    .size = sizeof(map_owned_value),
    .align = _Alignof(map_owned_value),
    .kind = CMETA_T_OBJECT,
    .pointee = NULL,
    .traits = &map_owned_traits,
    .identity = NULL
};

static int raw_int_compare_map(const void *left, const void *right,
                               void *context) {
    int lhs = *(const int *)left;
    int rhs = *(const int *)right;
    (void)context;
    return (lhs > rhs) - (lhs < rhs);
}

spec("Red-black-tree ordered Map") {
    it("admits compare-only keys and rejects a missing comparator") {
        map_t map = {0};

        check_equal(map_raw_init(&map, &compare_value_type,
                                   &compare_value_type, 2u), STL_OK);
        map_raw_destroy_storage(&map);
        check_equal(map_raw_init(&map, &missing_compare_type,
                                   &compare_value_type, 2u),
                    STL_TRAIT_MISSING);
        check_equal(map_init_bytes(
                        &map, sizeof(int), _Alignof(int), sizeof(long),
                        _Alignof(long), 2u, raw_int_compare_map, NULL),
                    STL_OK);
        map_raw_destroy_storage(&map);
    }

    it("iterates shuffled keys in sorted Map order") {
        Map(int, long, map);
        int keys[] = {7, 1, 9, 3, 5};
        cmeta_range range;
        cmeta_range_cursor cursor = {0};
        cmeta_entry entry = {0};
        int expected[] = {1, 3, 5, 7, 9};
        size_t index;

        check_equal(map_init(&map, 5u), STL_OK);
        for (index = 0u; index < 5u; ++index) {
            long mapped = (long)keys[index] * 10L;
            check_equal(map_put(&map, &keys[index], &mapped), STL_OK);
        }
        check_true(cmeta_container_range_view(&map,
                                              CMETA_CONTAINER_VIEW_ENTRIES,
                                              &range));
        check_true((range.flags & (CMETA_RANGE_ORDERED | CMETA_RANGE_SORTED |
                                  CMETA_RANGE_UNIQUE)) ==
                   (CMETA_RANGE_ORDERED | CMETA_RANGE_SORTED |
                    CMETA_RANGE_UNIQUE));
        check_true(cmeta_type_equal(range.element_type,
                                    &cmeta_type_ordered_entry));
        check_true((range.flags & CMETA_RANGE_CONSTRUCTS_VALUES) != 0u);
        for (index = 0u; index < 5u; ++index) {
            cmeta_gen_status status = cmeta_range_next(&range, &cursor,
                                                       &entry);
            check_true(status == CMETA_GEN_VALUE ||
                       status == CMETA_GEN_VALUE_AND_DONE);
            check_equal(*(const int *)entry.key, expected[index]);
            check_equal(*(const long *)entry.value,
                        (long)expected[index] * 10L);
            range.element_type->traits->destroy(&entry);
        }
        map_destroy(&map);
    }

    it("supports bidirectional bounds without rank indexing") {
        map_t map = {0};
        map_iter_t iterator;
        int keys[] = {8, 2, 6, 4};
        long value = 1L;
        int probe = 5;
        size_t index;

        check_equal(map_init_bytes(
                        &map, sizeof(int), _Alignof(int), sizeof(long),
                        _Alignof(long), 4u, raw_int_compare_map, NULL),
                    STL_OK);
        for (index = 0u; index < 4u; ++index)
            check_equal(map_put(&map, &keys[index], &value),
                        STL_OK);
        iterator = map_lower_bound(&map, &probe);
        check_equal(*(const int *)map_iter_key_const(iterator), 6);
        iterator = map_upper_bound(&map, &keys[2]);
        check_equal(*(const int *)map_iter_key_const(iterator), 8);
        check_equal(map_iter_prev(&iterator), STL_OK);
        check_equal(*(const int *)map_iter_key_const(iterator), 6);
        iterator = map_begin(&map);
        check_equal(*(const int *)map_iter_key_const(iterator), 2);
        check_equal(map_iter_next(&iterator), STL_OK);
        check_equal(*(const int *)map_iter_key_const(iterator), 4);
        map_raw_destroy_storage(&map);
    }

    it("keeps Set ordered and distinct from HashSet") {
        Set(int, set);
        cmeta_range range;
        cmeta_range_cursor cursor = {0};
        int keys[] = {9, 1, 5, 1};
        int out = 0;
        int expected[] = {1, 5, 9};
        size_t index;

        check_equal(set_init(&set, 3u), STL_OK);
        for (index = 0u; index < 4u; ++index)
            check_equal(set_add(&set, &keys[index]), STL_OK);
        check_equal(set_size(&set), (size_t)3u);
        check_true(cmeta_container_range_view(&set,
                                              CMETA_CONTAINER_VIEW_DEFAULT,
                                              &range));
        check_true((range.flags & (CMETA_RANGE_ORDERED | CMETA_RANGE_SORTED |
                                  CMETA_RANGE_UNIQUE)) ==
                   (CMETA_RANGE_ORDERED | CMETA_RANGE_SORTED |
                    CMETA_RANGE_UNIQUE));
        for (index = 0u; index < 3u; ++index) {
            cmeta_gen_status status = cmeta_range_next(&range, &cursor, &out);
            check_true(status == CMETA_GEN_VALUE ||
                       status == CMETA_GEN_VALUE_AND_DONE);
            check_equal(out, expected[index]);
        }
        set_destroy(&set);
    }

    it("replaces duplicate from rows and preserves output on overflow") {
        map_t map = {0};
        int duplicate_keys[] = {2, 2};
        long duplicate_values[] = {20L, 21L};
        int distinct_keys[] = {3, 4};
        long distinct_values[] = {30L, 40L};
        uint64_t generation;

        check_equal(map_from_arrays_bytes(
                        &map, duplicate_keys, duplicate_values, 2u,
                        sizeof(int), _Alignof(int), sizeof(long),
                        _Alignof(long), 1u, raw_int_compare_map, NULL),
                    STL_OK);
        check_equal(map_size(&map), (size_t)1u);
        check_equal(*(const long *)map_get_const(&map,
                                                       &duplicate_keys[0]),
                    21L);
        generation = map_generation(&map);
        check_equal(map_from_arrays_bytes(
                        &map, distinct_keys, distinct_values, 2u,
                        sizeof(int), _Alignof(int), sizeof(long),
                        _Alignof(long), 1u, raw_int_compare_map, NULL),
                    STL_CAPACITY_EXCEEDED);
        check_equal(map_generation(&map), generation);
        check_equal(*(const long *)map_get_const(&map,
                                                       &duplicate_keys[0]),
                    21L);
        map_raw_destroy_storage(&map);
    }

    it("rolls lazy materialization back when an owning first put fails") {
        map_t map =
            SALTS_STL_MAP_INITIALIZER_WITH_TYPES(
                map_owned_value, map_owned_value,
                &map_owned_type, &map_owned_type);
        map_t before = map;
        map_owned_value key = map_owned_make(1);
        map_owned_value value = map_owned_make(10);

        map_owned_fail_copy = true;
        check_equal(map_put(&map, &key, &value), STL_OUT_OF_MEMORY);
        check_equal(memcmp(&map, &before, sizeof(map)), 0);
        check_equal(map_owned_live, (size_t)2u);

        map_owned_fail_copy = false;
        map_owned_destroy(&value);
        map_owned_destroy(&key);
        check_equal(map_owned_live, (size_t)0u);
    }

    it("keeps owning replacement limit and removal transactional") {
        map_t map = {0};
        map_owned_value key = map_owned_make(1);
        map_owned_value other_key = map_owned_make(2);
        map_owned_value first = map_owned_make(10);
        map_owned_value replacement = map_owned_make(11);
        map_owned_value out = {0};
        uint64_t generation;

        map_owned_fail_copy = false;
        check_equal(map_raw_init(&map, &map_owned_type, &map_owned_type,
                                   1u), STL_OK);
        check_equal(map_put(&map, &key, &first), STL_OK);
        check_equal(map_put(&map, &key, &replacement), STL_OK);
        check_equal(*((const map_owned_value *)map_get_const(
                          &map, &key))->value, 11);
        generation = map_generation(&map);
        map_owned_fail_copy = true;
        check_equal(map_put(&map, &key, &first),
                    STL_OUT_OF_MEMORY);
        check_equal(map_generation(&map), generation);
        check_equal(*((const map_owned_value *)map_get_const(
                          &map, &key))->value, 11);
        map_owned_fail_copy = false;
        check_equal(map_put(&map, &other_key, &first),
                    STL_CAPACITY_EXCEEDED);
        check_equal(map_generation(&map), generation);
        check_equal(map_remove(&map, &key, &out), STL_OK);
        check_true(out.value != NULL && *out.value == 11);
        map_owned_destroy(&out);
        map_raw_destroy_storage(&map);
        map_owned_destroy(&replacement);
        map_owned_destroy(&first);
        map_owned_destroy(&other_key);
        map_owned_destroy(&key);
        check_equal(map_owned_live, (size_t)0u);
    }
}
