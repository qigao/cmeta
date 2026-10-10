#ifndef CSTL_IMPORTED_METADATA_FIXTURE_H
#define CSTL_IMPORTED_METADATA_FIXTURE_H

#include <cstl/typed.h>
#include <cmeta_cmeta_data.h>

#if defined(_WIN32)
#if defined(CSTL_METADATA_PRODUCER)
#define CSTL_METADATA_API __declspec(dllexport)
#else
#define CSTL_METADATA_API __declspec(dllimport)
#endif
#elif defined(__GNUC__)
#define CSTL_METADATA_API __attribute__((visibility("default")))
#else
#define CSTL_METADATA_API
#endif

/* One fixture matrix drives both sides of the DLL boundary and the tests. */
#define CSTL_IMPORTED_SEQUENCES(M) Schema(M, \
    (Vec, ImportedVec, imported_vec, push), \
    (Deque, ImportedDeque, imported_deque, push_back), \
    (List, ImportedList, imported_list, push_back), \
    (Stack, ImportedStack, imported_stack, push), \
    (Queue, ImportedQueue, imported_queue, push), \
    (Set, ImportedSet, imported_set, add))
#define CSTL_IMPORTED_MAPS(M) Schema(M, \
    (Map, ImportedMap, imported_map, put), \
    (MultiMap, ImportedMultiMap, imported_multi_map, put), \
    (BTree, ImportedBTree, imported_btree, put), \
    (BPlusTree, ImportedBPlusTree, imported_bplus_tree, put))

#ifdef __cplusplus
extern "C" {
#endif
typedef struct cstl_imported_layout {
    size_t size, alignment, raw_offset;
    size_t entry_size, entry_alignment, key_offset, value_offset;
} cstl_imported_layout;
#define CSTL_DECLARE_LAYOUT(kind, name, api, insert) \
    CSTL_METADATA_API cstl_imported_layout api##_layout(void);
Replay(CSTL_IMPORTED_SEQUENCES, CSTL_DECLARE_LAYOUT)
Replay(CSTL_IMPORTED_MAPS, CSTL_DECLARE_LAYOUT)
CSTL_DECLARE_LAYOUT(HashSet, ImportedHashSet, imported_hash_set, add)
CSTL_DECLARE_LAYOUT(HashMap, ImportedHashMap, imported_hash_map, put)
CSTL_DECLARE_LAYOUT(Heap, ImportedHeap, imported_heap, push)
#undef CSTL_DECLARE_LAYOUT
#define CSTL_DECLARE_IMPORTED(kind, name, api, insert) \
    CSTL_METADATA_API const cmeta_data_desc *api##_data(void); \
    CSTL_METADATA_API const cmeta_container_desc *api##_container(void); \
    CSTL_METADATA_API const cmeta_receiver_operation_set *api##_methods(void);
Replay(CSTL_IMPORTED_SEQUENCES, CSTL_DECLARE_IMPORTED)
Replay(CSTL_IMPORTED_MAPS, CSTL_DECLARE_IMPORTED)
CSTL_DECLARE_IMPORTED(HashSet, ImportedHashSet, imported_hash_set, add)
CSTL_DECLARE_IMPORTED(HashMap, ImportedHashMap, imported_hash_map, put)
#undef CSTL_DECLARE_IMPORTED
CSTL_METADATA_API const cmeta_container_desc *imported_heap_container(void);
CSTL_METADATA_API const cmeta_receiver_operation_set *imported_heap_methods(void);
#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
/* C++ consumes the C-owned callbacks through the existing layout-only facade. */
#define CSTL_DECLARE_SEQUENCE(kind, name, api, insert) \
    cstl_typed_decl(kind, name, tstr);
#define CSTL_DECLARE_MAP(kind, name, api, insert) \
    cstl_typed_decl(kind, name, tstr, tstr);
cstl_typed_decl(Heap, ImportedHeap, int);
cstl_typed_decl(HashSet, ImportedHashSet, int);
cstl_typed_decl(HashMap, ImportedHashMap, int, tstr);
#elif defined(CSTL_METADATA_PRODUCER)
#define CSTL_DECLARE_SEQUENCE(kind, name, api, insert) \
    cmeta_type(kind, name, tstr, SALTS_TSTR_CMETA_TYPE_REF, SALTS_TSTR_CMETA_DATA_REF);
#define CSTL_DECLARE_MAP(kind, name, api, insert) \
    cmeta_type(kind, name, tstr, tstr, \
               SALTS_TSTR_CMETA_TYPE_REF, SALTS_TSTR_CMETA_DATA_REF, \
               SALTS_TSTR_CMETA_TYPE_REF, SALTS_TSTR_CMETA_DATA_REF);
cmeta_type(Heap, ImportedHeap, int);
/* The canonical tstr provider has comparison but no hash/equality traits.
 * Use valid integer keys and keep an owned string value in the hash map. */
cmeta_type(HashSet, ImportedHashSet, int, &cmeta_type_int, &cmeta_data_int);
cmeta_type(HashMap, ImportedHashMap, int, tstr,
           &cmeta_type_int, &cmeta_data_int,
           SALTS_TSTR_CMETA_TYPE_REF, SALTS_TSTR_CMETA_DATA_REF);
#else
#define CSTL_DECLARE_SEQUENCE(kind, name, api, insert) \
    cstl_typed_import(kind, name, tstr, api##_container(), api##_data(), api##_methods());
#define CSTL_DECLARE_MAP(kind, name, api, insert) \
    cstl_typed_import(kind, name, tstr, tstr, api##_container(), api##_data(), api##_methods());
cstl_typed_import(Heap, ImportedHeap, int, imported_heap_container(), imported_heap_methods());
cstl_typed_import(HashSet, ImportedHashSet, int,
                  imported_hash_set_container(), imported_hash_set_data(), imported_hash_set_methods());
cstl_typed_import(HashMap, ImportedHashMap, int, tstr,
                  imported_hash_map_container(), imported_hash_map_data(), imported_hash_map_methods());
#endif
Replay(CSTL_IMPORTED_SEQUENCES, CSTL_DECLARE_SEQUENCE)
Replay(CSTL_IMPORTED_MAPS, CSTL_DECLARE_MAP)
#undef CSTL_DECLARE_SEQUENCE
#undef CSTL_DECLARE_MAP
#endif
