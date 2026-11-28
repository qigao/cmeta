#ifndef CMETA_FUNCTION_H
#define CMETA_FUNCTION_H

#include <cmeta/cmeta.h>
#include <cmeta/abi.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t cmeta_param_flags;
typedef uint32_t cmeta_result_flags;

enum {
    /* Zero preserves an explicit "direction not specified" state. */
    CMETA_PARAM_UNKNOWN = 0u,
    CMETA_PARAM_IN = 1u << 0,
    CMETA_PARAM_OUT = 1u << 1,
    CMETA_PARAM_INOUT = CMETA_PARAM_IN | CMETA_PARAM_OUT,
    CMETA_PARAM_NULLABLE = 1u << 2,
    CMETA_PARAM_BORROWED = 1u << 3,
    CMETA_PARAM_OWNED = 1u << 4,
    /* Compile/generation-time receiver identity. This does not change the C ABI
     * or imply runtime dispatch; the receiver remains an ordinary parameter. */
    CMETA_PARAM_RECEIVER = 1u << 5,
    CMETA_PARAM_DIRECTION_MASK = CMETA_PARAM_IN | CMETA_PARAM_OUT,
    CMETA_PARAM_OWNERSHIP_MASK = CMETA_PARAM_BORROWED | CMETA_PARAM_OWNED,
    CMETA_PARAM_FLAG_MASK = CMETA_PARAM_DIRECTION_MASK | CMETA_PARAM_NULLABLE |
                            CMETA_PARAM_OWNERSHIP_MASK | CMETA_PARAM_RECEIVER
};

enum {
    /* Zero preserves an explicit "result semantics not specified" state. */
    CMETA_RESULT_UNKNOWN = 0u,
    CMETA_RESULT_NULLABLE = 1u << 0,
    CMETA_RESULT_VALUE = 1u << 1,
    CMETA_RESULT_BORROWED = 1u << 2,
    CMETA_RESULT_SHARED = 1u << 3,
    CMETA_RESULT_OWNED = 1u << 4,
    CMETA_RESULT_CLASS_MASK = CMETA_RESULT_VALUE | CMETA_RESULT_BORROWED |
                              CMETA_RESULT_SHARED | CMETA_RESULT_OWNED,
    CMETA_RESULT_FLAG_MASK = CMETA_RESULT_NULLABLE | CMETA_RESULT_CLASS_MASK
};

/* All descriptor views are borrowed. Names, arrays, types, traits and their
 * callbacks must remain alive until the last consumer is finished. Copying a
 * descriptor does not retain its provider. A plugin owner must keep its module
 * loaded through all descriptor, interface and callable uses (including copies
 * in graphs/plans), and drain them before unloading. Validators require live,
 * ABI-compatible storage; they cannot validate expired or foreign-ABI pointers.
 * See CMETA_REFLECTION_ABI_VERSION before exchanging descriptors across DSOs. */
typedef struct cmeta_param_desc {
    size_t size;
    const char *name;
    const cmeta_type_desc *type;
    cmeta_param_flags flags;
} cmeta_param_desc;

typedef struct cmeta_function_desc {
    size_t size;
    const char *name;
    const cmeta_type_desc *return_type;
    const cmeta_param_desc *params;
    size_t param_count;
    cmeta_effects effects;
    cmeta_properties properties;
    cmeta_result_flags result_flags;
} cmeta_function_desc;

typedef struct cmeta_function_abi_desc {
    size_t size;
    const cmeta_function_desc *function;
    cmeta_abi_carrier return_carrier;
    const cmeta_abi_carrier *param_carriers;
    size_t param_count;
} cmeta_function_abi_desc;

bool cmeta_param_desc_valid(const cmeta_param_desc *desc);
bool cmeta_function_desc_valid(const cmeta_function_desc *desc);
bool cmeta_function_abi_desc_valid(const cmeta_function_abi_desc *desc);
bool cmeta_function_desc_equal(const cmeta_function_desc *left,
                               const cmeta_function_desc *right);
bool cmeta_function_abi_desc_equal(const cmeta_function_abi_desc *left,
                                   const cmeta_function_abi_desc *right);
