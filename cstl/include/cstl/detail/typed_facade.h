#ifndef CSTL_DETAIL_TYPED_FACADE_H
#define CSTL_DETAIL_TYPED_FACADE_H

/* Internal typed-facade generation. Raw public headers never include this. */
#include <cmeta/container.h>
#include <cmeta/data_select.h>
#include <cmeta/function.h>
#include <cmeta/lifecycle.h>
#include <cmeta/operation.h>
#include <cstl/status.h>
#include <cstl/detail/instance_meta.h>

#include <string.h>

CMETA_INLINE cmeta_status cmeta_stl_cmeta_status(stl_status status) {
 switch (status) {
  case STL_OK: return CMETA_OK;
  case STL_INVALID_ARGUMENT: return CMETA_INVALID_ARGUMENT;
  case STL_OUT_OF_MEMORY: return CMETA_OUT_OF_MEMORY;
  case STL_CAPACITY_EXCEEDED: return CMETA_CAPACITY_EXCEEDED;
  case STL_TYPE_MISMATCH: return CMETA_TYPE_MISMATCH;
  case STL_TRAIT_MISSING: return CMETA_TRAIT_MISSING;
  case STL_EMPTY:
  case STL_NOT_FOUND: return CMETA_CALLBACK_ERROR;
  default: return CMETA_CALLBACK_ERROR;
 }
}

#define SALTS_META_VEC_COLLECTION_DATA_WITH_DATA(name,type,data_expr) \
 CMETA_INLINE const cmeta_data_desc *name##_collection_element(const void *object){(void)object;return (data_expr);} \
 CMETA_INLINE cmeta_status name##_collection_read(const void *object,cmeta_data_collection_view *out){const name *self=(const name*)object;const cmeta_data_desc *element;if(self==NULL||out==NULL)return CMETA_INVALID_ARGUMENT;element=(data_expr);if(element==NULL)return CMETA_TRAIT_MISSING;out->data=name##_data_const(self);out->count=name##_size(self);out->stride=sizeof(type);out->element=element;return CMETA_OK;} \
 CMETA_INLINE size_t name##_borrow_size(const void *object){const name *self=(const name*)object;return self!=NULL?name##_size(self):0u;} \
 CMETA_INLINE cmeta_gen_status name##_borrow_next(const void *object,cmeta_range_cursor *cursor,const void **out_element){const name *self=(const name*)object;size_t count;if(self==NULL||cursor==NULL||out_element==NULL)return CMETA_GEN_ERROR;*out_element=NULL;count=name##_size(self);if(cursor->index>=count)return CMETA_GEN_DONE;*out_element=name##_at_const(self,cursor->index);if(*out_element==NULL)return CMETA_GEN_ERROR;++cursor->index;return cursor->index==count?CMETA_GEN_VALUE_AND_DONE:CMETA_GEN_VALUE;} \
 CMETA_LOCAL const cmeta_data_collection_borrow_ops name##_borrow_ops={sizeof(cmeta_data_collection_borrow_ops),CMETA_DATA_COLLECTION_BORROW_OPS_ABI_VERSION,name##_borrow_size,name##_borrow_next,name##_cmeta_generation}; \
 CMETA_LOCAL const cmeta_data_collection_ops name##_collection_ops={sizeof(cmeta_data_collection_ops),CMETA_DATA_COLLECTION_OPS_ABI_VERSION,CMETA_TYPEOF_OR(name,&name##_cmeta_type),CMETA_DATA_COLLECTION_CONTIGUOUS|CMETA_DATA_COLLECTION_ORDERED|CMETA_DATA_COLLECTION_RANDOM_ACCESS,name##_collection_element,name##_collection_read,NULL,name##_collector_erased,&name##_borrow_ops,(data_expr),NULL,NULL}; \
 CMETA_LOCAL const cmeta_data_desc name##_collection_data={sizeof(cmeta_data_desc),CMETA_DATA_DESC_ABI_VERSION,#name ".data",#name,CMETA_DATA_SEQUENCE,CMETA_TYPEOF_OR(name,&name##_cmeta_type),NULL,NULL,NULL,NULL,NULL,NULL,&name##_collection_ops,NULL,&name##_construct_ops}; \
 CMETA_INLINE const cmeta_data_desc *name##_cmeta_data(void){return &name##_collection_data;}
#define SALTS_META_VEC_COLLECTION_DATA(name,type) \
 SALTS_META_VEC_COLLECTION_DATA_WITH_DATA(name,type,CMETA_DATAOF(type))
#define SALTS_META_INDEX_SEQUENCE_DATA_WITH_DATA(name,type,data_expr,semantic_kind) \
 CMETA_INLINE const cmeta_data_desc *name##_collection_element(const void *object){(void)object;return (data_expr);} \
 CMETA_INLINE cmeta_status name##_collection_foreach(const void *object,cmeta_data_collection_visit_fn visit,void *context,size_t max_items){const name *self=(const name*)object;size_t i,count;if(self==NULL||visit==NULL)return CMETA_INVALID_ARGUMENT;count=name##_size(self);if(count>max_items)return CMETA_CAPACITY_EXCEEDED;for(i=0u;i<count;++i){cmeta_status status=visit(context,name##_at_const(self,i));if(status!=CMETA_OK)return status;}return CMETA_OK;} \
 CMETA_INLINE size_t name##_borrow_size(const void *object){const name *self=(const name*)object;return self!=NULL?name##_size(self):0u;} \
 CMETA_INLINE cmeta_gen_status name##_borrow_next(const void *object,cmeta_range_cursor *cursor,const void **out_element){const name *self=(const name*)object;size_t count;if(self==NULL||cursor==NULL||out_element==NULL)return CMETA_GEN_ERROR;*out_element=NULL;count=name##_size(self);if(cursor->index>=count)return CMETA_GEN_DONE;*out_element=name##_at_const(self,cursor->index);if(*out_element==NULL)return CMETA_GEN_ERROR;++cursor->index;return cursor->index==count?CMETA_GEN_VALUE_AND_DONE:CMETA_GEN_VALUE;} \
 CMETA_LOCAL const cmeta_data_collection_borrow_ops name##_borrow_ops={sizeof(cmeta_data_collection_borrow_ops),CMETA_DATA_COLLECTION_BORROW_OPS_ABI_VERSION,name##_borrow_size,name##_borrow_next,name##_cmeta_generation}; \
 CMETA_LOCAL const cmeta_data_collection_ops name##_collection_ops={sizeof(cmeta_data_collection_ops),CMETA_DATA_COLLECTION_OPS_ABI_VERSION,CMETA_TYPEOF_OR(name,&name##_cmeta_type),CMETA_DATA_COLLECTION_ORDERED|CMETA_DATA_COLLECTION_RANDOM_ACCESS,name##_collection_element,NULL,name##_collection_foreach,name##_collector_erased,&name##_borrow_ops,(data_expr),NULL,NULL}; \
 CMETA_LOCAL const cmeta_data_desc name##_collection_data={sizeof(cmeta_data_desc),CMETA_DATA_DESC_ABI_VERSION,#name ".data",#name,semantic_kind,CMETA_TYPEOF_OR(name,&name##_cmeta_type),NULL,NULL,NULL,NULL,NULL,NULL,&name##_collection_ops,NULL,&name##_construct_ops}; \
 CMETA_INLINE const cmeta_data_desc *name##_cmeta_data(void){return &name##_collection_data;}



#define SALTS_META_INDEX_SEQUENCE_DATA(name,type,semantic_kind) \
 SALTS_META_INDEX_SEQUENCE_DATA_WITH_DATA(name,type,CMETA_DATAOF(type),semantic_kind)
#define SALTS_META_LIST_COLLECTION_DATA_WITH_DATA(name,type,data_expr) \
 CMETA_INLINE const cmeta_data_desc *name##_collection_element(const void *object){(void)object;return (data_expr);} \
 CMETA_INLINE cmeta_status name##_collection_foreach(const void *object,cmeta_data_collection_visit_fn visit,void *context,size_t max_items){const name *self=(const name*)object;cmeta_range_cursor cursor={0};const void *value=NULL;size_t count=0u;if(self==NULL||visit==NULL)return CMETA_INVALID_ARGUMENT;while(cmeta_stl_typed_list_range_next(&self->raw,&cursor,&value)){cmeta_status status;if(count>=max_items)return CMETA_CAPACITY_EXCEEDED;status=visit(context,value);if(status!=CMETA_OK)return status;++count;}return CMETA_OK;} \
 CMETA_INLINE size_t name##_borrow_size(const void *object){const name *self=(const name*)object;return self!=NULL?name##_size(self):0u;} \
 CMETA_INLINE cmeta_gen_status name##_borrow_next(const void *object,cmeta_range_cursor *cursor,const void **out_element){const name *self=(const name*)object;if(self==NULL||cursor==NULL||out_element==NULL)return CMETA_GEN_ERROR;*out_element=NULL;if(!cmeta_stl_typed_list_range_next(&self->raw,cursor,out_element))return CMETA_GEN_DONE;return CMETA_GEN_VALUE;} \
 CMETA_LOCAL const cmeta_data_collection_borrow_ops name##_borrow_ops={sizeof(cmeta_data_collection_borrow_ops),CMETA_DATA_COLLECTION_BORROW_OPS_ABI_VERSION,name##_borrow_size,name##_borrow_next,name##_cmeta_generation}; \
 CMETA_LOCAL const cmeta_data_collection_ops name##_collection_ops={sizeof(cmeta_data_collection_ops),CMETA_DATA_COLLECTION_OPS_ABI_VERSION,CMETA_TYPEOF_OR(name,&name##_cmeta_type),CMETA_DATA_COLLECTION_ORDERED,name##_collection_element,NULL,name##_collection_foreach,name##_collector_erased,&name##_borrow_ops,(data_expr),NULL,NULL}; \
 CMETA_LOCAL const cmeta_data_desc name##_collection_data={sizeof(cmeta_data_desc),CMETA_DATA_DESC_ABI_VERSION,#name ".data",#name,CMETA_DATA_SEQUENCE,CMETA_TYPEOF_OR(name,&name##_cmeta_type),NULL,NULL,NULL,NULL,NULL,NULL,&name##_collection_ops,NULL,&name##_construct_ops}; \
 CMETA_INLINE const cmeta_data_desc *name##_cmeta_data(void){return &name##_collection_data;}

#define SALTS_META_LIST_COLLECTION_DATA(name,type) \
 SALTS_META_LIST_COLLECTION_DATA_WITH_DATA(name,type,CMETA_DATAOF(type))
#define SALTS_META_SET_COLLECTION_DATA_WITH_DATA(name,type,data_expr,prefix,semantic_kind) \
 CMETA_INLINE const cmeta_data_desc *name##_collection_element(const void *object){(void)object;return (data_expr);} \
 CMETA_INLINE cmeta_status name##_collection_foreach(const void *object,cmeta_data_collection_visit_fn visit,void *context,size_t max_items){const name *self=(const name*)object;cmeta_range_cursor cursor={0};const void *value=NULL;size_t count=0u;if(self==NULL||visit==NULL)return CMETA_INVALID_ARGUMENT;while(prefix##_range_next(&self->raw,&cursor,&value)){cmeta_status status;if(count>=max_items)return CMETA_CAPACITY_EXCEEDED;status=visit(context,value);if(status!=CMETA_OK)return status;++count;}return CMETA_OK;} \
 CMETA_INLINE size_t name##_borrow_size(const void *object){const name *self=(const name*)object;return self!=NULL?name##_size(self):0u;} \
 CMETA_INLINE cmeta_gen_status name##_borrow_next(const void *object,cmeta_range_cursor *cursor,const void **out_element){const name *self=(const name*)object;if(self==NULL||cursor==NULL||out_element==NULL)return CMETA_GEN_ERROR;*out_element=NULL;if(!prefix##_range_next(&self->raw,cursor,out_element))return CMETA_GEN_DONE;return CMETA_GEN_VALUE;} \
 CMETA_LOCAL const cmeta_data_collection_borrow_ops name##_borrow_ops={sizeof(cmeta_data_collection_borrow_ops),CMETA_DATA_COLLECTION_BORROW_OPS_ABI_VERSION,name##_borrow_size,name##_borrow_next,name##_cmeta_generation}; \
 CMETA_LOCAL const cmeta_data_collection_ops name##_collection_ops={sizeof(cmeta_data_collection_ops),CMETA_DATA_COLLECTION_OPS_ABI_VERSION,CMETA_TYPEOF_OR(name,&name##_cmeta_type),CMETA_DATA_COLLECTION_ORDERED|CMETA_DATA_COLLECTION_SORTED|CMETA_DATA_COLLECTION_UNIQUE,name##_collection_element,NULL,name##_collection_foreach,name##_collector_erased,&name##_borrow_ops,(data_expr),NULL,NULL}; \
 CMETA_LOCAL const cmeta_data_desc name##_collection_data={sizeof(cmeta_data_desc),CMETA_DATA_DESC_ABI_VERSION,#name ".data",#name,semantic_kind,CMETA_TYPEOF_OR(name,&name##_cmeta_type),NULL,NULL,NULL,NULL,NULL,NULL,&name##_collection_ops,NULL,&name##_construct_ops}; \
 CMETA_INLINE const cmeta_data_desc *name##_cmeta_data(void){return &name##_collection_data;}

#define SALTS_META_SET_COLLECTION_DATA(name,type,prefix,semantic_kind) \
 SALTS_META_SET_COLLECTION_DATA_WITH_DATA(name,type,CMETA_DATAOF(type),prefix,semantic_kind)
#define SALTS_META_SLOT_SET_COLLECTION_DATA_WITH_DATA(name,type,data_expr,prefix,semantic_kind) \
 CMETA_INLINE const cmeta_data_desc *name##_collection_element(const void *object){(void)object;return (data_expr);} \
 CMETA_INLINE cmeta_status name##_collection_foreach(const void *object,cmeta_data_collection_visit_fn visit,void *context,size_t max_items){const name *self=(const name*)object;size_t slot,count=0u,capacity;if(self==NULL||visit==NULL)return CMETA_INVALID_ARGUMENT;capacity=prefix##_capacity(&self->raw);for(slot=0u;slot<capacity;++slot){const void *value=prefix##_key_at(&self->raw,slot);cmeta_status status;if(value==NULL)continue;if(count>=max_items)return CMETA_CAPACITY_EXCEEDED;status=visit(context,value);if(status!=CMETA_OK)return status;++count;}return CMETA_OK;} \
 CMETA_INLINE size_t name##_borrow_size(const void *object){const name *self=(const name*)object;return self!=NULL?name##_size(self):0u;} \
 CMETA_INLINE cmeta_gen_status name##_borrow_next(const void *object,cmeta_range_cursor *cursor,const void **out_element){const name *self=(const name*)object;size_t capacity;if(self==NULL||cursor==NULL||out_element==NULL)return CMETA_GEN_ERROR;*out_element=NULL;capacity=prefix##_capacity(&self->raw);while(cursor->index<capacity){const void *value=prefix##_key_at(&self->raw,cursor->index++);if(value!=NULL){*out_element=value;return CMETA_GEN_VALUE;}}return CMETA_GEN_DONE;} \
 CMETA_LOCAL const cmeta_data_collection_borrow_ops name##_borrow_ops={sizeof(cmeta_data_collection_borrow_ops),CMETA_DATA_COLLECTION_BORROW_OPS_ABI_VERSION,name##_borrow_size,name##_borrow_next,name##_cmeta_generation}; \
 CMETA_LOCAL const cmeta_data_collection_ops name##_collection_ops={sizeof(cmeta_data_collection_ops),CMETA_DATA_COLLECTION_OPS_ABI_VERSION,CMETA_TYPEOF_OR(name,&name##_cmeta_type),CMETA_DATA_COLLECTION_UNIQUE,name##_collection_element,NULL,name##_collection_foreach,name##_collector_erased,&name##_borrow_ops,(data_expr),NULL,NULL}; \
 CMETA_LOCAL const cmeta_data_desc name##_collection_data={sizeof(cmeta_data_desc),CMETA_DATA_DESC_ABI_VERSION,#name ".data",#name,semantic_kind,CMETA_TYPEOF_OR(name,&name##_cmeta_type),NULL,NULL,NULL,NULL,NULL,NULL,&name##_collection_ops,NULL,&name##_construct_ops}; \
 CMETA_INLINE const cmeta_data_desc *name##_cmeta_data(void){return &name##_collection_data;}

