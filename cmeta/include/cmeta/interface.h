#ifndef CMETA_META_INTERFACE_H
#define CMETA_META_INTERFACE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <cmeta/pp.h>
#include <cmeta/function.h>

/*
 * CMeta Interface/Object Protocol
 * -------------------------------
 *
 * Natural declaration vocabulary is intentional:
 *
 *   interface(name, METHODS)
 *   implements(name, implementation, capabilities, ...)
 *
 * Generated public C symbols remain namespaced by the interface name itself
 * (normally cmeta_* or cflow_*).  There is no class/inheritance model here:
 * an interface value is only { self, vtable } plus capability metadata.
 *
 * A method schema is a single X-list replayed by interface() for the vtable,
 * inline wrappers, and translation-unit-local reflection metadata.
 * Reflection, vtables and self are borrowed from their providers. Keep those
 * providers alive through every use; an owning destructor releases self, not
 * the code module. Module owners must drain all handles/callbacks before unload.
 * Negotiate CMETA_REFLECTION_ABI_VERSION before reading foreign descriptors;
 * interface dispatch schemas need a separate application protocol agreement.
 *
 * Legacy ABI-only rows:
 *
 *   R0..R4  non-void return, 0..4 arguments after self
 *   V0..V4  void return,     0..4 arguments after self
 *   D0       owning destructor; V0 ABI plus handle invalidation after return
 *
 * These rows preserve the historical exact C dispatch surface. Their
 * cmeta_interface_method_desc publishes dispatch arity/flags only and leaves
 * function/abi NULL; CMeta does not infer semantic type descriptors, parameter
 * direction, effects, or ABI carriers from C spelling.
 *
 * Fully reflected rows:
 *
 *   F0..F4   non-void return + canonical FunctionDesc/FunctionAbi;
 *             result semantics remain UNKNOWN
 *   FR0..FR4  non-void return + canonical FunctionDesc/FunctionAbi +
 *             explicit canonical result semantics
 *   FV0..FV4  void return + canonical FunctionDesc/FunctionAbi
 *   FD0       owning destructor + canonical FunctionDesc/FunctionAbi
 *
 * FR rows add one result_flags field after return ABI carrier. Reflected
 * parameters use the exact five-field FunctionDecl parameter row:
 *
 *   (type, name, flags, descriptor, abi_carrier)
 *
 * and each reflected method row also states contract, return descriptor, and
 * return ABI carrier. Thus the same X-list remains the single source for exact
 * vtable ABI, wrappers, and canonical function semantics.
 *
 * Example:
 *
 *   #define WAITABLE_METHODS(X,I) \
 *       X(I,R1,bool,arm,sample_waker,waker) \
 *       X(I,V0,void,cancel,_)
 *
 *   interface(sample_waitable, WAITABLE_METHODS);
 */

typedef uint32_t cmeta_interface_method_flags;

#ifdef __cplusplus
#define CMETA_IFACE_SIZE_CAST(value) static_cast<size_t>(value)
#define CMETA_IFACE_U64_CAST(value) static_cast<uint64_t>(value)
#define CMETA_IFACE_METHOD_FLAGS_CAST(value) \
    static_cast<cmeta_interface_method_flags>(value)
#else
#define CMETA_IFACE_SIZE_CAST(value) ((size_t)(value))
#define CMETA_IFACE_U64_CAST(value) ((uint64_t)(value))
#define CMETA_IFACE_METHOD_FLAGS_CAST(value) \
    ((cmeta_interface_method_flags)(value))
#endif

enum {
    CMETA_INTERFACE_METHOD_NONE = 0u,
    /*
     * Dispatch consumes the Interface capability's self ownership and
     * invalidates that handle after the call. A borrowed Interface projection
     * over another live owner (for example cmeta_object_ref) must not expose
     * this authority without an explicit ownership transfer.
     */
    CMETA_INTERFACE_METHOD_OWNS_SELF = 1u << 0,
    CMETA_INTERFACE_METHOD_FLAG_MASK = CMETA_INTERFACE_METHOD_OWNS_SELF
};