/** Replacement compatibility ignores function/parameter names, retaining exact
 * type, explicit ABI carrier, ownership, effect and property contracts.
 * Invalid or unspecified ABI metadata is rejected. O(n^2) including validation. */
bool cmeta_function_abi_contract_compatible(const cmeta_function_abi_desc *expected,
                                          const cmeta_function_abi_desc *candidate);

cmeta_abi_carrier
cmeta_function_param_abi(const cmeta_function_abi_desc *desc, size_t index);

static inline bool
cmeta_param_direction_known(const cmeta_param_desc *desc) {
    return desc != NULL &&
           (desc->flags & CMETA_PARAM_DIRECTION_MASK) != CMETA_PARAM_UNKNOWN;
}

const cmeta_param_desc *
cmeta_function_param(const cmeta_function_desc *desc, size_t index);

const cmeta_param_desc *
cmeta_function_find_param(const cmeta_function_desc *desc, const char *name);

/* Returns parameter zero when it is explicitly marked as the receiver.
 * Receiver metadata is descriptive only and is intended for compile/generation
 * lowering such as obj.method(x) -> C_symbol(&obj, x). */
const cmeta_param_desc *
cmeta_function_receiver(const cmeta_function_desc *desc);

/** Validate a canonical function whose first parameter is a pointer receiver
 * with a valid pointee type. No operation set or execution provider is needed. */
bool cmeta_function_receiver_valid(const cmeta_function_desc *function);

/** Validate removal of exactly the receiver parameter. Preserve return type,
 * result ownership/nullability, effects, properties and each remaining named
 * parameter's type/flags. The projected function name may differ. Both inputs
 * are borrowed live descriptors. O(n^2) including full descriptor validation. */
bool cmeta_function_receiver_projection_valid(
    const cmeta_function_desc *function,
    const cmeta_function_desc *projected);

/** Validate an order-preserving parameter projection. bound[i] removes source
 * parameter i; remaining parameter names/types/flags and the complete result
 * contract must match. Arrays are borrowed and count must equal source arity. */
bool cmeta_function_projection_valid(const cmeta_function_abi_desc *source,
    const cmeta_function_abi_desc *projected, const bool *bound, size_t count);

#ifdef __cplusplus
}
#endif

#include <cmeta/pp.h>
#include <cmeta/type_select.h>

#ifdef __cplusplus
#define CMETA_FUNCTION_PARAM_FLAGS_CAST(value) \
    static_cast<cmeta_param_flags>(value)
#define CMETA_FUNCTION_RESULT_FLAGS_CAST(value) \
    static_cast<cmeta_result_flags>(value)
#else
#define CMETA_FUNCTION_PARAM_FLAGS_CAST(value) \
    ((cmeta_param_flags)(value))
#define CMETA_FUNCTION_RESULT_FLAGS_CAST(value) \
    ((cmeta_result_flags)(value))
#endif

/*
 * Parameter semantic/ABI rows and canonical metadata emitters are shared by
 * C/C++ explicit declarations and interface reflection. Inferred descriptors
 * (three-field parameters and inferred returns) require C11 _Generic; C++
 * declarations use explicit descriptor/ABI rows.
 */
/* Normalize source syntax once per projection. Admission stays source-aware:
 * only the three-field form is inferred; four fields never infer an ABI. */
#define CMETA_FUNCTION_PARAM_NORMALIZE_3(type,name,flags) \
    (type,name,flags,CMETA_TYPEOF(type),CMETA_ABI_SCALAR)
#define CMETA_FUNCTION_PARAM_NORMALIZE_4(type,name,flags,descriptor) \
    (type,name,flags,descriptor,CMETA_ABI_UNSPECIFIED)
#define CMETA_FUNCTION_PARAM_NORMALIZE_5(type,name,flags,descriptor,carrier) \
    (type,name,flags,descriptor,carrier)
#define CMETA_FUNCTION_PARAM_NORMALIZE_I(...) \
    CMETA_PP_OVERLOAD(CMETA_FUNCTION_PARAM_NORMALIZE_,__VA_ARGS__)(__VA_ARGS__)