#define SALTS_META_SLOT_SET_COLLECTION_DATA(name,type,prefix,semantic_kind) \
 SALTS_META_SLOT_SET_COLLECTION_DATA_WITH_DATA(name,type,CMETA_DATAOF(type),prefix,semantic_kind)
#define SALTS_META_MAP_DATA_WITH_DATA(name,key_type,value_type,key_data_expr,value_data_expr) \
 CMETA_INLINE const cmeta_data_desc *name##_map_key(const void *object){(void)object;return (key_data_expr);} \
 CMETA_INLINE const cmeta_data_desc *name##_map_value(const void *object){(void)object;return (value_data_expr);} \
 CMETA_INLINE cmeta_status name##_map_foreach(const void *object,cmeta_data_map_visit_fn visit,void *context,size_t max_items){const name *self=(const name*)object;cmeta_range_cursor cursor={0};const void *key=NULL,*value=NULL;size_t count=0u;if(self==NULL||visit==NULL)return CMETA_INVALID_ARGUMENT;while(cmeta_stl_typed_map_range_next(&self->raw,&cursor,&key,&value)){cmeta_status status;if(count>=max_items)return CMETA_CAPACITY_EXCEEDED;status=visit(context,key,value);if(status!=CMETA_OK)return status;++count;}return CMETA_OK;} \
 CMETA_INLINE size_t name##_map_borrow_size(const void *object){const name *self=(const name*)object;return self!=NULL?name##_size(self):0u;} \
 CMETA_INLINE cmeta_gen_status name##_map_borrow_next(const void *object,cmeta_range_cursor *cursor,const void **out_key,const void **out_value){const name *self=(const name*)object;if(self==NULL||cursor==NULL||out_key==NULL||out_value==NULL)return CMETA_GEN_ERROR;*out_key=NULL;*out_value=NULL;return cmeta_stl_typed_map_range_next(&self->raw,cursor,out_key,out_value)?CMETA_GEN_VALUE:CMETA_GEN_DONE;} \
 CMETA_LOCAL const cmeta_data_map_borrow_ops name##_map_borrow_ops={sizeof(cmeta_data_map_borrow_ops),CMETA_DATA_MAP_BORROW_OPS_ABI_VERSION,name##_map_borrow_size,name##_map_borrow_next,name##_cmeta_generation}; \
 CMETA_LOCAL const cmeta_data_map_ops name##_map_ops={sizeof(cmeta_data_map_ops),CMETA_DATA_MAP_OPS_ABI_VERSION,CMETA_TYPEOF_OR(name,&name##_cmeta_type),CMETA_DATA_MAP_UNIQUE_KEYS|CMETA_DATA_MAP_ORDERED|CMETA_DATA_MAP_SORTED,name##_map_key,name##_map_value,name##_map_foreach,name##_collector_erased,name##_map_accept,&name##_map_borrow_ops,(key_data_expr),(value_data_expr)}; \
 CMETA_LOCAL const cmeta_data_desc name##_map_data={sizeof(cmeta_data_desc),CMETA_DATA_DESC_ABI_VERSION,#name ".data",#name,CMETA_DATA_MAP,CMETA_TYPEOF_OR(name,&name##_cmeta_type),NULL,NULL,NULL,NULL,NULL,NULL,NULL,&name##_map_ops,&name##_construct_ops}; \
 CMETA_INLINE const cmeta_data_desc *name##_cmeta_data(void){return &name##_map_data;}

#define SALTS_META_MAP_DATA(name,key_type,value_type) \
 SALTS_META_MAP_DATA_WITH_DATA(name,key_type,value_type,CMETA_DATAOF(key_type),CMETA_DATAOF(value_type))
#define SALTS_META_HASH_MAP_DATA_WITH_DATA(name,key_type,value_type,key_data_expr,value_data_expr) \
 CMETA_INLINE const cmeta_data_desc *name##_map_key(const void *object){(void)object;return (key_data_expr);} \
 CMETA_INLINE const cmeta_data_desc *name##_map_value(const void *object){(void)object;return (value_data_expr);} \
 CMETA_INLINE cmeta_status name##_map_foreach(const void *object,cmeta_data_map_visit_fn visit,void *context,size_t max_items){const name *self=(const name*)object;size_t slot,count=0u,capacity;if(self==NULL||visit==NULL)return CMETA_INVALID_ARGUMENT;capacity=hash_map_capacity(&self->raw);for(slot=0u;slot<capacity;++slot){const void *key=hash_map_key_at_const(&self->raw,slot);const void *value;cmeta_status status;if(key==NULL)continue;value=hash_map_value_at_const(&self->raw,slot);if(value==NULL)return CMETA_CALLBACK_ERROR;if(count>=max_items)return CMETA_CAPACITY_EXCEEDED;status=visit(context,key,value);if(status!=CMETA_OK)return status;++count;}return CMETA_OK;} \
 CMETA_INLINE size_t name##_map_borrow_size(const void *object){const name *self=(const name*)object;return self!=NULL?name##_size(self):0u;} \
 CMETA_INLINE cmeta_gen_status name##_map_borrow_next(const void *object,cmeta_range_cursor *cursor,const void **out_key,const void **out_value){const name *self=(const name*)object;size_t capacity;if(self==NULL||cursor==NULL||out_key==NULL||out_value==NULL)return CMETA_GEN_ERROR;*out_key=NULL;*out_value=NULL;capacity=hash_map_capacity(&self->raw);while(cursor->index<capacity){const void *key=hash_map_key_at_const(&self->raw,cursor->index);const void *value=hash_map_value_at_const(&self->raw,cursor->index++);if(key!=NULL){if(value==NULL)return CMETA_GEN_ERROR;*out_key=key;*out_value=value;return CMETA_GEN_VALUE;}}return CMETA_GEN_DONE;} \
 CMETA_LOCAL const cmeta_data_map_borrow_ops name##_map_borrow_ops={sizeof(cmeta_data_map_borrow_ops),CMETA_DATA_MAP_BORROW_OPS_ABI_VERSION,name##_map_borrow_size,name##_map_borrow_next,name##_cmeta_generation}; \
 CMETA_LOCAL const cmeta_data_map_ops name##_map_ops={sizeof(cmeta_data_map_ops),CMETA_DATA_MAP_OPS_ABI_VERSION,CMETA_TYPEOF_OR(name,&name##_cmeta_type),CMETA_DATA_MAP_UNIQUE_KEYS,name##_map_key,name##_map_value,name##_map_foreach,name##_collector_erased,name##_map_accept,&name##_map_borrow_ops,(key_data_expr),(value_data_expr)}; \
 CMETA_LOCAL const cmeta_data_desc name##_map_data={sizeof(cmeta_data_desc),CMETA_DATA_DESC_ABI_VERSION,#name ".data",#name,CMETA_DATA_MAP,CMETA_TYPEOF_OR(name,&name##_cmeta_type),NULL,NULL,NULL,NULL,NULL,NULL,NULL,&name##_map_ops,&name##_construct_ops}; \
 CMETA_INLINE const cmeta_data_desc *name##_cmeta_data(void){return &name##_map_data;}

#define SALTS_META_HASH_MAP_DATA(name,key_type,value_type) \
 SALTS_META_HASH_MAP_DATA_WITH_DATA(name,key_type,value_type,CMETA_DATAOF(key_type),CMETA_DATAOF(value_type))
#define SALTS_META_TREE_MAP_DATA_WITH_DATA(name,key_type,value_type,key_data_expr,value_data_expr,prefix) \
 CMETA_INLINE const cmeta_data_desc *name##_map_key(const void *object){(void)object;return (key_data_expr);} \
 CMETA_INLINE const cmeta_data_desc *name##_map_value(const void *object){(void)object;return (value_data_expr);} \
 CMETA_INLINE cmeta_status name##_map_foreach(const void *object,cmeta_data_map_visit_fn visit,void *context,size_t max_items){const name *self=(const name*)object;cmeta_range_cursor cursor={0};const void *key=NULL,*value=NULL;size_t count=0u;if(self==NULL||visit==NULL)return CMETA_INVALID_ARGUMENT;while(prefix##_range_next(&self->raw,&cursor,&key,&value)){cmeta_status status;if(count>=max_items)return CMETA_CAPACITY_EXCEEDED;status=visit(context,key,value);if(status!=CMETA_OK)return status;++count;}return CMETA_OK;} \
 CMETA_INLINE size_t name##_map_borrow_size(const void *object){const name *self=(const name*)object;return self!=NULL?name##_size(self):0u;} \
 CMETA_INLINE cmeta_gen_status name##_map_borrow_next(const void *object,cmeta_range_cursor *cursor,const void **out_key,const void **out_value){const name *self=(const name*)object;if(self==NULL||cursor==NULL||out_key==NULL||out_value==NULL)return CMETA_GEN_ERROR;*out_key=NULL;*out_value=NULL;return prefix##_range_next(&self->raw,cursor,out_key,out_value)?CMETA_GEN_VALUE:CMETA_GEN_DONE;} \
 CMETA_LOCAL const cmeta_data_map_borrow_ops name##_map_borrow_ops={sizeof(cmeta_data_map_borrow_ops),CMETA_DATA_MAP_BORROW_OPS_ABI_VERSION,name##_map_borrow_size,name##_map_borrow_next,name##_cmeta_generation}; \
 CMETA_LOCAL const cmeta_data_map_ops name##_map_ops={sizeof(cmeta_data_map_ops),CMETA_DATA_MAP_OPS_ABI_VERSION,CMETA_TYPEOF_OR(name,&name##_cmeta_type),CMETA_DATA_MAP_UNIQUE_KEYS|CMETA_DATA_MAP_ORDERED|CMETA_DATA_MAP_SORTED,name##_map_key,name##_map_value,name##_map_foreach,name##_collector_erased,name##_map_accept,&name##_map_borrow_ops,(key_data_expr),(value_data_expr)}; \
 CMETA_LOCAL const cmeta_data_desc name##_map_data={sizeof(cmeta_data_desc),CMETA_DATA_DESC_ABI_VERSION,#name ".data",#name,CMETA_DATA_MAP,CMETA_TYPEOF_OR(name,&name##_cmeta_type),NULL,NULL,NULL,NULL,NULL,NULL,NULL,&name##_map_ops,&name##_construct_ops}; \
 CMETA_INLINE const cmeta_data_desc *name##_cmeta_data(void){return &name##_map_data;}

#define SALTS_META_TREE_MAP_DATA(name,key_type,value_type,prefix) \
 SALTS_META_TREE_MAP_DATA_WITH_DATA(name,key_type,value_type,CMETA_DATAOF(key_type),CMETA_DATAOF(value_type),prefix)
#define SALTS_META_MULTIMAP_DATA_WITH_DATA(name,key_type,value_type,key_data_expr,value_data_expr) \
 CMETA_INLINE const cmeta_data_desc *name##_map_key(const void *object){(void)object;return (key_data_expr);} \
 CMETA_INLINE const cmeta_data_desc *name##_map_value(const void *object){(void)object;return (value_data_expr);} \
 CMETA_INLINE cmeta_status name##_map_foreach(const void *object,cmeta_data_map_visit_fn visit,void *context,size_t max_items){const name *self=(const name*)object;cmeta_range_cursor cursor={0};const void *key=NULL,*value=NULL;size_t count=0u;if(self==NULL||visit==NULL)return CMETA_INVALID_ARGUMENT;while(multimap_range_next(&self->raw,&cursor,&key,&value)){cmeta_status status;if(count>=max_items)return CMETA_CAPACITY_EXCEEDED;status=visit(context,key,value);if(status!=CMETA_OK)return status;++count;}return CMETA_OK;} \
 CMETA_INLINE size_t name##_map_borrow_size(const void *object){const name *self=(const name*)object;return self!=NULL?name##_size(self):0u;} \
 CMETA_INLINE cmeta_gen_status name##_map_borrow_next(const void *object,cmeta_range_cursor *cursor,const void **out_key,const void **out_value){const name *self=(const name*)object;if(self==NULL||cursor==NULL||out_key==NULL||out_value==NULL)return CMETA_GEN_ERROR;*out_key=NULL;*out_value=NULL;return multimap_range_next(&self->raw,cursor,out_key,out_value)?CMETA_GEN_VALUE:CMETA_GEN_DONE;} \
 CMETA_LOCAL const cmeta_data_map_borrow_ops name##_map_borrow_ops={sizeof(cmeta_data_map_borrow_ops),CMETA_DATA_MAP_BORROW_OPS_ABI_VERSION,name##_map_borrow_size,name##_map_borrow_next,name##_cmeta_generation}; \
 CMETA_LOCAL const cmeta_data_map_ops name##_map_ops={sizeof(cmeta_data_map_ops),CMETA_DATA_MAP_OPS_ABI_VERSION,CMETA_TYPEOF_OR(name,&name##_cmeta_type),CMETA_DATA_MAP_REPEATED_KEYS|CMETA_DATA_MAP_ORDERED|CMETA_DATA_MAP_SORTED,name##_map_key,name##_map_value,name##_map_foreach,name##_collector_erased,name##_map_accept,&name##_map_borrow_ops,(key_data_expr),(value_data_expr)}; \
 CMETA_LOCAL const cmeta_data_desc name##_map_data={sizeof(cmeta_data_desc),CMETA_DATA_DESC_ABI_VERSION,#name ".data",#name,CMETA_DATA_MAP,CMETA_TYPEOF_OR(name,&name##_cmeta_type),NULL,NULL,NULL,NULL,NULL,NULL,NULL,&name##_map_ops,&name##_construct_ops}; \
 CMETA_INLINE const cmeta_data_desc *name##_cmeta_data(void){return &name##_map_data;}

#define SALTS_META_MULTIMAP_DATA(name,key_type,value_type) \
 SALTS_META_MULTIMAP_DATA_WITH_DATA(name,key_type,value_type,CMETA_DATAOF(key_type),CMETA_DATAOF(value_type))
