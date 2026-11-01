#include <cstl/sort.h>
#include "tinytest.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct stable_item {
    int key;
    int order;
} stable_item;

static int stable_item_compare(const void *left_, const void *right_) {
    const stable_item *left = (const stable_item *)left_;
    const stable_item *right = (const stable_item *)right_;
    return (left->key > right->key) - (left->key < right->key);
}

static bool stable_item_copy(void *destination, const void *source) {
    *(stable_item *)destination = *(const stable_item *)source;
    return true;
}

static void stable_item_move(void *destination, void *source) {
    *(stable_item *)destination = *(stable_item *)source;
}

static void stable_item_destroy(void *value) { (void)value; }

static const cmeta_type_traits stable_item_traits = {
    CMETA_TRAIT_COMPARE | CMETA_TRAIT_COPY | CMETA_TRAIT_MOVE |
        CMETA_TRAIT_DESTROY | CMETA_TRAIT_TRIVIAL_COPY |
        CMETA_TRAIT_TRIVIAL_DESTROY,
    NULL, NULL, stable_item_compare, stable_item_copy, stable_item_move,
    stable_item_destroy
};

static const cmeta_type_desc stable_item_type = {
    "stable_item", sizeof(stable_item), _Alignof(stable_item), CMETA_T_OBJECT,
    NULL, &stable_item_traits
};

typedef struct owned_sort_item {
    int key;
    int order;
    int *resource;
} owned_sort_item;

static size_t owned_sort_live;
static size_t owned_sort_copy_count;
static size_t owned_sort_fail_copy_at;

static int owned_sort_compare(const void *left_, const void *right_) {
    const owned_sort_item *left = (const owned_sort_item *)left_;
    const owned_sort_item *right = (const owned_sort_item *)right_;
    return (left->key > right->key) - (left->key < right->key);
}

static bool owned_sort_copy(void *destination_, const void *source_) {
    owned_sort_item *destination = (owned_sort_item *)destination_;
    const owned_sort_item *source = (const owned_sort_item *)source_;
    ++owned_sort_copy_count;
    if (owned_sort_fail_copy_at != 0u &&
        owned_sort_copy_count == owned_sort_fail_copy_at)
        return false;
    *destination = *source;
    destination->resource = (int *)malloc(sizeof(*destination->resource));
    if (destination->resource == NULL) return false;
    *destination->resource = *source->resource;
    ++owned_sort_live;
    return true;
}

static void owned_sort_move(void *destination_, void *source_) {
    owned_sort_item *destination = (owned_sort_item *)destination_;
    owned_sort_item *source = (owned_sort_item *)source_;
    *destination = *source;
    source->resource = NULL;
}

static void owned_sort_destroy(void *value_) {
    owned_sort_item *value = (owned_sort_item *)value_;
    if (value->resource != NULL) {
        free(value->resource);
        value->resource = NULL;
        --owned_sort_live;
    }
}

static const cmeta_type_traits owned_sort_traits = {
    CMETA_TRAIT_COMPARE | CMETA_TRAIT_COPY | CMETA_TRAIT_MOVE |
        CMETA_TRAIT_DESTROY,
    NULL, NULL, owned_sort_compare, owned_sort_copy, owned_sort_move,
    owned_sort_destroy
};

static const cmeta_type_desc owned_sort_type = {
    "owned_sort_item", sizeof(owned_sort_item), _Alignof(owned_sort_item),
    CMETA_T_OBJECT, NULL, &owned_sort_traits
};

static owned_sort_item owned_sort_make(int key, int order) {
    owned_sort_item result = {key, order, NULL};
    result.resource = (int *)malloc(sizeof(*result.resource));
    if (result.resource != NULL) {
        *result.resource = order;
        ++owned_sort_live;
    }
    return result;
}

