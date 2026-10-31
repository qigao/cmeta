# CMeta Language Reference

CMeta is a pragmatic ordinary-C metaprogramming and Reflection layer built on
strict C11. It combines finite declaration macros, compile-time schema replay,
metadata descriptors, lifecycle helpers, and ordinary C runtime protocols.

CMeta is deliberately **not** a replacement for C++, Rust, or a separate source
language. Public application forms must remain valid ordinary C macros,
declarations, inline helpers, or functions.

This document is the authoritative language vocabulary. The public surface is
split into four categories so application syntax does not get mixed with
framework generation or runtime APIs.

---

## 1. Application DSL

These declarations are intended for normal application code.

### `cmeta_struct(...)` / `cmeta_field(...)`

Declares a C struct and reflection metadata in one statement.

```c
cmeta_struct(User,
    cmeta_field(int, id)
    cmeta_field(double, score)
    cmeta_field(const char *, name)
);
```

Fields use `cmeta_field(type, name)` without separating commas. The same
declaration generates the ordinary C layout and static Reflection metadata.
Tuple rows remain internal Schema/Replay input, not canonical application syntax.
The declaration provides the concrete C type plus field metadata helpers such
as:

```c
StructMeta(User);
FieldCount(User);
FieldMeta(User, 0);
FieldFind(User, "score");
```

Use `cmeta_struct(...)` when a type benefits from structural metadata. Ordinary C
`struct` remains valid and should be preferred when metadata is unnecessary.

### `cmeta_enum(...)`

Declares an enum and immutable reflection metadata.

Auto-valued rows:

```c
cmeta_enum(State,
    (READY,   "ready"),
    (RUNNING, "running"),
    (DONE,    "done")
);
```

Explicit stable values:

```c
cmeta_enum(HttpStatus,
    (HTTP_OK,        200, "ok"),
    (HTTP_NOT_FOUND, 404, "not_found")
);
```

Rows are either `(symbol, text)` or `(symbol, value, text)`. There is no
`Item(...)` wrapper.

Generated helpers include typed string/symbol conversion and parsing:

```c
State_to_string(value);
State_to_symbol(value);
State_from_string(text, &out);

EnumMeta(State);
EnumString(State, value);
EnumSymbol(State, value);
EnumParse(State, text, &out);
```

### `cmeta_traits(...)`

Declares callable type capabilities from tagged rows.

```c
cmeta_traits(User,
    (equal, user_equal),
    (hash, user_hash),
    (compare, user_compare),
    (copy, user_copy),
    (move, user_move),
    (destroy, user_destroy)
);
```

Supported callable tags are:

```text
equal
hash
compare
copy
move
destroy
```

CMeta derives both the capability flags and the matching function slots from the
same rows. Duplicate, unknown, or malformed rows are compile-time errors.

`TRIVIAL_COPY` and `TRIVIAL_DESTROY` are descriptor properties, not inferred
callable traits.

### Compile-time capabilities, tagged variants and flags

`cmeta_trait(Capability, callback)` names the existing tagged trait row.
`cmeta_has_trait(Name, Capability)` and `cmeta_require_trait(Name, Capability)`
query/check the declaration at compile time; `cmeta_require_field(Owner, member,
Type)` checks exact native field type without evaluating an object.

`cmeta_variant(Name, "stable.Name", cases...)` takes consecutive
`cmeta_case(Case, nonzero_int_tag, PayloadType, &payload_data)` rows and generates
checked typed operations over one inline tag/union and canonical DataDesc.
`cmeta_match(pointer)` is ordinary switch with `case Name_Case:` labels.
`cmeta_flags(Name, "stable.Name", rows...)` takes consecutive
`cmeta_flag(Symbol, uint64_bits, "text")` rows and derives distinct typed values
and one canonical unsigned64 enum domain. Both finite declarations accept 1–16
rows; traits still use comma-separated rows.

See [capability contracts and executable examples](CAPABILITIES.md) for callback
signatures, provider admission, ownership, failure cleanup and compatibility.

### Schema-driven Data selection

`<cmeta/data_select.h>` exposes `cmeta_data_of(pointer)` for builtin types and
`cmeta_data_of_in(pointer, schema)` for an explicit `Schema(M, (NativeType,
descriptor_expression), ...)`. A row names an unqualified native type and an
expression of exactly `const cmeta_data_desc *`; its descriptor remains the
provider's canonical object. No registry, copied metadata or lifetime retention
is introduced. C11 uses `_Generic`; C++17 uses exact pointer type matching.

The pointer is an unevaluated type witness and may be null. Mutable and const
pointees select the same descriptor. Unknown/volatile pointers, duplicate
compatible types (including typedef aliases), and wrong descriptor expression
types are compile errors. Only the selected descriptor expression executes,
exactly once. Schema expressions must be usable from static context in C++;
provider accessors are the portable way to express computed descriptors.
This proves native selection and descriptor pointer type, not arbitrary foreign
descriptor validity: provider/Plugin admission remains mandatory at its boundary.

Existing `CMETA_DATAOF(Type)` and `CMETA_DATAOF_OR(Type, fallback)` keep their
historical unknown-type behavior. Builtin associations now replay the same
`CMETA_BUILTIN_DATA_SCHEMA` used by the strict pointer frontend. Fixed-width
aliases still use `cmeta_data_integer_width`, avoiding duplicate associations.

### Structured scope

`cmeta_scope(status, autos, body)` owns 1 through 16 explicit
`(Type, local_name)` rows separated by commas inside `cmeta_autos(...)`.
The wrapper keeps the tuple list in one preprocessor argument and has no
runtime representation. `CMETA_PP_UNIQUE` supplies the internal scope identifier;
the frontend requires `CMETA_HAS_COUNTER`, supported by the qualified GCC, Clang
and MSVC backends. Each type exposes a native-typed `Type_cmeta_lifecycle`
accessor to canonical concrete `construct_ops`. CSTL generates it together with
the type's DataDesc. The body is one ISO C expression returning
`cmeta_status`, usually a typed function call borrowing the local values:

```c
#include <cstl/typed.h>

cmeta_type(List, ScopeList, int);

static cmeta_status fill_values(ScopeList *values) {
    if (ScopeList_init(values, 2u) != STL_OK) return CMETA_CALLBACK_ERROR;
    if (ScopeList_add(values, 11) != STL_OK) return CMETA_CALLBACK_ERROR;
    return CMETA_OK;
}

int main(void) {
    cmeta_status status;
    cmeta_scope(status,
        cmeta_autos((ScopeList, values)),
        cmeta_body(fill_values(&values)));
    return status == CMETA_OK ? 0 : 1;
}
```

Link this container example with `CMeta::CSTL`. Body functions can take typed
context parameters as well as resource pointers; no closure or erased callback
registry is needed. Resources belong to the generated scope, and borrowing ends
when the body returns. They must not escape through a saved pointer or suspended
operation. Move ownership into caller-owned storage explicitly with `cmeta_move`
when a value must outlive the scope; moved-from locals still receive cleanup.

The ordinary-C reference expansion has this control flow:

```text
declare zero storage and live flags
bind each resource's canonical concrete construct ops
initialize resources in declaration order
call body and store its returned status
cleanup:
    restore live resources to semantic zero in reverse order
continue with status
```

An initialization error restores the failed partial value once, skips the body,
then cleans earlier initialized resources. A body error follows the same cleanup
path. The returned body status replaces `status`; cleanup does not overwrite it.
The same cached canonical ops supply initialization and destruction. Static
entry checks the accessor's exact native function type at compile time and
borrows its constant-address ops without runtime descriptor admission.

For hand-written or runtime-selected descriptors, use
`cmeta_scope_checked(status, autos, body)` with the same tuple/body syntax.
Each Type must expose `Type_cmeta_data()`; `cmeta_lifecycle_bind()` checks the
descriptor, ops ABI, callback presence, semantic storage identity and native
size/alignment before construction. Missing ops return `CMETA_TRAIT_MISSING`,
malformed ABI/callbacks return `CMETA_INVALID_ARGUMENT`, and incompatible native
storage returns `CMETA_TYPE_MISMATCH`. Admission failure calls no lifecycle
callback for that resource. Static and checked entry share cleanup lowering;
neither silently selects the other.