#define SALTS_META_C1_CONSTRUCT(name,init_expr,destroy_expr) \
 CMETA_INLINE cmeta_status name##_construct_init_zero(void *object){name *self=(name*)object;if(self==NULL)return CMETA_INVALID_ARGUMENT;*self=(name){.cmeta={&name##_cmeta_container_desc},.raw=init_expr};return CMETA_OK;} \
 CMETA_INLINE void name##_construct_restore_zero(void *object){name *self=(name*)object;if(self==NULL)return;destroy_expr;*self=(name){.cmeta={&name##_cmeta_container_desc},.raw=init_expr};} \
 CMETA_INLINE void name##_construct_move(void *destination,void *source){name *dst=(name*)destination;name *src=(name*)source;if(dst==NULL||src==NULL||dst==src)return;{name *self=dst;destroy_expr;}*dst=*src;*src=(name){.cmeta={&name##_cmeta_container_desc},.raw=init_expr};} \
 enum { name##_construct_flags = CMETA_LIFECYCLE_INIT_NOFAIL | CMETA_LIFECYCLE_MOVABLE }; \
 CMETA_LOCAL const cmeta_data_construct_ops name##_construct_ops={sizeof(cmeta_data_construct_ops),CMETA_DATA_CONSTRUCT_OPS_ABI_VERSION,CMETA_TYPEOF_OR(name,&name##_cmeta_type),name##_construct_init_zero,name##_construct_restore_zero,name##_construct_move,name##_construct_flags}; \
 CMETA_DEFINE_STATIC_LIFECYCLE(name, name##_construct_ops, name##_construct_flags)

#define SALTS_META_C2_CONSTRUCT(name,init_expr,destroy_expr) \
 CMETA_INLINE cmeta_status name##_construct_init_zero(void *object){name *self=(name*)object;if(self==NULL)return CMETA_INVALID_ARGUMENT;*self=(name){.cmeta={&name##_cmeta_container_desc},.raw=init_expr};return CMETA_OK;} \
 CMETA_INLINE void name##_construct_restore_zero(void *object){name *self=(name*)object;if(self==NULL)return;destroy_expr;*self=(name){.cmeta={&name##_cmeta_container_desc},.raw=init_expr};} \
 CMETA_INLINE void name##_construct_move(void *destination,void *source){name *dst=(name*)destination;name *src=(name*)source;if(dst==NULL||src==NULL||dst==src)return;{name *self=dst;destroy_expr;}*dst=*src;*src=(name){.cmeta={&name##_cmeta_container_desc},.raw=init_expr};} \
 enum { name##_construct_flags = CMETA_LIFECYCLE_INIT_NOFAIL | CMETA_LIFECYCLE_MOVABLE }; \
 CMETA_LOCAL const cmeta_data_construct_ops name##_construct_ops={sizeof(cmeta_data_construct_ops),CMETA_DATA_CONSTRUCT_OPS_ABI_VERSION,CMETA_TYPEOF_OR(name,&name##_cmeta_type),name##_construct_init_zero,name##_construct_restore_zero,name##_construct_move,name##_construct_flags}; \
 CMETA_DEFINE_STATIC_LIFECYCLE(name, name##_construct_ops, name##_construct_flags)



#define SALTS_META_MAP_ACCEPT(name,key_type,value_type) \
 CMETA_INLINE cmeta_status name##_map_accept(cmeta_collector *collector,const cmeta_data_desc *key_data,const void *key,const cmeta_data_desc *value_data,const void *value){name##_entry entry;if(collector==NULL||key_data==NULL||value_data==NULL||key==NULL||value==NULL)return CMETA_INVALID_ARGUMENT;entry.key=*(const key_type*)key;entry.value=*(const value_type*)value;return cmeta_collector_accept(collector,&name##_entry_cmeta_type,&entry);}

#define SALTS_META_ADAPTER_SEQUENCE_DATA_WITH_DATA(name,type,data_expr,prefix,semantic_kind) \
 CMETA_INLINE const cmeta_data_desc *name##_collection_element(const void *object){(void)object;return (data_expr);} \
 CMETA_INLINE cmeta_status name##_collection_foreach(const void *object,cmeta_data_collection_visit_fn visit,void *context,size_t max_items){const name *self=(const name*)object;size_t i,count;if(self==NULL||visit==NULL)return CMETA_INVALID_ARGUMENT;count=prefix##_size(&self->raw);if(count>max_items)return CMETA_CAPACITY_EXCEEDED;for(i=0u;i<count;++i){const void *value=prefix##_at_const(&self->raw,i);cmeta_status status;if(value==NULL)return CMETA_CALLBACK_ERROR;status=visit(context,value);if(status!=CMETA_OK)return status;}return CMETA_OK;} \
 CMETA_INLINE size_t name##_borrow_size(const void *object){const name *self=(const name*)object;return self!=NULL?prefix##_size(&self->raw):0u;} \
 CMETA_INLINE cmeta_gen_status name##_borrow_next(const void *object,cmeta_range_cursor *cursor,const void **out_element){const name *self=(const name*)object;size_t count;if(self==NULL||cursor==NULL||out_element==NULL)return CMETA_GEN_ERROR;*out_element=NULL;count=prefix##_size(&self->raw);if(cursor->index>=count)return CMETA_GEN_DONE;*out_element=prefix##_at_const(&self->raw,cursor->index);if(*out_element==NULL)return CMETA_GEN_ERROR;++cursor->index;return cursor->index==count?CMETA_GEN_VALUE_AND_DONE:CMETA_GEN_VALUE;} \
 CMETA_LOCAL const cmeta_data_collection_borrow_ops name##_borrow_ops={sizeof(cmeta_data_collection_borrow_ops),CMETA_DATA_COLLECTION_BORROW_OPS_ABI_VERSION,name##_borrow_size,name##_borrow_next,name##_cmeta_generation}; \
 CMETA_LOCAL const cmeta_data_collection_ops name##_collection_ops={sizeof(cmeta_data_collection_ops),CMETA_DATA_COLLECTION_OPS_ABI_VERSION,CMETA_TYPEOF_OR(name,&name##_cmeta_type),CMETA_DATA_COLLECTION_ORDERED|CMETA_DATA_COLLECTION_RANDOM_ACCESS,name##_collection_element,NULL,name##_collection_foreach,name##_collector_erased,&name##_borrow_ops,(data_expr),NULL,NULL}; \
 CMETA_LOCAL const cmeta_data_desc name##_collection_data={sizeof(cmeta_data_desc),CMETA_DATA_DESC_ABI_VERSION,#name ".data",#name,semantic_kind,CMETA_TYPEOF_OR(name,&name##_cmeta_type),NULL,NULL,NULL,NULL,NULL,NULL,&name##_collection_ops,NULL,&name##_construct_ops}; \
 CMETA_INLINE const cmeta_data_desc *name##_cmeta_data(void){return &name##_collection_data;}

#define SALTS_META_ADAPTER_SEQUENCE_DATA(name,type,prefix,semantic_kind) \
 SALTS_META_ADAPTER_SEQUENCE_DATA_WITH_DATA(name,type,CMETA_DATAOF(type),prefix,semantic_kind)

#define SALTS_META_C1_COLLECTOR_WITH_TYPE(name,type,type_desc,accept_method) \
 CMETA_INLINE cmeta_status name##_collector_begin_cb(void *context,const cmeta_type_desc *input,size_t limit){name *output=(name*)context;if(output==NULL)return CMETA_INVALID_ARGUMENT;if(!cmeta_type_equal(input,(type_desc)))return CMETA_TYPE_MISMATCH;name##_destroy(output);return cmeta_stl_cmeta_status((stl_status)name##_init(output,limit));} \
 CMETA_INLINE cmeta_status name##_collector_accept_cb(void *context,const void *value){return cmeta_stl_cmeta_status((stl_status)name##_##accept_method((name*)context,*(const type*)value));} \
 CMETA_INLINE cmeta_status name##_collector_finish_cb(void *context){(void)context;return CMETA_OK;} \
 CMETA_INLINE void name##_collector_abort_cb(void *context){name *output=(name*)context;if(output){name##_destroy(output);memset(output,0,sizeof(*output));}} \
 CMETA_LOCAL const cmeta_collector_ops name##_collector_ops={name##_collector_begin_cb,name##_collector_accept_cb,name##_collector_finish_cb,name##_collector_abort_cb}; \
 CMETA_INLINE cmeta_collector name##_collector(name *zero_output,size_t limit){cmeta_collector result={&name##_collector_ops,zero_output,zero_output,(type_desc),limit,0u,CMETA_COLLECTOR_ZERO,CMETA_OK};return result;} \
 CMETA_INLINE cmeta_collector name##_collector_erased(void *zero_output,size_t limit){return name##_collector((name*)zero_output,limit);}
#define SALTS_META_C1_COLLECTOR(name,type,accept_method) \
 SALTS_META_C1_COLLECTOR_WITH_TYPE(name,type,CMETA_TYPEOF(type),accept_method)

#define SALTS_META_C2_COLLECTOR(name) \
 CMETA_LOCAL cmeta_type_desc name##_entry_cmeta_type; \
 CMETA_INLINE cmeta_status name##_collector_begin_cb(void *context,const cmeta_type_desc *input,size_t limit){name *output=(name*)context;if(output==NULL)return CMETA_INVALID_ARGUMENT;if(!cmeta_type_equal(input,&name##_entry_cmeta_type))return CMETA_TYPE_MISMATCH;if(cmeta_type_require_traits(input,CMETA_TRAIT_COPY|CMETA_TRAIT_MOVE|CMETA_TRAIT_DESTROY)!=CMETA_OK)return CMETA_TRAIT_MISSING;name##_destroy(output);return cmeta_stl_cmeta_status((stl_status)name##_init(output,limit));} \
 CMETA_INLINE cmeta_status name##_collector_accept_cb(void *context,const void *value){const name##_entry *entry=(const name##_entry*)value;return cmeta_stl_cmeta_status((stl_status)name##_put((name*)context,entry->key,entry->value));} \
 CMETA_INLINE cmeta_status name##_collector_finish_cb(void *context){(void)context;return CMETA_OK;} \
 CMETA_INLINE void name##_collector_abort_cb(void *context){name *output=(name*)context;if(output){name##_destroy(output);memset(output,0,sizeof(*output));}} \
 CMETA_LOCAL const cmeta_collector_ops name##_collector_ops={name##_collector_begin_cb,name##_collector_accept_cb,name##_collector_finish_cb,name##_collector_abort_cb}; \
 CMETA_INLINE cmeta_collector name##_collector(name *zero_output,size_t limit){cmeta_collector result={&name##_collector_ops,zero_output,zero_output,&name##_entry_cmeta_type,limit,0u,CMETA_COLLECTOR_ZERO,CMETA_OK};return result;} \
 CMETA_INLINE cmeta_collector name##_collector_erased(void *zero_output,size_t limit){return name##_collector((name*)zero_output,limit);}

/* A multimap collector's limit bounds total retained key/value pairs. */
#define SALTS_META_MULTIMAP_COLLECTOR(name) \
 CMETA_LOCAL cmeta_type_desc name##_entry_cmeta_type; \
 CMETA_INLINE cmeta_status name##_collector_begin_cb(void *context,const cmeta_type_desc *input,size_t limit){name *output=(name*)context;if(output==NULL)return CMETA_INVALID_ARGUMENT;if(!cmeta_type_equal(input,&name##_entry_cmeta_type))return CMETA_TYPE_MISMATCH;if(cmeta_type_require_traits(input,CMETA_TRAIT_COPY|CMETA_TRAIT_MOVE|CMETA_TRAIT_DESTROY)!=CMETA_OK)return CMETA_TRAIT_MISSING;name##_destroy(output);return cmeta_stl_cmeta_status((stl_status)name##_init(output,limit));} \
 CMETA_INLINE cmeta_status name##_collector_accept_cb(void *context,const void *value){const name##_entry *entry=(const name##_entry*)value;return cmeta_stl_cmeta_status((stl_status)name##_put((name*)context,entry->key,entry->value));} \
 CMETA_INLINE cmeta_status name##_collector_finish_cb(void *context){(void)context;return CMETA_OK;} \
 CMETA_INLINE void name##_collector_abort_cb(void *context){name *output=(name*)context;if(output){name##_destroy(output);memset(output,0,sizeof(*output));}} \
 CMETA_LOCAL const cmeta_collector_ops name##_collector_ops={name##_collector_begin_cb,name##_collector_accept_cb,name##_collector_finish_cb,name##_collector_abort_cb}; \
 CMETA_INLINE cmeta_collector name##_collector(name *zero_output,size_t limit){cmeta_collector result={&name##_collector_ops,zero_output,zero_output,&name##_entry_cmeta_type,limit,0u,CMETA_COLLECTOR_ZERO,CMETA_OK};return result;} \
 CMETA_INLINE cmeta_collector name##_collector_erased(void *zero_output,size_t limit){return name##_collector((name*)zero_output,limit);}

#define SALTS_META_VEC_METHODS(M,C) \
 M(INIT_SIZE,init,raw_init,_,C) M(FROM_ARRAY_SIZE,from,raw_from_array,_,C) \
 M(DESTROY,destroy,raw_destroy_storage,_,C) M(CLEAR,clear,clear,_,C) \
 M(RESERVE,reserve,reserve,_,C) M(PUSH_VALUE,push,push,_,C) \
 M(POP_BOOL,pop,pop,_,C) M(PTR_INDEX,at,at,_,C) \
 M(CONST_PTR_INDEX,at_const,at_const,_,C) M(PTR,data,data,_,C) \
 M(CONST_PTR,data_const,data_const,_,C) M(SIZE,size,size,_,C) \
 M(SIZE,capacity,capacity,_,C) M(BOOL,empty,empty,_,C)

#define SALTS_META_DEQUE_METHODS(M,C) \
 M(INIT_SIZE,init,raw_init,_,C) M(FROM_ARRAY_SIZE,from,raw_from_array,_,C) \
 M(DESTROY,destroy,raw_destroy_storage,_,C) M(CLEAR,clear,clear,_,C) \
 M(RESERVE,reserve,reserve,_,C) M(PUSH_VALUE,push_back,push_back,_,C) \
 M(PUSH_VALUE,push_front,push_front,_,C) M(POP_BOOL,pop_back,pop_back,_,C) \
 M(POP_BOOL,pop_front,pop_front,_,C) M(PTR,front,front,_,C) \
 M(CONST_PTR,front_const,front_const,_,C) M(PTR,back,back,_,C) \
 M(CONST_PTR,back_const,back_const,_,C) M(PTR_INDEX,at,at,_,C) \
 M(CONST_PTR_INDEX,at_const,at_const,_,C) M(SIZE,size,size,_,C) \
 M(SIZE,capacity,capacity,_,C) M(BOOL,empty,empty,_,C)

#define SALTS_META_LIST_METHODS(M,C) \
 M(INIT_SIZE,init,raw_init,_,C) M(FROM_ARRAY_SIZE,from,raw_from_array,_,C) \
 M(DESTROY,destroy,raw_destroy_storage,_,C) M(CLEAR,clear,clear,_,C) \
 M(PUSH_VALUE_ITER,add,push_back,_,C) \
 M(PUSH_VALUE_ITER,push_back,push_back,_,C) \
 M(PUSH_VALUE_ITER,push_front,push_front,_,C) \
 M(POP_BOOL,pop_back,pop_back,_,C) M(POP_BOOL,pop_front,pop_front,_,C) \
 M(PTR,front,front,_,C) M(CONST_PTR,front_const,front_const,_,C) \
 M(PTR,back,back,_,C) M(CONST_PTR,back_const,back_const,_,C) \
 M(SIZE,size,size,_,C) M(BOOL,empty,empty,_,C)

#define SALTS_META_STACK_METHODS(M,C) \
 M(INIT_SIZE,init,raw_init,_,C) M(FROM_ARRAY_SIZE,from,raw_from_array,_,C) \
 M(DESTROY,destroy,destroy,_,C) M(CLEAR,clear,clear,_,C) \
 M(RESERVE,reserve,reserve,_,C) M(PUSH_VALUE,push,push,_,C) \
 M(POP_BOOL,pop,pop,_,C) M(PTR,top,top,_,C) \
 M(CONST_PTR,top_const,top_const,_,C) M(SIZE,size,size,_,C) \
 M(SIZE,capacity,capacity,_,C) M(BOOL,empty,empty,_,C)

#define SALTS_META_QUEUE_METHODS(M,C) \
 M(INIT_SIZE,init,raw_init,_,C) M(FROM_ARRAY_SIZE,from,raw_from_array,_,C) \
 M(DESTROY,destroy,destroy,_,C) M(CLEAR,clear,clear,_,C) \
 M(RESERVE,reserve,reserve,_,C) M(PUSH_VALUE,push,push,_,C) \
 M(POP_BOOL,pop,pop,_,C) M(PTR,front,front,_,C) \
 M(CONST_PTR,front_const,front_const,_,C) M(PTR,back,back,_,C) \
 M(CONST_PTR,back_const,back_const,_,C) M(SIZE,size,size,_,C) \
 M(SIZE,capacity,capacity,_,C) M(BOOL,empty,empty,_,C)

#define SALTS_META_HEAP_METHODS(M,C) \
 M(INIT_SIZE_COMPARE,init,raw_init,_,C) M(FROM_ARRAY_SIZE_COMPARE,from,raw_from_array,_,C) \
 M(DESTROY,destroy,raw_destroy_storage,_,C) M(CLEAR,clear,clear,_,C) \
 M(RESERVE,reserve,reserve,_,C) M(PUSH_VALUE,push,push,_,C) \
 M(POP_BOOL,pop,pop,_,C) M(CONST_PTR,peek,peek,_,C) \
 M(SIZE,size,size,_,C) M(SIZE,capacity,capacity,_,C) M(BOOL,empty,empty,_,C)

#define SALTS_META_SET_METHODS(M,C) \
 M(INIT_KEY_COMPARE,init,raw_init,_,C) M(FROM_KEYS_COMPARE,from,raw_from_array,_,C) \
 M(DESTROY,destroy,raw_destroy_storage,_,C) M(CLEAR,clear,clear,_,C) \
 M(KEY_VALUE,add,add,_,C) \
 M(KEY_CONTAINS,contains,contains,_,C) M(KEY_REMOVE_BOOL,remove,remove,_,C) \
 M(SIZE,size,size,_,C) M(BOOL,empty,empty,_,C)
#define SALTS_META_HASH_SET_METHODS(M,C) \
 M(INIT_KEY_HASH,init,raw_init,_,C) M(FROM_KEYS_HASH,from,raw_from_array,_,C) \
 M(DESTROY,destroy,raw_destroy_storage,_,C) M(CLEAR,clear,clear,_,C) \
 M(RESERVE,reserve,reserve,_,C) M(KEY_VALUE,add,add,_,C) \
 M(KEY_CONTAINS,contains,contains,_,C) M(KEY_REMOVE_BOOL,remove,remove,_,C) \
 M(SIZE,size,size,_,C) M(SIZE,capacity,capacity,_,C) M(BOOL,empty,empty,_,C)

#define SALTS_META_C1_GENERATION(name,prefix) \
 CMETA_INLINE uint64_t name##_cmeta_generation(const void *object) { \
   const name *self = (const name *)object; \
   return self == NULL ? UINT64_C(0) : prefix##_generation(&self->raw); \
 }
#define SALTS_META_C2_GENERATION(name,prefix) SALTS_META_C1_GENERATION(name,prefix)

#define SALTS_META_HASH_MAP_METHODS(M,C) \
 M(INIT_KV_HASH,init,raw_init,_,C) M(FROM_ENTRIES,from,raw_from_arrays,STL_INVALID_ARGUMENT,C) \
 M(DESTROY,destroy,raw_destroy_storage,_,C) M(CLEAR,clear,clear,_,C) \
 M(RESERVE,reserve,reserve,_,C) M(PUT,put,put,_,C) M(GET,get,get,_,C) \
 M(GET_CONST,get_const,get_const,_,C) M(CONTAINS,contains,contains,_,C) \
 M(REMOVE_STATUS_BOOL,remove,remove,_,C) M(SIZE,size,size,_,C) \
 M(CAPACITY,capacity,capacity,_,C) M(EMPTY,empty,empty,_,C) \
 M(KEY_AT,key_at,key_at,_,C) M(KEY_AT_CONST,key_at_const,key_at_const,_,C) \
 M(VALUE_AT,value_at,value_at,_,C) M(VALUE_AT_CONST,value_at_const,value_at_const,_,C)

#define SALTS_META_MAP_METHODS(M,C) \
 M(INIT_KV_COMPARE,init,raw_init,_,C) M(FROM_ENTRIES_LINK,from,raw_from_arrays,STL_INVALID_ARGUMENT,C) \
 M(DESTROY,destroy,raw_destroy_storage,_,C) M(CLEAR,clear,clear,_,C) \
 M(PUT,put,put,_,C) M(GET,get,get,_,C) \
 M(GET_CONST,get_const,get_const,_,C) M(CONTAINS,contains,contains,_,C) \
 M(REMOVE_STATUS_BOOL,remove,remove,_,C) M(SIZE,size,size,_,C) \
 M(EMPTY,empty,empty,_,C)

#define SALTS_META_MULTIMAP_METHODS(M,C) \
 M(INIT_KV_COMPARE,init,raw_init,_,C) \
 M(FROM_ENTRIES_LINK,from,raw_from_arrays,STL_INVALID_ARGUMENT,C) \
 M(DESTROY,destroy,raw_destroy_storage,_,C) M(CLEAR,clear,clear,_,C) \
 M(PUT,put,put,_,C) \
 M(CONTAINS,contains,contains,_,C) M(COUNT_KEY,count,count,_,C) \
 M(REMOVE_BOOL,remove,remove,_,C) M(ERASE_KEY,erase,erase,_,C) \
 M(SIZE,size,size,_,C) M(EMPTY,empty,empty,_,C)

#define SALTS_META_TREE_METHODS(M,C) \
 M(INIT_KV_COMPARE,init,raw_init,_,C) M(FROM_ENTRIES,from,raw_from_arrays,STL_INVALID_ARGUMENT,C) \
 M(INIT_WITH_ORDER_COMPARE,init_with_order,raw_init_with_order,_,C) \
 M(DESTROY,destroy,raw_destroy_storage,_,C) M(CLEAR,clear,clear,_,C) \
 M(RESERVE,reserve,reserve,_,C) M(PUT,put,put,_,C) M(GET,get,get,_,C) \
 M(GET_CONST,get_const,get_const,_,C) M(CONTAINS,contains,contains,_,C) \
 M(REMOVE_STATUS_BOOL,remove,remove,_,C) M(SIZE,size,size,_,C) \
 M(CAPACITY,capacity,capacity,_,C) M(EMPTY,empty,empty,_,C) \
 M(KEY_AT,key_at,key_at,_,C) M(KEY_AT_CONST,key_at_const,key_at_const,_,C) \
 M(VALUE_AT,value_at,value_at,_,C) M(VALUE_AT_CONST,value_at_const,value_at_const,_,C)
#define SALTS_META_BTREE_METHODS(M,C) SALTS_META_TREE_METHODS(M,C)
#define SALTS_META_BPLUS_TREE_METHODS(M,C) SALTS_META_TREE_METHODS(M,C)


/*
 * Receiver-method reflection replays the same typed method schema that emits
 * the inline API. The first slice reflects mutation signatures only.
 * Element/key/value ABI carriers stay UNSPECIFIED: a semantic descriptor does
 * not by itself define a C ABI carrier. Receiver and status return are exact.
 */
#define SALTS_META_METHOD_REFLECT_SKIP(...)

#define SALTS_META_RECEIVER_TYPE(name) \
 CMETA_LOCAL const cmeta_type_desc name##_cmeta_receiver_type = { \
   CMETA_CONTAINER_STR(name) " *", sizeof(name *), _Alignof(name *), \
   CMETA_T_POINTER, &name##_cmeta_type, NULL, NULL \
 };

#define SALTS_META_C1_VALUE_METHOD(pub,name,type,type_desc) \
 CMETA_LOCAL const cmeta_param_desc name##_##pub##__function_params[] = { \
   { sizeof(cmeta_param_desc), "self", &name##_cmeta_receiver_type, \
     CMETA_PARAM_INOUT | CMETA_PARAM_BORROWED | CMETA_PARAM_RECEIVER }, \
   { sizeof(cmeta_param_desc), "value", (type_desc), CMETA_PARAM_IN } \
 }; \
 CMETA_LOCAL const cmeta_function_desc name##_##pub##__function_meta = { \
   sizeof(cmeta_function_desc), \
   CMETA_CONTAINER_STR(name) "_" CMETA_CONTAINER_STR(pub), &cmeta_type_int, \
   name##_##pub##__function_params, 2u, \
   CMETA_EFFECT_STATEFUL | CMETA_EFFECT_MAY_FAIL, CMETA_PROP_NONE, \
   CMETA_RESULT_UNKNOWN \
 }; \
 CMETA_LOCAL const cmeta_abi_carrier name##_##pub##__function_param_abi[] = { \
   CMETA_ABI_OBJECT_POINTER, CMETA_ABI_UNSPECIFIED \
 }; \
 CMETA_LOCAL const cmeta_function_abi_desc name##_##pub##__function_abi_meta = { \
   sizeof(cmeta_function_abi_desc), &name##_##pub##__function_meta, \
   CMETA_ABI_SCALAR, name##_##pub##__function_param_abi, 2u \
 }; \
 CMETA_INLINE const cmeta_function_desc *name##_##pub##_function(void) { \
   return &name##_##pub##__function_meta; \
 } \
 CMETA_INLINE const cmeta_function_abi_desc *name##_##pub##_function_abi(void) { \
   return &name##_##pub##__function_abi_meta; \
 }