typedef struct cmeta_interface_method_desc {
    size_t size;
    const char *name;
    /* Exact dispatch shape retained even for legacy ABI-only rows. */
    unsigned dispatch_arity;
    cmeta_interface_method_flags flags;
    /* Canonical CMeta function semantics; NULL for legacy ABI-only rows. */
    const cmeta_function_desc *function;
    const cmeta_function_abi_desc *abi;
} cmeta_interface_method_desc;

typedef struct cmeta_interface_desc {
    size_t size;
    const char *name;
    const cmeta_interface_method_desc *methods;
    size_t method_count;
} cmeta_interface_desc;

CMETA_INLINE const cmeta_function_desc *
cmeta_interface_method_function(const cmeta_interface_method_desc *method) {
    return method != NULL ? method->function : NULL;
}

CMETA_INLINE const cmeta_function_abi_desc *
cmeta_interface_method_abi(const cmeta_interface_method_desc *method) {
    return method != NULL ? method->abi : NULL;
}

CMETA_INLINE size_t
cmeta_interface_method_arity(const cmeta_interface_method_desc *method) {
    if (method == NULL) return 0u;
    return method->function != NULL ? method->function->param_count
                                    : CMETA_IFACE_SIZE_CAST(method->dispatch_arity);
}

/* True only for a method whose dispatch consumes the Interface self owner. */
CMETA_INLINE bool
cmeta_interface_method_owns_self(const cmeta_interface_method_desc *method) {
    return method != NULL &&
           (method->flags &
            CMETA_IFACE_METHOD_FLAGS_CAST(CMETA_INTERFACE_METHOD_OWNS_SELF)) != 0u;
}

CMETA_INLINE bool
cmeta_interface_method_reflection_valid(const cmeta_interface_method_desc *method) {
    return method != NULL && method->size >= sizeof(*method) &&
           method->name != NULL && method->name[0] != '\0' &&
           method->function != NULL && method->abi != NULL &&
           cmeta_function_desc_valid(method->function) &&
           cmeta_function_abi_desc_valid(method->abi) &&
           method->abi->function == method->function &&
           method->function->param_count == CMETA_IFACE_SIZE_CAST(method->dispatch_arity) &&
           (method->flags & ~CMETA_IFACE_METHOD_FLAGS_CAST(CMETA_INTERFACE_METHOD_FLAG_MASK)) == 0u;
}

#ifdef __cplusplus
extern "C" {
#endif
bool cmeta_interface_desc_equal(const cmeta_interface_desc *left,
                                const cmeta_interface_desc *right);
#ifdef __cplusplus
}
#endif

CMETA_INLINE bool
cmeta_interface_desc_valid(const cmeta_interface_desc *desc) {
    size_t i;
    if (desc == NULL || desc->size < sizeof(*desc) ||
        desc->name == NULL || desc->name[0] == '\0' ||
        (desc->method_count != 0u && desc->methods == NULL))
        return false;
    for (i = 0u; i < desc->method_count; ++i) {
        const cmeta_interface_method_desc *method = &desc->methods[i];
        if (method->size < sizeof(*method) ||
            method->name == NULL || method->name[0] == '\0' ||
            method->dispatch_arity > 4u ||
            (method->flags & ~CMETA_IFACE_METHOD_FLAGS_CAST(CMETA_INTERFACE_METHOD_FLAG_MASK)) != 0u)
            return false;
        if ((method->function == NULL) != (method->abi == NULL))
            return false;
        if (method->function != NULL &&
            !cmeta_interface_method_reflection_valid(method))
            return false;
    }
    return true;
}

/*
 * Report whether a valid Interface contract contains any dispatch that owns
 * self. This is a semantic query only: it does not grant ownership authority
 * or make an Interface projection safe. Runtime-selected object adapters must
 * use it to fail closed unless ownership is explicitly transferred.
 */
CMETA_INLINE bool
cmeta_interface_desc_has_owning_method(const cmeta_interface_desc *desc) {
    size_t i;
    if (!cmeta_interface_desc_valid(desc))
        return false;
    for (i = 0u; i < desc->method_count; ++i)
        if (cmeta_interface_method_owns_self(&desc->methods[i]))
            return true;
    return false;
}

