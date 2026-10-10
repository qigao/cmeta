#ifndef SALTS_TYPED_H
#define SALTS_TYPED_H

#include <cmeta/meta.h>
#include <cmeta/container.h>

#include <cstl/vec.h>
#include <cstl/deque.h>
#include <cstl/list.h>
#include <cstl/stack.h>
#include <cstl/queue.h>
#include <cstl/heap.h>
#include <cstl/set.h>
#include <cstl/hash_set.h>
#include <cstl/hash_map.h>
#include <cstl/map.h>
#include <cstl/multimap.h>
#include <cstl/btree.h>
#include <cstl/bplus_tree.h>
#include <cstl/detail/instance_meta.h>

#ifndef __cplusplus
#include <cstl/detail/typed_facade.h>
#endif

/* TYPE(...) provider registrations. CMeta owns the generic declaration
 * protocol; CSTL supplies only storage, constructor and bind capability. */
#define CMETA_DECLARED_STORAGE_Vec vec_t
#define CMETA_DECLARED_STORAGE_DESC_Vec (&stl_vec_storage_type)
#define CMETA_DECLARED_CONSTRUCTOR_Vec (&stl_vec_generic_desc)
#define CMETA_DECLARED_CONSTRUCTION_Vec (&stl_vec_construct_ops)

#define CMETA_DECLARED_STORAGE_Deque deque_t
#define CMETA_DECLARED_STORAGE_DESC_Deque (&stl_deque_storage_type)
#define CMETA_DECLARED_CONSTRUCTOR_Deque (&stl_deque_generic_desc)
#define CMETA_DECLARED_CONSTRUCTION_Deque (&stl_deque_construct_ops)

#define CMETA_DECLARED_STORAGE_List list_t
#define CMETA_DECLARED_STORAGE_DESC_List (&stl_list_storage_type)
#define CMETA_DECLARED_CONSTRUCTOR_List (&stl_list_generic_desc)
#define CMETA_DECLARED_CONSTRUCTION_List (&stl_list_construct_ops)

#define CMETA_DECLARED_STORAGE_Stack cstl_stack_t
#define CMETA_DECLARED_STORAGE_DESC_Stack (&stl_stack_storage_type)
#define CMETA_DECLARED_CONSTRUCTOR_Stack (&stl_stack_generic_desc)
#define CMETA_DECLARED_CONSTRUCTION_Stack (&stl_stack_construct_ops)

#define CMETA_DECLARED_STORAGE_Queue queue_t
#define CMETA_DECLARED_STORAGE_DESC_Queue (&stl_queue_storage_type)
#define CMETA_DECLARED_CONSTRUCTOR_Queue (&stl_queue_generic_desc)
#define CMETA_DECLARED_CONSTRUCTION_Queue (&stl_queue_construct_ops)

#define CMETA_DECLARED_STORAGE_Heap heap_t
#define CMETA_DECLARED_STORAGE_DESC_Heap (&stl_heap_storage_type)
#define CMETA_DECLARED_CONSTRUCTOR_Heap (&stl_heap_generic_desc)
#define CMETA_DECLARED_CONSTRUCTION_Heap (&stl_heap_construct_ops)

#define CMETA_DECLARED_STORAGE_Set set_t
#define CMETA_DECLARED_STORAGE_DESC_Set (&stl_set_storage_type)
#define CMETA_DECLARED_CONSTRUCTOR_Set (&stl_set_generic_desc)
#define CMETA_DECLARED_CONSTRUCTION_Set (&stl_set_construct_ops)

#define CMETA_DECLARED_STORAGE_HashSet hash_set_t
#define CMETA_DECLARED_STORAGE_DESC_HashSet (&stl_hash_set_storage_type)
#define CMETA_DECLARED_CONSTRUCTOR_HashSet (&stl_hash_set_generic_desc)
#define CMETA_DECLARED_CONSTRUCTION_HashSet (&stl_hash_set_construct_ops)

#define CMETA_DECLARED_STORAGE_HashMap hash_map_t
#define CMETA_DECLARED_STORAGE_DESC_HashMap (&stl_hash_map_storage_type)
#define CMETA_DECLARED_CONSTRUCTOR_HashMap (&stl_hash_map_generic_desc)
#define CMETA_DECLARED_CONSTRUCTION_HashMap (&stl_hash_map_construct_ops)

#define CMETA_DECLARED_STORAGE_Map map_t
#define CMETA_DECLARED_STORAGE_DESC_Map (&stl_map_storage_type)
#define CMETA_DECLARED_CONSTRUCTOR_Map (&stl_map_generic_desc)
#define CMETA_DECLARED_CONSTRUCTION_Map (&stl_map_construct_ops)