#define SALTS_META_C2_PUT_METHOD(pub,name,kt,vt,key_desc,value_desc) \
 CMETA_LOCAL const cmeta_param_desc name##_##pub##__function_params[] = { \
   { sizeof(cmeta_param_desc), "self", &name##_cmeta_receiver_type, \
     CMETA_PARAM_INOUT | CMETA_PARAM_BORROWED | CMETA_PARAM_RECEIVER }, \
   { sizeof(cmeta_param_desc), "key", (key_desc), CMETA_PARAM_IN }, \
   { sizeof(cmeta_param_desc), "value", (value_desc), CMETA_PARAM_IN } \
 }; \
 CMETA_LOCAL const cmeta_function_desc name##_##pub##__function_meta = { \
   sizeof(cmeta_function_desc), \
   CMETA_CONTAINER_STR(name) "_" CMETA_CONTAINER_STR(pub), &cmeta_type_int, \
   name##_##pub##__function_params, 3u, \
   CMETA_EFFECT_STATEFUL | CMETA_EFFECT_MAY_FAIL, CMETA_PROP_NONE, \
   CMETA_RESULT_UNKNOWN \
 }; \
 CMETA_LOCAL const cmeta_abi_carrier name##_##pub##__function_param_abi[] = { \
   CMETA_ABI_OBJECT_POINTER, CMETA_ABI_UNSPECIFIED, CMETA_ABI_UNSPECIFIED \
 }; \
 CMETA_LOCAL const cmeta_function_abi_desc name##_##pub##__function_abi_meta = { \
   sizeof(cmeta_function_abi_desc), &name##_##pub##__function_meta, \
   CMETA_ABI_SCALAR, name##_##pub##__function_param_abi, 3u \
 }; \
 CMETA_INLINE const cmeta_function_desc *name##_##pub##_function(void) { \
   return &name##_##pub##__function_meta; \
 } \
 CMETA_INLINE const cmeta_function_abi_desc *name##_##pub##_function_abi(void) { \
   return &name##_##pub##__function_abi_meta; \
 }

#define SALTS_META_C1_METHOD_REFLECT_DISPATCH(kind,pub,op,extra,ctx) \
 SALTS_META_C1_METHOD_REFLECT_DISPATCH_I( \
   kind,pub,op,extra,CMETA_PP_UNPAREN ctx)
#define SALTS_META_C1_METHOD_REFLECT_DISPATCH_I(kind,pub,op,extra,...) \
 CMETA_PP_CAT(SALTS_META_C1_METHOD_REFLECT_,kind)(pub,op,extra,__VA_ARGS__)

#define SALTS_META_C1_METHOD_REFLECT_PUSH_VALUE(pub,op,extra,name,type,type_desc,emit) \
 emit(pub,name,type,type_desc)
#define SALTS_META_C1_METHOD_REFLECT_PUSH_VALUE_ITER(pub,op,extra,name,type,type_desc,emit) \
 emit(pub,name,type,type_desc)
#define SALTS_META_C1_METHOD_REFLECT_KEY_VALUE(pub,op,extra,name,type,type_desc,emit) \
 emit(pub,name,type,type_desc)

#define SALTS_META_C1_METHOD_REFLECT_INIT_SIZE SALTS_META_METHOD_REFLECT_SKIP
#define SALTS_META_C1_METHOD_REFLECT_FROM_ARRAY_SIZE SALTS_META_METHOD_REFLECT_SKIP
#define SALTS_META_C1_METHOD_REFLECT_DESTROY SALTS_META_METHOD_REFLECT_SKIP
#define SALTS_META_C1_METHOD_REFLECT_CLEAR SALTS_META_METHOD_REFLECT_SKIP
#define SALTS_META_C1_METHOD_REFLECT_RESERVE SALTS_META_METHOD_REFLECT_SKIP
#define SALTS_META_C1_METHOD_REFLECT_POP_BOOL SALTS_META_METHOD_REFLECT_SKIP
#define SALTS_META_C1_METHOD_REFLECT_PTR_INDEX SALTS_META_METHOD_REFLECT_SKIP
#define SALTS_META_C1_METHOD_REFLECT_CONST_PTR_INDEX SALTS_META_METHOD_REFLECT_SKIP
#define SALTS_META_C1_METHOD_REFLECT_PTR SALTS_META_METHOD_REFLECT_SKIP
#define SALTS_META_C1_METHOD_REFLECT_CONST_PTR SALTS_META_METHOD_REFLECT_SKIP
#define SALTS_META_C1_METHOD_REFLECT_SIZE SALTS_META_METHOD_REFLECT_SKIP
#define SALTS_META_C1_METHOD_REFLECT_BOOL SALTS_META_METHOD_REFLECT_SKIP
#define SALTS_META_C1_METHOD_REFLECT_INIT_SIZE_COMPARE SALTS_META_METHOD_REFLECT_SKIP
#define SALTS_META_C1_METHOD_REFLECT_FROM_ARRAY_SIZE_COMPARE SALTS_META_METHOD_REFLECT_SKIP
#define SALTS_META_C1_METHOD_REFLECT_INIT_KEY_HASH SALTS_META_METHOD_REFLECT_SKIP
#define SALTS_META_C1_METHOD_REFLECT_FROM_KEYS_HASH SALTS_META_METHOD_REFLECT_SKIP
#define SALTS_META_C1_METHOD_REFLECT_INIT_KEY_COMPARE SALTS_META_METHOD_REFLECT_SKIP
#define SALTS_META_C1_METHOD_REFLECT_FROM_KEYS_COMPARE SALTS_META_METHOD_REFLECT_SKIP
#define SALTS_META_C1_METHOD_REFLECT_KEY_CONTAINS SALTS_META_METHOD_REFLECT_SKIP
#define SALTS_META_C1_METHOD_REFLECT_KEY_REMOVE_BOOL SALTS_META_METHOD_REFLECT_SKIP

#define SALTS_META_C1_METHOD_ENTRY_DISPATCH(kind,pub,op,extra,ctx) \
 SALTS_META_C1_METHOD_ENTRY_DISPATCH_I( \
   kind,pub,op,extra,CMETA_PP_UNPAREN ctx)
#define SALTS_META_C1_METHOD_ENTRY_DISPATCH_I(kind,pub,op,extra,...) \
 CMETA_PP_CAT(SALTS_META_C1_METHOD_ENTRY_,kind)(pub,op,extra,__VA_ARGS__)