/*
 * Opt-in admission for exact borrowed dynamic-service ABI.
 * ABI-only R/V methods stay valid for static/legacy use, but cannot prove a
 * cross-DSO native call. Descriptors must be live under the provider's lease.
 * The provider/compiler remains responsible for matching metadata to code.
 * No lookup, allocation, registration, invocation or lease retention occurs.
 */
CMETA_INLINE bool
cmeta_interface_borrowed_service_abi_valid(const cmeta_interface_desc *desc) {
    size_t i;
    if (!cmeta_interface_desc_valid(desc))
        return false;
    for (i = 0u; i < desc->method_count; ++i) {
        const cmeta_interface_method_desc *method = &desc->methods[i];
        const cmeta_function_desc *function;
        size_t j;
        if (!cmeta_interface_method_reflection_valid(method) ||
            cmeta_interface_method_owns_self(method))
            return false;
        function = method->function;
        if (method->abi->return_carrier == CMETA_ABI_UNSPECIFIED ||
            (function->return_type->kind != CMETA_T_VOID &&
             (function->result_flags & CMETA_RESULT_CLASS_MASK) == CMETA_RESULT_UNKNOWN))
            return false;
        for (j = 0u; j < function->param_count; ++j) {
            const cmeta_param_desc *param = &function->params[j];
            if (method->abi->param_carriers[j] == CMETA_ABI_UNSPECIFIED ||
                (param->flags & CMETA_PARAM_DIRECTION_MASK) == CMETA_PARAM_UNKNOWN ||
                (param->type->kind == CMETA_T_POINTER &&
                 (param->flags & CMETA_PARAM_OWNERSHIP_MASK) == 0u))
                return false;
        }
        for (j = 0u; j < i; ++j)
            if (strcmp(desc->methods[j].name, method->name) == 0)
                return false;
    }
    return true;
}

/* Decode source rows once into:
 * (arity, result action, reflected, contract, result descriptor/carrier/flags,
 *  parameter tuples). Only these adapters know the legacy spellings.
 * Result action is authoritative; ownership is never inferred from C spelling. */
#define CMETA_IFACE_PAIR(T,A,C) (T,A)
#define CMETA_IFACE_DECODE_R0(M,I,R,N,_) \
    M(I,R,N,0,result,0,value,NULL,CMETA_ABI_UNSPECIFIED,CMETA_RESULT_UNKNOWN,)
#define CMETA_IFACE_DECODE_R1(M,I,R,N,T0,A0) \
    M(I,R,N,1,result,0,value,NULL,CMETA_ABI_UNSPECIFIED,CMETA_RESULT_UNKNOWN,CMETA_PP_PAIR_MAP_COMMA_N(1,CMETA_IFACE_PAIR,~,T0,A0))
#define CMETA_IFACE_DECODE_R2(M,I,R,N,T0,A0,T1,A1) \
    M(I,R,N,2,result,0,value,NULL,CMETA_ABI_UNSPECIFIED,CMETA_RESULT_UNKNOWN,CMETA_PP_PAIR_MAP_COMMA_N(2,CMETA_IFACE_PAIR,~,T0,A0,T1,A1))
#define CMETA_IFACE_DECODE_R3(M,I,R,N,T0,A0,T1,A1,T2,A2) \
    M(I,R,N,3,result,0,value,NULL,CMETA_ABI_UNSPECIFIED,CMETA_RESULT_UNKNOWN,CMETA_PP_PAIR_MAP_COMMA_N(3,CMETA_IFACE_PAIR,~,T0,A0,T1,A1,T2,A2))
#define CMETA_IFACE_DECODE_R4(M,I,R,N,T0,A0,T1,A1,T2,A2,T3,A3) \
    M(I,R,N,4,result,0,value,NULL,CMETA_ABI_UNSPECIFIED,CMETA_RESULT_UNKNOWN,CMETA_PP_PAIR_MAP_COMMA_N(4,CMETA_IFACE_PAIR,~,T0,A0,T1,A1,T2,A2,T3,A3))
#define CMETA_IFACE_DECODE_V0(M,I,R,N,_) \
    M(I,void,N,0,discard,0,value,NULL,CMETA_ABI_UNSPECIFIED,CMETA_RESULT_UNKNOWN,)