#define CMETA_DECLARED_STORAGE_MultiMap multimap_t
#define CMETA_DECLARED_STORAGE_DESC_MultiMap (&stl_multimap_storage_type)
#define CMETA_DECLARED_CONSTRUCTOR_MultiMap (&stl_multimap_generic_desc)
#define CMETA_DECLARED_CONSTRUCTION_MultiMap (&stl_multimap_construct_ops)

#define CMETA_DECLARED_STORAGE_BTree btree_t
#define CMETA_DECLARED_STORAGE_DESC_BTree (&stl_btree_storage_type)
#define CMETA_DECLARED_CONSTRUCTOR_BTree (&stl_btree_generic_desc)
#define CMETA_DECLARED_CONSTRUCTION_BTree (&stl_btree_construct_ops)

#define CMETA_DECLARED_STORAGE_BPlusTree bplus_tree_t
#define CMETA_DECLARED_STORAGE_DESC_BPlusTree (&stl_bplus_tree_storage_type)
#define CMETA_DECLARED_CONSTRUCTOR_BPlusTree (&stl_bplus_tree_generic_desc)
#define CMETA_DECLARED_CONSTRUCTION_BPlusTree (&stl_bplus_tree_construct_ops)

#include <cstl/detail/typed_initializers.h>

/*
 * Declaration-only typed container wrappers.
 *
 * cstl_typed_decl(...) emits only the physical wrapper type shared by the
 * full C typed façade. It generates no methods, descriptors, collectors or
 * semantic provider objects, so a C++ translation unit can embed a typed C ABI
 * defined by a C translation unit without duplicating provider definitions.
 *
 * Extra explicit-metadata arguments are accepted and intentionally ignored:
 * storage layout depends only on the container kind and C key/value types.
 */
#define CSTL_TYPED_DECL_Vec(name, type, ...) \
  CMETA_CONTAINER1_DECLARE(name, vec_t)
#define CSTL_TYPED_DECL_Deque(name, type, ...) \
  CMETA_CONTAINER1_DECLARE(name, deque_t)
#define CSTL_TYPED_DECL_List(name, type, ...) \
  CMETA_CONTAINER1_DECLARE(name, list_t)
#define CSTL_TYPED_DECL_Stack(name, type, ...) \
  CMETA_CONTAINER1_DECLARE(name, cstl_stack_t)
#define CSTL_TYPED_DECL_Queue(name, type, ...) \
  CMETA_CONTAINER1_DECLARE(name, queue_t)
#define CSTL_TYPED_DECL_Heap(name, type, ...) \
  CMETA_CONTAINER1_DECLARE(name, heap_t)
#define CSTL_TYPED_DECL_Set(name, type, ...) \
  CMETA_CONTAINER1_DECLARE(name, set_t)
#define CSTL_TYPED_DECL_HashSet(name, type, ...) \
  CMETA_CONTAINER1_DECLARE(name, hash_set_t)
#define CSTL_TYPED_DECL_HashMap(name, key_type, value_type, ...) \
  CMETA_CONTAINER2_DECLARE(name, key_type, value_type, hash_map_t)
#define CSTL_TYPED_DECL_Map(name, key_type, value_type, ...) \
  CMETA_CONTAINER2_DECLARE(name, key_type, value_type, map_t)
#define CSTL_TYPED_DECL_MultiMap(name, key_type, value_type, ...) \
  CMETA_CONTAINER2_DECLARE(name, key_type, value_type, multimap_t)
#define CSTL_TYPED_DECL_BTree(name, key_type, value_type, ...) \
  CMETA_CONTAINER2_DECLARE(name, key_type, value_type, btree_t)
#define CSTL_TYPED_DECL_BPlusTree(name, key_type, value_type, ...) \
  CMETA_CONTAINER2_DECLARE(name, key_type, value_type, bplus_tree_t)

#define cstl_typed_decl(kind, name, ...) \
  CMETA_PP_CAT(CSTL_TYPED_DECL_, kind)(name, __VA_ARGS__)

#ifndef __cplusplus

/* Full C facade borrowing immutable metadata from another module. The three
 * expressions yield the exact wrapper's ContainerDesc, DataDesc and receiver
 * operation set; they are evaluated only at runtime and must not return NULL.
 * The owner uses the normal explicit declaration without an extra wrapper
 * identity argument, or the normal two-argument Heap declaration. Kind, name,
 * layout and argument providers must match.
 * The module owns all providers and must outlive values, ranges and metadata
 * borrows. No local collection/map provider objects are generated. Heap has
 * no DataDesc/lifecycle provider and takes only container and methods queries. */