#define SALTS_META_C1_METHOD_ENTRY_VALUE(pub,op,extra,name,type,type_desc) \
 { CMETA_CONTAINER_STR(pub), \
   &name##_##pub##__function_abi_meta },

#define SALTS_META_C1_METHOD_ENTRY_PUSH_VALUE SALTS_META_C1_METHOD_ENTRY_VALUE
#define SALTS_META_C1_METHOD_ENTRY_PUSH_VALUE_ITER SALTS_META_C1_METHOD_ENTRY_VALUE
#define SALTS_META_C1_METHOD_ENTRY_KEY_VALUE SALTS_META_C1_METHOD_ENTRY_VALUE
#define SALTS_META_C1_METHOD_ENTRY_INIT_SIZE SALTS_META_METHOD_REFLECT_SKIP
#define SALTS_META_C1_METHOD_ENTRY_FROM_ARRAY_SIZE SALTS_META_METHOD_REFLECT_SKIP
#define SALTS_META_C1_METHOD_ENTRY_DESTROY SALTS_META_METHOD_REFLECT_SKIP
#define SALTS_META_C1_METHOD_ENTRY_CLEAR SALTS_META_METHOD_REFLECT_SKIP
#define SALTS_META_C1_METHOD_ENTRY_RESERVE SALTS_META_METHOD_REFLECT_SKIP
#define SALTS_META_C1_METHOD_ENTRY_POP_BOOL SALTS_META_METHOD_REFLECT_SKIP
#define SALTS_META_C1_METHOD_ENTRY_PTR_INDEX SALTS_META_METHOD_REFLECT_SKIP
#define SALTS_META_C1_METHOD_ENTRY_CONST_PTR_INDEX SALTS_META_METHOD_REFLECT_SKIP
#define SALTS_META_C1_METHOD_ENTRY_PTR SALTS_META_METHOD_REFLECT_SKIP
#define SALTS_META_C1_METHOD_ENTRY_CONST_PTR SALTS_META_METHOD_REFLECT_SKIP
#define SALTS_META_C1_METHOD_ENTRY_SIZE SALTS_META_METHOD_REFLECT_SKIP
#define SALTS_META_C1_METHOD_ENTRY_BOOL SALTS_META_METHOD_REFLECT_SKIP
#define SALTS_META_C1_METHOD_ENTRY_INIT_SIZE_COMPARE SALTS_META_METHOD_REFLECT_SKIP
#define SALTS_META_C1_METHOD_ENTRY_FROM_ARRAY_SIZE_COMPARE SALTS_META_METHOD_REFLECT_SKIP
#define SALTS_META_C1_METHOD_ENTRY_INIT_KEY_HASH SALTS_META_METHOD_REFLECT_SKIP
#define SALTS_META_C1_METHOD_ENTRY_FROM_KEYS_HASH SALTS_META_METHOD_REFLECT_SKIP
#define SALTS_META_C1_METHOD_ENTRY_INIT_KEY_COMPARE SALTS_META_METHOD_REFLECT_SKIP
#define SALTS_META_C1_METHOD_ENTRY_FROM_KEYS_COMPARE SALTS_META_METHOD_REFLECT_SKIP
#define SALTS_META_C1_METHOD_ENTRY_KEY_CONTAINS SALTS_META_METHOD_REFLECT_SKIP
#define SALTS_META_C1_METHOD_ENTRY_KEY_REMOVE_BOOL SALTS_META_METHOD_REFLECT_SKIP

#define SALTS_META_C1_METHOD_METADATA(kind,name,type,type_desc,methods) \
 SALTS_META_RECEIVER_TYPE(name) \
 methods(SALTS_META_C1_METHOD_REFLECT_DISPATCH,(name,type,type_desc,SALTS_META_C1_VALUE_METHOD)) \
 CMETA_LOCAL const cmeta_receiver_operation name##_receiver_operations[] = { \
   methods(SALTS_META_C1_METHOD_ENTRY_DISPATCH,(name,type,type_desc)) \
 }; \
 CMETA_LOCAL const cmeta_receiver_operation_set name##_receiver_operation_set_value = { \
   sizeof(cmeta_receiver_operation_set), &name##_cmeta_type, \
   name##_receiver_operations, \
   sizeof(name##_receiver_operations) / sizeof(name##_receiver_operations[0]), \
   SALTS_META_GENERIC_DESC(kind) \
 }; \
 CMETA_INLINE const cmeta_receiver_operation_set *name##_receiver_operation_set(void) { \
   return &name##_receiver_operation_set_value; \
 }

#define SALTS_META_C2_METHOD_REFLECT_DISPATCH(kind,pub,op,extra,ctx) \
 SALTS_META_C2_METHOD_REFLECT_DISPATCH_I( \
   kind,pub,op,extra,CMETA_PP_UNPAREN ctx)
#define SALTS_META_C2_METHOD_REFLECT_DISPATCH_I(kind,pub,op,extra,...) \
 CMETA_PP_CAT(SALTS_META_C2_METHOD_REFLECT_,kind)(pub,op,extra,__VA_ARGS__)

#define SALTS_META_C2_METHOD_REFLECT_PUT( \
    pub,op,extra,name,kt,vt,key_desc,value_desc,emit) \
 emit(pub,name,kt,vt,key_desc,value_desc)

#define SALTS_META_C2_METHOD_REFLECT_INIT_KV_HASH SALTS_META_METHOD_REFLECT_SKIP
#define SALTS_META_C2_METHOD_REFLECT_FROM_ENTRIES SALTS_META_METHOD_REFLECT_SKIP
#define SALTS_META_C2_METHOD_REFLECT_DESTROY SALTS_META_METHOD_REFLECT_SKIP
#define SALTS_META_C2_METHOD_REFLECT_CLEAR SALTS_META_METHOD_REFLECT_SKIP
#define SALTS_META_C2_METHOD_REFLECT_RESERVE SALTS_META_METHOD_REFLECT_SKIP
#define SALTS_META_C2_METHOD_REFLECT_GET SALTS_META_METHOD_REFLECT_SKIP
#define SALTS_META_C2_METHOD_REFLECT_GET_CONST SALTS_META_METHOD_REFLECT_SKIP
#define SALTS_META_C2_METHOD_REFLECT_CONTAINS SALTS_META_METHOD_REFLECT_SKIP
#define SALTS_META_C2_METHOD_REFLECT_REMOVE_STATUS_BOOL SALTS_META_METHOD_REFLECT_SKIP
#define SALTS_META_C2_METHOD_REFLECT_SIZE SALTS_META_METHOD_REFLECT_SKIP
#define SALTS_META_C2_METHOD_REFLECT_CAPACITY SALTS_META_METHOD_REFLECT_SKIP
#define SALTS_META_C2_METHOD_REFLECT_EMPTY SALTS_META_METHOD_REFLECT_SKIP
#define SALTS_META_C2_METHOD_REFLECT_KEY_AT SALTS_META_METHOD_REFLECT_SKIP
#define SALTS_META_C2_METHOD_REFLECT_KEY_AT_CONST SALTS_META_METHOD_REFLECT_SKIP
#define SALTS_META_C2_METHOD_REFLECT_VALUE_AT SALTS_META_METHOD_REFLECT_SKIP
#define SALTS_META_C2_METHOD_REFLECT_VALUE_AT_CONST SALTS_META_METHOD_REFLECT_SKIP
#define SALTS_META_C2_METHOD_REFLECT_INIT_KV_COMPARE SALTS_META_METHOD_REFLECT_SKIP
#define SALTS_META_C2_METHOD_REFLECT_FROM_ENTRIES_LINK SALTS_META_METHOD_REFLECT_SKIP
#define SALTS_META_C2_METHOD_REFLECT_INIT_WITH_ORDER_COMPARE SALTS_META_METHOD_REFLECT_SKIP
#define SALTS_META_C2_METHOD_REFLECT_INIT_KV_MULTIMAP SALTS_META_METHOD_REFLECT_SKIP
#define SALTS_META_C2_METHOD_REFLECT_FROM_ENTRIES_MULTIMAP SALTS_META_METHOD_REFLECT_SKIP
#define SALTS_META_C2_METHOD_REFLECT_REMOVE_BOOL SALTS_META_METHOD_REFLECT_SKIP
#define SALTS_META_C2_METHOD_REFLECT_COUNT_KEY SALTS_META_METHOD_REFLECT_SKIP
#define SALTS_META_C2_METHOD_REFLECT_ERASE_KEY SALTS_META_METHOD_REFLECT_SKIP

#define SALTS_META_C2_METHOD_ENTRY_DISPATCH(kind,pub,op,extra,ctx) \
 SALTS_META_C2_METHOD_ENTRY_DISPATCH_I( \
   kind,pub,op,extra,CMETA_PP_UNPAREN ctx)
#define SALTS_META_C2_METHOD_ENTRY_DISPATCH_I(kind,pub,op,extra,...) \
 CMETA_PP_CAT(SALTS_META_C2_METHOD_ENTRY_,kind)(pub,op,extra,__VA_ARGS__)

#define SALTS_META_C2_METHOD_ENTRY_PUT( \
    pub,op,extra,name,kt,vt,key_desc,value_desc) \
 { CMETA_CONTAINER_STR(pub), \
   &name##_##pub##__function_abi_meta },

#define SALTS_META_C2_METHOD_ENTRY_INIT_KV_HASH SALTS_META_METHOD_REFLECT_SKIP
#define SALTS_META_C2_METHOD_ENTRY_FROM_ENTRIES SALTS_META_METHOD_REFLECT_SKIP
#define SALTS_META_C2_METHOD_ENTRY_DESTROY SALTS_META_METHOD_REFLECT_SKIP
#define SALTS_META_C2_METHOD_ENTRY_CLEAR SALTS_META_METHOD_REFLECT_SKIP
#define SALTS_META_C2_METHOD_ENTRY_RESERVE SALTS_META_METHOD_REFLECT_SKIP
#define SALTS_META_C2_METHOD_ENTRY_GET SALTS_META_METHOD_REFLECT_SKIP
#define SALTS_META_C2_METHOD_ENTRY_GET_CONST SALTS_META_METHOD_REFLECT_SKIP
#define SALTS_META_C2_METHOD_ENTRY_CONTAINS SALTS_META_METHOD_REFLECT_SKIP
#define SALTS_META_C2_METHOD_ENTRY_REMOVE_STATUS_BOOL SALTS_META_METHOD_REFLECT_SKIP
#define SALTS_META_C2_METHOD_ENTRY_SIZE SALTS_META_METHOD_REFLECT_SKIP
#define SALTS_META_C2_METHOD_ENTRY_CAPACITY SALTS_META_METHOD_REFLECT_SKIP
#define SALTS_META_C2_METHOD_ENTRY_EMPTY SALTS_META_METHOD_REFLECT_SKIP
#define SALTS_META_C2_METHOD_ENTRY_KEY_AT SALTS_META_METHOD_REFLECT_SKIP
#define SALTS_META_C2_METHOD_ENTRY_KEY_AT_CONST SALTS_META_METHOD_REFLECT_SKIP
#define SALTS_META_C2_METHOD_ENTRY_VALUE_AT SALTS_META_METHOD_REFLECT_SKIP
#define SALTS_META_C2_METHOD_ENTRY_VALUE_AT_CONST SALTS_META_METHOD_REFLECT_SKIP
#define SALTS_META_C2_METHOD_ENTRY_INIT_KV_COMPARE SALTS_META_METHOD_REFLECT_SKIP
#define SALTS_META_C2_METHOD_ENTRY_FROM_ENTRIES_LINK SALTS_META_METHOD_REFLECT_SKIP
#define SALTS_META_C2_METHOD_ENTRY_INIT_WITH_ORDER_COMPARE SALTS_META_METHOD_REFLECT_SKIP
#define SALTS_META_C2_METHOD_ENTRY_INIT_KV_MULTIMAP SALTS_META_METHOD_REFLECT_SKIP
#define SALTS_META_C2_METHOD_ENTRY_FROM_ENTRIES_MULTIMAP SALTS_META_METHOD_REFLECT_SKIP
#define SALTS_META_C2_METHOD_ENTRY_REMOVE_BOOL SALTS_META_METHOD_REFLECT_SKIP
#define SALTS_META_C2_METHOD_ENTRY_COUNT_KEY SALTS_META_METHOD_REFLECT_SKIP
#define SALTS_META_C2_METHOD_ENTRY_ERASE_KEY SALTS_META_METHOD_REFLECT_SKIP

#define SALTS_META_C2_METHOD_METADATA( \
    kind,name,kt,vt,key_desc,value_desc,methods) \
 SALTS_META_RECEIVER_TYPE(name) \
 methods(SALTS_META_C2_METHOD_REFLECT_DISPATCH, \
         (name,kt,vt,key_desc,value_desc,SALTS_META_C2_PUT_METHOD)) \
 CMETA_LOCAL const cmeta_receiver_operation name##_receiver_operations[] = { \
   methods(SALTS_META_C2_METHOD_ENTRY_DISPATCH, \
           (name,kt,vt,key_desc,value_desc)) \
 }; \
 CMETA_LOCAL const cmeta_receiver_operation_set name##_receiver_operation_set_value = { \
   sizeof(cmeta_receiver_operation_set), &name##_cmeta_type, \
   name##_receiver_operations, \
   sizeof(name##_receiver_operations) / sizeof(name##_receiver_operations[0]), \
   SALTS_META_GENERIC_DESC(kind) \
 }; \
 CMETA_INLINE const cmeta_receiver_operation_set *name##_receiver_operation_set(void) { \
   return &name##_receiver_operation_set_value; \
 }

/* List and Map use small storage bridges where their raw initialization or
 * destruction entry point differs from the generated method contract. */
CMETA_INLINE stl_status cmeta_stl_typed_list_raw_init(
    list_t *self, const cmeta_type_desc *type, size_t limit) {
  return list_raw_init(self, type, limit);
}
CMETA_INLINE stl_status cmeta_stl_typed_list_raw_from_array(
    list_t *self, const void *values, size_t count,
    const cmeta_type_desc *type, size_t limit) {
  return list_raw_from_array(self, values, count, type, limit);
}
CMETA_INLINE void cmeta_stl_typed_list_raw_destroy_storage(list_t *self) {
  list_raw_destroy_storage(self);
}
CMETA_INLINE void cmeta_stl_typed_list_clear(list_t *self) {
  list_clear(self);
}
CMETA_INLINE stl_status cmeta_stl_typed_list_push_back(
    list_t *self, const void *value, list_iter_t *iterator) {
  return list_push_back(self, value, iterator);
}
CMETA_INLINE stl_status cmeta_stl_typed_list_push_front(
    list_t *self, const void *value, list_iter_t *iterator) {
  return list_push_front(self, value, iterator);
}
CMETA_INLINE stl_status cmeta_stl_typed_list_pop_back(
    list_t *self, void *value) {
  return list_pop_back(self, value);
}
CMETA_INLINE stl_status cmeta_stl_typed_list_pop_front(
    list_t *self, void *value) {
  return list_pop_front(self, value);
}
CMETA_INLINE void *cmeta_stl_typed_list_front(list_t *self) {
  return list_front(self);
}
CMETA_INLINE const void *cmeta_stl_typed_list_front_const(
    const list_t *self) {
  return list_front_const(self);
}
CMETA_INLINE void *cmeta_stl_typed_list_back(list_t *self) {
  return list_back(self);
}
CMETA_INLINE const void *cmeta_stl_typed_list_back_const(
    const list_t *self) {
  return list_back_const(self);
}
CMETA_INLINE size_t cmeta_stl_typed_list_size(const list_t *self) {
  return list_size(self);
}
CMETA_INLINE uint64_t cmeta_stl_typed_list_generation(const list_t *self) {
  return list_generation(self);
}
CMETA_INLINE bool cmeta_stl_typed_list_empty(const list_t *self) {
  return list_empty(self);
}
CMETA_INLINE bool cmeta_stl_typed_list_range_next(
    const list_t *self, cmeta_range_cursor *cursor, const void **value) {
  return list_range_next(self, cursor, value);
}

CMETA_INLINE stl_status cmeta_stl_typed_map_raw_init(
    map_t *self, const cmeta_type_desc *key_type,
    const cmeta_type_desc *value_type, size_t limit) {
  return map_raw_init(self, key_type, value_type, limit);
}
CMETA_INLINE stl_status cmeta_stl_typed_map_raw_from_arrays(
    map_t *self, const void *keys, const void *values, size_t count,
    const cmeta_type_desc *key_type, const cmeta_type_desc *value_type,
    size_t limit) {
  return map_raw_from_arrays(self, keys, values, count, key_type, value_type,
                             limit);
}
CMETA_INLINE void cmeta_stl_typed_map_raw_destroy_storage(map_t *self) {
  map_raw_destroy_storage(self);
}
CMETA_INLINE void cmeta_stl_typed_map_destroy(map_t *self) {
  map_raw_destroy_storage(self);
}
CMETA_INLINE void cmeta_stl_typed_map_clear(map_t *self) {
  map_clear(self);
}
CMETA_INLINE stl_status cmeta_stl_typed_map_put(
    map_t *self, const void *key, const void *value) {
  return map_put(self, key, value);
}
CMETA_INLINE void *cmeta_stl_typed_map_get(map_t *self, const void *key) {
  return map_get(self, key);
}
CMETA_INLINE const void *cmeta_stl_typed_map_get_const(
    const map_t *self, const void *key) {
  return map_get_const(self, key);
}
CMETA_INLINE bool cmeta_stl_typed_map_contains(
    const map_t *self, const void *key) {
  return map_contains(self, key);
}
CMETA_INLINE stl_status cmeta_stl_typed_map_remove(
    map_t *self, const void *key, void *value) {
  return map_remove(self, key, value);
}
CMETA_INLINE size_t cmeta_stl_typed_map_size(const map_t *self) {
  return map_size(self);
}
CMETA_INLINE uint64_t cmeta_stl_typed_map_generation(const map_t *self) {
  return map_generation(self);
}
CMETA_INLINE bool cmeta_stl_typed_map_empty(const map_t *self) {
  return map_empty(self);
}
CMETA_INLINE bool cmeta_stl_typed_map_range_next(
    const map_t *self, cmeta_range_cursor *cursor, const void **key,
    const void **value) {
  return map_range_next(self, cursor, key, value);
}