spec("CSTL stable sort") {
    it("sorts stably while preserving duplicate encounter order") {
        stable_item values[] = {{2, 0}, {1, 1}, {2, 2}, {1, 3}};

        check_equal(stable_sort(values, 4u, &stable_item_type,
                                      sizeof(values)), STL_OK);
        check_equal(values[0].key, 1);
        check_equal(values[0].order, 1);
        check_equal(values[1].order, 3);
        check_equal(values[2].key, 2);
        check_equal(values[2].order, 0);
        check_equal(values[3].order, 2);
    }

    it("rejects insufficient scratch and overflow without changing input") {
        stable_item values[] = {{2, 0}, {1, 1}};
        stable_item before[2];
        cmeta_type_desc overflow_type = stable_item_type;

        memcpy(before, values, sizeof(values));
        check_equal(stable_sort(values, 2u, &stable_item_type,
                                      sizeof(stable_item)),
                    STL_CAPACITY_EXCEEDED);
        check_equal(memcmp(values, before, sizeof(values)), 0);
        overflow_type.size = SIZE_MAX;
        overflow_type.align = 1u;
        check_equal(stable_sort(values, 2u, &overflow_type, SIZE_MAX),
                    STL_CAPACITY_EXCEEDED);
        check_equal(memcmp(values, before, sizeof(values)), 0);
    }

    it("accepts empty and singleton ranges and rejects missing traits") {
        stable_item value = {1, 0};
        cmeta_type_desc missing = stable_item_type;

        missing.traits = NULL;
        check_equal(stable_sort(NULL, 0u, &stable_item_type, 0u),
                    STL_OK);
        check_equal(stable_sort(&value, 1u, &stable_item_type, 0u),
                    STL_OK);
        check_equal(stable_sort(&value, 1u, &missing, 0u),
                    STL_TRAIT_MISSING);
    }

    it("keeps owning input unchanged on copy failure and balances success") {
        owned_sort_item values[] = {
            owned_sort_make(3, 0), owned_sort_make(1, 1),
            owned_sort_make(3, 2), owned_sort_make(2, 3)
        };
        int *before_resources[4];
        size_t index;

        for (index = 0u; index < 4u; ++index)
            before_resources[index] = values[index].resource;
        owned_sort_copy_count = 0u;
        owned_sort_fail_copy_at = 3u;
        check_equal(stable_sort(values, 4u, &owned_sort_type,
                                      sizeof(values)),
                    STL_OUT_OF_MEMORY);
        for (index = 0u; index < 4u; ++index)
            check_true(values[index].resource == before_resources[index]);
        check_equal(owned_sort_live, (size_t)4u);

        owned_sort_copy_count = 0u;
        owned_sort_fail_copy_at = 0u;
        check_equal(stable_sort(values, 4u, &owned_sort_type,
                                      sizeof(values)), STL_OK);
        check_equal(values[0].key, 1);
        check_equal(values[1].key, 2);
        check_equal(values[2].order, 0);
        check_equal(values[3].order, 2);
        check_equal(owned_sort_live, (size_t)4u);
        for (index = 0u; index < 4u; ++index)
            owned_sort_destroy(&values[index]);
        check_equal(owned_sort_live, (size_t)0u);
    }

    it("rejects misaligned base and handles sorted and reverse inputs") {
        _Alignas(stable_item)
            unsigned char storage[sizeof(stable_item) * 2u +
                                  _Alignof(stable_item)];
        stable_item sorted[] = {{1, 0}, {2, 1}, {3, 2}};
        stable_item reverse[] = {{3, 0}, {2, 1}, {1, 2}};

        check_equal(stable_sort(storage + 1u, 2u, &stable_item_type,
                                      sizeof(stable_item) * 2u),
                    STL_INVALID_ARGUMENT);
        check_equal(stable_sort(sorted, 3u, &stable_item_type,
                                      sizeof(sorted)), STL_OK);
        check_equal(sorted[0].key, 1);
        check_equal(sorted[2].key, 3);
        check_equal(stable_sort(reverse, 3u, &stable_item_type,
                                      sizeof(reverse)), STL_OK);
        check_equal(reverse[0].key, 1);
        check_equal(reverse[2].key, 3);
    }
}