#define CMETA_IFACE_DECODE_V1(M,I,R,N,T0,A0) \
    M(I,void,N,1,discard,0,value,NULL,CMETA_ABI_UNSPECIFIED,CMETA_RESULT_UNKNOWN,CMETA_PP_PAIR_MAP_COMMA_N(1,CMETA_IFACE_PAIR,~,T0,A0))
#define CMETA_IFACE_DECODE_V2(M,I,R,N,T0,A0,T1,A1) \
    M(I,void,N,2,discard,0,value,NULL,CMETA_ABI_UNSPECIFIED,CMETA_RESULT_UNKNOWN,CMETA_PP_PAIR_MAP_COMMA_N(2,CMETA_IFACE_PAIR,~,T0,A0,T1,A1))
#define CMETA_IFACE_DECODE_V3(M,I,R,N,T0,A0,T1,A1,T2,A2) \
    M(I,void,N,3,discard,0,value,NULL,CMETA_ABI_UNSPECIFIED,CMETA_RESULT_UNKNOWN,CMETA_PP_PAIR_MAP_COMMA_N(3,CMETA_IFACE_PAIR,~,T0,A0,T1,A1,T2,A2))
#define CMETA_IFACE_DECODE_V4(M,I,R,N,T0,A0,T1,A1,T2,A2,T3,A3) \
    M(I,void,N,4,discard,0,value,NULL,CMETA_ABI_UNSPECIFIED,CMETA_RESULT_UNKNOWN,CMETA_PP_PAIR_MAP_COMMA_N(4,CMETA_IFACE_PAIR,~,T0,A0,T1,A1,T2,A2,T3,A3))
#define CMETA_IFACE_DECODE_D0(M,I,R,N,_) \
    M(I,void,N,0,destroy,0,value,NULL,CMETA_ABI_UNSPECIFIED,CMETA_RESULT_UNKNOWN,)
#define CMETA_IFACE_DECODE_F0(M,I,R,N,C,RD,RA) \
    M(I,R,N,0,result,1,C,RD,RA,CMETA_RESULT_UNKNOWN,)
#define CMETA_IFACE_DECODE_F1(M,I,R,N,C,RD,RA,P0) \
    M(I,R,N,1,result,1,C,RD,RA,CMETA_RESULT_UNKNOWN,P0)
#define CMETA_IFACE_DECODE_F2(M,I,R,N,C,RD,RA,P0,P1) \
    M(I,R,N,2,result,1,C,RD,RA,CMETA_RESULT_UNKNOWN,P0,P1)
#define CMETA_IFACE_DECODE_F3(M,I,R,N,C,RD,RA,P0,P1,P2) \
    M(I,R,N,3,result,1,C,RD,RA,CMETA_RESULT_UNKNOWN,P0,P1,P2)
#define CMETA_IFACE_DECODE_F4(M,I,R,N,C,RD,RA,P0,P1,P2,P3) \
    M(I,R,N,4,result,1,C,RD,RA,CMETA_RESULT_UNKNOWN,P0,P1,P2,P3)
#define CMETA_IFACE_DECODE_FR0(M,I,R,N,C,RD,RA,RF) \
    M(I,R,N,0,result,1,C,RD,RA,RF,)
#define CMETA_IFACE_DECODE_FR1(M,I,R,N,C,RD,RA,RF,P0) \
    M(I,R,N,1,result,1,C,RD,RA,RF,P0)
#define CMETA_IFACE_DECODE_FR2(M,I,R,N,C,RD,RA,RF,P0,P1) \
    M(I,R,N,2,result,1,C,RD,RA,RF,P0,P1)
#define CMETA_IFACE_DECODE_FR3(M,I,R,N,C,RD,RA,RF,P0,P1,P2) \
    M(I,R,N,3,result,1,C,RD,RA,RF,P0,P1,P2)
#define CMETA_IFACE_DECODE_FR4(M,I,R,N,C,RD,RA,RF,P0,P1,P2,P3) \
    M(I,R,N,4,result,1,C,RD,RA,RF,P0,P1,P2,P3)
#define CMETA_IFACE_DECODE_FV0(M,I,R,N,C,RD,RA) \
    M(I,void,N,0,discard,1,C,RD,RA,CMETA_RESULT_UNKNOWN,)