/* Named typed facades retain their wrapper storage while exposing the
 * canonical CSTL generic constructor and exact compile-time type arguments. */
#define SALTS_META_GENERIC_DESC_Vec (&stl_vec_generic_desc)
#define SALTS_META_GENERIC_DESC_Deque (&stl_deque_generic_desc)
#define SALTS_META_GENERIC_DESC_List (&stl_list_generic_desc)
#define SALTS_META_GENERIC_DESC_Stack (&stl_stack_generic_desc)
#define SALTS_META_GENERIC_DESC_Queue (&stl_queue_generic_desc)
#define SALTS_META_GENERIC_DESC_Heap (&stl_heap_generic_desc)
#define SALTS_META_GENERIC_DESC_Set (&stl_set_generic_desc)
#define SALTS_META_GENERIC_DESC_HashSet (&stl_hash_set_generic_desc)
#define SALTS_META_GENERIC_DESC_HashMap (&stl_hash_map_generic_desc)
#define SALTS_META_GENERIC_DESC_Map (&stl_map_generic_desc)
#define SALTS_META_GENERIC_DESC_MultiMap (&stl_multimap_generic_desc)
#define SALTS_META_GENERIC_DESC_BTree (&stl_btree_generic_desc)
#define SALTS_META_GENERIC_DESC_BPlusTree (&stl_bplus_tree_generic_desc)
#define SALTS_META_GENERIC_DESC(kind) CMETA_PP_CAT(SALTS_META_GENERIC_DESC_,kind)

#define SALTS_META_C1_GENERIC_EXTENSION(kind,name,type_desc) \
 CMETA_INLINE const cmeta_type_desc *name##_cmeta_type_argument(const void *object,size_t index){(void)object;return index==0u?(type_desc):NULL;} \
 CMETA_LOCAL const cmeta_container_type_ops name##_cmeta_type_ops={sizeof(cmeta_container_type_ops),CMETA_CONTAINER_TYPE_OPS_ABI_VERSION,SALTS_META_GENERIC_DESC(kind),1u,name##_cmeta_type_argument}; \
 CMETA_LOCAL const cmeta_container_ext name##_cmeta_ext={sizeof(cmeta_container_ext),CMETA_CONTAINER_EXT_ABI_VERSION,&name##_cmeta_type_ops,NULL,NULL};

#define SALTS_META_C2_GENERIC_EXTENSION(kind,name,key_desc,value_desc) \
 CMETA_INLINE const cmeta_type_desc *name##_cmeta_type_argument(const void *object,size_t index){(void)object;return index==0u?(key_desc):(index==1u?(value_desc):NULL);} \
 CMETA_LOCAL const cmeta_container_type_ops name##_cmeta_type_ops={sizeof(cmeta_container_type_ops),CMETA_CONTAINER_TYPE_OPS_ABI_VERSION,SALTS_META_GENERIC_DESC(kind),2u,name##_cmeta_type_argument}; \
 CMETA_LOCAL const cmeta_container_ext name##_cmeta_ext={sizeof(cmeta_container_ext),CMETA_CONTAINER_EXT_ABI_VERSION,&name##_cmeta_type_ops,NULL,NULL};

/* The row macros are the sole semantic mapping for every standard kind.
 * C cannot emit #define directives from Replay, so the public kind markers and
 * adapters remain thin language-policy shims that reference these rows. */
#define SALTS_STL_KIND_ROW_Vec (Vec,1,C1_INDEX,vec_t,vec,SALTS_META_VEC_METHODS,push,_,_,CMETA_RANGE_SIZED|CMETA_RANGE_ORDERED|CMETA_RANGE_CONTIGUOUS|CMETA_RANGE_RANDOM_ACCESS|CMETA_RANGE_REUSABLE,0,0,0)
#define SALTS_STL_KIND_ROW_Deque (Deque,1,C1_INDEX,deque_t,deque,SALTS_META_DEQUE_METHODS,push_back,_,_,CMETA_RANGE_SIZED|CMETA_RANGE_ORDERED|CMETA_RANGE_RANDOM_ACCESS|CMETA_RANGE_REUSABLE,0,0,0)
#define SALTS_STL_KIND_ROW_List (List,1,C1_LINK,list_t,cmeta_stl_typed_list,SALTS_META_LIST_METHODS,push_back,_,_,CMETA_RANGE_SIZED|CMETA_RANGE_ORDERED|CMETA_RANGE_REUSABLE,0,0,0)
#define SALTS_STL_KIND_ROW_Stack (Stack,1,C1_INDEX,cstl_stack_t,stack,SALTS_META_STACK_METHODS,push,_,_,CMETA_RANGE_SIZED|CMETA_RANGE_ORDERED|CMETA_RANGE_RANDOM_ACCESS|CMETA_RANGE_REUSABLE,0,0,0)
#define SALTS_STL_KIND_ROW_Queue (Queue,1,C1_INDEX,queue_t,queue,SALTS_META_QUEUE_METHODS,push,_,_,CMETA_RANGE_SIZED|CMETA_RANGE_ORDERED|CMETA_RANGE_RANDOM_ACCESS|CMETA_RANGE_REUSABLE,0,0,0)
#define SALTS_STL_KIND_ROW_Heap (Heap,1,C1_INDEX,heap_t,heap,SALTS_META_HEAP_METHODS,push,_,_,CMETA_RANGE_SIZED|CMETA_RANGE_RANDOM_ACCESS|CMETA_RANGE_REUSABLE,0,0,0)
#define SALTS_STL_KIND_ROW_Set (Set,1,C1_LINK,set_t,set,SALTS_META_SET_METHODS,add,_,_,CMETA_RANGE_SIZED|CMETA_RANGE_ORDERED|CMETA_RANGE_SORTED|CMETA_RANGE_UNIQUE|CMETA_RANGE_REUSABLE,0,0,0)
#define SALTS_STL_KIND_ROW_HashSet (HashSet,1,C1_SLOT,hash_set_t,hash_set,SALTS_META_HASH_SET_METHODS,add,_,_,CMETA_RANGE_SIZED|CMETA_RANGE_UNIQUE|CMETA_RANGE_REUSABLE,0,0,0)
#define SALTS_STL_KIND_ROW_HashMap (HashMap,2,C2_HASH,hash_map_t,hash_map,SALTS_META_HASH_MAP_METHODS,_,key_at,value_at_const,0,CMETA_RANGE_SIZED|CMETA_RANGE_UNIQUE|CMETA_RANGE_REUSABLE,CMETA_RANGE_SIZED|CMETA_RANGE_REUSABLE,CMETA_RANGE_SIZED|CMETA_RANGE_UNIQUE|CMETA_RANGE_REUSABLE)
#define SALTS_STL_KIND_ROW_Map (Map,2,C2_LINK,map_t,cmeta_stl_typed_map,SALTS_META_MAP_METHODS,_,_,_,0,CMETA_RANGE_SIZED|CMETA_RANGE_ORDERED|CMETA_RANGE_SORTED|CMETA_RANGE_UNIQUE|CMETA_RANGE_REUSABLE,CMETA_RANGE_SIZED|CMETA_RANGE_ORDERED|CMETA_RANGE_REUSABLE,CMETA_RANGE_SIZED|CMETA_RANGE_ORDERED|CMETA_RANGE_SORTED|CMETA_RANGE_UNIQUE|CMETA_RANGE_REUSABLE)
#define SALTS_STL_KIND_ROW_MultiMap (MultiMap,2,C2_LINK,multimap_t,multimap,SALTS_META_MULTIMAP_METHODS,_,_,_,0,CMETA_RANGE_SIZED|CMETA_RANGE_ORDERED|CMETA_RANGE_SORTED|CMETA_RANGE_REUSABLE,CMETA_RANGE_SIZED|CMETA_RANGE_ORDERED|CMETA_RANGE_REUSABLE,CMETA_RANGE_SIZED|CMETA_RANGE_ORDERED|CMETA_RANGE_SORTED|CMETA_RANGE_REUSABLE)
#define SALTS_STL_KIND_ROW_BTree (BTree,2,C2_LINK,btree_t,btree,SALTS_META_BTREE_METHODS,_,_,_,0,CMETA_RANGE_SIZED|CMETA_RANGE_ORDERED|CMETA_RANGE_SORTED|CMETA_RANGE_UNIQUE|CMETA_RANGE_REUSABLE,CMETA_RANGE_SIZED|CMETA_RANGE_ORDERED|CMETA_RANGE_REUSABLE,CMETA_RANGE_SIZED|CMETA_RANGE_ORDERED|CMETA_RANGE_SORTED|CMETA_RANGE_UNIQUE|CMETA_RANGE_REUSABLE)
#define SALTS_STL_KIND_ROW_BPlusTree (BPlusTree,2,C2_LINK,bplus_tree_t,bplus_tree,SALTS_META_BPLUS_TREE_METHODS,_,_,_,0,CMETA_RANGE_SIZED|CMETA_RANGE_ORDERED|CMETA_RANGE_SORTED|CMETA_RANGE_UNIQUE|CMETA_RANGE_REUSABLE,CMETA_RANGE_SIZED|CMETA_RANGE_ORDERED|CMETA_RANGE_REUSABLE,CMETA_RANGE_SIZED|CMETA_RANGE_ORDERED|CMETA_RANGE_SORTED|CMETA_RANGE_UNIQUE|CMETA_RANGE_REUSABLE)

#define SALTS_STL_KIND_SCHEMA(M) Schema(M,SALTS_STL_KIND_ROW_Vec,SALTS_STL_KIND_ROW_Deque,SALTS_STL_KIND_ROW_List,SALTS_STL_KIND_ROW_Stack,SALTS_STL_KIND_ROW_Queue,SALTS_STL_KIND_ROW_Heap,SALTS_STL_KIND_ROW_Set,SALTS_STL_KIND_ROW_HashSet,SALTS_STL_KIND_ROW_HashMap,SALTS_STL_KIND_ROW_Map,SALTS_STL_KIND_ROW_MultiMap,SALTS_STL_KIND_ROW_BTree,SALTS_STL_KIND_ROW_BPlusTree)

#define SALTS_STL_KIND_APPLY(row,name,...) SALTS_STL_KIND_APPLY_E(row,name,__VA_ARGS__)
#define SALTS_STL_KIND_APPLY_E(row,name,...) SALTS_STL_KIND_APPLY_EXPAND(CMETA_PP_UNPAREN row,name,__VA_ARGS__)
#define SALTS_STL_KIND_APPLY_EXPAND(...) SALTS_STL_KIND_APPLY_I(__VA_ARGS__)
#define SALTS_STL_KIND_APPLY_I(kind,arity,family,raw,prefix,methods,accept,key_at_op,value_at_op,range_flags,key_flags,value_flags,entry_flags,name,...) CMETA_PP_CAT(SALTS_STL_KIND_DEFINE_,family)(kind,name,__VA_ARGS__,raw,prefix,methods,accept,key_at_op,value_at_op,range_flags,key_flags,value_flags,entry_flags)