#define CMETA_FUNCTION_PARAM_NORMALIZE(row) CMETA_FUNCTION_PARAM_NORMALIZE_I row
#define CMETA_FUNCTION_PARAM_TYPE(row) CMETA_PP_TUPLE_GET_0(row)
#define CMETA_FUNCTION_PARAM_NAME(row) CMETA_PP_TUPLE_GET_1(row)
#define CMETA_FUNCTION_PARAM_FLAGS(row) CMETA_PP_TUPLE_GET_2(row)
#define CMETA_FUNCTION_PARAM_DESCRIPTOR(row) \
    CMETA_PP_TUPLE_GET_3(CMETA_FUNCTION_PARAM_NORMALIZE(row))
#define CMETA_FUNCTION_PARAM_CARRIER(row) \
    CMETA_PP_TUPLE_GET_4(CMETA_FUNCTION_PARAM_NORMALIZE(row))
#define CMETA_FUNCTION_PARAM_META_APPLY(row) \
    { sizeof(cmeta_param_desc), CMETA_PP_STRINGIFY(CMETA_FUNCTION_PARAM_NAME(row)), \
      (CMETA_FUNCTION_PARAM_DESCRIPTOR(row)), \
      CMETA_FUNCTION_PARAM_FLAGS_CAST(CMETA_FUNCTION_PARAM_FLAGS(row)) },
#define CMETA_FUNCTION_PARAM_META(row, ignored) \
    CMETA_FUNCTION_PARAM_META_APPLY(row)

#define CMETA_FUNCTION_PARAM_ABI_APPLY(row) \
    (CMETA_FUNCTION_PARAM_CARRIER(row))
#define CMETA_FUNCTION_PARAM_ABI_ROW(row, ignored) \
    CMETA_FUNCTION_PARAM_ABI_APPLY(row),

#define CMETA_FUNCTION_PARAM_PROOF(index,row,ignored) \
    CMETA_STATIC_ASSERT(((CMETA_FUNCTION_PARAM_FLAGS(row)) & ~CMETA_PARAM_FLAG_MASK) == 0 && \
        ((CMETA_FUNCTION_PARAM_FLAGS(row)) & CMETA_PARAM_OWNERSHIP_MASK) != CMETA_PARAM_OWNERSHIP_MASK && \
        (index == 0 || ((CMETA_FUNCTION_PARAM_FLAGS(row)) & CMETA_PARAM_RECEIVER) == 0), \
        "CMeta Function parameter flags or receiver position invalid");
#define CMETA_FUNCTION_RESULT_PROOF(flags) \
    CMETA_STATIC_ASSERT(((flags) & ~CMETA_RESULT_FLAG_MASK) == 0 && \
        (((flags) & CMETA_RESULT_CLASS_MASK) & (((flags) & CMETA_RESULT_CLASS_MASK) - 1u)) == 0, \
        "CMeta Function result flags invalid");

/*
 * Canonical metadata emitters reused by FunctionDecl and interface-method
 * reflection. 'symbol' is the C identifier used for TU-local storage while
 * display_name is the stable semantic name exposed by the descriptor.
 */
#define CMETA_FUNCTION_METADATA_AS_ABI_RESULT( \
    symbol, display_name, contract, return_desc, return_abi_carrier, \
    result_flags, ...) \
    CMETA_FUNCTION_RESULT_PROOF(result_flags) \
    CMETA_PP_FOR_EACH_I(CMETA_FUNCTION_PARAM_PROOF,~,__VA_ARGS__) \
    CMETA_LOCAL const cmeta_param_desc symbol##__function_params[] = { \
        CMETA_PP_FOR_EACH_A(CMETA_FUNCTION_PARAM_META, ~, __VA_ARGS__) \
    }; \
    CMETA_LOCAL const cmeta_function_desc symbol##__function_meta = { \
        sizeof(cmeta_function_desc), (display_name), (return_desc), \
        symbol##__function_params, CMETA_PP_NARG(__VA_ARGS__), \
        CMETA_CONTRACT_EFFECTS(contract), CMETA_CONTRACT_PROPERTIES(contract), \
        CMETA_FUNCTION_RESULT_FLAGS_CAST(result_flags) \
    }; \
    CMETA_LOCAL const cmeta_abi_carrier symbol##__function_param_abi[] = { \
        CMETA_PP_FOR_EACH_A(CMETA_FUNCTION_PARAM_ABI_ROW, ~, __VA_ARGS__) \
    }; \
    CMETA_LOCAL const cmeta_function_abi_desc symbol##__function_abi_meta = { \
        sizeof(cmeta_function_abi_desc), &symbol##__function_meta, \
        (return_abi_carrier), symbol##__function_param_abi, \
        CMETA_PP_NARG(__VA_ARGS__) \
    }