#define CSTL_TYPED_IMPORT_Vec(name, type, container_expr, data_expr, methods_expr) \
  SALTS_STL_KIND_IMPORT(SALTS_STL_KIND_ROW_Vec, name, type, container_expr, data_expr, methods_expr)
#define CSTL_TYPED_IMPORT_Deque(name, type, container_expr, data_expr, methods_expr) \
  SALTS_STL_KIND_IMPORT(SALTS_STL_KIND_ROW_Deque, name, type, container_expr, data_expr, methods_expr)
#define CSTL_TYPED_IMPORT_List(name, type, container_expr, data_expr, methods_expr) \
  SALTS_STL_KIND_IMPORT(SALTS_STL_KIND_ROW_List, name, type, container_expr, data_expr, methods_expr)
#define CSTL_TYPED_IMPORT_Stack(name, type, container_expr, data_expr, methods_expr) \
  SALTS_STL_KIND_IMPORT(SALTS_STL_KIND_ROW_Stack, name, type, container_expr, data_expr, methods_expr)
#define CSTL_TYPED_IMPORT_Queue(name, type, container_expr, data_expr, methods_expr) \
  SALTS_STL_KIND_IMPORT(SALTS_STL_KIND_ROW_Queue, name, type, container_expr, data_expr, methods_expr)
#define CSTL_TYPED_IMPORT_Set(name, type, container_expr, data_expr, methods_expr) \
  SALTS_STL_KIND_IMPORT(SALTS_STL_KIND_ROW_Set, name, type, container_expr, data_expr, methods_expr)
#define CSTL_TYPED_IMPORT_HashSet(name, type, container_expr, data_expr, methods_expr) \
  SALTS_STL_KIND_IMPORT(SALTS_STL_KIND_ROW_HashSet, name, type, container_expr, data_expr, methods_expr)
#define CSTL_TYPED_IMPORT_Heap(name, type, container_expr, methods_expr) \
  SALTS_STL_HEAP_IMPORT(name, type, container_expr, methods_expr)
#define CSTL_TYPED_IMPORT_Map(name, key, value, container_expr, data_expr, methods_expr) \
  SALTS_STL_KIND_IMPORT(SALTS_STL_KIND_ROW_Map, name, key, value, container_expr, data_expr, methods_expr)
#define CSTL_TYPED_IMPORT_HashMap(name, key, value, container_expr, data_expr, methods_expr) \
  SALTS_STL_KIND_IMPORT(SALTS_STL_KIND_ROW_HashMap, name, key, value, container_expr, data_expr, methods_expr)
#define CSTL_TYPED_IMPORT_MultiMap(name, key, value, container_expr, data_expr, methods_expr) \
  SALTS_STL_KIND_IMPORT(SALTS_STL_KIND_ROW_MultiMap, name, key, value, container_expr, data_expr, methods_expr)
#define CSTL_TYPED_IMPORT_BTree(name, key, value, container_expr, data_expr, methods_expr) \
  SALTS_STL_KIND_IMPORT(SALTS_STL_KIND_ROW_BTree, name, key, value, container_expr, data_expr, methods_expr)
#define CSTL_TYPED_IMPORT_BPlusTree(name, key, value, container_expr, data_expr, methods_expr) \
  SALTS_STL_KIND_IMPORT(SALTS_STL_KIND_ROW_BPlusTree, name, key, value, container_expr, data_expr, methods_expr)
#define cstl_typed_import(kind, name, ...) \
  CMETA_PP_CAT(CSTL_TYPED_IMPORT_, kind)(name, __VA_ARGS__)

/* CSTL is a finite CMeta Generic provider. One cmeta_type(...) declaration
 * emits the concrete wrapper type, typed ABI, metadata, Range views and
 * collector. The declaration/expression initializers above remain erased
 * handle construction forms and do not generate Generic types. */
#define CMETA_GENERIC_KIND_Vec CMETA_GENERIC_PROBE()
#define CMETA_GENERIC_KIND_Deque CMETA_GENERIC_PROBE()
#define CMETA_GENERIC_KIND_List CMETA_GENERIC_PROBE()
#define CMETA_GENERIC_KIND_Stack CMETA_GENERIC_PROBE()
#define CMETA_GENERIC_KIND_Queue CMETA_GENERIC_PROBE()
#define CMETA_GENERIC_KIND_Heap CMETA_GENERIC_PROBE()
#define CMETA_GENERIC_KIND_Set CMETA_GENERIC_PROBE()
#define CMETA_GENERIC_KIND_HashSet CMETA_GENERIC_PROBE()
#define CMETA_GENERIC_KIND_HashMap CMETA_GENERIC_PROBE()
#define CMETA_GENERIC_KIND_Map CMETA_GENERIC_PROBE()
#define CMETA_GENERIC_KIND_MultiMap CMETA_GENERIC_PROBE()
#define CMETA_GENERIC_KIND_BTree CMETA_GENERIC_PROBE()
#define CMETA_GENERIC_KIND_BPlusTree CMETA_GENERIC_PROBE()