#define SALTS_STL_KIND_DEFINE_C1_INDEX(kind,name,type,raw,prefix,methods,accept,key_at_op,value_at_op,range_flags,key_flags,value_flags,entry_flags) \
 CMETA_CONTAINER1_DEFINE(name,type,raw,prefix,STL_OK,_,methods) SALTS_META_C1_GENERIC_EXTENSION(kind,name,CMETA_TYPEOF(type)) SALTS_META_C1_GENERATION(name,prefix) SALTS_META_C1_COLLECTOR(name,type,accept) CMETA_CONTAINER1_INDEX_RANGE_DEFINE_WITH_TYPE_EXT(name,type,CMETA_TYPEOF(type),prefix,range_flags,name##_cmeta_generation,name##_collector_erased,&name##_cmeta_ext) SALTS_META_C1_METHOD_METADATA(kind,name,type,CMETA_TYPEOF(type),methods)
#define SALTS_STL_KIND_DEFINE_C1_LINK(kind,name,type,raw,prefix,methods,accept,key_at_op,value_at_op,range_flags,key_flags,value_flags,entry_flags) \
 CMETA_CONTAINER1_DEFINE(name,type,raw,prefix,STL_OK,_,methods) SALTS_META_C1_GENERIC_EXTENSION(kind,name,CMETA_TYPEOF(type)) SALTS_META_C1_GENERATION(name,prefix) SALTS_META_C1_COLLECTOR(name,type,accept) CMETA_CONTAINER1_LINK_RANGE_DEFINE_WITH_TYPE_EXT(name,type,CMETA_TYPEOF(type),prefix,range_flags,name##_cmeta_generation,name##_collector_erased,&name##_cmeta_ext) SALTS_META_C1_METHOD_METADATA(kind,name,type,CMETA_TYPEOF(type),methods)
#define SALTS_STL_KIND_DEFINE_C1_SLOT(kind,name,type,raw,prefix,methods,accept,key_at_op,value_at_op,range_flags,key_flags,value_flags,entry_flags) \
 CMETA_CONTAINER1_DEFINE(name,type,raw,prefix,STL_OK,_,methods) SALTS_META_C1_GENERIC_EXTENSION(kind,name,CMETA_TYPEOF(type)) SALTS_META_C1_GENERATION(name,prefix) SALTS_META_C1_COLLECTOR(name,type,accept) CMETA_CONTAINER1_SLOT_RANGE_DEFINE_WITH_TYPE_EXT(name,type,CMETA_TYPEOF(type),prefix,range_flags,name##_cmeta_generation,name##_collector_erased,&name##_cmeta_ext) SALTS_META_C1_METHOD_METADATA(kind,name,type,CMETA_TYPEOF(type),methods)
#define SALTS_STL_KIND_DEFINE_C2_HASH(kind,name,k,v,raw,prefix,methods,accept,key_at_op,value_at_op,range_flags,key_flags,value_flags,entry_flags) \
 CMETA_CONTAINER2_DEFINE(name,k,v,raw,prefix,STL_OK,_,methods) SALTS_META_C2_GENERIC_EXTENSION(kind,name,CMETA_TYPEOF(k),CMETA_TYPEOF(v)) SALTS_META_C2_GENERATION(name,prefix) SALTS_META_C2_COLLECTOR(name) CMETA_CONTAINER2_RANGES_DEFINE_WITH_TYPES_EXT(name,k,v,CMETA_TYPEOF(k),CMETA_TYPEOF(v),prefix,key_at_op,value_at_op,key_flags,value_flags,entry_flags,name##_cmeta_generation,name##_collector_erased,&name##_cmeta_ext) SALTS_META_C2_METHOD_METADATA(kind,name,k,v,CMETA_TYPEOF(k),CMETA_TYPEOF(v),methods)
#define SALTS_STL_KIND_DEFINE_C2_MULTIMAP(kind,name,k,v,raw,prefix,methods,accept,key_at_op,value_at_op,range_flags,key_flags,value_flags,entry_flags) \
 CMETA_CONTAINER2_DEFINE(name,k,v,raw,prefix,STL_OK,_,methods) SALTS_META_C2_GENERIC_EXTENSION(kind,name,CMETA_TYPEOF(k),CMETA_TYPEOF(v)) SALTS_META_C2_GENERATION(name,prefix) SALTS_META_MULTIMAP_COLLECTOR(name) CMETA_CONTAINER2_RANGES_DEFINE_WITH_TYPES_EXT(name,k,v,CMETA_TYPEOF(k),CMETA_TYPEOF(v),multimap,key_at_op,value_at_op,key_flags,value_flags,entry_flags,name##_cmeta_generation,name##_collector_erased,&name##_cmeta_ext) SALTS_META_C2_METHOD_METADATA(kind,name,k,v,CMETA_TYPEOF(k),CMETA_TYPEOF(v),methods)
#define SALTS_STL_KIND_DEFINE_C2_LINK(kind,name,k,v,raw,prefix,methods,accept,key_at_op,value_at_op,range_flags,key_flags,value_flags,entry_flags) \
 CMETA_CONTAINER2_DEFINE(name,k,v,raw,prefix,STL_OK,_,methods) SALTS_META_C2_GENERIC_EXTENSION(kind,name,CMETA_TYPEOF(k),CMETA_TYPEOF(v)) SALTS_META_C2_GENERATION(name,prefix) SALTS_META_C2_COLLECTOR(name) CMETA_CONTAINER2_LINK_RANGES_DEFINE_WITH_TYPES_EXT(name,k,v,CMETA_TYPEOF(k),CMETA_TYPEOF(v),prefix,key_flags,value_flags,entry_flags,name##_cmeta_generation,name##_collector_erased,&name##_cmeta_ext) SALTS_META_C2_METHOD_METADATA(kind,name,k,v,CMETA_TYPEOF(k),CMETA_TYPEOF(v),methods)

#define SALTS_STL_KIND_APPLY_EXPLICIT(row,name,...) SALTS_STL_KIND_APPLY_EXPLICIT_E(row,name,__VA_ARGS__)
#define SALTS_STL_KIND_APPLY_EXPLICIT_E(row,name,...) SALTS_STL_KIND_APPLY_EXPLICIT_EXPAND(CMETA_PP_UNPAREN row,name,__VA_ARGS__)
#define SALTS_STL_KIND_APPLY_EXPLICIT_EXPAND(...) SALTS_STL_KIND_APPLY_EXPLICIT_I(__VA_ARGS__)
#define SALTS_STL_KIND_EXPLICIT_FAMILY(family) CMETA_PP_CAT(CMETA_PP_CAT(SALTS_STL_KIND_DEFINE_,family),_EXPLICIT)
#define SALTS_STL_KIND_APPLY_EXPLICIT_I(kind,arity,family,raw,prefix,methods,accept,key_at_op,value_at_op,range_flags,key_flags,value_flags,entry_flags,name,...) SALTS_STL_KIND_EXPLICIT_FAMILY(family)(kind,name,__VA_ARGS__,raw,prefix,methods,accept,key_at_op,value_at_op,range_flags,key_flags,value_flags,entry_flags)

#define SALTS_STL_KIND_DEFINE_C1_INDEX_EXPLICIT(kind,name,type,type_desc,data_desc,value_identity,raw,prefix,methods,accept,key_at_op,value_at_op,range_flags,key_flags,value_flags,entry_flags) \
 CMETA_CONTAINER1_DEFINE_WITH_TYPE(name,type,type_desc,raw,prefix,STL_OK,_,methods) CMETA_INLINE const cmeta_data_desc *name##_cmeta_data(void); CMETA_DEFINE_DATA_TRAITS(name,name##_cmeta_data()); SALTS_META_C1_GENERIC_EXTENSION(kind,name,type_desc) SALTS_META_C1_GENERATION(name,prefix) SALTS_META_C1_COLLECTOR_WITH_TYPE(name,type,type_desc,accept) CMETA_CONTAINER1_INDEX_RANGE_DEFINE_WITH_TYPE_EXT_VALUE(name,type,type_desc,prefix,range_flags,name##_cmeta_generation,name##_collector_erased,&name##_cmeta_ext,&cmeta_traits_##name,value_identity) SALTS_META_C1_METHOD_METADATA(kind,name,type,type_desc,methods)
#define SALTS_STL_KIND_DEFINE_C1_LINK_EXPLICIT(kind,name,type,type_desc,data_desc,value_identity,raw,prefix,methods,accept,key_at_op,value_at_op,range_flags,key_flags,value_flags,entry_flags) \
 CMETA_CONTAINER1_DEFINE_WITH_TYPE(name,type,type_desc,raw,prefix,STL_OK,_,methods) CMETA_INLINE const cmeta_data_desc *name##_cmeta_data(void); CMETA_DEFINE_DATA_TRAITS(name,name##_cmeta_data()); SALTS_META_C1_GENERIC_EXTENSION(kind,name,type_desc) SALTS_META_C1_GENERATION(name,prefix) SALTS_META_C1_COLLECTOR_WITH_TYPE(name,type,type_desc,accept) CMETA_CONTAINER1_LINK_RANGE_DEFINE_WITH_TYPE_EXT_VALUE(name,type,type_desc,prefix,range_flags,name##_cmeta_generation,name##_collector_erased,&name##_cmeta_ext,&cmeta_traits_##name,value_identity) SALTS_META_C1_METHOD_METADATA(kind,name,type,type_desc,methods)
#define SALTS_STL_KIND_DEFINE_C1_SLOT_EXPLICIT(kind,name,type,type_desc,data_desc,value_identity,raw,prefix,methods,accept,key_at_op,value_at_op,range_flags,key_flags,value_flags,entry_flags) \
 CMETA_CONTAINER1_DEFINE_WITH_TYPE(name,type,type_desc,raw,prefix,STL_OK,_,methods) CMETA_INLINE const cmeta_data_desc *name##_cmeta_data(void); CMETA_DEFINE_DATA_TRAITS(name,name##_cmeta_data()); SALTS_META_C1_GENERIC_EXTENSION(kind,name,type_desc) SALTS_META_C1_GENERATION(name,prefix) SALTS_META_C1_COLLECTOR_WITH_TYPE(name,type,type_desc,accept) CMETA_CONTAINER1_SLOT_RANGE_DEFINE_WITH_TYPE_EXT_VALUE(name,type,type_desc,prefix,range_flags,name##_cmeta_generation,name##_collector_erased,&name##_cmeta_ext,&cmeta_traits_##name,value_identity) SALTS_META_C1_METHOD_METADATA(kind,name,type,type_desc,methods)
#define SALTS_STL_KIND_DEFINE_C2_HASH_EXPLICIT(kind,name,k,v,key_desc,key_data,value_desc,value_data,value_identity,raw,prefix,methods,accept,key_at_op,value_at_op,range_flags,key_flags,value_flags,entry_flags) \
 CMETA_CONTAINER2_DEFINE_WITH_TYPES(name,k,v,key_desc,value_desc,raw,prefix,STL_OK,_,methods) CMETA_INLINE const cmeta_data_desc *name##_cmeta_data(void); CMETA_DEFINE_DATA_TRAITS(name,name##_cmeta_data()); SALTS_META_C2_GENERIC_EXTENSION(kind,name,key_desc,value_desc) SALTS_META_C2_GENERATION(name,prefix) SALTS_META_C2_COLLECTOR(name) CMETA_CONTAINER2_RANGES_DEFINE_WITH_TYPES_EXT_VALUE(name,k,v,key_desc,value_desc,prefix,key_at_op,value_at_op,key_flags,value_flags,entry_flags,name##_cmeta_generation,name##_collector_erased,&name##_cmeta_ext,&cmeta_traits_##name,value_identity) SALTS_META_C2_METHOD_METADATA(kind,name,k,v,key_desc,value_desc,methods)
#define SALTS_STL_KIND_DEFINE_C2_MULTIMAP_EXPLICIT(kind,name,k,v,key_desc,key_data,value_desc,value_data,value_identity,raw,prefix,methods,accept,key_at_op,value_at_op,range_flags,key_flags,value_flags,entry_flags) \
 CMETA_CONTAINER2_DEFINE_WITH_TYPES(name,k,v,key_desc,value_desc,raw,prefix,STL_OK,_,methods) CMETA_INLINE const cmeta_data_desc *name##_cmeta_data(void); CMETA_DEFINE_DATA_TRAITS(name,name##_cmeta_data()); SALTS_META_C2_GENERIC_EXTENSION(kind,name,key_desc,value_desc) SALTS_META_C2_GENERATION(name,prefix) SALTS_META_MULTIMAP_COLLECTOR(name) CMETA_CONTAINER2_RANGES_DEFINE_WITH_TYPES_EXT_VALUE(name,k,v,key_desc,value_desc,multimap,key_at_op,value_at_op,key_flags,value_flags,entry_flags,name##_cmeta_generation,name##_collector_erased,&name##_cmeta_ext,&cmeta_traits_##name,value_identity) SALTS_META_C2_METHOD_METADATA(kind,name,k,v,key_desc,value_desc,methods)
#define SALTS_STL_KIND_DEFINE_C2_LINK_EXPLICIT(kind,name,k,v,key_desc,key_data,value_desc,value_data,value_identity,raw,prefix,methods,accept,key_at_op,value_at_op,range_flags,key_flags,value_flags,entry_flags) \
 CMETA_CONTAINER2_DEFINE_WITH_TYPES(name,k,v,key_desc,value_desc,raw,prefix,STL_OK,_,methods) CMETA_INLINE const cmeta_data_desc *name##_cmeta_data(void); CMETA_DEFINE_DATA_TRAITS(name,name##_cmeta_data()); SALTS_META_C2_GENERIC_EXTENSION(kind,name,key_desc,value_desc) SALTS_META_C2_GENERATION(name,prefix) SALTS_META_C2_COLLECTOR(name) CMETA_CONTAINER2_LINK_RANGES_DEFINE_WITH_TYPES_EXT_VALUE(name,k,v,key_desc,value_desc,prefix,key_flags,value_flags,entry_flags,name##_cmeta_generation,name##_collector_erased,&name##_cmeta_ext,&cmeta_traits_##name,value_identity) SALTS_META_C2_METHOD_METADATA(kind,name,k,v,key_desc,value_desc,methods)

#define SALTS_VEC_DEFINE(name,type) SALTS_STL_KIND_APPLY(SALTS_STL_KIND_ROW_Vec,name,type) SALTS_META_C1_CONSTRUCT(name,SALTS_STL_VEC_INITIALIZER(type),name##_destroy(self)) SALTS_META_VEC_COLLECTION_DATA(name,type)
#define SALTS_DEQUE_DEFINE(name,type) SALTS_STL_KIND_APPLY(SALTS_STL_KIND_ROW_Deque,name,type) SALTS_META_C1_CONSTRUCT(name,SALTS_STL_DEQUE_INITIALIZER(type),name##_destroy(self)) SALTS_META_INDEX_SEQUENCE_DATA(name,type,CMETA_DATA_SEQUENCE)
#define SALTS_LIST_DEFINE(name,type) SALTS_STL_KIND_APPLY(SALTS_STL_KIND_ROW_List,name,type) SALTS_META_C1_CONSTRUCT(name,SALTS_STL_LIST_INITIALIZER(type),name##_destroy(self)) SALTS_META_LIST_COLLECTION_DATA(name,type)
#define SALTS_STACK_DEFINE(name,type) SALTS_STL_KIND_APPLY(SALTS_STL_KIND_ROW_Stack,name,type) SALTS_META_C1_CONSTRUCT(name,SALTS_STL_STACK_INITIALIZER(type),name##_destroy(self)) SALTS_META_ADAPTER_SEQUENCE_DATA(name,type,stack,CMETA_DATA_SEQUENCE)
#define SALTS_QUEUE_DEFINE(name,type) SALTS_STL_KIND_APPLY(SALTS_STL_KIND_ROW_Queue,name,type) SALTS_META_C1_CONSTRUCT(name,SALTS_STL_QUEUE_INITIALIZER(type),name##_destroy(self)) SALTS_META_ADAPTER_SEQUENCE_DATA(name,type,queue,CMETA_DATA_SEQUENCE)
#define SALTS_HEAP_DEFINE(name,type) SALTS_STL_KIND_APPLY(SALTS_STL_KIND_ROW_Heap,name,type)
#define SALTS_SET_DEFINE(name,type) SALTS_STL_KIND_APPLY(SALTS_STL_KIND_ROW_Set,name,type) SALTS_META_C1_CONSTRUCT(name,SALTS_STL_SET_INITIALIZER(type),name##_destroy(self)) SALTS_META_SET_COLLECTION_DATA(name,type,set,CMETA_DATA_SET)
#define SALTS_HASH_SET_DEFINE(name,type) SALTS_STL_KIND_APPLY(SALTS_STL_KIND_ROW_HashSet,name,type) SALTS_META_C1_CONSTRUCT(name,SALTS_STL_HASH_SET_INITIALIZER(type),name##_destroy(self)) SALTS_META_SLOT_SET_COLLECTION_DATA(name,type,hash_set,CMETA_DATA_SET)
#define SALTS_HASH_MAP_DEFINE(name,k,v) SALTS_STL_KIND_APPLY(SALTS_STL_KIND_ROW_HashMap,name,k,v) SALTS_META_MAP_ACCEPT(name,k,v) SALTS_META_C2_CONSTRUCT(name,SALTS_STL_HASH_MAP_INITIALIZER(k,v),name##_destroy(self)) SALTS_META_HASH_MAP_DATA(name,k,v)
#define SALTS_MAP_DEFINE(name,k,v) SALTS_STL_KIND_APPLY(SALTS_STL_KIND_ROW_Map,name,k,v) SALTS_META_MAP_ACCEPT(name,k,v) SALTS_META_C2_CONSTRUCT(name,SALTS_STL_MAP_INITIALIZER(k,v),name##_destroy(self)) SALTS_META_MAP_DATA(name,k,v)
#define SALTS_MULTI_MAP_DEFINE(name,k,v) SALTS_STL_KIND_APPLY(SALTS_STL_KIND_ROW_MultiMap,name,k,v) SALTS_META_MAP_ACCEPT(name,k,v) SALTS_META_C2_CONSTRUCT(name,SALTS_STL_MULTIMAP_INITIALIZER(k,v),name##_destroy(self)) SALTS_META_MULTIMAP_DATA(name,k,v)
#define SALTS_BTREE_DEFINE(name,k,v) SALTS_STL_KIND_APPLY(SALTS_STL_KIND_ROW_BTree,name,k,v) SALTS_META_MAP_ACCEPT(name,k,v) SALTS_META_C2_CONSTRUCT(name,SALTS_STL_BTREE_INITIALIZER(k,v),name##_destroy(self)) SALTS_META_TREE_MAP_DATA(name,k,v,btree)
#define SALTS_BPLUS_TREE_DEFINE(name,k,v) SALTS_STL_KIND_APPLY(SALTS_STL_KIND_ROW_BPlusTree,name,k,v) SALTS_META_MAP_ACCEPT(name,k,v) SALTS_META_C2_CONSTRUCT(name,SALTS_STL_BPLUS_TREE_INITIALIZER(k,v),name##_destroy(self)) SALTS_META_TREE_MAP_DATA(name,k,v,bplus_tree)
#define SALTS_VEC_DEFINE_EXPLICIT(name,type,type_desc,data_desc) SALTS_VEC_DEFINE_EXPLICIT_IDENTITY(name,type,type_desc,data_desc,NULL)
/* Borrow the owner's metadata instead of statically initializing imported
 * addresses. Raw methods are replayed from the same schema as local facades. */
#define SALTS_META_IMPORTED_BASE(name,container_expr,methods_expr) \
 CMETA_INLINE const cmeta_container_desc *name##_cmeta_descriptor(void){return (container_expr);} \
 CMETA_INLINE const cmeta_receiver_operation_set *name##_receiver_operation_set(void){return (methods_expr);} \
 CMETA_INLINE cmeta_collector name##_collector(name *output,size_t limit){return name##_cmeta_descriptor()->collector(output,limit);} \
 CMETA_INLINE cmeta_collector name##_collector_erased(void *output,size_t limit){return name##_collector((name*)output,limit);} \
 enum { name##_cmeta_typed = 1 };
#define SALTS_META_IMPORTED_COMMON(name,container_expr,data_expr,methods_expr) \
 SALTS_META_IMPORTED_BASE(name,container_expr,methods_expr) \
 CMETA_INLINE const cmeta_data_desc *name##_cmeta_data(void){return (data_expr);} \
 CMETA_DEFINE_DATA_TRAITS(name,name##_cmeta_data()); \
 CMETA_LOCAL const cmeta_type_desc name##_cmeta_type={#name,sizeof(name),_Alignof(name),CMETA_T_OBJECT,NULL,&cmeta_traits_##name,NULL}; \
 CMETA_INLINE cmeta_status name##_construct_init_zero(void *object){return cmeta_data_construct_init_zero(name##_cmeta_data(),object);} \
 CMETA_INLINE void name##_construct_restore_zero(void *object){(void)cmeta_data_construct_restore_zero(name##_cmeta_data(),object);} \
 CMETA_INLINE void name##_construct_move(void *destination,void *source){(void)cmeta_data_construct_move(name##_cmeta_data(),destination,source);} \
 enum { name##_construct_flags = CMETA_LIFECYCLE_INIT_NOFAIL | CMETA_LIFECYCLE_MOVABLE }; \
 CMETA_LOCAL const cmeta_data_construct_ops name##_construct_ops={sizeof(cmeta_data_construct_ops),CMETA_DATA_CONSTRUCT_OPS_ABI_VERSION,&name##_cmeta_type,name##_construct_init_zero,name##_construct_restore_zero,name##_construct_move,name##_construct_flags}; \
 CMETA_DEFINE_STATIC_LIFECYCLE(name,name##_construct_ops,name##_construct_flags)
#define SALTS_META_IMPORTED_METHOD(name,pub) \
 CMETA_INLINE const cmeta_function_abi_desc *name##_##pub##_function_abi(void){const cmeta_receiver_operation *op=cmeta_receiver_operation_find(name##_receiver_operation_set(),#pub);return op!=NULL?op->abi:NULL;} \
 CMETA_INLINE const cmeta_function_desc *name##_##pub##_function(void){const cmeta_function_abi_desc *abi=name##_##pub##_function_abi();return abi!=NULL?abi->function:NULL;}
#define SALTS_META_C1_IMPORTED_METHOD(pub,name,type,type_desc) SALTS_META_IMPORTED_METHOD(name,pub)
#define SALTS_META_C2_IMPORTED_METHOD(pub,name,k,v,key_desc,value_desc) SALTS_META_IMPORTED_METHOD(name,pub)
#define SALTS_META_IMPORTED_RANGE(name,pub,member) \
 CMETA_INLINE cmeta_range name##_##pub(const name *self){return name##_cmeta_descriptor()->member(self);}

/* Storage, raw operation prefix and method membership come from the same kind
 * row as local declarations. Replay the same reflection filter as well. */
#define SALTS_STL_KIND_IMPORT(row,name,...) SALTS_STL_KIND_IMPORT_E(row,name,__VA_ARGS__)
#define SALTS_STL_KIND_IMPORT_E(row,name,...) SALTS_STL_KIND_IMPORT_EXPAND(CMETA_PP_UNPAREN row,name,__VA_ARGS__)
#define SALTS_STL_KIND_IMPORT_EXPAND(...) SALTS_STL_KIND_IMPORT_I(__VA_ARGS__)
#define SALTS_STL_KIND_IMPORT_I(kind,arity,family,raw,prefix,methods,accept,key_at_op,value_at_op,range_flags,key_flags,value_flags,entry_flags,name,...) \
 CMETA_PP_CAT(SALTS_META_IMPORT_C,arity)(name,raw,prefix,methods,__VA_ARGS__)
#define SALTS_META_IMPORT_C1(name,raw,prefix,methods,type,container_expr,data_expr,methods_expr) \
 CMETA_CONTAINER1_DECLARE(name,raw); \
 SALTS_META_IMPORTED_COMMON(name,container_expr,data_expr,methods_expr) \
 CMETA_CONTAINER1_METHODS_WITH_DESCRIPTOR(name,type,name##_cmeta_descriptor()->element_type,raw,prefix,STL_OK,name##_cmeta_descriptor(),methods) \
 SALTS_META_IMPORTED_RANGE(name,range,range) \
 methods(SALTS_META_C1_METHOD_REFLECT_DISPATCH,(name,type,_,SALTS_META_C1_IMPORTED_METHOD))
#define SALTS_META_IMPORT_C2(name,raw,prefix,methods,k,v,container_expr,data_expr,methods_expr) \
 CMETA_CONTAINER2_DECLARE(name,k,v,raw); \
 SALTS_META_IMPORTED_COMMON(name,container_expr,data_expr,methods_expr) \
 CMETA_CONTAINER2_METHODS_WITH_DESCRIPTOR(name,k,v,name##_cmeta_descriptor()->key_type,name##_cmeta_descriptor()->value_type,raw,prefix,STL_OK,name##_cmeta_descriptor(),methods) \
 SALTS_META_IMPORTED_RANGE(name,keys_range,keys_range) \
 SALTS_META_IMPORTED_RANGE(name,values_range,values_range) \
 SALTS_META_IMPORTED_RANGE(name,entries_range,entries_range) \
 methods(SALTS_META_C2_METHOD_REFLECT_DISPATCH,(name,k,v,_,_,SALTS_META_C2_IMPORTED_METHOD))
/* Heap deliberately keeps its existing method/range/collector-only contract. */
#define SALTS_STL_HEAP_IMPORT(name,type,container_expr,methods_expr) \
 SALTS_STL_HEAP_IMPORT_E(SALTS_STL_KIND_ROW_Heap,name,type,container_expr,methods_expr)
#define SALTS_STL_HEAP_IMPORT_E(row,...) SALTS_STL_HEAP_IMPORT_EXPAND(CMETA_PP_UNPAREN row,__VA_ARGS__)
#define SALTS_STL_HEAP_IMPORT_EXPAND(...) SALTS_STL_HEAP_IMPORT_I(__VA_ARGS__)
#define SALTS_STL_HEAP_IMPORT_I(kind,arity,family,raw,prefix,methods,accept,key_at_op,value_at_op,range_flags,key_flags,value_flags,entry_flags,name,type,container_expr,methods_expr) \
 CMETA_CONTAINER1_DECLARE(name,raw); \
 SALTS_META_IMPORTED_BASE(name,container_expr,methods_expr) \
 CMETA_LOCAL const cmeta_type_desc name##_cmeta_type={#name,sizeof(name),_Alignof(name),CMETA_T_OBJECT,NULL,NULL,NULL}; \
 CMETA_CONTAINER1_METHODS_WITH_DESCRIPTOR(name,type,name##_cmeta_descriptor()->element_type,raw,prefix,STL_OK,name##_cmeta_descriptor(),methods) \
 SALTS_META_IMPORTED_RANGE(name,range,range) \
 methods(SALTS_META_C1_METHOD_REFLECT_DISPATCH,(name,type,_,SALTS_META_C1_IMPORTED_METHOD))
#define SALTS_VEC_DEFINE_EXPLICIT_IDENTITY(name,type,type_desc,data_desc,value_identity) SALTS_STL_KIND_APPLY_EXPLICIT(SALTS_STL_KIND_ROW_Vec,name,type,type_desc,data_desc,value_identity) SALTS_META_C1_CONSTRUCT(name,SALTS_STL_VEC_INITIALIZER_WITH_TYPE(type,type_desc),name##_destroy(self)) SALTS_META_VEC_COLLECTION_DATA_WITH_DATA(name,type,data_desc)
#define SALTS_DEQUE_DEFINE_EXPLICIT(name,type,type_desc,data_desc) SALTS_DEQUE_DEFINE_EXPLICIT_IDENTITY(name,type,type_desc,data_desc,NULL)
#define SALTS_DEQUE_DEFINE_EXPLICIT_IDENTITY(name,type,type_desc,data_desc,value_identity) SALTS_STL_KIND_APPLY_EXPLICIT(SALTS_STL_KIND_ROW_Deque,name,type,type_desc,data_desc,value_identity) SALTS_META_C1_CONSTRUCT(name,SALTS_STL_DEQUE_INITIALIZER_WITH_TYPE(type,type_desc),name##_destroy(self)) SALTS_META_INDEX_SEQUENCE_DATA_WITH_DATA(name,type,data_desc,CMETA_DATA_SEQUENCE)
#define SALTS_LIST_DEFINE_EXPLICIT(name,type,type_desc,data_desc) SALTS_LIST_DEFINE_EXPLICIT_IDENTITY(name,type,type_desc,data_desc,NULL)
#define SALTS_LIST_DEFINE_EXPLICIT_IDENTITY(name,type,type_desc,data_desc,value_identity) SALTS_STL_KIND_APPLY_EXPLICIT(SALTS_STL_KIND_ROW_List,name,type,type_desc,data_desc,value_identity) SALTS_META_C1_CONSTRUCT(name,SALTS_STL_LIST_INITIALIZER_WITH_TYPE(type,type_desc),name##_destroy(self)) SALTS_META_LIST_COLLECTION_DATA_WITH_DATA(name,type,data_desc)
#define SALTS_STACK_DEFINE_EXPLICIT(name,type,type_desc,data_desc) SALTS_STACK_DEFINE_EXPLICIT_IDENTITY(name,type,type_desc,data_desc,NULL)
#define SALTS_STACK_DEFINE_EXPLICIT_IDENTITY(name,type,type_desc,data_desc,value_identity) SALTS_STL_KIND_APPLY_EXPLICIT(SALTS_STL_KIND_ROW_Stack,name,type,type_desc,data_desc,value_identity) SALTS_META_C1_CONSTRUCT(name,SALTS_STL_STACK_INITIALIZER_WITH_TYPE(type,type_desc),name##_destroy(self)) SALTS_META_ADAPTER_SEQUENCE_DATA_WITH_DATA(name,type,data_desc,stack,CMETA_DATA_SEQUENCE)
#define SALTS_QUEUE_DEFINE_EXPLICIT(name,type,type_desc,data_desc) SALTS_QUEUE_DEFINE_EXPLICIT_IDENTITY(name,type,type_desc,data_desc,NULL)
#define SALTS_QUEUE_DEFINE_EXPLICIT_IDENTITY(name,type,type_desc,data_desc,value_identity) SALTS_STL_KIND_APPLY_EXPLICIT(SALTS_STL_KIND_ROW_Queue,name,type,type_desc,data_desc,value_identity) SALTS_META_C1_CONSTRUCT(name,SALTS_STL_QUEUE_INITIALIZER_WITH_TYPE(type,type_desc),name##_destroy(self)) SALTS_META_ADAPTER_SEQUENCE_DATA_WITH_DATA(name,type,data_desc,queue,CMETA_DATA_SEQUENCE)
#define SALTS_SET_DEFINE_EXPLICIT(name,type,type_desc,data_desc) SALTS_SET_DEFINE_EXPLICIT_IDENTITY(name,type,type_desc,data_desc,NULL)
#define SALTS_SET_DEFINE_EXPLICIT_IDENTITY(name,type,type_desc,data_desc,value_identity) SALTS_STL_KIND_APPLY_EXPLICIT(SALTS_STL_KIND_ROW_Set,name,type,type_desc,data_desc,value_identity) SALTS_META_C1_CONSTRUCT(name,SALTS_STL_SET_INITIALIZER_WITH_TYPE(type,type_desc),name##_destroy(self)) SALTS_META_SET_COLLECTION_DATA_WITH_DATA(name,type,data_desc,set,CMETA_DATA_SET)
#define SALTS_HASH_SET_DEFINE_EXPLICIT(name,type,type_desc,data_desc) SALTS_HASH_SET_DEFINE_EXPLICIT_IDENTITY(name,type,type_desc,data_desc,NULL)
#define SALTS_HASH_SET_DEFINE_EXPLICIT_IDENTITY(name,type,type_desc,data_desc,value_identity) SALTS_STL_KIND_APPLY_EXPLICIT(SALTS_STL_KIND_ROW_HashSet,name,type,type_desc,data_desc,value_identity) SALTS_META_C1_CONSTRUCT(name,SALTS_STL_HASH_SET_INITIALIZER_WITH_TYPE(type,type_desc),name##_destroy(self)) SALTS_META_SLOT_SET_COLLECTION_DATA_WITH_DATA(name,type,data_desc,hash_set,CMETA_DATA_SET)

#define SALTS_HASH_MAP_DEFINE_EXPLICIT(name,k,v,key_desc,key_data,value_desc,value_data) SALTS_HASH_MAP_DEFINE_EXPLICIT_IDENTITY(name,k,v,key_desc,key_data,value_desc,value_data,NULL)
#define SALTS_HASH_MAP_DEFINE_EXPLICIT_IDENTITY(name,k,v,key_desc,key_data,value_desc,value_data,value_identity) SALTS_STL_KIND_APPLY_EXPLICIT(SALTS_STL_KIND_ROW_HashMap,name,k,v,key_desc,key_data,value_desc,value_data,value_identity) SALTS_META_MAP_ACCEPT(name,k,v) SALTS_META_C2_CONSTRUCT(name,SALTS_STL_HASH_MAP_INITIALIZER_WITH_TYPES(k,v,key_desc,value_desc),name##_destroy(self)) SALTS_META_HASH_MAP_DATA_WITH_DATA(name,k,v,key_data,value_data)
#define SALTS_MAP_DEFINE_EXPLICIT(name,k,v,key_desc,key_data,value_desc,value_data) SALTS_MAP_DEFINE_EXPLICIT_IDENTITY(name,k,v,key_desc,key_data,value_desc,value_data,NULL)
#define SALTS_MAP_DEFINE_EXPLICIT_IDENTITY(name,k,v,key_desc,key_data,value_desc,value_data,value_identity) SALTS_STL_KIND_APPLY_EXPLICIT(SALTS_STL_KIND_ROW_Map,name,k,v,key_desc,key_data,value_desc,value_data,value_identity) SALTS_META_MAP_ACCEPT(name,k,v) SALTS_META_C2_CONSTRUCT(name,SALTS_STL_MAP_INITIALIZER_WITH_TYPES(k,v,key_desc,value_desc),name##_destroy(self)) SALTS_META_MAP_DATA_WITH_DATA(name,k,v,key_data,value_data)
#define SALTS_MULTI_MAP_DEFINE_EXPLICIT(name,k,v,key_desc,key_data,value_desc,value_data) SALTS_MULTI_MAP_DEFINE_EXPLICIT_IDENTITY(name,k,v,key_desc,key_data,value_desc,value_data,NULL)
#define SALTS_MULTI_MAP_DEFINE_EXPLICIT_IDENTITY(name,k,v,key_desc,key_data,value_desc,value_data,value_identity) SALTS_STL_KIND_APPLY_EXPLICIT(SALTS_STL_KIND_ROW_MultiMap,name,k,v,key_desc,key_data,value_desc,value_data,value_identity) SALTS_META_MAP_ACCEPT(name,k,v) SALTS_META_C2_CONSTRUCT(name,SALTS_STL_MULTIMAP_INITIALIZER_WITH_TYPES(k,v,key_desc,value_desc),name##_destroy(self)) SALTS_META_MULTIMAP_DATA_WITH_DATA(name,k,v,key_data,value_data)
#define SALTS_BTREE_DEFINE_EXPLICIT(name,k,v,key_desc,key_data,value_desc,value_data) SALTS_BTREE_DEFINE_EXPLICIT_IDENTITY(name,k,v,key_desc,key_data,value_desc,value_data,NULL)
#define SALTS_BTREE_DEFINE_EXPLICIT_IDENTITY(name,k,v,key_desc,key_data,value_desc,value_data,value_identity) SALTS_STL_KIND_APPLY_EXPLICIT(SALTS_STL_KIND_ROW_BTree,name,k,v,key_desc,key_data,value_desc,value_data,value_identity) SALTS_META_MAP_ACCEPT(name,k,v) SALTS_META_C2_CONSTRUCT(name,SALTS_STL_BTREE_INITIALIZER_WITH_TYPES(k,v,key_desc,value_desc),name##_destroy(self)) SALTS_META_TREE_MAP_DATA_WITH_DATA(name,k,v,key_data,value_data,btree)
#define SALTS_BPLUS_TREE_DEFINE_EXPLICIT(name,k,v,key_desc,key_data,value_desc,value_data) SALTS_BPLUS_TREE_DEFINE_EXPLICIT_IDENTITY(name,k,v,key_desc,key_data,value_desc,value_data,NULL)
#define SALTS_BPLUS_TREE_DEFINE_EXPLICIT_IDENTITY(name,k,v,key_desc,key_data,value_desc,value_data,value_identity) SALTS_STL_KIND_APPLY_EXPLICIT(SALTS_STL_KIND_ROW_BPlusTree,name,k,v,key_desc,key_data,value_desc,value_data,value_identity) SALTS_META_MAP_ACCEPT(name,k,v) SALTS_META_C2_CONSTRUCT(name,SALTS_STL_BPLUS_TREE_INITIALIZER_WITH_TYPES(k,v,key_desc,value_desc),name##_destroy(self)) SALTS_META_TREE_MAP_DATA_WITH_DATA(name,k,v,key_data,value_data,bplus_tree)

#endif /* CSTL_DETAIL_TYPED_FACADE_H */