#define CMETA_FUNCTION_METADATA_AS_ABI( \
    symbol, display_name, contract, return_desc, return_abi_carrier, ...) \
    CMETA_FUNCTION_METADATA_AS_ABI_RESULT( \
        symbol, display_name, contract, return_desc, return_abi_carrier, \
        CMETA_RESULT_UNKNOWN, __VA_ARGS__)

#define CMETA_FUNCTION0_METADATA_AS_ABI_RESULT( \
    symbol, display_name, contract, return_desc, return_abi_carrier, \
    result_flags) \
    CMETA_FUNCTION_RESULT_PROOF(result_flags) \
    CMETA_LOCAL const cmeta_function_desc symbol##__function_meta = { \
        sizeof(cmeta_function_desc), (display_name), (return_desc), NULL, 0u, \
        CMETA_CONTRACT_EFFECTS(contract), CMETA_CONTRACT_PROPERTIES(contract), \
        CMETA_FUNCTION_RESULT_FLAGS_CAST(result_flags) \
    }; \
    CMETA_LOCAL const cmeta_function_abi_desc symbol##__function_abi_meta = { \
        sizeof(cmeta_function_abi_desc), &symbol##__function_meta, \
        (return_abi_carrier), NULL, 0u \
    }

#define CMETA_FUNCTION0_METADATA_AS_ABI( \
    symbol, display_name, contract, return_desc, return_abi_carrier) \
    CMETA_FUNCTION0_METADATA_AS_ABI_RESULT( \
        symbol, display_name, contract, return_desc, return_abi_carrier, \
        CMETA_RESULT_UNKNOWN)

/*
 * Function reflection is descriptive only. The declaration macros below emit
 * a normal C prototype plus immutable TU-local metadata and a static-inline
 * descriptor getter. They do not generate an erased ABI invocation path.
 *
 * Parameter rows are:
 *
 *   (type, name, flags)
 *   (type, name, flags, descriptor)
 *   (type, name, flags, descriptor, abi_carrier)
 *
 * The three-field form requires a registered scalar type and carries scalar
 * ABI metadata. Non-scalar or unregistered types require an explicit descriptor.
 * The four-field form remains the compatibility escape hatch for an explicit
 * descriptor and leaves ABI carrier unspecified.
 * The five-field form adds explicit ABI/FFI carrier metadata without changing
 * cmeta_param_desc/cmeta_function_desc binary layout.
 */

#define CMETA_FUNCTION_RETURN_TYPEOF(type) \
    _Generic((type *)0, \
        void *: &cmeta_type_void, \
        default: CMETA_TYPEOF(type))

/* Use the registered kind, not sizeof or spelling: an object can have the
 * same size as a scalar, and typedefs must retain their underlying contract. */
#define CMETA_FUNCTION_SCALAR_ASSOC(row, ignored) \
    CMETA_TYPE_CTYPE(row) *: (CMETA_TYPE_KIND(row) == CMETA_T_BOOL || \
                            CMETA_TYPE_KIND(row) == CMETA_T_INTEGER || \
                            CMETA_TYPE_KIND(row) == CMETA_T_FLOAT),
#define CMETA_FUNCTION_PARAM_ADMIT_3(type, name, flags) \
    _Static_assert(_Generic((type *)0, \
        CMETA_PP_FOR_EACH_B(CMETA_FUNCTION_SCALAR_ASSOC, ~, CMETA_KNOWN_TYPE_LIST) \
        default: 0), \
        "CMeta three-field parameter " #name \
        " requires a registered scalar; use an explicit descriptor and ABI carrier");