#define CMETA_IFACE_DECODE_FV1(M,I,R,N,C,RD,RA,P0) \
    M(I,void,N,1,discard,1,C,RD,RA,CMETA_RESULT_UNKNOWN,P0)
#define CMETA_IFACE_DECODE_FV2(M,I,R,N,C,RD,RA,P0,P1) \
    M(I,void,N,2,discard,1,C,RD,RA,CMETA_RESULT_UNKNOWN,P0,P1)
#define CMETA_IFACE_DECODE_FV3(M,I,R,N,C,RD,RA,P0,P1,P2) \
    M(I,void,N,3,discard,1,C,RD,RA,CMETA_RESULT_UNKNOWN,P0,P1,P2)
#define CMETA_IFACE_DECODE_FV4(M,I,R,N,C,RD,RA,P0,P1,P2,P3) \
    M(I,void,N,4,discard,1,C,RD,RA,CMETA_RESULT_UNKNOWN,P0,P1,P2,P3)
#define CMETA_IFACE_DECODE_FD0(M,I,R,N,C,RD,RA) \
    M(I,void,N,0,destroy,1,C,RD,RA,CMETA_RESULT_UNKNOWN,)

#define CMETA_IFACE_DECODE(M,I,K,R,N,...) \
    CMETA_PP_CAT(CMETA_IFACE_DECODE_,K)(M,I,R,N,__VA_ARGS__)
#define CMETA_IFACE_PARAM_DECL(row) \
    CMETA_FUNCTION_PARAM_DECL_APPLY(row)
#define CMETA_IFACE_PARAM_NAME(row) CMETA_FUNCTION_PARAM_NAME(row)
#define CMETA_IFACE_DECL_ROW(row,C) CMETA_IFACE_PARAM_DECL(row)
#define CMETA_IFACE_ARG_ROW(row,C) CMETA_IFACE_PARAM_NAME(row)

#define CMETA_IFACE_VT(I,R,N,A,K,F,C,RD,RA,RF,...) \
    R (*N)(void *self CMETA_PP_MAP_PREFIX_COMMA_N(A,CMETA_IFACE_DECL_ROW,~,__VA_ARGS__));
#define CMETA_IFACE_VT_ROW(I,K,R,N,...) \
    CMETA_IFACE_DECODE(CMETA_IFACE_VT,I,K,R,N,__VA_ARGS__)

#ifdef __cplusplus
#define CMETA_IFACE_MEMBER_ADDRESS_(I,N) (&static_cast<I##_vtable *>(nullptr)->N)
#else
#define CMETA_IFACE_MEMBER_ADDRESS_(I,N) (&((I##_vtable *)0)->N)
#endif
#define CMETA_IFACE_PROOF(I,R,N,A,K,F,C,RD,RA,RF,...) \
    CMETA_STATIC_ASSERT(CMETA_TYPE_MATCHES(CMETA_IFACE_MEMBER_ADDRESS_(I,N), \
        R (**)(void * CMETA_PP_MAP_PREFIX_COMMA_N(A,CMETA_IFACE_DECL_ROW,~,__VA_ARGS__))), \
        "CMeta Interface native vtable member mismatch"); \
    CMETA_STATIC_ASSERT(!(F) || (RA) == CMETA_ABI_UNSPECIFIED || \
        CMETA_TYPE_IS_VOID(R) == ((RA) == CMETA_ABI_VOID), \
        "CMeta Interface native result carrier mismatch");
#define CMETA_IFACE_PROOF_ROW(I,K,R,N,...) \
    CMETA_IFACE_DECODE(CMETA_IFACE_PROOF,I,K,R,N,__VA_ARGS__)

/* D remains a separate semantic operation: dispatch first, then invalidate. */
#define CMETA_IFACE_BEFORE_result(N)
#define CMETA_IFACE_BEFORE_discard(N)
#define CMETA_IFACE_BEFORE_destroy(N) \
    if (!self || !self->self || !self->vtable || !self->vtable->N) return;
