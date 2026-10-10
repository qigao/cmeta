#include "cstl_imported_metadata_fixture.h"
#include <tinytest.h>

/* The same behavioral cases run against all imported storage families. */
#define CHECK_IMPORTED_SEQUENCE(kind, Name, api, insert) \
static void check_##Name(void) { \
  Name values = {0}, copied = {0}, collected = {0}; \
  tstr first = tstr_dup("alpha"), second = tstr_dup("beta"), third = tstr_dup("gamma"); \
  tstr materialized = NULL; \
  cmeta_range_cursor cursor = {0}; \
  cmeta_range range; \
  cmeta_gen_status step; \
  cmeta_collector collector = Name##_collector(&collected, 1u); \
  const cmeta_data_desc *data = Name##_cmeta_data(); \
  check_true(cmeta_data_desc_equal(cmeta_data_collection_element_data(data), &cmeta_tstr_cmeta_data)); \
  check_equal(Name##_construct_init_zero(&values), CMETA_OK); \
  check_equal(Name##_init(&values, 2u), STL_OK); \
  check_equal(Name##_##insert(&values, first), STL_OK); \
  check_equal(Name##_##insert(&values, second), STL_OK); \
  check_equal(Name##_##insert(&values, third), STL_CAPACITY_EXCEEDED); \
  check_equal(Name##_size(&values), (size_t)2u); \
  check_true(cmeta_container_type_application_valid(&values)); \
  check_true(cmeta_type_equal(cmeta_function_param(Name##_##insert##_function(), 1u)->type, &cmeta_tstr_cmeta_type)); \
  check_equal(cmeta_data_value_copy(data, &copied, &values), CMETA_OK); \
  range = Name##_range(&values); \
  check_true((range.flags & CMETA_RANGE_CONSTRUCTS_VALUES) != 0u); \
  step = cmeta_range_next(&range, &cursor, &materialized); \
  check_true(step == CMETA_GEN_VALUE || step == CMETA_GEN_VALUE_AND_DONE); \
  check_true(materialized != first && materialized != second); \
  Name##_clear(&values); \
  check_equal(cmeta_range_next(&range, &cursor, &materialized), CMETA_GEN_MUTATED); \
  Name##_destroy(&values); \
  check_equal(Name##_size(&copied), (size_t)2u); \
  check_true(strcmp(materialized, "alpha") == 0 || strcmp(materialized, "beta") == 0); \
  check_equal(cmeta_collector_begin(&collector), CMETA_OK); \
  check_equal(cmeta_collector_accept(&collector, &cmeta_tstr_cmeta_type, &first), CMETA_OK); \
  check_equal(cmeta_collector_accept(&collector, &cmeta_tstr_cmeta_type, &second), CMETA_CAPACITY_EXCEEDED); \
  check_equal(Name##_size(&collected), (size_t)0u); \
  check_equal(first, "alpha"); \
  Name##_destroy(&collected); \
  Name##_construct_restore_zero(&copied); \
  check_equal(Name##_size(&copied), (size_t)0u); \
  Name##_destroy(&copied); \
  tstr_free(materialized); tstr_free(third); tstr_free(second); tstr_free(first); \
}
Replay(CSTL_IMPORTED_SEQUENCES, CHECK_IMPORTED_SEQUENCE)
#undef CHECK_IMPORTED_SEQUENCE

#define CHECK_IMPORTED_MAP(kind, Name, api, insert) \
static void check_##Name(void) { \
  Name values = {0}, copied = {0}, collected = {0}; \
  tstr first = tstr_dup("alpha"), second = tstr_dup("beta"), third = tstr_dup("gamma"); \
  tstr value = tstr_dup("owned"); \
  Name##_entry input = { first, value }, materialized = {0}; \
  cmeta_range_cursor cursor = {0}; \
  cmeta_range range; \
  cmeta_gen_status step; \
  cmeta_collector collector = Name##_collector(&collected, 1u); \
  const cmeta_data_desc *data = Name##_cmeta_data(); \
  check_true(cmeta_data_desc_equal(cmeta_data_map_key_data(data), &cmeta_tstr_cmeta_data)); \
  check_true(cmeta_data_desc_equal(cmeta_data_map_value_data(data), &cmeta_tstr_cmeta_data)); \
  check_equal(Name##_construct_init_zero(&values), CMETA_OK); \
  check_equal(Name##_init(&values, 2u), STL_OK); \
  check_equal(Name##_put(&values, first, value), STL_OK); \
  check_equal(Name##_put(&values, second, value), STL_OK); \
  check_equal(Name##_put(&values, third, value), STL_CAPACITY_EXCEEDED); \
  check_equal(Name##_size(&values), (size_t)2u); \
  check_true(cmeta_container_type_application_valid(&values)); \
  check_true(cmeta_type_equal(cmeta_function_param(Name##_put_function(), 2u)->type, &cmeta_tstr_cmeta_type)); \
  check_equal(cmeta_data_value_copy(data, &copied, &values), CMETA_OK); \
  range = Name##_entries_range(&values); \
  check_true((range.flags & CMETA_RANGE_CONSTRUCTS_VALUES) != 0u); \
  step = cmeta_range_next(&range, &cursor, &materialized); \
  check_true(step == CMETA_GEN_VALUE || step == CMETA_GEN_VALUE_AND_DONE); \
  check_true(materialized.key != first && materialized.key != second); \
  check_true(materialized.value != value); \
  Name##_clear(&values); \
  check_equal(cmeta_range_next(&range, &cursor, &materialized), CMETA_GEN_MUTATED); \
  Name##_destroy(&values); \
  check_equal(Name##_size(&copied), (size_t)2u); \
  check_equal(materialized.value, "owned"); \
  range.element_type->traits->destroy(&materialized); \
  check_equal(cmeta_collector_begin(&collector), CMETA_OK); \
  check_equal(cmeta_collector_accept(&collector, collector.input_type, &input), CMETA_OK); \
  check_equal(cmeta_collector_accept(&collector, collector.input_type, &input), CMETA_CAPACITY_EXCEEDED); \
  check_equal(Name##_size(&collected), (size_t)0u); \
  check_equal(value, "owned"); \
  Name##_destroy(&collected); \
  Name##_construct_restore_zero(&copied); \
  check_equal(Name##_size(&copied), (size_t)0u); \
  Name##_destroy(&copied); \
  tstr_free(value); tstr_free(third); tstr_free(second); tstr_free(first); \
}
Replay(CSTL_IMPORTED_MAPS, CHECK_IMPORTED_MAP)
#undef CHECK_IMPORTED_MAP

suite("CSTL imported metadata") {
#define RUN_IMPORTED_CASE(kind, Name, api, insert) \
  it("preserves " #kind " ownership, reflection, limits and range invalidation") { check_##Name(); }
  Replay(CSTL_IMPORTED_SEQUENCES, RUN_IMPORTED_CASE)
  Replay(CSTL_IMPORTED_MAPS, RUN_IMPORTED_CASE)
#undef RUN_IMPORTED_CASE

  it("retains both Deque ends, all List mutations, and LIFO/FIFO behavior") {
    ImportedDeque deque = {0};
    ImportedList list = {0};
    ImportedStack stack = {0};
    ImportedQueue queue = {0};
    tstr first = tstr_dup("first"), second = tstr_dup("second"), output = NULL;
    check_equal(ImportedDeque_init(&deque, 2u), STL_OK);
    check_equal(ImportedList_init(&list, 3u), STL_OK);
    check_equal(ImportedStack_init(&stack, 2u), STL_OK);
    check_equal(ImportedQueue_init(&queue, 2u), STL_OK);
    check_not_null(ImportedDeque_push_front_function());
    check_not_null(ImportedDeque_push_back_function());
    check_not_null(ImportedList_add_function());
    check_not_null(ImportedList_push_front_function());
    check_not_null(ImportedList_push_back_function());
    check_equal(ImportedDeque_push_back(&deque, first), STL_OK);
    check_equal(ImportedDeque_push_front(&deque, second), STL_OK);
    check_equal(*ImportedDeque_front_const(&deque), "second");
    check_equal(ImportedList_add(&list, first), STL_OK);
    check_equal(ImportedList_push_front(&list, second), STL_OK);
    check_equal(ImportedList_push_back(&list, first), STL_OK);
    check_equal(*ImportedList_front_const(&list), "second");
    check_equal(*ImportedList_back_const(&list), "first");
    check_equal(ImportedStack_push(&stack, first), STL_OK);
    check_equal(ImportedStack_push(&stack, second), STL_OK);
    check_equal(ImportedStack_pop(&stack, &output), STL_OK);
    check_equal(output, "second");
    tstr_free(output); output = NULL;
    check_equal(ImportedQueue_push(&queue, first), STL_OK);
    check_equal(ImportedQueue_push(&queue, second), STL_OK);
    check_equal(ImportedQueue_pop(&queue, &output), STL_OK);
    check_equal(output, "first");
    tstr_free(output);
    ImportedQueue_destroy(&queue); ImportedStack_destroy(&stack);
    ImportedList_destroy(&list); ImportedDeque_destroy(&deque);
    tstr_free(second); tstr_free(first);
  }

  it("keeps MultiMap duplicates and unique-map replacement semantics") {
    ImportedMultiMap multi = {0}, copied = {0};
    ImportedHashMap unique = {0};
    tstr key = tstr_dup("key"), first = tstr_dup("first"), second = tstr_dup("second");
    check_equal(ImportedMultiMap_init(&multi, 2u), STL_OK);
    check_equal(ImportedHashMap_init(&unique, 1u), STL_OK);
    check_equal(ImportedMultiMap_put(&multi, key, first), STL_OK);
    check_equal(ImportedMultiMap_put(&multi, key, second), STL_OK);
    check_equal(ImportedMultiMap_count(&multi, key), (size_t)2u);
    check_equal(cmeta_data_value_copy(ImportedMultiMap_cmeta_data(), &copied, &multi), CMETA_OK);
    ImportedMultiMap_destroy(&multi);
    check_equal(ImportedMultiMap_count(&copied, key), (size_t)2u);
    check_equal(ImportedHashMap_put(&unique, 7, first), STL_OK);
    check_equal(ImportedHashMap_put(&unique, 7, second), STL_OK);
    check_equal(ImportedHashMap_size(&unique), (size_t)1u);
    check_equal(*ImportedHashMap_get_const(&unique, 7), "second");
    ImportedHashMap_destroy(&unique); ImportedMultiMap_destroy(&copied);
    tstr_free(second); tstr_free(first); tstr_free(key);
  }

  it("imports HashSet uniqueness, metadata and bounded collector rollback") {
    ImportedHashSet values = {0}, copied = {0}, collected = {0};
    int first = 1, second = 2, output = 0;
    cmeta_range range;
    cmeta_range_cursor cursor = {0};
    cmeta_collector collector = ImportedHashSet_collector(&collected, 1u);
    check_equal(ImportedHashSet_init(&values, 2u), STL_OK);
    check_equal(ImportedHashSet_add(&values, first), STL_OK);
    check_equal(ImportedHashSet_add(&values, first), STL_OK);
    check_equal(ImportedHashSet_add(&values, second), STL_OK);
    check_equal(ImportedHashSet_add(&values, 3), STL_CAPACITY_EXCEEDED);
    check_equal(ImportedHashSet_size(&values), (size_t)2u);
    check_true(cmeta_type_equal(cmeta_function_param(ImportedHashSet_add_function(), 1u)->type, &cmeta_type_int));
    check_true(cmeta_data_desc_equal(cmeta_data_collection_element_data(ImportedHashSet_cmeta_data()), &cmeta_data_int));
    check_equal(cmeta_data_value_copy(ImportedHashSet_cmeta_data(), &copied, &values), CMETA_OK);
    range = ImportedHashSet_range(&values);
    check_true((range.flags & CMETA_RANGE_UNIQUE) != 0u);
    check_equal(cmeta_range_next(&range, &cursor, &output), CMETA_GEN_VALUE);
    check_true(output == first || output == second);
    ImportedHashSet_clear(&values);
    check_equal(cmeta_range_next(&range, &cursor, &output), CMETA_GEN_MUTATED);
    check_true(ImportedHashSet_contains(&copied, first));
    check_true(ImportedHashSet_contains(&copied, second));
    check_equal(cmeta_collector_begin(&collector), CMETA_OK);
    check_equal(cmeta_collector_accept(&collector, &cmeta_type_int, &first), CMETA_OK);
    check_equal(cmeta_collector_accept(&collector, &cmeta_type_int, &second), CMETA_CAPACITY_EXCEEDED);
    check_equal(ImportedHashSet_size(&collected), (size_t)0u);
    ImportedHashSet_destroy(&collected); ImportedHashSet_destroy(&copied); ImportedHashSet_destroy(&values);
  }

  it("imports HashMap owned values, entry ranges and bounded collector rollback") {
    ImportedHashMap values = {0}, copied = {0}, collected = {0};
    tstr value = tstr_dup("owned");
    ImportedHashMap_entry input = {7, value}, materialized = {0};
    cmeta_range range;
    cmeta_range_cursor cursor = {0};
    cmeta_collector collector = ImportedHashMap_collector(&collected, 1u);
    check_equal(ImportedHashMap_init(&values, 1u), STL_OK);
    check_equal(ImportedHashMap_put(&values, 7, value), STL_OK);
    check_equal(ImportedHashMap_put(&values, 8, value), STL_CAPACITY_EXCEEDED);
    check_true(cmeta_type_equal(cmeta_function_param(ImportedHashMap_put_function(), 1u)->type, &cmeta_type_int));
    check_true(cmeta_data_desc_equal(cmeta_data_map_key_data(ImportedHashMap_cmeta_data()), &cmeta_data_int));
    check_true(cmeta_data_desc_equal(cmeta_data_map_value_data(ImportedHashMap_cmeta_data()), &cmeta_tstr_cmeta_data));
    check_equal(cmeta_data_value_copy(ImportedHashMap_cmeta_data(), &copied, &values), CMETA_OK);
    check_true(*ImportedHashMap_get_const(&copied, 7) != *ImportedHashMap_get_const(&values, 7));
    range = ImportedHashMap_entries_range(&values);
    check_equal(cmeta_range_next(&range, &cursor, &materialized), CMETA_GEN_VALUE);
    check_true(materialized.value != value);
    ImportedHashMap_clear(&values);
    check_equal(cmeta_range_next(&range, &cursor, &materialized), CMETA_GEN_MUTATED);
    check_equal(materialized.key, 7);
    check_equal(materialized.value, "owned");
    range.element_type->traits->destroy(&materialized);
    check_equal(cmeta_collector_begin(&collector), CMETA_OK);
    check_equal(cmeta_collector_accept(&collector, collector.input_type, &input), CMETA_OK);
    check_equal(cmeta_collector_accept(&collector, collector.input_type, &input), CMETA_CAPACITY_EXCEEDED);
    check_equal(ImportedHashMap_size(&collected), (size_t)0u);
    ImportedHashMap_destroy(&collected); ImportedHashMap_destroy(&values);
    tstr_free(value);
    check_equal(*ImportedHashMap_get_const(&copied, 7), "owned");
    ImportedHashMap_destroy(&copied);
  }

  it("imports Heap ordering, from, collector, range and reflection without a DataDesc") {
    ImportedHeap heap = {0}, collected = {0};
    const int input[] = {7, 2, 5};
    int output = 0;
    cmeta_range_cursor cursor = {0};
    cmeta_range range;
    cmeta_collector collector = ImportedHeap_collector(&collected, 1u);
    check_equal(ImportedHeap_from(&heap, input, 3u, 3u), STL_OK);
    check_equal(*ImportedHeap_peek(&heap), 2);
    check_true(cmeta_type_equal(cmeta_function_param(ImportedHeap_push_function(), 1u)->type, &cmeta_type_int));
    check_true(cmeta_type_equal(ImportedHeap_cmeta_descriptor()->container_type, &ImportedHeap_cmeta_type));
    check_null(ImportedHeap_cmeta_type.traits);
    range = ImportedHeap_range(&heap);
    check_equal(cmeta_range_size(&range), (size_t)3u);
    check_equal(cmeta_range_next(&range, &cursor, &output), CMETA_GEN_VALUE);
    check_equal(ImportedHeap_pop(&heap, &output), STL_OK);
    check_equal(output, 2);
    check_equal(cmeta_range_next(&range, &cursor, &output), CMETA_GEN_MUTATED);
    check_equal(ImportedHeap_pop(&heap, &output), STL_OK);
    check_equal(output, 5);
    check_equal(ImportedHeap_pop(&heap, &output), STL_OK);
    check_equal(output, 7);
    check_equal(cmeta_collector_begin(&collector), CMETA_OK);
    check_equal(cmeta_collector_accept(&collector, &cmeta_type_int, &input[0]), CMETA_OK);
    check_equal(cmeta_collector_accept(&collector, &cmeta_type_int, &input[1]), CMETA_CAPACITY_EXCEEDED);
    check_equal(ImportedHeap_size(&collected), (size_t)0u);
    ImportedHeap_destroy(&collected); ImportedHeap_destroy(&heap);
  }

  it("retains object-independent metadata and complete method reflection") {
    const cmeta_data_desc *vec = ImportedVec_cmeta_data();
    const cmeta_data_desc *set = ImportedSet_cmeta_data();
    const cmeta_data_desc *map = ImportedMap_cmeta_data();
    check_true(cmeta_data_desc_equal(cmeta_data_collection_element_data(vec),
                                    &cmeta_tstr_cmeta_data));
    check_true(cmeta_data_desc_equal(cmeta_data_collection_element_data(set),
                                    &cmeta_tstr_cmeta_data));
    check_true(cmeta_data_desc_equal(cmeta_data_map_key_data(map), &cmeta_tstr_cmeta_data));
    check_true(cmeta_data_desc_equal(cmeta_data_map_value_data(map), &cmeta_tstr_cmeta_data));
    check_true(cmeta_type_equal(vec->storage_type, &ImportedVec_cmeta_type));
    check_true(cmeta_type_equal(set->storage_type, &ImportedSet_cmeta_type));
    check_true(cmeta_type_equal(map->storage_type, &ImportedMap_cmeta_type));
    check_true(cmeta_receiver_operation_set_valid(ImportedVec_receiver_operation_set()));
    check_true(cmeta_receiver_operation_set_valid(ImportedSet_receiver_operation_set()));
    check_true(cmeta_receiver_operation_set_valid(ImportedMap_receiver_operation_set()));
    check_not_null(cmeta_receiver_operation_find(ImportedMap_receiver_operation_set(), "put"));
    check_true(cmeta_type_equal(cmeta_function_param(ImportedMap_put_function(), 1u)->type,
                                &cmeta_tstr_cmeta_type));
    check_true(cmeta_type_equal(cmeta_function_param(ImportedVec_push_function(), 1u)->type,
                                &cmeta_tstr_cmeta_type));
    check_true(cmeta_type_equal(cmeta_function_param(ImportedSet_add_function(), 1u)->type,
                                &cmeta_tstr_cmeta_type));
  }

  it("copies managed values through methods, range and collection traits") {
    ImportedVec values = {0}, copied = {0};
    ImportedSet unique = {0};
    ImportedMap mapping = {0};
    tstr key = tstr_dup("key"), value = tstr_dup("value"), ranged = NULL;
    cmeta_range_cursor cursor = {0};
    cmeta_range range;
    check_not_null(key);
    check_not_null(value);
    check_equal(ImportedVec_init(&values, 4u), STL_OK);
    check_equal(ImportedSet_init(&unique, 4u), STL_OK);
    check_equal(ImportedMap_init(&mapping, 4u), STL_OK);
    check_equal(ImportedVec_push(&values, value), STL_OK);
    check_equal(ImportedSet_add(&unique, value), STL_OK);
    check_equal(ImportedMap_put(&mapping, key, value), STL_OK);
    check_equal(cmeta_data_value_copy(ImportedVec_cmeta_data(), &copied, &values), CMETA_OK);
    range = ImportedVec_range(&values);
    check_equal(cmeta_range_next(&range, &cursor, &ranged), CMETA_GEN_VALUE_AND_DONE);
    check_true(ranged != value);
    check_true(*ImportedVec_at_const(&values, 0u) != *ImportedVec_at_const(&copied, 0u));
    ImportedVec_destroy(&values);
    tstr_free(value);
    check_equal(*ImportedVec_at_const(&copied, 0u), "value");
    check_equal(*ImportedMap_get_const(&mapping, key), "value");
    check_equal(ImportedSet_size(&unique), (size_t)1u);
    check_equal(ranged, "value");
    tstr_free(ranged);
    tstr_free(key);
    ImportedMap_destroy(&mapping);
    ImportedSet_destroy(&unique);
    ImportedVec_destroy(&copied);
  }

  it("rolls back a full collector without consuming caller ownership") {
    ImportedVec values = {0};
    tstr value = tstr_dup("retained");
    cmeta_collector collector = ImportedVec_collector(&values, 1u);
    check_equal(cmeta_collector_begin(&collector), CMETA_OK);
    check_equal(cmeta_collector_accept(&collector, &cmeta_tstr_cmeta_type, &value), CMETA_OK);
    check_equal(cmeta_collector_accept(&collector, &cmeta_tstr_cmeta_type, &value),
                CMETA_CAPACITY_EXCEEDED);
    check_equal(ImportedVec_size(&values), (size_t)0u);
    check_equal(value, "retained");
    ImportedVec_destroy(&values);
    tstr_free(value);
  }

  it("retains structured construction and managed wrapper traits") {
    ImportedVec source, copied = {0}, moved = {0};
    tstr value = tstr_dup("owned");
    check_equal(ImportedVec_construct_init_zero(&source), CMETA_OK);
    check_true(cmeta_type_equal(cmeta_container_descriptor(&source)->element_type,
                                &cmeta_tstr_cmeta_type));
    check_equal(ImportedVec_init(&source, 2u), STL_OK);
    check_equal(ImportedVec_push(&source, value), STL_OK);
    check_true(ImportedVec_cmeta_type.traits->copy_construct(&copied, &source));
    check_true(*ImportedVec_at_const(&source, 0u) != *ImportedVec_at_const(&copied, 0u));
    ImportedVec_construct_move(&moved, &source);
    check_equal(ImportedVec_size(&source), (size_t)0u);
    check_equal(*ImportedVec_at_const(&moved, 0u), "owned");
    ImportedVec_construct_restore_zero(&moved);
    check_equal(ImportedVec_size(&moved), (size_t)0u);
    check_equal(ImportedVec_init(&moved, 2u), STL_OK);
    check_equal(ImportedVec_push(&moved, value), STL_OK);
    ImportedVec_destroy(&moved);
    ImportedVec_destroy(&source);
    ImportedVec_cmeta_type.traits->destroy(&copied);
    check_equal(value, "owned");
    tstr_free(value);
  }
}