#define CMETA_FUNCTION_PARAM_ADMIT_4(type, name, flags, descriptor)
#define CMETA_FUNCTION_PARAM_ADMIT_5(type, name, flags, descriptor, carrier)
#define CMETA_FUNCTION_PARAM_ADMIT_I(...) \
    CMETA_PP_OVERLOAD(CMETA_FUNCTION_PARAM_ADMIT_,__VA_ARGS__)(__VA_ARGS__)
#define CMETA_FUNCTION_PARAM_ADMIT(row, ignored) CMETA_FUNCTION_PARAM_ADMIT_I row
#define CMETA_FUNCTION_RETURN_ADMIT(type) \
    _Static_assert(_Generic((type *)0, \
        void *: CMETA_FUNCTION_RETURN_IS_VOID_(type), \
        CMETA_PP_FOR_EACH_B(CMETA_FUNCTION_SCALAR_ASSOC, ~, CMETA_KNOWN_TYPE_LIST) \
        default: 0), \
        "CMeta inferred return requires literal void or a registered scalar; use FunctionDeclAsAbi or Function0DeclAsAbi");

#define CMETA_FUNCTION_RETURN_IS_VOID_(type) CMETA_PP_IS_VOID(type)
#define CMETA_FUNCTION_RETURN_ABI_0 CMETA_ABI_SCALAR
#define CMETA_FUNCTION_RETURN_ABI_1 CMETA_ABI_VOID
#define CMETA_FUNCTION_RETURN_ABI(type) \
    CMETA_PP_CAT(CMETA_FUNCTION_RETURN_ABI_, \
                 CMETA_FUNCTION_RETURN_IS_VOID_(type))


#define CMETA_FUNCTION_PARAM_DECL_APPLY(row) \
    CMETA_FUNCTION_PARAM_TYPE(row) CMETA_FUNCTION_PARAM_NAME(row)
#define CMETA_FUNCTION_PARAM_DECL(index, row, ignored) \
    CMETA_PP_SEP_COMMA(index) \
    CMETA_FUNCTION_PARAM_DECL_APPLY(row)

/*
 * Optional declaration consumers may replay the exact FunctionDecl schema at
 * preprocessing time (for example to generate a test-only exact-ABI adapter).
 * The default is empty and CMeta never depends on those consumers.
 *
 * Extensions expand before the final declaration anchor, so they must emit
 * complete declarations/definitions of their own.
 */
#ifndef CMETA_FUNCTION_DECL_EXTENSION
#define CMETA_FUNCTION_DECL_EXTENSION(contract, return_type, return_desc, name, ...)
#endif

#ifndef CMETA_FUNCTION0_DECL_EXTENSION
#define CMETA_FUNCTION0_DECL_EXTENSION(contract, return_type, return_desc, name)
#endif

#ifndef CMETA_FUNCTION_DECL_ABI_EXTENSION
#define CMETA_FUNCTION_DECL_ABI_EXTENSION( \
    contract, return_type, return_desc, return_abi_carrier, name, ...)
#endif

#ifndef CMETA_FUNCTION0_DECL_ABI_EXTENSION
#define CMETA_FUNCTION0_DECL_ABI_EXTENSION( \
    contract, return_type, return_desc, return_abi_carrier, name)
#endif

#define CMETA_FUNCTION_DECL_AS_ABI_RESULT( \
    contract, return_type, return_desc, return_abi_carrier, result_flags, \
    name, ...) \
    CMETA_STATIC_ASSERT((return_abi_carrier) == CMETA_ABI_UNSPECIFIED || \
        CMETA_TYPE_IS_VOID(return_type) == ((return_abi_carrier) == CMETA_ABI_VOID), \
        "CMeta Function native result carrier mismatch"); \
    CMETA_PP_FOR_EACH_A(CMETA_FUNCTION_PARAM_ADMIT, ~, __VA_ARGS__) \
    return_type name( \
        CMETA_PP_FOR_EACH_I(CMETA_FUNCTION_PARAM_DECL, ~, __VA_ARGS__)); \
    typedef return_type (*name##_function_type)( \
        CMETA_PP_FOR_EACH_I(CMETA_FUNCTION_PARAM_DECL, ~, __VA_ARGS__)); \
    CMETA_FUNCTION_METADATA_AS_ABI_RESULT( \
        name, #name, contract, return_desc, return_abi_carrier, \
        result_flags, __VA_ARGS__); \
    CMETA_INLINE const cmeta_function_desc *name##_function(void) { \
        return &name##__function_meta; \
    } \
    CMETA_INLINE const cmeta_function_abi_desc *name##_function_abi(void) { \
        return &name##__function_abi_meta; \
    } \
    CMETA_FUNCTION_DECL_EXTENSION( \
        contract, return_type, return_desc, name, __VA_ARGS__) \
    CMETA_FUNCTION_DECL_ABI_EXTENSION( \
        contract, return_type, return_desc, return_abi_carrier, name, __VA_ARGS__) \
    typedef char name##__function_declaration_complete[1]