#define CMETA_IFACE_RETURN_result return
#define CMETA_IFACE_RETURN_discard
#define CMETA_IFACE_RETURN_destroy
#define CMETA_IFACE_AFTER_result
#define CMETA_IFACE_AFTER_discard
#define CMETA_IFACE_AFTER_destroy self->self = NULL; self->vtable = NULL;
#define CMETA_IFACE_IMPL(I,R,N,A,K,F,C,RD,RA,RF,...) \
    CMETA_INLINE R I##_##N(I *self \
        CMETA_PP_MAP_PREFIX_COMMA_N(A,CMETA_IFACE_DECL_ROW,~,__VA_ARGS__)) { \
        CMETA_PP_CAT(CMETA_IFACE_BEFORE_,K)(N) \
        CMETA_PP_CAT(CMETA_IFACE_RETURN_,K) self->vtable->N(self->self \
            CMETA_PP_MAP_PREFIX_COMMA_N(A,CMETA_IFACE_ARG_ROW,~,__VA_ARGS__)); \
        CMETA_PP_CAT(CMETA_IFACE_AFTER_,K) \
    }
#define CMETA_IFACE_IMPL_ROW(I,K,R,N,...) \
    CMETA_IFACE_DECODE(CMETA_IFACE_IMPL,I,K,R,N,__VA_ARGS__)
#define CMETA_IFACE_VALID_ROW(I,K,R,N,...) && self->vtable->N != NULL

#define CMETA_IFACE_FUNCTION_GETTERS(I,N) \
    CMETA_INLINE const cmeta_function_desc *I##_##N##_function(void) { \
        return &I##_##N##__function_meta; \
    } \
    CMETA_INLINE const cmeta_function_abi_desc *I##_##N##_function_abi(void) { \
        return &I##_##N##__function_abi_meta; \
    }
#define CMETA_IFACE_FUNCTION_ZERO(I,N,C,RD,RA,RF,...) \
    CMETA_FUNCTION0_METADATA_AS_ABI_RESULT(I##_##N, \
        CMETA_PP_STRINGIFY(I) "." CMETA_PP_STRINGIFY(N),C,RD,RA,RF)
#define CMETA_IFACE_FUNCTION_PARAMS(I,N,C,RD,RA,RF,...) \
    CMETA_FUNCTION_METADATA_AS_ABI_RESULT(I##_##N, \
        CMETA_PP_STRINGIFY(I) "." CMETA_PP_STRINGIFY(N),C,RD,RA,RF,__VA_ARGS__)
#define CMETA_IFACE_FUNCTION_REFLECTED(I,R,N,A,K,F,C,RD,RA,RF,...) \
    CMETA_PP_IF(A)(CMETA_IFACE_FUNCTION_PARAMS,CMETA_IFACE_FUNCTION_ZERO)( \
        I,N,C,RD,RA,RF,__VA_ARGS__); \
    CMETA_IFACE_FUNCTION_GETTERS(I,N)
#define CMETA_IFACE_FUNCTION(I,R,N,A,K,F,C,RD,RA,RF,...) \
    CMETA_PP_IIF(F)(CMETA_IFACE_FUNCTION_REFLECTED,CMETA_PP_EMPTY)( \
        I,R,N,A,K,F,C,RD,RA,RF,__VA_ARGS__)
#define CMETA_IFACE_FUNCTION_ROW(I,K,R,N,...) \
    CMETA_IFACE_DECODE(CMETA_IFACE_FUNCTION,I,K,R,N,__VA_ARGS__)

#define CMETA_IFACE_FLAGS_result CMETA_INTERFACE_METHOD_NONE
#define CMETA_IFACE_FLAGS_discard CMETA_INTERFACE_METHOD_NONE
#define CMETA_IFACE_FLAGS_destroy CMETA_INTERFACE_METHOD_OWNS_SELF
#define CMETA_IFACE_META_REFS_0(I,N) NULL, NULL
#define CMETA_IFACE_META_REFS_1(I,N) &I##_##N##__function_meta, &I##_##N##__function_abi_meta
#define CMETA_IFACE_META(I,R,N,A,K,F,C,RD,RA,RF,...) \
    { sizeof(cmeta_interface_method_desc), CMETA_PP_STRINGIFY(N), A, \
      CMETA_PP_CAT(CMETA_IFACE_FLAGS_,K), CMETA_PP_CAT(CMETA_IFACE_META_REFS_,F)(I,N) },
