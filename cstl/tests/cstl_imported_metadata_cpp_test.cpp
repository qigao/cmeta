#include "cstl_imported_metadata_fixture.h"
#include <tinytest.hpp>

#include <cstddef>
#include <type_traits>

template <class Container>
static void check_layout(cstl_imported_layout layout,
                         const cmeta_container_desc *descriptor) {
  static_assert(std::is_standard_layout_v<Container>, "C ABI wrappers require standard layout");
  static_assert(offsetof(Container, cmeta) == 0, "CMeta header must be the first member");
  check_equal(layout.size, sizeof(Container));
  check_equal(layout.alignment, alignof(Container));
  check_equal(layout.raw_offset, offsetof(Container, raw));
  check_equal(descriptor->container_type->size, sizeof(Container));
  check_equal(descriptor->container_type->align, alignof(Container));
}

template <class Entry>
static void check_entry_layout(cstl_imported_layout layout) {
  static_assert(std::is_standard_layout_v<Entry>, "C ABI entries require standard layout");
  check_equal(layout.entry_size, sizeof(Entry));
  check_equal(layout.entry_alignment, alignof(Entry));
  check_equal(layout.key_offset, offsetof(Entry, key));
  check_equal(layout.value_offset, offsetof(Entry, value));
}

static void check_detached(int actual, int expected) {
  check_equal(actual, expected);
}

static void check_detached(tstr actual, tstr expected) {
  check_true(actual != expected);
  check_equal(actual, expected);
}

template <class Entry>
static void check_detached(const Entry &actual, const Entry &expected) {
  check_detached(actual.key, expected.key);
  check_detached(actual.value, expected.value);
}

/* All storage and element callbacks here belong to the C DLL. The C++ side
 * supplies only the public layout and owns the resulting handle lifetimes. */
template <class Container, class Element>
static void check_owned(const cmeta_container_desc *descriptor,
                        const cmeta_data_desc *data,
                        const cmeta_receiver_operation_set *methods,
                        const Element &input, cmeta_container_view view) {
  Container source{}, copied{}, moved{}, failed{};
  check_true(cmeta_data_desc_valid(data));
  check_true(cmeta_receiver_operation_set_valid(methods));
  check_true(cmeta_type_equal(data->storage_type, descriptor->container_type));
  check_equal(cmeta_data_construct_init_zero(data, &source), CMETA_OK);
  auto collector = descriptor->collector(&source, 1u);
  check_equal(cmeta_collector_begin(&collector), CMETA_OK);
  check_equal(cmeta_collector_accept(&collector, collector.input_type, &input), CMETA_OK);
  check_equal(cmeta_collector_finish(&collector), CMETA_OK);
  check_true(cmeta_container_type_application_valid(&source));
  check_equal(cmeta_data_value_copy(data, &copied, &source), CMETA_OK);
  check_equal(cmeta_data_construct_move(data, &moved, &copied), CMETA_OK);
  check_equal(cmeta_data_construct_restore_zero(data, &copied), CMETA_OK);

  cmeta_range old_range{}, range{};
  check_true(cmeta_container_range_view(&source, view, &old_range));
  check_equal(cmeta_data_construct_restore_zero(data, &source), CMETA_OK);
  cmeta_range_cursor cursor{};
  Element output{};
  check_equal(cmeta_range_next(&old_range, &cursor, &output), CMETA_GEN_MUTATED);
  check_true(cmeta_container_range_view(&moved, view, &range));
  check_equal(cmeta_range_size(&range), size_t{1});
  check_equal(range.element_type->size, sizeof(Element));
  check_equal(range.element_type->align, alignof(Element));
  cursor = {};
  const auto step = cmeta_range_next(&range, &cursor, &output);
  check_true(step == CMETA_GEN_VALUE || step == CMETA_GEN_VALUE_AND_DONE);
  check_equal(cmeta_data_construct_restore_zero(data, &moved), CMETA_OK);
  check_detached(output, input);
  if (range.element_type->traits && range.element_type->traits->destroy)
    range.element_type->traits->destroy(&output);

  auto rejected = descriptor->collector(&failed, 1u);
  check_equal(cmeta_collector_begin(&rejected), CMETA_OK);
  check_equal(cmeta_collector_accept(&rejected, rejected.input_type, &input), CMETA_OK);
  check_equal(cmeta_collector_accept(&rejected, rejected.input_type, &input),
              CMETA_CAPACITY_EXCEEDED);
  check_equal(rejected.state, CMETA_COLLECTOR_ABORTED);
  check_equal(cmeta_data_construct_restore_zero(data, &failed), CMETA_OK);
}