#define CMETA_FUNCTION_DECL_AS_ABI( \
    contract, return_type, return_desc, return_abi_carrier, name, ...) \
    CMETA_FUNCTION_DECL_AS_ABI_RESULT( \
        contract, return_type, return_desc, return_abi_carrier, \
        CMETA_RESULT_UNKNOWN, name, __VA_ARGS__)

#define CMETA_FUNCTION_DECL_AS_RESULT( \
    contract, return_type, return_desc, result_flags, name, ...) \
    CMETA_FUNCTION_DECL_AS_ABI_RESULT( \
        contract, return_type, return_desc, CMETA_ABI_UNSPECIFIED, \
        result_flags, name, __VA_ARGS__)

#define CMETA_FUNCTION_DECL_AS(contract, return_type, return_desc, name, ...) \
    CMETA_FUNCTION_DECL_AS_ABI( \
        contract, return_type, return_desc, CMETA_ABI_UNSPECIFIED, \
        name, __VA_ARGS__)

#define CMETA_FUNCTION_DECL_RESULT( \
    contract, return_type, result_flags, name, ...) \
    CMETA_FUNCTION_RETURN_ADMIT(return_type) \
    CMETA_FUNCTION_DECL_AS_ABI_RESULT( \
        contract, return_type, CMETA_FUNCTION_RETURN_TYPEOF(return_type), \
        CMETA_FUNCTION_RETURN_ABI(return_type), result_flags, name, __VA_ARGS__)

#define CMETA_FUNCTION_DECL(contract, return_type, name, ...) \
    CMETA_FUNCTION_RETURN_ADMIT(return_type) \
    CMETA_FUNCTION_DECL_AS_ABI( \
        contract, return_type, CMETA_FUNCTION_RETURN_TYPEOF(return_type), \
        CMETA_FUNCTION_RETURN_ABI(return_type), name, __VA_ARGS__)

#define CMETA_FUNCTION0_DECL_AS_ABI_RESULT( \
    contract, return_type, return_desc, return_abi_carrier, result_flags, name) \
    CMETA_STATIC_ASSERT((return_abi_carrier) == CMETA_ABI_UNSPECIFIED || \
        CMETA_TYPE_IS_VOID(return_type) == ((return_abi_carrier) == CMETA_ABI_VOID), \
        "CMeta Function native result carrier mismatch"); \
    return_type name(void); \
    typedef return_type (*name##_function_type)(void); \
    CMETA_FUNCTION0_METADATA_AS_ABI_RESULT( \
        name, #name, contract, return_desc, return_abi_carrier, result_flags); \
    CMETA_INLINE const cmeta_function_desc *name##_function(void) { \
        return &name##__function_meta; \
    } \
    CMETA_INLINE const cmeta_function_abi_desc *name##_function_abi(void) { \
        return &name##__function_abi_meta; \
    } \
    CMETA_FUNCTION0_DECL_EXTENSION(contract, return_type, return_desc, name) \
    CMETA_FUNCTION0_DECL_ABI_EXTENSION( \
        contract, return_type, return_desc, return_abi_carrier, name) \
    typedef char name##__function_declaration_complete[1]

#define CMETA_FUNCTION0_DECL_AS_ABI( \
    contract, return_type, return_desc, return_abi_carrier, name) \
    CMETA_FUNCTION0_DECL_AS_ABI_RESULT( \
        contract, return_type, return_desc, return_abi_carrier, \
        CMETA_RESULT_UNKNOWN, name)