#define CMETA_IFACE_META_ROW(I,K,R,N,...) \
    CMETA_IFACE_DECODE(CMETA_IFACE_META,I,K,R,N,__VA_ARGS__)


#define CMETA_INTERFACE(I, METHODS) CMETA_INTERFACE_I(I, METHODS)
#define CMETA_INTERFACE_I(I, METHODS) \
    typedef struct I I; \
    typedef struct I##_vtable I##_vtable; \
    struct I##_vtable { \
        const char *implementation; \
        uint64_t capabilities; \
        METHODS(CMETA_IFACE_VT_ROW, I) \
    }; \
    struct I { void *self; const I##_vtable *vtable; }; \
    METHODS(CMETA_IFACE_PROOF_ROW, I) \
    METHODS(CMETA_IFACE_FUNCTION_ROW, I) \
    CMETA_LOCAL const cmeta_interface_method_desc I##_method_meta[] = { METHODS(CMETA_IFACE_META_ROW, I) }; \
    CMETA_LOCAL const cmeta_interface_desc I##_interface_meta = { \
        sizeof(cmeta_interface_desc), CMETA_PP_STRINGIFY(I), I##_method_meta, \
        sizeof(I##_method_meta)/sizeof(I##_method_meta[0]) \
    }; \
    METHODS(CMETA_IFACE_IMPL_ROW, I) \
    CMETA_INLINE I I##_bind(void *self, const I##_vtable *vtable) { I out = { self, vtable }; return out; } \
    CMETA_INLINE bool I##_valid(const I *self) { \
        return self && self->self && self->vtable METHODS(CMETA_IFACE_VALID_ROW, I); \
    } \
    CMETA_INLINE const char *I##_implementation(const I *self) { return I##_valid(self) && self->vtable->implementation ? self->vtable->implementation : "none"; } \
    CMETA_INLINE uint64_t I##_capabilities(const I *self) { return I##_valid(self) ? self->vtable->capabilities : 0u; } \
    CMETA_INLINE bool I##_has(const I *self, uint64_t capability) { return (I##_capabilities(self) & capability) == capability; } \
    CMETA_INLINE const cmeta_interface_desc *I##_interface(void) { return &I##_interface_meta; } \
    typedef int I##_interface_anchor_t

/* Bind a conventional C implementation to an interface.  Method functions use
 * the interface ABI directly: first parameter is void *self.  Implementations
 * cast self to their concrete state type internally. */
#ifdef __cplusplus
#define CMETA_IMPLEMENTS_CPP_ASSIGN_(member_, value_) value_ member_;
#define CMETA_IMPLEMENTS(I, NAME, CAPS, ...) \
    static constexpr I##_vtable NAME##_make_vtable(void) { \
        I##_vtable value = {}; \
        value.implementation = #NAME; \
        value.capabilities = CMETA_IFACE_U64_CAST(CAPS); \
        CMETA_PP_MAP(CMETA_IMPLEMENTS_CPP_ASSIGN_, value, __VA_ARGS__) \
        return value; \
    } \
    CMETA_LOCAL constexpr I##_vtable NAME##_vtable = NAME##_make_vtable(); \
    static I NAME##_as_##I(void *self) { return I##_bind(self, &NAME##_vtable); } \
    typedef int NAME##_implements_anchor_t
#else
#define CMETA_IMPLEMENTS(I, NAME, CAPS, ...) \
    CMETA_LOCAL const I##_vtable NAME##_vtable = { \
        .implementation = #NAME, \
        .capabilities = CMETA_IFACE_U64_CAST(CAPS), \
        __VA_ARGS__ \
    }; \
    static I NAME##_as_##I(void *self) { return I##_bind(self, &NAME##_vtable); } \
    typedef int NAME##_implements_anchor_t
#endif


/* Natural DSL spellings are the default when the host headers have not already
 * claimed them (notably some Windows/COM environments define `interface`).
 * Framework/internal code should use the collision-safe CMETA_* spellings. */
#ifndef CMETA_NO_NATURAL_INTERFACE_NAMES
#  ifndef interface
#    define interface(...) CMETA_INTERFACE(__VA_ARGS__)
#  endif
#  ifndef implements
#    define implements(...) CMETA_IMPLEMENTS(__VA_ARGS__)
#  endif
#endif

#endif