suite("CSTL C DLL metadata in C++") {
#define CHECK_SEQUENCE(kind, Name, api, insert) \
  it("preserves " #kind " layout and owned values across the C DLL") { \
    check_layout<Name>(api##_layout(), api##_container()); \
    tstr input = tstr_dup("owned"); \
    check_not_null(input); \
    check_owned<Name>(api##_container(), api##_data(), api##_methods(), \
                      input, CMETA_CONTAINER_VIEW_DEFAULT); \
    tstr_free(input); \
  }
  Replay(CSTL_IMPORTED_SEQUENCES, CHECK_SEQUENCE)
#undef CHECK_SEQUENCE
#define CHECK_MAP(kind, Name, api, insert) \
  it("preserves " #kind " entry layout and owned values across the C DLL") { \
    check_layout<Name>(api##_layout(), api##_container()); \
    check_entry_layout<Name##_entry>(api##_layout()); \
    Name##_entry input{tstr_dup("key"), tstr_dup("value")}; \
    check_not_null(input.key); check_not_null(input.value); \
    check_owned<Name>(api##_container(), api##_data(), api##_methods(), \
                      input, CMETA_CONTAINER_VIEW_ENTRIES); \
    tstr_free(input.value); tstr_free(input.key); \
  }
  Replay(CSTL_IMPORTED_MAPS, CHECK_MAP)
#undef CHECK_MAP
  it("preserves HashSet layout, values and rollback across the C DLL") {
    check_layout<ImportedHashSet>(imported_hash_set_layout(), imported_hash_set_container());
    check_owned<ImportedHashSet>(imported_hash_set_container(), imported_hash_set_data(),
                                 imported_hash_set_methods(), 7, CMETA_CONTAINER_VIEW_DEFAULT);
  }
  it("preserves HashMap entry layout and owned values across the C DLL") {
    check_layout<ImportedHashMap>(imported_hash_map_layout(), imported_hash_map_container());
    check_entry_layout<ImportedHashMap_entry>(imported_hash_map_layout());
    ImportedHashMap_entry input{7, tstr_dup("owned")};
    check_not_null(input.value);
    check_owned<ImportedHashMap>(imported_hash_map_container(), imported_hash_map_data(),
                                 imported_hash_map_methods(), input, CMETA_CONTAINER_VIEW_ENTRIES);
    tstr_free(input.value);
  }
  it("preserves Heap layout, range and collector cleanup without a DataDesc") {
    auto descriptor = imported_heap_container();
    check_layout<ImportedHeap>(imported_heap_layout(), descriptor);
    check_null(descriptor->container_type->traits);
    check_true(cmeta_receiver_operation_set_valid(imported_heap_methods()));
    ImportedHeap heap{};
    const int input = 7;
    auto collector = descriptor->collector(&heap, 1u);
    check_equal(cmeta_collector_begin(&collector), CMETA_OK);
    check_equal(cmeta_collector_accept(&collector, &cmeta_type_int, &input), CMETA_OK);
    cmeta_range range{};
    check_true(cmeta_container_range_view(&heap, CMETA_CONTAINER_VIEW_DEFAULT, &range));
    cmeta_range_cursor cursor{};
    int output = 0;
    check_equal(cmeta_range_next(&range, &cursor, &output), CMETA_GEN_VALUE_AND_DONE);
    check_equal(output, input);
    check_equal(cmeta_collector_accept(&collector, &cmeta_type_int, &input),
                CMETA_CAPACITY_EXCEEDED);
    check_equal(collector.state, CMETA_COLLECTOR_ABORTED);
    check_equal(cmeta_range_next(&range, &cursor, &output), CMETA_GEN_MUTATED);
  }
}
