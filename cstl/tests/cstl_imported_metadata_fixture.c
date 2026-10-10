#include "cstl_imported_metadata_fixture.h"

#define CSTL_DEFINE_SEQUENCE_LAYOUT(kind, name, api, insert) \
cstl_imported_layout api##_layout(void) { \
  const cstl_imported_layout layout = { \
    sizeof(name), _Alignof(name), offsetof(name, raw), 0, 0, 0, 0 }; \
  return layout; \
}
#define CSTL_DEFINE_MAP_LAYOUT(kind, name, api, insert) \
cstl_imported_layout api##_layout(void) { \
  const cstl_imported_layout layout = { \
    sizeof(name), _Alignof(name), offsetof(name, raw), \
    sizeof(name##_entry), _Alignof(name##_entry), \
    offsetof(name##_entry, key), offsetof(name##_entry, value) }; \
  return layout; \
}
Replay(CSTL_IMPORTED_SEQUENCES, CSTL_DEFINE_SEQUENCE_LAYOUT)
Replay(CSTL_IMPORTED_MAPS, CSTL_DEFINE_MAP_LAYOUT)
CSTL_DEFINE_SEQUENCE_LAYOUT(HashSet, ImportedHashSet, imported_hash_set, add)
CSTL_DEFINE_SEQUENCE_LAYOUT(Heap, ImportedHeap, imported_heap, push)
CSTL_DEFINE_MAP_LAYOUT(HashMap, ImportedHashMap, imported_hash_map, put)
#undef CSTL_DEFINE_SEQUENCE_LAYOUT
#undef CSTL_DEFINE_MAP_LAYOUT

#define CSTL_DEFINE_IMPORTED(kind, name, api, insert) \
const cmeta_data_desc *api##_data(void) { return name##_cmeta_data(); } \
const cmeta_container_desc *api##_container(void) { return &name##_cmeta_container_desc; } \
const cmeta_receiver_operation_set *api##_methods(void) { return name##_receiver_operation_set(); }
Replay(CSTL_IMPORTED_SEQUENCES, CSTL_DEFINE_IMPORTED)
Replay(CSTL_IMPORTED_MAPS, CSTL_DEFINE_IMPORTED)
CSTL_DEFINE_IMPORTED(HashSet, ImportedHashSet, imported_hash_set, add)
CSTL_DEFINE_IMPORTED(HashMap, ImportedHashMap, imported_hash_map, put)
#undef CSTL_DEFINE_IMPORTED

const cmeta_container_desc *imported_heap_container(void) {
  return &ImportedHeap_cmeta_container_desc;
}
const cmeta_receiver_operation_set *imported_heap_methods(void) {
  return ImportedHeap_receiver_operation_set();
}