#define CMETA_TYPED_UNARY_DISPATCH(kind, ...) \
  CMETA_PP_CAT(CMETA_PP_CAT(CMETA_TYPED_, kind), \
               CMETA_PP_CAT(_, CMETA_PP_NARG(__VA_ARGS__)))(__VA_ARGS__)
#define CMETA_TYPED_BINARY_DISPATCH(kind, ...) \
  CMETA_PP_CAT(CMETA_PP_CAT(CMETA_TYPED_, kind), \
               CMETA_PP_CAT(_, CMETA_PP_NARG(__VA_ARGS__)))(__VA_ARGS__)

#define CMETA_TYPED_Vec(...) CMETA_TYPED_UNARY_DISPATCH(Vec, __VA_ARGS__)
#define CMETA_TYPED_Vec_2(name, type) \
  SALTS_VEC_DEFINE(name, type) enum { name##_cmeta_typed = 1 }
#define CMETA_TYPED_Vec_4(name, type, type_desc, data_desc) \
  SALTS_VEC_DEFINE_EXPLICIT(name, type, type_desc, data_desc) enum { name##_cmeta_typed = 1 }
#define CMETA_TYPED_Vec_5(name, type, type_desc, data_desc, identity) \
  SALTS_VEC_DEFINE_EXPLICIT_IDENTITY(name, type, type_desc, data_desc, identity) enum { name##_cmeta_typed = 1 }

#define CMETA_TYPED_Deque(...) CMETA_TYPED_UNARY_DISPATCH(Deque, __VA_ARGS__)
#define CMETA_TYPED_Deque_2(name, type) \
  SALTS_DEQUE_DEFINE(name, type) enum { name##_cmeta_typed = 1 }
#define CMETA_TYPED_Deque_4(name, type, type_desc, data_desc) \
  SALTS_DEQUE_DEFINE_EXPLICIT(name, type, type_desc, data_desc) enum { name##_cmeta_typed = 1 }

#define CMETA_TYPED_List(...) CMETA_TYPED_UNARY_DISPATCH(List, __VA_ARGS__)
#define CMETA_TYPED_List_2(name, type) \
  SALTS_LIST_DEFINE(name, type) enum { name##_cmeta_typed = 1 }
#define CMETA_TYPED_List_4(name, type, type_desc, data_desc) \
  SALTS_LIST_DEFINE_EXPLICIT(name, type, type_desc, data_desc) enum { name##_cmeta_typed = 1 }

#define CMETA_TYPED_Stack(...) CMETA_TYPED_UNARY_DISPATCH(Stack, __VA_ARGS__)
#define CMETA_TYPED_Stack_2(name, type) \
  SALTS_STACK_DEFINE(name, type) enum { name##_cmeta_typed = 1 }
#define CMETA_TYPED_Stack_4(name, type, type_desc, data_desc) \
  SALTS_STACK_DEFINE_EXPLICIT(name, type, type_desc, data_desc) enum { name##_cmeta_typed = 1 }

#define CMETA_TYPED_Queue(...) CMETA_TYPED_UNARY_DISPATCH(Queue, __VA_ARGS__)
#define CMETA_TYPED_Queue_2(name, type) \
  SALTS_QUEUE_DEFINE(name, type) enum { name##_cmeta_typed = 1 }
#define CMETA_TYPED_Queue_4(name, type, type_desc, data_desc) \
  SALTS_QUEUE_DEFINE_EXPLICIT(name, type, type_desc, data_desc) enum { name##_cmeta_typed = 1 }

#define CMETA_TYPED_Heap(name, type) \
  SALTS_HEAP_DEFINE(name, type) enum { name##_cmeta_typed = 1 }

#define CMETA_TYPED_Set(...) CMETA_TYPED_UNARY_DISPATCH(Set, __VA_ARGS__)
#define CMETA_TYPED_Set_2(name, type) \
  SALTS_SET_DEFINE(name, type) enum { name##_cmeta_typed = 1 }
#define CMETA_TYPED_Set_4(name, type, type_desc, data_desc) \
  SALTS_SET_DEFINE_EXPLICIT(name, type, type_desc, data_desc) enum { name##_cmeta_typed = 1 }

#define CMETA_TYPED_HashSet(...) CMETA_TYPED_UNARY_DISPATCH(HashSet, __VA_ARGS__)
#define CMETA_TYPED_HashSet_2(name, type) \
  SALTS_HASH_SET_DEFINE(name, type) enum { name##_cmeta_typed = 1 }
#define CMETA_TYPED_HashSet_4(name, type, type_desc, data_desc) \
  SALTS_HASH_SET_DEFINE_EXPLICIT(name, type, type_desc, data_desc) enum { name##_cmeta_typed = 1 }

#define CMETA_TYPED_HashMap(...) CMETA_TYPED_BINARY_DISPATCH(HashMap, __VA_ARGS__)
#define CMETA_TYPED_HashMap_3(name, key_type, value_type) \
  SALTS_HASH_MAP_DEFINE(name, key_type, value_type) enum { name##_cmeta_typed = 1 }
#define CMETA_TYPED_HashMap_7(name, key_type, value_type, key_desc, key_data, value_desc, value_data) \
  SALTS_HASH_MAP_DEFINE_EXPLICIT(name, key_type, value_type, key_desc, key_data, value_desc, value_data) enum { name##_cmeta_typed = 1 }

#define CMETA_TYPED_Map(...) CMETA_TYPED_BINARY_DISPATCH(Map, __VA_ARGS__)
#define CMETA_TYPED_Map_3(name, key_type, value_type) \
  SALTS_MAP_DEFINE(name, key_type, value_type) enum { name##_cmeta_typed = 1 }
#define CMETA_TYPED_Map_7(name, key_type, value_type, key_desc, key_data, value_desc, value_data) \
  SALTS_MAP_DEFINE_EXPLICIT(name, key_type, value_type, key_desc, key_data, value_desc, value_data) enum { name##_cmeta_typed = 1 }
#define CMETA_TYPED_Map_8(name, key_type, value_type, key_desc, key_data, value_desc, value_data, identity) \
  SALTS_MAP_DEFINE_EXPLICIT_IDENTITY(name, key_type, value_type, key_desc, key_data, value_desc, value_data, identity) enum { name##_cmeta_typed = 1 }

#define CMETA_TYPED_MultiMap(...) CMETA_TYPED_BINARY_DISPATCH(MultiMap, __VA_ARGS__)
#define CMETA_TYPED_MultiMap_3(name, key_type, value_type) \
  SALTS_MULTI_MAP_DEFINE(name, key_type, value_type) enum { name##_cmeta_typed = 1 }
#define CMETA_TYPED_MultiMap_7(name, key_type, value_type, key_desc, key_data, value_desc, value_data) \
  SALTS_MULTI_MAP_DEFINE_EXPLICIT(name, key_type, value_type, key_desc, key_data, value_desc, value_data) enum { name##_cmeta_typed = 1 }

#define CMETA_TYPED_BTree(...) CMETA_TYPED_BINARY_DISPATCH(BTree, __VA_ARGS__)
#define CMETA_TYPED_BTree_3(name, key_type, value_type) \
  SALTS_BTREE_DEFINE(name, key_type, value_type) enum { name##_cmeta_typed = 1 }
#define CMETA_TYPED_BTree_7(name, key_type, value_type, key_desc, key_data, value_desc, value_data) \
  SALTS_BTREE_DEFINE_EXPLICIT(name, key_type, value_type, key_desc, key_data, value_desc, value_data) enum { name##_cmeta_typed = 1 }

#define CMETA_TYPED_BPlusTree(...) CMETA_TYPED_BINARY_DISPATCH(BPlusTree, __VA_ARGS__)
#define CMETA_TYPED_BPlusTree_3(name, key_type, value_type) \
  SALTS_BPLUS_TREE_DEFINE(name, key_type, value_type) enum { name##_cmeta_typed = 1 }
#define CMETA_TYPED_BPlusTree_7(name, key_type, value_type, key_desc, key_data, value_desc, value_data) \
  SALTS_BPLUS_TREE_DEFINE_EXPLICIT(name, key_type, value_type, key_desc, key_data, value_desc, value_data) enum { name##_cmeta_typed = 1 }

/* Generated Type_method functions are the typed calling convention. Raw
 * list_* and map_* names remain ordinary functions so every valid C argument
 * expression, including compound literals containing commas, is preserved. */

#endif

#endif /* SALTS_TYPED_H */