A local declaration owner may expose its existing canonical ops using
`CMETA_DEFINE_STATIC_LIFECYCLE(Type, immutable_ops_object)`. The object must have
static storage duration and be the exact ops referenced by its DataDesc. The
generated accessor takes `const Type *` as a type witness (NULL is allowed),
never dereferences or retains it, and returns the borrowed ops pointer. The
declaration owner remains responsible for callback/metadata semantics; this
macro must not be used to bypass foreign/Plugin admission. Details and migration
are in [lifecycle lowering](LIFECYCLE_LOWERING.md).

Scope setup/cleanup is O(N) time and O(N) bounded automatic storage for N resource
rows, excluding provider-owned payloads and checked descriptor validation.
No heap allocation, source lowerer,
cleanup attribute, SEH or assembly backend is required. DataDesc layout and the
generic checked runtime lifecycle APIs retain their existing contracts.

**Source migration (#980):** remove the first, caller-supplied scope token and
replace adjacent `cmeta_auto(Type, name)` entries with comma-separated
`(Type, name)` tuples. The old four-argument form and leading-comma entry macro
are removed. Generated identifiers are unique even for two expansions on the
same source line. The common PP tuple replay replaces the sentinel/drop layer;
resource ownership, error status, partial rollback and LIFO cleanup are unchanged.
Ordinary two-field rows automatically select lowering from canonical lifecycle
facts. Trivial rows have no callbacks/ops/live state; managed nofail rows have no
partial-init state. Fallible rows use the caller's status and nested control flow
for rollback, without per-value live flags or cached static ops. Checked scopes
retain borrowed ops only after full admission. Explicit `trivial`/nofail spellings
are assertions, not required application annotations. See
[lifecycle classification and binding trust](LIFECYCLE_LOWERING.md).
Types that only expose a DataDesc accessor must explicitly use
`cmeta_scope_checked` or publish an authoritative static lifecycle declaration.
No descriptor layout or binary ABI changes follow from this source migration.

**Migration and design decision (#920/#929):** the earlier statement-block
`cmeta_body(...)` could let native exits bypass cleanup. Pure C macros cannot
intercept arbitrary `return/goto`, and compiler cleanup attributes do not provide
an equivalent MSVC implementation. Move the block into a typed status-returning
function and replace `cmeta_leave(...)` or the earlier `cmeta_scope_exit(...)`
with `return status` in that function. Both old exit macros now fail compilation.
Function boundaries prevent jumps to an outer scope's cleanup label. Nested body functions return the inner scope's
status only after inner cleanup, so outer cleanup follows automatically. Only
code using the earlier scope body/exit syntax needs migration; field layout and
container APIs are unaffected. Providers formerly using generic-only lifecycle
dispatch must expose canonical concrete construct ops before joining this
static scope backend; there is no automatic runtime dispatch fallback.

The tradeoff is an explicit function and explicit borrowed parameters in place
of implicit captures. This is an intentional source-compatibility change to make
ordinary-C exits safe on every supported compiler. Qualification covers native
body returns, goto/break inside body functions, partial initialization, moves,
nested error propagation and rejection of escaping block syntax. A rollback
requires reverting the scope header and its migrated callers together; restoring
only the former block expander would restore the resource-leak paths.

### `cmeta_type(...)`

The ordinary-C finite generic type declaration entry point.

```c
#include <cmeta/meta.h>

typedef int Key;
typedef int Value;
typedef struct User {
    int id;
} User;

cmeta_type(Option, MaybeUser, User);
cmeta_type(Pair, Entry, Key, Value);
cmeta_type(Tuple, Point3, double, double, double);
```

General form:

```text
cmeta_type(kind, generated_name, type_arguments...)
```

CMeta implements **finite generic routing**, not unrestricted templates.
`cmeta_type(...)` accepts only registered generic kinds and fails compilation
for an unregistered kind.

Algorithmic container kinds such as `List`, `Vec`, and `HashMap` belong to
CSTL:

```c
#include <cstl/typed.h>

cmeta_type(List, IntList, int);
cmeta_type(Vec, IntVec, int);
cmeta_type(HashMap, IntValuesById, int, int);
```

The declared type exposes ordinary concrete C operations:

```c
IntList values = {0};

IntList_init(&values, 100u);
IntList_push_back(&values, 7);
IntList_destroy(&values);
```

One declaration may generate the wrapper type, static-inline typed forwarding
functions, canonical metadata, Range factories, and relevant traits. Allocation
and container algorithms remain ordinary compiled C in the provider library.

Callable/function DSLs use separate explicit entry points such as
`cmeta_function(...)`; `cmeta_type(...)` never routes into callable syntax.

### `typed_any(...)`

Declares a typed callable together with semantic contract metadata.

```c
typed_any(value, int, increment, (int value)) {
    return value + 1;
}

typed_any(associative, long, add, (long left, long right)) {
    return left + right;
}
```

Form:

```text
typed_any(contract, return_type, name, parameters)
```

Current contract vocabulary:

```text
unknown
value
pure
idempotent
associative
fallible
io
async
stateful
```

Contracts map to the existing CMeta effect/property bitsets. They express
programmer intent and optimization/runtime constraints; they do not introduce a
new execution model.

### `FunctionDecl(...)`

Declares an ordinary C function prototype together with immutable descriptive
metadata. It does not replace `typed_any(...)` and does not create an erased
runtime invocation mechanism.

声明还生成 `name_function_type` 精确函数指针 typedef；static call 复用同一组
参数行，不另写原生签名。既有 descriptor 布局及相等语义不变。

```c
FunctionDecl(io, int, send_packet,
    (int, fd, CMETA_PARAM_IN),
    (size_t *, written, CMETA_PARAM_OUT, &cmeta_type_size_ptr));
```

General parameter rows are:

```text
(type, name, flags)
(type, name, flags, explicit_descriptor)
(type, name, flags, explicit_descriptor, abi_carrier)
```

The three-field form requires a registered scalar type, resolves `type` with
`CMETA_TYPEOF(type)`, and records a scalar ABI carrier. Non-scalar and unregistered
types are rejected at declaration time; use the five-field form for pointer,
aggregate, or callback parameters. Likewise, inferred returns require a registered
scalar or literal `void`; a typedef of `void` requires an explicit return descriptor
and carrier. Use `FunctionDeclAsAbi` / `Function0DeclAsAbi` for other return types.
The four-field form remains the compatibility form for a
provider-owned semantic descriptor and leaves ABI carrier unspecified. Consumers
that generate exact C call boundaries (TinyMock, FFI, plugin bridges) should use
the five-field form to state an explicit ABI carrier such as
`CMETA_ABI_OBJECT_POINTER`, `CMETA_ABI_AGGREGATE`,
`CMETA_ABI_FUNCTION_POINTER`, or `CMETA_ABI_ENUM`.

For an application-defined return type, `FunctionDeclAs` preserves an
unspecified ABI carrier. Use `FunctionDeclAsAbi` when the return carrier is
part of the execution contract. `FunctionAbi(name)` returns the immutable
`cmeta_function_abi_desc` sidecar paired with `FunctionMeta(name)`.

Use `FunctionDeclAs(...)` with its explicit return descriptor.
`Function0Decl(...)` and `Function0DeclAs(...)` are the zero-parameter
forms.

Direction flags are:

```text
CMETA_PARAM_UNKNOWN
CMETA_PARAM_IN
CMETA_PARAM_OUT
CMETA_PARAM_INOUT
```

`CMETA_PARAM_UNKNOWN` is the zero value and is a valid explicit statement
that direction is not authoritative. Consumers may use
`cmeta_param_direction_known()` before applying IN/OUT-specific behavior and
must not infer direction from pointer spelling or parameter names.

Pointer parameters may additionally declare `CMETA_PARAM_NULLABLE`,
`CMETA_PARAM_BORROWED`, or `CMETA_PARAM_OWNED`. Borrowed and owned are
mutually exclusive. These pointer semantics are independent of direction:
nullable/borrowed/owned may be known while direction remains UNKNOWN.
OUT/nullability/ownership metadata still requires a pointer descriptor where
the respective semantic demands it.

CMeta publishes canonical pointer-boundary descriptors for common reflected
contracts: `cmeta_type_void_ptr`, `cmeta_type_char_ptr`, `cmeta_type_char_ptr_ptr`, and
`cmeta_type_descriptor_ptr` (plus their pointee descriptors where applicable).
They provide type identity and exact pointer shape only. A source declaration
such as `const char *` may use `cmeta_type_char_ptr`; `const` does not imply
BORROWED, and ownership/lifetime still comes exclusively from explicit
parameter/result flags.

Function results have an independent semantic flag set:

```text
CMETA_RESULT_UNKNOWN
CMETA_RESULT_VALUE
CMETA_RESULT_BORROWED
CMETA_RESULT_SHARED
CMETA_RESULT_OWNED
CMETA_RESULT_NULLABLE
```

VALUE/BORROWED/SHARED/OWNED are mutually exclusive. NULLABLE is orthogonal and
is valid only for pointer-shaped return types. A void return admits only
UNKNOWN. Existing `FunctionDecl*` forms deliberately publish UNKNOWN rather
than inferring ownership from return spelling, type kind, names, or ABI carrier.

Use the explicit result-aware forms when the result contract is authoritative:

```c
Function0DeclResult(value, int, CMETA_RESULT_VALUE, current_count);

Function0DeclAsAbiResult(
    value, Buffer *, &buffer_ptr_type, CMETA_ABI_OBJECT_POINTER,
    CMETA_RESULT_OWNED | CMETA_RESULT_NULLABLE, make_buffer);
```

Result semantics remain descriptive. They tell compilers/planners whether a
returned resource is a value, borrow, shared reference, or ownership transfer;
they do not themselves execute retain/release/destroy or insert lexical cleanup.

The generated `FunctionMeta(name)` view is TU-local immutable metadata.
Consumers compare the referenced CMeta types semantically rather than relying
on descriptor address identity across translation units.

Function reflection answers "what is this C function?". Execution remains a
separate concern: an exact-ABI generated adapter, `cmeta_callable`, CFlow, or
another consumer-specific mechanism must perform the actual call. CMeta does not
parse arbitrary C prototypes at runtime and does not provide libffi-style
universal invocation.

### Reflection across native modules

Function, parameter, interface and type descriptors are borrowed views. Their
names, arrays, nested descriptors, traits, callbacks and generated wrappers remain
owned by the provider. Copying a descriptor, interface, `cmeta_callable`, or CFlow
projection does not retain the module containing its data or executable code.
The host must hold a module reference until all consumers finish, including copied
Graph/Plan callables, active runs and values requiring provider trait callbacks.
Unload order is: stop new admissions, drain calls/runs, destroy dependent values
and consumers while their callbacks are live, then release the final module
reference. Validators require live storage and cannot detect an unloaded pointer.

`CMETA_REFLECTION_ABI_VERSION` is the reflection layout epoch. Epoch 2 introduced result-semantic FunctionDesc layout; epoch 3 replaced receiver-method owner strings with optional canonical `cmeta_generic_desc` identity. Epoch 4 replaces receiver methods with thin `{ name, abi }` operation rows and moves receiver projection validation into Function. Ordinary receiver operations use a null generic owner, while generic operation sets publish their constructor descriptor. Source consumers and Plugin hosts/providers must migrate together; see [receiver operation migration](RECEIVER_OPERATIONS.md). A provider bootstrap
must accept a fixed-width requested epoch and reject a mismatch **before publishing
descriptor pointers**. The provider compares against its own header constant;
`cmeta_reflection_abi_version()` returns the linked CMeta library's epoch and checks
header/library agreement, but a host-resolved query cannot identify a plugin's
compile-time ABI. Providers predating this handshake require an explicit legacy
adapter or rejection; a matching `sizeof` is not proof of compatibility.

Within an epoch, existing descriptor layouts, field meanings, enum values and
array element strides are frozen, including reachable type/identity/trait layouts.
Add metadata through separate sidecars; incompatible changes require a new epoch.
The `size` fields are validation guards, not permission to append fields to array
elements or to reinterpret another epoch. Host and provider must also agree on
native architecture, calling convention, packing and enum representation.

This epoch covers reflection only. Salts Plugin exports embed borrowed pointers
to these descriptors, so the Plugin ABI is also advanced when an incompatible
Reflection layout becomes part of the exported manifest contract; hosts must
reject the older Plugin ABI before consuming reflected exports.

Application interface/vtable versions and the
finite callable type/signature configuration require separate agreement before
dispatch; equal reflection epochs do not authorize exchanging arbitrary
`cmeta_callable` builds. CMeta owns neither the platform loader nor its references.

Dynamic module publication, loading, lease and unload verification belong to the
Salts Plugin module and its tests.

### `interface(...)`

Declares a small protocol/vtable interface.

```c
#include <cmeta/meta.h>

#define SOURCE_METHODS(X, I) \
    X(I, R1, bool, next, int *, out) \
    X(I, V0, void, close, _)

interface(Source, SOURCE_METHODS);
```

Method row kinds are split into two levels:

```text
R0..R4   non-void ABI-only rows
V0..V4   void ABI-only rows
D0       owning destructor ABI-only row

F0..F4   fully reflected non-void rows; result semantics remain UNKNOWN
FR0..FR4 fully reflected non-void rows with explicit canonical result semantics
FV0..FV4 fully reflected void rows
FD0      fully reflected owning destructor
```

Legacy `R/V/D` rows preserve the exact vtable/wrapper ABI but intentionally do
not synthesize semantic function metadata from C spelling. Their
`cmeta_interface_method_desc.function` and `.abi` are `NULL`; use
`cmeta_interface_method_arity()` for dispatch arity.

Fully reflected `F/FV/FD` rows additionally state the semantic contract,
return descriptor, return ABI carrier, and each parameter as the exact
five-field FunctionDecl row. `F` rows intentionally preserve
`CMETA_RESULT_UNKNOWN`. Result-aware `FR0..FR4` rows add one explicit
`result_flags` field after the return ABI carrier and otherwise preserve the
same vtable/wrapper ABI:

```text
(type, name, flags, descriptor, abi_carrier)
```

For all reflected rows, `cmeta_interface_method_function()` and
`cmeta_interface_method_abi()` expose the same canonical
`cmeta_function_desc` / `cmeta_function_abi_desc` model used by ordinary
`FunctionDecl`. CMeta never infers OUT/INOUT, ownership, effects, or custom
type identity from pointer spelling or names.

Example:

```c
#define CLOCK_METHODS(X, I) \
    X(I, F0, cflow_instant, now, stateful,
      &cflow_type_instant, CMETA_ABI_AGGREGATE) \
    X(I, F1, bool, advance, stateful,
      &cmeta_type_bool, CMETA_ABI_SCALAR,
      (cflow_duration, delta, CMETA_PARAM_IN,
       &cflow_type_duration, CMETA_ABI_AGGREGATE)) \
    X(I, FR0, cflow_buffer *, create_buffer, stateful,
      &cflow_buffer_ptr_type, CMETA_ABI_OBJECT_POINTER,
      CMETA_RESULT_OWNED | CMETA_RESULT_NULLABLE)
```

An interface value is conceptually `{ self, vtable }` plus implementation and
capability metadata. This is a protocol mechanism, not a class hierarchy.

### `implements(...)`

Binds an ordinary C implementation to an interface.

```c
implements(Source,
           file_source,
           SOURCE_CAN_SEEK,
           .next = file_source_next,
           .close = file_source_close);
```

`implements(...)` only means "implements this interface/protocol". It is not a
container generation phase.

The natural names are conditional aliases. If
`CMETA_NO_NATURAL_INTERFACE_NAMES` is defined, or a host header has already
claimed `interface` or `implements`, use the collision-safe spellings:

```c
CMETA_INTERFACE(Source, SOURCE_METHODS);

CMETA_IMPLEMENTS(Source,
                 file_source,
                 SOURCE_CAN_SEEK,
                 .next = file_source_next,
                 .close = file_source_close);
```

Framework and public headers should prefer `CMETA_INTERFACE(...)` and
`CMETA_IMPLEMENTS(...)`; application-local code may use the natural aliases when
the host environment leaves them available.

---

### Static key / typed static call

`<cmeta/fastpath.h>` 的 C11 `SALTS_FAST_KEY(name, initial)` 定义原子 bool，
`cmeta_fast_branch(&name)` acquire 读取；enable/disable 在控制面 release 发布。
`cmeta_static_call(slot, default_function)` 从 FunctionDecl 生成精确类型原子槽，
`cmeta_static_update(slot, function)` 检查签名与完整 ABI 契约后替换。
`cmeta_static_invoke(slot, args...)` 直接调用该次读取的目标，零参数用
`cmeta_static_invoke0(slot)`。
call 声明要求文件作用域、每槽一个 TU；热路径无 Reflection 查询。

使用可移植 C 原子读取 gate 和函数指针槽；调用保留精确的 C 函数签名。
C++17 借用 C-owned opaque key。更新失败不改变目标；更新不 retain 或 drain 旧
提供者，所有旧代码目标必须活到在途调用结束。完整契约、边界、benchmark
和可编译测试示例见 [FASTPATH.md](FASTPATH.md)。

## 2. Framework DSL

These forms are primarily for libraries and CMeta/CFlow internals.

### `Schema(...)`

Defines or expands parenthesized row data through a mapper.

```c
#define MyRows(M) \
    Schema(M,
        (ROW_A, 1),
        (ROW_B, 2))
```

`Schema(...)` owns row unpacking. It is a finite C-preprocessor code-generation
kernel, not a runtime data structure. The tuple-list kernel accepts at most 16
rows per invocation. `cmeta_struct(...)`, `cmeta_enum(...)`, `cmeta_traits(...)`, `Schema(...)`,
and `Operators(...)` share that row-count limit; `Tuple` accepts 2 through 16
type arguments. Split larger declarations into separate stable concepts instead
of depending on an implementation-specific macro expansion failure.

### `Replay(...)`

Applies a named schema/producer to a consumer mapper.

```c
#define DECLARE(name, value) int name = value;
Replay(MyRows, DECLARE)
```

Conceptually:

```text
Schema = source rows / compile-time representation
Replay = consumer application
```

One schema can therefore feed declaration, metadata, validation, counting, or
other framework mappers without creating another user-facing language layer.

### Finite compile-time functions

Finite functions declare explicit mappings and evaluate them through generated
C declarations. They do not add C++ template syntax or a runtime evaluator.

```c
TypeFunction(StorageType,
    (small, SmallStorage),
    (wide, WideStorage));

TypeFunction(CommonType,
    (small, small, SmallStorage),
    (small, wide, WideStorage),
    (wide, small, WideStorage),
    (wide, wide, WideStorage));

typedef TypeEval(StorageType, small) storage_type;
typedef TypeEval(CommonType, small, wide) common_type;
```

`TypeFunction` infers the input arity from its first row, while `TypeEval`
infers it from the number of input keys. The corresponding integer-constant
forms are `ValueFunction` and `ValueEval`:

```c
ValueFunction(TypeRank,
    (small, 1),
    (wide, 2));

enum { wide_rank = ValueEval(TypeRank, wide) };
```

`Predicate` is a unary boolean value function. `Satisfies` evaluates it and
`Require` rejects a false row with a C/C++ static assertion:

```c
Predicate(Hashable, (small, 1), (opaque, 0));
Require(Hashable, small);
```

The declaration contract is deliberately finite and fail-fast:

- A function name and every input key must each be one stable preprocessor
  identifier.
- A declaration contains 1 through 16 rows. More rows can be added by repeating
  the same function name in separate bounded declarations.
- The first row determines input arity; every later row in that declaration
  must have the same shape or compilation fails.
- A value result is an integer constant expression. Complex type results should
  first receive a stable typedef name.
- Evaluating an absent mapping produces an unknown generated identifier, while
  declaring conflicting rows produces a C declaration conflict. Neither case
  has a fallback.

Arity is part of each generated identifier, so unary, binary, and ternary
functions with the same public name remain distinct. Declaration work is linear
in the number of rows; an evaluation is fixed token lookup rather than a scan
or a generated binary/ternary Cartesian product. Only the unified entry points
are public; numbered implementation families remain internal.

### Finite DFA inference

`InferenceRules` projects explicit integer-symbol rows into an ordinary C
relation and infers arity from the first row. A row contains one through three
input symbols followed by one result.
Each declaration accepts the same bounded 1–16 row list as the underlying
`CMETA_PP_FOR_EACH_A` projection.
The row source may be shared with a matching `ValueFunction` so compile-time and
admission-time evaluation cannot drift:

```c
#include <cmeta/meta.h>

enum {
    OP_ADD = 1,
    TYPE_SMALL = 10,
    TYPE_WIDE = 11
};

#define CommonRows \
    (TYPE_SMALL, TYPE_SMALL, TYPE_SMALL), \
    (TYPE_SMALL, TYPE_WIDE, TYPE_WIDE), \
    (TYPE_WIDE, TYPE_SMALL, TYPE_WIDE)

ValueFunction(CommonType, CommonRows);
InferenceRules(common_type_relation, CommonRows);

int main(void) {
    cmeta_infer_state states[
        CMETA_INFER_STATE_BOUND(
            InferenceRuleCount(common_type_relation),
            InferenceRuleArity(common_type_relation))];
    cmeta_infer_transition transitions[
        CMETA_INFER_TRANSITION_BOUND(
            InferenceRuleCount(common_type_relation),
            InferenceRuleArity(common_type_relation))];
    cmeta_infer_dfa dfa;
    cmeta_infer_value result = 0u;
    const cmeta_infer_symbol input[] = {TYPE_SMALL, TYPE_WIDE};

    cmeta_infer_dfa_init(
        &dfa, states, sizeof(states) / sizeof(states[0]), transitions,
        sizeof(transitions) / sizeof(transitions[0]));
    if (cmeta_infer_dfa_build(&dfa, &common_type_relation) !=
        CMETA_INFER_OK)
        return 1;
    if (cmeta_infer_dfa_eval(&dfa, input, 2u, &result) != CMETA_INFER_OK)
        return 2;
    return result == TYPE_WIDE ? 0 : 3;
}
```

The relation borrows its macro-projected static row array. The DFA owns no
memory: it borrows both arrays passed to `cmeta_infer_dfa_init`. The maximum
required capacities are `1 + rule_count * arity` states and
`rule_count * arity` transitions. These macros are intended for bounded
compile-time counts; code deriving capacities from untrusted runtime values
must perform checked arithmetic first.

`cmeta_infer_dfa_build` rejects duplicate input rows, conflicting results, and
insufficient workspaces. `cmeta_infer_dfa_eval` rejects wrong arity and reports
missing rules. Failed evaluation does not modify the output result. There is no
fallback mapping.

Building or rebuilding a DFA is a control-plane operation that requires no
concurrent readers. After a successful build, read-only evaluation is safe as
long as the relation and caller workspaces remain alive and unchanged. A
compiled executor should retain the inferred result or handler rather than
querying the DFA per value.

### Schema constant folds

`SchemaCount`, `SchemaAll`, and `SchemaAny` turn an existing schema into an
integer constant expression:

```c
#define Checks(M) Schema(M, (1), (1), (0))

_Static_assert(SchemaCount(Checks) == 3u, "row count");
_Static_assert(!SchemaAll(Checks), "not every row is true");
_Static_assert(SchemaAny(Checks), "at least one row is true");
```

`SchemaCount` accepts arbitrary non-empty row shapes because it ignores row
contents. `SchemaAll` and `SchemaAny` require every row to contain exactly one
integer constant expression. These folds retain the existing 16-row limit of a
single `Schema(...)` invocation.

### `Operators(...)`

CFlow's specialized operator-schema normalizer.

Structured source rows group call, function, flow, semantic, and effect
information, then normalize to the established flat consumer ABI before replay.
This keeps source syntax readable without inventing a second operator semantics.

`Operators(...)` is framework vocabulary. Application code normally consumes the
higher-level CFlow API instead of authoring raw operator schemas.

---

## 3. Runtime Protocol

The following are normal C APIs/protocols, not additional language keywords.

### Type metadata and identity

Core concepts include:

```text
cmeta_type_desc
cmeta_type_traits
cmeta_type_identity
CMETA_TYPEOF(T)
```

Descriptors carry semantic type information such as name, size, alignment,
kind, pointee, traits, and identity. Header-generated descriptor addresses are
not required to be process-global type IDs; consumers compare semantic type
identity/content.

`CMETA_TYPEOF(T)` is a finite `_Generic` lookup, not reflection over arbitrary C
types. `T` must be compatible with exactly one C type named by the active
`CMETA_TYPE_LIST`; by default that aliases `CMETA_CALLABLE_TYPE_LIST`, which in
turn defaults to `CMETA_KNOWN_TYPE_LIST`. A `typedef` alias reuses its underlying
compatible type's descriptor. Do not register a second row for such an alias,
because two compatible `_Generic` associations are a compile-time error.

Applications normally extend the defaults through `CMETA_USER_TYPE_LIST` in a
shared configuration header included before the first CMeta header in every
affected translation unit. Overrides of `CMETA_KNOWN_TYPE_LIST` and
`CMETA_CALLABLE_TYPE_LIST` follow the same rule. Changing the callable list
changes the ABI of `cmeta_sig` and `cmeta_callable`, so the list must match in
every translation unit and in the linked CMeta library build. Every custom row
must name descriptors and traits that the program defines with the ownership
operations required by its consumers.

The built-in type rows and finite callable relation graph are declared in
`formal/cmeta_cflow_calculus/CMetaCFlowCalculus/CMeta/BuiltinSignatures.lean`.
Validation rejects empty categories, duplicates, and relations that reference
unknown built-in type tokens before rendering. The checked-in
`cmeta/generated/builtin_signature_manifest.h` preserves the public macro and
signature order, is consumed by normal C/C++ compilation without a Lean
dependency, and must not be edited manually. From the formal package, use:

```text
lake exe cmeta-signature-gen --write ../../cmeta/include/cmeta/generated/builtin_signature_manifest.h
lake exe cmeta-signature-gen --check ../../cmeta/include/cmeta/generated/builtin_signature_manifest.h
```

This generation boundary owns only built-ins. Application rows and
`CMETA_USER_UNARY_RELATION_LIST`, `CMETA_USER_BINARY_RELATION_LIST`, and
`CMETA_USER_GENERATOR_RELATION_LIST` remain shared compile-time configuration;
all translation units must still see the same callable ABI.

Mechanical consumers can select `CMETA_VALUE_SIGNATURES(U, B)` or
`CMETA_GENERATOR_SIGNATURES(G)` when their protocol is already known.
`CMETA_ALL_SIGNATURES(U, B, G)` remains the complete ABI universe and preserves
unary, binary, then generator ordering. Protocol grouping changes only which
adapters or switch cases are emitted at an already-validated boundary;
`cmeta_fn_invoke` and `cmeta_fn_generate` retain their documented rejection
results for the other protocol.

`cmeta_struct(T, ...)` and `cmeta_traits(T, ...)` generate structural and callable metadata,
but do not add `T` to either finite type universe. `CMETA_TYPEOF(T)` returns
`NULL` when no compatible registered type exists. APIs that require a descriptor
then fail according to their own contract: a container range lookup can return
`false`, range traversal can return `CMETA_GEN_ERROR`, and initialization or
collector boundaries can return invalid-argument or type-mismatch errors. Check
the exact API result; there is no implicit fallback.

### Callable protocol

Core runtime values include:

```text
cmeta_fn
cmeta_callable
cmeta_sig_desc
```

They provide typed signature metadata, effect/property contracts, erased invoke
or generator entry points, and optional inline captures.

### Range protocol

`cmeta_range` is an allocation-free borrowed traversal protocol. Range capability
flags include:

```text
SIZED
ORDERED
SORTED
UNIQUE
CONTIGUOUS
RANDOM_ACCESS
REUSABLE
```

Typed containers may expose default, key, value, or entry ranges through their
descriptors.

A Range borrows both its source object and its element descriptor.
The source handle storage must outlive the Range and every cursor used with it.
Creating a Range does not allocate, retain, or extend either lifetime. Unless a
concrete container promises otherwise, append, erase, resize, reset, destroy,
slot reuse, and undocumented cross-thread access invalidate the traversal.

When a Range supplies version tracking, `cmeta_range_next(...)` reports
`CMETA_GEN_MUTATED` before changing the cursor or output, but only while the
source handle storage remains alive. Version tracking cannot make expired or
freed storage safe, and a Range without a version callback cannot detect
mutation generically.

### Collector protocol

`cmeta_collector` is a bounded transactional protocol for constructing a
caller-owned output from typed borrowed values.

Stable states:

```text
ZERO
BEGUN
ACCEPTING
COMMITTED
ABORTED
```

The facade performs validation and exactly-once abort behavior but does not own a
general allocator, scheduler, retry system, or synchronization policy.

### Container descriptor protocol

`cmeta_container_desc` describes the runtime capabilities of a typed container,
including type metadata, Range factories, and an optional collector factory.
The raw algorithms remain ordinary C implementation code.

Declaration-side construction uses two distinct lifecycle operations:

```c
cmeta_status cmeta_container_bind_types(
    void *object, const cmeta_declared_type *declared);
cmeta_status cmeta_container_restore_zero(
    void *object, const cmeta_declared_type *declared);
```

`bind_types` accepts a canonical zero handle and installs its concrete
descriptor plus T/K/V metadata without allocating. `restore_zero` accepts a
zero, bound, active, or committed handle for the same declared provider,
releases provider-owned storage, and restores the complete handle to canonical
all-bits-zero. It returns `CMETA_TYPE_MISMATCH` when a nonzero handle belongs to
a different provider and `CMETA_INVALID_ARGUMENT` for an invalid declaration,
missing lifecycle callback, or invalid pointer.

Complete C usage is compiled in
`cstl/tests/cstl_construction_binding_test.c`.

The restore operation is not a synonym for `cmeta_collector_abort()`.
Collector abort owns only a begun/accepting collection transaction and remains
a no-op after commit; restore-to-zero is the declaration/provider lifecycle
boundary used by larger object transactions.

### Interface runtime values

`interface(...)` generates an ordinary C `{ self, vtable }` protocol value,
inline forwarding functions, capability metadata, and reflection metadata.

This value is an exact typed protocol capability. It is not the canonical
runtime-selected identity/lifetime representation for an arbitrary native
object; that role belongs to `cmeta_object_ref`. The two compose through an
explicit provider relation:

```text
one native provider
    +-- cmeta_object_ref     identity / field+receiver reflection / lifetime
    +-- Interface A         typed protocol capability
    +-- Interface B         typed protocol capability
```

An ObjectRef-to-Interface view is normally borrowed. Its validity is bounded by
the object/provider lifetime and, for Plugin-backed code, by the outer Plugin
lease. CMeta never derives this relation from pointer equality, struct offsets,
vtable layout, descriptor names, or semantic descriptor equality.

A method marked `CMETA_INTERFACE_METHOD_OWNS_SELF` is different: dispatch
consumes the Interface self owner and invalidates that handle. Such an Interface
cannot be exposed as a borrowed view of a still-live ObjectRef without creating
two competing ownership authorities. Runtime projection must therefore reject
that shape until an explicit ownership transfer is performed. The canonical
query is `cmeta_interface_desc_has_owning_method()`.

This does not affect static dispatch. Compile-time-known Interface calls remain
ordinary typed calls through their generated wrappers/vtable; ObjectRef is not a
mandatory hop on the hot path.

---

## 4. Reserved future syntax

The following names describe useful directions but are **not current language
features and are not compatibility promises**:

```text
Lambda
Bind
Variant
Match
Array
SmallVec
RingBuffer
```

Related ideas may already exist in Lean models, CFlow internals, experiments, or
ordinary C implementations. They become CMeta syntax only after a concrete,
useful, maintainable C11 implementation exists.

The preferred evolution rule is:

1. solve the concrete use case with ordinary C and existing CMeta patterns;
2. identify repeated boilerplate or a stable semantic pattern;
3. add the smallest composable CMeta abstraction;
4. validate it on GCC, Clang, and MSVC portability lanes;
5. add formal proof only when the new feature needs a property not already
   covered by existing evidence.

CMeta does not need to "complete" a universal language design before it is
useful.

---

## Removed syntax

### `Containers(...)`

Removed. Do not keep or reintroduce it as an alias.

Old style:

```c
/* removed */
Containers(
    (List, IntList, int),
    (Vec, IntVec, int)
);
```

Current style:

```c
#include <cstl/typed.h>

cmeta_type(List, IntList, int);
cmeta_type(Vec, IntVec, int);
```

The explicit form keeps one generic entry point and avoids a second batch DSL
that adds no semantic capability.

### Container `implement(...)`

The earlier declaration/implementation split is also removed. Typed container
instantiation is single-stage.

---

## Design rule of thumb

Use the smallest layer that solves the problem:

```text
ordinary C
    ↓ when repetitive metadata/code generation appears
Application DSL
    ↓ when a library needs reusable compile-time row generation
Framework DSL
    ↓ for execution/storage/interoperation
Runtime Protocol
```

Do not add a new keyword when an existing declaration, schema mapper, descriptor,
interface, or ordinary C function composes cleanly enough.

## 有限宏与精确调用声明

`<cmeta/pp.h>` 维护有限宏展开；`<cmeta/compiler.h>` 维护编译器与语言差异。
`<cmeta/invoke_decl.h>` 在 canonical Function 声明上增加显式选择的精确 thunk。
现有 `FunctionDecl` 的描述用途、Interface 的源行语法与运行时 ABI 保持不变。

编译器能力统一通过 `CMETA_HAS_BUILTIN(name)`、`CMETA_HAS_ATTRIBUTE(name)`、
`CMETA_HAS_FEATURE(name)` 查询，结果为 0/1，可用于 `#if`。底层编译器没有相应探测器时
返回 0，表示未获支持保证，不启用替代实现。`CMETA_UNUSED` 也消费这个统一探测层。
参数及能力含义沿用编译器自身词汇，见 [Clang 扩展说明](https://clang.llvm.org/docs/LanguageExtensions.html)
与 [GCC builtin 探测](https://gcc.gnu.org/onlinedocs/cpp/_005f_005fhas_005fbuiltin.html)。
`CMETA_HAS_COUNTER` 表示唯一名称生成能力；不支持时不提供 `CMETA_PP_UNIQUE`，不会以行号代替。
唯一编号只用于命名，不定义声明次序、资源顺序或跨 TU 身份；顺序由 schema 和 indexed/reverse replay 决定。

原生类型助手也归属 `compiler.h`，与返回反射 descriptor 的既有 `CMETA_TYPEOF(type)` 分离：

| 入口 | 参数与结果 | 能力与边界 |
| --- | --- | --- |
| `CMETA_NATIVE_TYPEOF(expr)` | 产生原生类型，可用于 typedef 或声明；保留 cv、数组边界和函数类型；C++ 去掉引用 | 先检查 `CMETA_HAS_NATIVE_TYPEOF`；只对非变长类型保证不求值 |
| `CMETA_SAME_TYPE(a,b)` | 两个表达式的原生类型比较，返回编译期 0/1；顶层及指针目标的 cv 限定参与比较 | 先检查 `CMETA_HAS_SAME_TYPE`；C 使用类型兼容规则，C++ 使用去引用后的类型相等；不比较反射身份、所有权或 ABI |
| `CMETA_AUTO(name,expr)` | 声明一个局部值，初始化表达式恰好求值一次；类型按原生 `auto` / `__auto_type` 规则推导，数组/函数初始化器退化 | 先检查 `CMETA_HAS_AUTO`；name 是新的局部标识符，expr 必须可推导、可初始化，不接受裸花括号初始化列表 |

三个能力宏均可在 `#if` 中使用。不支持时不定义对应操作宏，调用方必须明确要求能力，
不能用转换或 `typeof(expr) name = expr` 模拟单次求值。当前支持 C++17（含 MSVC）、
GCC C 和 Clang C；MSVC C 不声明支持。C++ 沿用普通值初始化的复制、移动和析构规则，
不从声明推导 CMeta 资源所有权；这些助手不改变 descriptor、Plugin 生命周期或运行时分派。

`CMETA_NATIVE_TYPEOF` / `CMETA_SAME_TYPE` 的可移植查询域是固定类型、非 void 的非位域表达式。
不要将变长数组或变长数组指针作为类型查询操作数；GNU `typeof` 可能求值这类表达式。
`CMETA_AUTO` 使用独立的原生推导设施，对变长数组指针初始化器也只求值一次。
原生查询沿用编译器的语言规则，不修复编译器自身的表达式推导差异：本地 MSVC 19.44
对直接 `*&function_name` 的 `decltype` 得到函数指针类型，而命名函数指针变量的解引用
得到函数类型。跨编译器查询应使用函数名或命名指针的解引用，避免直接 `*&function_name`。
实现依据见 [GCC typeof / auto type](https://gcc.gnu.org/onlinedocs/gcc/Typeof.html)
与 [Clang auto type](https://clang.llvm.org/docs/LanguageExtensions.html#auto-type)。

```c
#include <cmeta/compiler.h>
#if !CMETA_HAS_AUTO || !CMETA_HAS_NATIVE_TYPEOF
#error "This example requires native type deduction"
#endif
int main(void) {
    int calls = 0;
    CMETA_AUTO(value, ++calls);
    typedef CMETA_NATIVE_TYPEOF(value) value_type;
    value_type copy = value;
    return calls == 1 && copy == 1 ? 0 : 1;
}
```

`CMETA_LAYOUT_REQUIRE(condition)` 和 `CMETA_FLAGS_REQUIRE(value,mask)` 均为表达式级编译期约束，
成功贡献整数零，可放入静态初始化器。前者要求常量条件为真；后者要求非负整数常量的全部
置位都包含在显式 mask 中。失败必须导致编译错误，运行时值也不能充当条件或 flag 输入。
两者复用 `CMETA_CONST_REQUIRE`，不进行运行时检查，不从类型拼写推断业务允许位。

Function、Interface、精确 thunk 与 TinyMock 的参数投影共用五字段内部行：
`(type,name,flags,descriptor,carrier)`。三字段输入仍先进行标量准入，descriptor 来自
注册类型且 carrier 为 `CMETA_ABI_SCALAR`；四字段输入保留显式 descriptor 和
`CMETA_ABI_UNSPECIFIED`；五字段输入保留全部显式语义。归一化不会让描述性元数据
自动获得可调用资格，未指定 ABI 的声明仍不能用于精确 thunk 或 ABI 替换。

`CMETA_STRUCT`、`CMETA_ENUM` 及 `StructMeta`、`EnumMeta`、`EnumParse` 现在统一先展开
宏别名再生成符号和反射名称。普通标识符、字段布局、枚举显式值及自动递增规则不变。
兼容性边界：过去直接向底层声明宏传类型别名宏时，元数据可能保留别名 token 名称；
现在记录展开后的实际类型名，与 `Struct`/`Enum` 前端一致。依赖旧别名字符串的查询需
改用实际类型名；descriptor 的二进制布局及生命周期没有变化。

静态发现与 Component 反射声明也使用同一展开规则：`cmeta_entry(symbol)` 的名称是展开后的
符号名，`cmeta_registry(name, entries)` 的名称和生成数组使用同一展开后的标识符。
这会修正旧代码传宏别名时保留别名字符串的行为；显式 `cmeta_manifest_*_entry("name", ...)`
的名称不变。entry 顺序、kind、descriptor 指针、format version 和 ABI 布局不变。
底层 `CMETA_INTERFACE` 同样先展开类型别名，避免方法名与 interface 元数据符号使用不同 token；
宏别名声明的反射名称统一使用实际接口名，普通接口声明的行为不变。

`cmeta_registry` 接受非空 entry 流；零项使用 `cmeta_registry_empty(name)`，生成
`entries == NULL, count == 0` 的普通 `static const cmeta_manifest`。
`cmeta_component` 接受 1–16 个 provides/requires 行，使用共享逗号 map 保留行顺序与重复项；
零项使用 `cmeta_component_empty(name)`，生成 `capabilities == NULL, count == 0`，仍通过
`cmeta_component_meta(name)` 取得描述符。两种空声明都不生成零长数组或占位项。
空 Component 描述符只有放入显式 manifest 才会被该表发现；它不是运行中的 CMeta::Plugin 模块。
这两个声明宏没有运行时返回值；非法标识符或不合约的非空行在编译期报错。
空表的索引查询仍返回 `CMETA_INVALID_ARGUMENT`，失败时输出参数保持原值。

生成对象均为 TU-local 不可变元数据，不包含句柄、分配、注册副作用或隐藏 lease。
查询视图仍借用 provider 元数据；跨 DSO 使用时必须由既有 Plugin lease 保证其生命周期。
静态数组仍是参考表示，没有新增 linker 聚合或构造函数注册。

```c
#include <cmeta/manifest_view.h>
cmeta_registry_empty(NoExports);
cmeta_component_empty(NoCapabilities);
cmeta_registry(Discovery,
    cmeta_manifest_component_entry("provider", cmeta_component_meta(NoCapabilities)));
int main(void) {
    const cmeta_manifest_limits limits = {CMETA_MANIFEST_DEFAULT_ITEMS,
        CMETA_MANIFEST_DEFAULT_DEPTH, CMETA_MANIFEST_DEFAULT_NODES};
    const cmeta_component_desc *provider = NULL;
    return cmeta_manifest_get_component(&Discovery, 0u, &limits, &provider) == CMETA_OK
        && provider->count == 0u && NoExports.count == 0u ? 0 : 1;
}
```

| 原语 | 契约 |
| --- | --- |
| `CMETA_PP_MAP(M,C,...)` | 1–16 项，调用 `M(item,C)`，不插分隔符 |
| `CMETA_PP_MAP_COMMA/SEMI/PREFIX_COMMA` | 同上，分别在项间插逗号、分号，或每项前插逗号；分号形式不追加末尾分号 |
| 上述各形式的 `_N(N,M,C,...)` | 显式 0–16 项；例如零项写成 `CMETA_PP_MAP_N(0,M,C,)`；不调用 mapper、不输出分隔符 |
| `CMETA_PP_MAP_I_N(N,M,C,...)` | `M(index,item,C)`，下标从零起；0–16 项 |
| `CMETA_PP_PAIR_MAP_N` / `PAIR_MAP_COMMA_N` / `PAIR_MAP_PREFIX_COMMA_N` | 恰好 N 组平铺 `type,name`，0–16 组，调用 `M(type,name,C)`；零组最后保留一个空实参 |
| `CMETA_PP_BOOL/NOT/AND/OR/IF/WHEN` | 输入是单个 token，只有 `0` 为假；不是预处理整数表达式求值器 |
| `CMETA_PP_IIF(c)(t,f)` | c 必须为 0 或 1；`IF` 会先用 BOOL 归一化 |
| `CMETA_PP_WHEN(c)(...)` | 条件为真时输出可变参数，否则不输出 |
| `CMETA_PP_IS_VOID(x)` | 只识别展开后的单个 `void` token，不识别 typedef 或任意 C 类型拼写 |
| `CMETA_PP_STRINGIFY(x)` | 先展开再字符串化；`STRINGIFY_I` 保留原 token 拼写 |
| `CMETA_PP_OVERLOAD(prefix,...)` | 1–16 项，返回拼接了参数数目的宏名，由调用点继续传参 |
| `CMETA_PP_TUPLE_GET_0..15` / `HEAD` / `TAIL` / `APPLY` | 有界 tuple 投影；索引必须存在，TAIL 的输入至少两项，APPLY 调用 `M tuple` |
| `CMETA_PP_UNIQUE(prefix)` | 当前编译器提供 `__COUNTER__` 时可用；同一 TU 内唯一，不承诺跨 TU 名字唯一 |

这些 map 复用同一有限 indexed 展开族，mapper 内不支持再次嵌套同一 map。
需要已有多层乘积展开时仍使用 `FOR_EACH_A/B/C`。自然参数计数仍要求非空；
严格 C11 的零项使用显式 `_N`，不依赖 `__VA_OPT__` 扩展或隐式回退。

`CMETA_HAS_VA_OPT` 是标准模式准入标志：C++20 及以后、声明
`__STDC_VERSION__ >= 202311L` 的 C23 及以后为 1；MSVC 还要求一致性预处理器。
MSVC 的语言模式同时识别 `_MSVC_LANG`，不依赖 `/Zc:__cplusplus`。
C11/C++17、C23 草案模式及 MSVC 传统预处理器为 0，即使编译器接受扩展也不开放。
只有该标志为 1 时，以下宏才有定义：

| 原语 | 契约 |
| --- | --- |
| `CMETA_PP_HAS_ARGS(...)` | 展开后含 token 返回 1，否则为 0；`()` 和逗号本身也属于 token |
| `CMETA_PP_NARG_ZERO(...)` | 计算 0–16 项；省略参数或展开为空的宏均为零，括号保护项内逗号 |
| `CMETA_PP_PREFIX_COMMA(...)` | 非空时输出一个前导逗号及原参数；为空时不输出 |
| `CMETA_PP_MAP_ZERO(M,C,...)` | 0–16 项，复用显式计数 map；零项不调用 mapper |
| `CMETA_PP_MAP_COMMA_ZERO/SEMI_ZERO/PREFIX_COMMA_ZERO` | 同一零项规则，分别复用相应分隔符策略 |

计数不推断语义：`CMETA_PP_NARG_ZERO(())` 为 1，`CMETA_PP_NARG_ZERO(,)` 为 2。
多项中的空项仍由 mapper 解释。超过 16 项不支持；与既有 map 一样，mapper
不能递归嵌套同一展开族。使用方必须先检查能力标志；未准入时应使用显式 `_N`
或明确要求更高语言模式，不存在自动切换实现。现有 `NARG`、`MAP`、`FOR_EACH`
和 Interface 零参数声明的契约不变。

完整 C++20 示例：

```cpp
#include <cmeta/pp.h>
#if !CMETA_HAS_VA_OPT
#error "This example requires standard zero-argument variadics"
#endif
#define VALUE(item,context) ((item) + (context))
int main() {
    const int empty[] = { 7 CMETA_PP_MAP_PREFIX_COMMA_ZERO(VALUE,0) };
    const int values[] = { CMETA_PP_MAP_COMMA_ZERO(VALUE,1,2,3) };
    return empty[0] == 7 && values[0] == 3 && values[1] == 4 ? 0 : 1;
}
```

语言依据：[GCC Variadic Macros](https://gcc.gnu.org/onlinedocs/cpp/Variadic-Macros.html)、
[MSVC 一致性预处理器](https://learn.microsoft.com/en-us/cpp/preprocessor/preprocessor-experimental-overview)、
[C23 草案 N3096 的 6.10.4 宏替换](https://www.open-std.org/jtc1/sc22/wg14/www/docs/n3096.pdf)。

`CMETA_STATIC_ASSERT(condition,message)` 用于声明位置；
`CMETA_CONST_REQUIRE(condition)` 用于表达式位置，成功值为整数零。
条件必须是编译期常量，假值使编译失败。`CMETA_TYPE_MATCHES(expr,T)` 不求值：
C11 使用 generic selection 的转换后类型，C++17 使用 `decltype`；
`CMETA_TYPE_IS_VALUE(T)` 要求 T 在参数退化前后是同一无顶层限定的值类型。

完整的 C 函数声明示例（定义放在同一 TU）：

```c
#include <cmeta/invoke_decl.h>
FunctionInvokeDecl(value, int, increment, (int, input, CMETA_PARAM_IN));
int increment(int input) { return input + 1; }

int main(void) {
    int input = 4, output = 0;
    void *params[] = { &input };
    return FunctionInvoke(increment)(&output, params, 1) && output == 5 ? 0 : 1;
}
```

`FunctionInvokeDecl` / `Function0InvokeDecl` 对应既有 inferred 声明。
`FunctionInvokeDeclAsAbi[Result]` / `Function0InvokeDeclAsAbi[Result]`
与同名去掉 `Invoke` 的既有声明使用相同实参顺序，生成同一份 FunctionDesc/FunctionAbi。
C++17 使用显式 `AsAbi` 形式及五字段参数行。返回 ABI carrier 必须是 canonical
`CMETA_ABI_VOID/SCALAR/OBJECT_POINTER/AGGREGATE/FUNCTION_POINTER/OPAQUE/ENUM`
token（允许宏别名）；不接受任意 carrier 表达式或 UNSPECIFIED。

`FunctionInvoke(name)(return_storage, params, param_count)` 返回 bool。
错误数量、缺失参数数组、任一缺失参数对象、非 void 缺失返回对象时返回 false，
不执行 native 函数；成功恰好执行一次，void 返回不要求返回存储。
参数数量为 0 时允许 params 为 NULL。参数与返回对象必须具有声明中的精确类型、
对齐和有效生命周期；普通 `void *` 不能检查这些调用方前置条件。
指针参数也需要一个指针对象的地址，允许该对象保存 NULL。
数组、函数 typedef 和顶层 const/volatile 参数不能直接作为 carrier；
使用其精确的无顶层限定值类型或显式指针类型。受指针指向的 const/volatile 不受影响。

生成器只借用存储，不分配、不 retain、不转移所有权。Function 的 result flags
仍是语义事实源，Plugin 描述符和 thunk 的有效期仍受 live lease 约束。
外来描述符继续完整 admission；生成 thunk 不解释 Reflection，不执行 ABI 猜测。

### 固定布局的 container_of 与 cleanup 能力（#976）

`<cmeta/container_of.h>` 的 `cmeta_container_of_as(ptr,Owner,MemberType,member)`
从成员地址恢复 `Owner *`。它同时检查 `Owner.member` 和传入指针的精确原生类型，
包括 cv 限定与数组范围；`ptr` 只求值一次。C11（包括 MSVC C）使用这个显式类型入口。
`CMETA_HAS_CONTAINER_OF` 为 1 时还提供 `cmeta_container_of(ptr,Owner,member)`，
通过已有原生 typeof 推导 MemberType，复用同一检查与地址计算。
不支持推导的编译器不定义三参数入口，不提供无检查替代。

Owner 必须是实际存活、固定地址的外围对象类型；C++ 要求 standard-layout。
ptr 必须非 NULL，且恰好指向这个对象中指定成员；类型相同不代表成员来源正确。
位域、柔性数组、动态字段和过期指针不在契约内。数组成员须使用数组 typedef 并传整个数组的地址。
只读或 volatile 对象显式传 `const Owner` / `volatile Owner`，MemberType 带相应限定；
仅有 const 成员的指针不能证明整个对象可写，调用者不能借此声明一个不真实的可写 Owner。
没有分配、引用计数、状态迁移或线程同步，时间和额外空间均为 O(1)，借用在原对象移动或销毁时失效。
错误原生类型在编译期拒绝；对象来源和生命周期属于调用前置条件，不进行运行时猜测。

`CMETA_HAS_CLEANUP` / `CMETA_ATTR_CLEANUP(function)` 只探测和封装原生 cleanup attribute。
函数接收自动变量的地址，负责该资源自己的清理；没有隐含 free、retain 或返回值处理。
不支持的编译器令能力值为 0，且不定义 attribute 宏。普通返回和块退出可用于 lexical cleanup；
longjmp 等非局部退出不属于此入口的保证，异常清理还取决于后端和异常编译选项。
MSVC 的结构化 scope 入口继续保持相同资源语义，不将缺失 attribute 解释为不清理。
平台机制参见
[GCC cleanup 文档](https://gcc.gnu.org/onlinedocs/gcc/Common-Variable-Attributes.html)。

这些入口均为增量能力，不改变 Reflection ABI；既有 descriptor 与 scope 不会隐式改用它们。
成员地址布局基于标准 offsetof，编译器机制见
[GCC offsetof 文档](https://gcc.gnu.org/onlinedocs/gcc/Offsetof.html)。

Interface 的 R/V/F/FR/FV/D/FD 行先归一化为参数 tuple、arity、结果动作和 Reflection
属性，再由公共生成器输出 vtable、wrapper、验证与 metadata。析构仍先调用再清空 handle。
验证覆盖 C/C++ 的所有 0–4 参数行、16 项 PP 上界、零项、错误展开和精确 native 调用。
迁移只需要显式选择新声明；撤回声明层时可恢复手写 thunk，无数据迁移或运行时格式变化。
Plugin 的可选跨 TU linker 聚合与 C/C++ lease 作用域入口见
[Plugin 声明与生命周期协议](../plugin/README.md)。共享 section 原语位于 `cmeta/compiler.h`，
平台聚合和 lease 所有权仍归 Plugin；CMeta descriptor 不持有 lease。
通用 capture/bind 与通用生命周期 guard 不在这两个 Plugin 入口的范围内。

### Explicit lifetime and parameter binding

`<cmeta/lifecycle.h>` 提供 `cmeta_lifecycle_binding`：先用 `cmeta_lifecycle_admit` 验证
canonical DataDesc，再重复 init/move/restore。`<cmeta/cleanup.h>` 把 Data、ObjectRef、
Plugin 各自的释放 authority 适配为有限 lexical obligation；`<cmeta/object_scope.h>` 提供
ObjectRef C adapter 和不可复制的 C++ `cmeta::object_scope`。初始化失败回滚、逆序清理、
provider 寿命及参数/返回错误约定见 [生命周期协议](LIFECYCLE_LOWERING.md)。

`<cmeta/bind.h>` 从显式行生成捕获和精确 thunk，例如：

```c
#include <cmeta/bind.h>

FunctionDeclAsAbiResult(value, int, &cmeta_type_int, CMETA_ABI_SCALAR, CMETA_RESULT_VALUE,
    add, (int, a, CMETA_PARAM_IN, &cmeta_type_int, CMETA_ABI_SCALAR),
         (int, b, CMETA_PARAM_IN, &cmeta_type_int, CMETA_ABI_SCALAR));
int add(int a, int b) { return a + b; }

FunctionBindDeclAsAbiResult(value, int, &cmeta_type_int, CMETA_ABI_SCALAR, CMETA_RESULT_VALUE,
    plus10, add, CMETA_SIG_U_I_I,
    (value, (int, a, CMETA_PARAM_IN, &cmeta_type_int, CMETA_ABI_SCALAR)),
    (arg, (int, b, CMETA_PARAM_IN, &cmeta_type_int, CMETA_ABI_SCALAR)));

int main(void) {
    plus10_capture capture = {10};
    cmeta_invokable call = CMETA_INVOKABLE_INIT;
    int input = 5, output = 0;
    const void *args[] = {&input};
    if (plus10_bind(&capture, &call) != CMETA_OK) return 1;
    if (cmeta_invokable_invoke_admitted(&call, &output, args) != CMETA_OK) return 1;
    return output == 15 ? 0 : 1;
}
```

`value` 是已注册平凡标量的 snapshot，`borrow` 是显式借用的 object pointer，`arg` 保留
调用参数。receiver 使用含 `CMETA_PARAM_RECEIVER | CMETA_PARAM_BORROWED` 的 borrow 行。
capture 总量在编译期限制为 `CMETA_CAPTURE_INLINE`，不得捕获 managed ownership；
剩余参数须匹配现有注册的一元/二元 signature。返回 flags 和全部未绑定参数契约保持不变。
更多边界和迁移说明见 [receiver 与参数投影](RECEIVER_OPERATIONS.md)。