#define CMETA_FUNCTION0_DECL_AS_RESULT( \
    contract, return_type, return_desc, result_flags, name) \
    CMETA_FUNCTION0_DECL_AS_ABI_RESULT( \
        contract, return_type, return_desc, CMETA_ABI_UNSPECIFIED, \
        result_flags, name)

#define CMETA_FUNCTION0_DECL_AS(contract, return_type, return_desc, name) \
    CMETA_FUNCTION0_DECL_AS_ABI( \
        contract, return_type, return_desc, CMETA_ABI_UNSPECIFIED, name)

#define CMETA_FUNCTION0_DECL_RESULT( \
    contract, return_type, result_flags, name) \
    CMETA_FUNCTION_RETURN_ADMIT(return_type) \
    CMETA_FUNCTION0_DECL_AS_ABI_RESULT( \
        contract, return_type, CMETA_FUNCTION_RETURN_TYPEOF(return_type), \
        CMETA_FUNCTION_RETURN_ABI(return_type), result_flags, name)

#define CMETA_FUNCTION0_DECL(contract, return_type, name) \
    CMETA_FUNCTION_RETURN_ADMIT(return_type) \
    CMETA_FUNCTION0_DECL_AS_ABI( \
        contract, return_type, CMETA_FUNCTION_RETURN_TYPEOF(return_type), \
        CMETA_FUNCTION_RETURN_ABI(return_type), name)

/* Static discovery needs the canonical symbol as a constant initializer. */
#define CMETA_FUNCTION_META(name) (&name##__function_meta)
#define CMETA_FUNCTION_ABI(name) (&name##__function_abi_meta)

#ifndef FunctionDecl
#define FunctionDecl(...) CMETA_FUNCTION_DECL(__VA_ARGS__)
#endif

#ifndef FunctionDeclAs
#define FunctionDeclAs(...) CMETA_FUNCTION_DECL_AS(__VA_ARGS__)
#endif

#ifndef FunctionDeclAsAbi
#define FunctionDeclAsAbi(...) CMETA_FUNCTION_DECL_AS_ABI(__VA_ARGS__)
#endif

#ifndef FunctionDeclResult
#define FunctionDeclResult(...) CMETA_FUNCTION_DECL_RESULT(__VA_ARGS__)
#endif

#ifndef FunctionDeclAsResult
#define FunctionDeclAsResult(...) CMETA_FUNCTION_DECL_AS_RESULT(__VA_ARGS__)
#endif

#ifndef FunctionDeclAsAbiResult
#define FunctionDeclAsAbiResult(...) CMETA_FUNCTION_DECL_AS_ABI_RESULT(__VA_ARGS__)
#endif

#ifndef Function0Decl
#define Function0Decl(...) CMETA_FUNCTION0_DECL(__VA_ARGS__)
#endif

#ifndef Function0DeclAs
#define Function0DeclAs(...) CMETA_FUNCTION0_DECL_AS(__VA_ARGS__)
#endif

#ifndef Function0DeclAsAbi
#define Function0DeclAsAbi(...) CMETA_FUNCTION0_DECL_AS_ABI(__VA_ARGS__)
#endif

#ifndef Function0DeclResult
#define Function0DeclResult(...) CMETA_FUNCTION0_DECL_RESULT(__VA_ARGS__)
#endif

#ifndef Function0DeclAsResult
#define Function0DeclAsResult(...) CMETA_FUNCTION0_DECL_AS_RESULT(__VA_ARGS__)
#endif

#ifndef Function0DeclAsAbiResult
#define Function0DeclAsAbiResult(...) CMETA_FUNCTION0_DECL_AS_ABI_RESULT(__VA_ARGS__)
#endif

#ifndef FunctionMeta
#define FunctionMeta(name) CMETA_FUNCTION_META(name)
#endif

#ifndef FunctionAbi
#define FunctionAbi(name) CMETA_FUNCTION_ABI(name)
#endif

/*
 * Preserve the historical transitive interface surface only after the complete
 * function reflection types/macros above are visible. This order keeps direct
 * <cmeta/function.h> and direct <cmeta/interface.h> inclusion acyclic.
 */
#if !defined(CMETA_META_INTERFACE_H)
#include <cmeta/interface.h>
#endif

#endif /* CMETA_FUNCTION_H */
