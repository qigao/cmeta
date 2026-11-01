# CMeta v50

声明生成新增入口：有限 PP 原语见 [`pp.h`](include/cmeta/pp.h)，编译器约束见
[`compiler.h`](include/cmeta/compiler.h)，精确函数 thunk 见
[`invoke_decl.h`](include/cmeta/invoke_decl.h)。使用与边界详见
[`LANGUAGE_REFERENCE.md`](LANGUAGE_REFERENCE.md#有限宏与精确调用声明)。

**Finite generic programming, typed metadata, and semantic code generation for strict C11.**

CMeta is the semantic foundation of Salts. It uses standard C11 techniques such
as finite macro specialization and generic selection where appropriate to make
strongly typed APIs practical without introducing a managed runtime or changing
native C object layout. It provides type identity, metadata, traits, interfaces,
contracts, ranges, typed callables, and finite compile-time computation.

CMeta is intentionally finite and schema-driven. It does not try to reproduce
unrestricted C++ templates or invent a universal replacement language before
concrete use cases require it.

**Tags:** C11 · generic-programming · metadata · reflection · traits · interfaces · contracts · code-generation

The authoritative syntax and layering contract is
[`LANGUAGE_REFERENCE.md`](LANGUAGE_REFERENCE.md). Compiler/tooling reflection
inspection follows [`INSPECTION.md`](INSPECTION.md): immutable descriptors plus
canonical semantic queries, without a second meta object model or runtime RTTI.

[`FASTPATH.md`](FASTPATH.md) specifies bounded C11 static keys and typed static
calls, their publication/provider lifetimes, and explicitly enabled x86_64 /
AArch64 assembly backends. Portable atomics remain the default implementation.

## Public programming model

Application users normally use semantic DSLs:

```c
cmeta_struct(User,
    cmeta_field(int, id)
    cmeta_field(double, score)
);

cmeta_enum(State,
    (READY, "ready"),
    (DONE,  "done")
);

cmeta_traits(User,
    (equal, user_equal),
    (hash, user_hash),
    (copy, user_copy)
);

cmeta_type(Option, MaybeUser, User);
cmeta_type(Pair, UserScore, User, double);
```

Trait rows are the only supported public `cmeta_traits` declaration form. CMeta derives
both capability flags and function slots from those tagged rows. Positional
`Traits(name, flags, ...)` compatibility has been removed.

Named `cmeta_trait(Capability, callback)` rows, compile-time trait/field
requirements, generated tagged variants and distinct unsigned64 flags reuse
this canonical metadata and DataDesc lifecycle. See
[capabilities, variants and flags](CAPABILITIES.md) for contracts and executable
C11/C++17 examples.

`cmeta_type(...)` is the finite-generic type declaration entry point. CMeta owns value kinds
such as `Pair`, `Tuple`, `Option`, and `Result`. Container kinds such as `List`,
`Vec`, and `HashMap` are provided by `container`, not by the CMeta aggregate
header.

`cmeta_struct` and `cmeta_field` generate layout and Reflection metadata from
one declaration. Tuple rows remain framework machinery, not the canonical
application field syntax.

已有 C/C++ 类型可通过 `<cmeta/data_reflect.h>` 外部声明字段，原生头文件无需
包含 CMeta。`cmeta_reflect_data` 复用 Struct 的布局验证与元数据生成器，并从
同一字段列表生成布局、字段数据语义和 DataDesc：

```c
#include <cmeta/data_reflect.h>

typedef struct Account { int id; double balance; } Account;
cmeta_reflect_data(Account, "app.Account",
    cmeta_field(int, id)
    cmeta_field(double, balance)
);

/* StructMeta(Account): layout; cmeta_reflected_data(Account): DataDesc. */
```

参数依次为原生类型的单一标识符（带命名空间或复杂类型先用 typedef/using
取别名）、非空 stable ID 字符串字面量，以及 1–16 个字段声明；宏用于文件或
命名空间作用域，没有运行时返回值。它不重新定义原生类型。每个类型在同一 TU
只能生成一份布局，不与该类型已有的
`cmeta_struct` 声明叠加；头文件可被多个 TU 引用，描述符按语义比较。

- `cmeta_field(type, member)` 自动选择已有 `CMETA_DATAOF` 基础类型：bool、int、
  long、float、double 及其兼容 typedef。名称与偏移来自真实成员，默认字段 ID
  为 `"类型 stable ID.成员名"`。
- `cmeta_data_field(type, member, descriptor[, storage])` 显式指定数据语义和
  canonical storage descriptor。省略 storage 时使用 `CMETA_TYPEOF(type)`；
  自定义类型必须提供 storage，不能以 NULL 布局元数据代替。例如嵌套字段使用
  `cmeta_data_field(Child, child, cmeta_reflected_data(Child), cmeta_reflected_storage(Child))`。
  字符串/容器使用同一 provider 的 DataDesc 和 storage descriptor。两者必须是
  静态对象地址；C11 静态初始化不允许通过 `descriptor->storage_type` 求值。
- `cmeta_data_field_id(type, member, stable_id, descriptor[, storage])` 保留历史
  字段 ID；反射字段名仍为当前成员名，ID 必须在该视图内唯一。

未知基础类型、声明与真实成员类型不符、重复成员及 C++ 非标准布局类型在
编译期拒绝。位域不能取地址/偏移，也不能用于此入口。显式 descriptor 必须是
具有静态存储期的不可变 `const cmeta_data_desc` 对象地址，并准确描述该成员的
存储和语义；指针不自动解释为字符串、借用或所有权。生成的视图不会授予字段
写权限，也不会接管 Plugin/provider 生命周期。字段与对象的借用有效期和线程
同步责任仍由调用方承担；非标准布局类走既有成员访问器。volatile 字段拒绝
编译；const 字段可通过显式 descriptor 和 storage 纳入只读视图。

`cmeta_reflect_data` 默认是只读投影视图，允许省略字段；即使存储是 POD，也不
授予 semantic init/copy/move/restore/is_zero 能力，对应查询返回 false，操作
返回 `CMETA_TRAIT_MISSING`。读取仍通过 `cmeta_object_borrow` 和
`cmeta_object_field_read`；资源生命周期、锁和对象不变量仍归原生 owner。

确需字段级值生命周期时，使用同参数的 `cmeta_reflect_value`。这是调用方对
**完整字段列表和字段级零值/复制/移动/清理语义**的显式承诺，C11 无法自动发现
未声明字段。它拒绝 const/volatile 字段，C++ 还要求 trivial、standard-layout、
非 union 类型；带构造/析构不变量的类使用只读视图和原生 provider。
字段仍使用各自 provider 的复制、转移和清理协议，声明本身不分配资源。
目标存储须处于语义零值，访问默认单线程或由调用方外部同步。嵌套只读视图不会
被父记录升级为可构造值；缺少所需能力时返回 `CMETA_TRAIT_MISSING`。

**设计与兼容性决策。** 单凭 standard-layout 不能推导生命周期授权；仅将
操作指针置空也不能关闭既有 v1 STRUCT 的字段级生命周期。为保持手写 v1
描述符行为，新增反射使用 DataDesc 版本 2，仅用于 STRUCT；其 shape 为
`cmeta_data_reflection_shape`，首成员保留既有 `cmeta_data_struct_shape` 读取视图，
尾部显式记录 VIEW/VALUE 模式。既有描述符布局与 Reflection ABI epoch 不变。
旧运行时拒绝版本 2，不会忽略只读限制；运行时和生成头文件需一起更新。

相较运行时注册表，该方案无元数据分配、无可变缓存、无初始化顺序依赖。
在既有 descriptor/object admission 边界交叉校验布局与数据字段的大小、对齐、
canonical 类型和偏移，拒绝重复 ID；VALUE 还拒绝字段重叠。非法元数据使
`cmeta_data_desc_valid` 返回 false、`cmeta_object_borrow` 返回
`CMETA_INVALID_ARGUMENT`，不会产生对象副作用。验证成本 O(n²)、辅助空间 O(1)，
n 上限 16；不以未经测量的缓存增加状态归属复杂度。

高频重复读取可先调用 `cmeta_object_field_bind`，检查成功后复用
`cmeta_object_field_read_admitted`，避免每次进行完整元数据校验。binding 不保留
对象、描述符或 provider lease；这些对象必须在使用期间有效，且 binding 不可
篡改。需要读取动态字段时仍由既有 provider 每次提供当前借用视图。
[读取基准](benchmarks/cmeta_data_reflect_benchmark.c) 对同一 16 字段记录比较 v1、
v2 校验读取和 v2 绑定后读取；绑定准备放在计时外，结果不代表端到端吞吐。
Windows VS 开发环境中可复验：

```sh
cmake --build --preset win-release-user --target cmeta_data_reflect_benchmark
ctest --preset win-release-user -R "^cmeta_data_reflect_benchmark$" -LE "^$" -V
```

迁移时保留 stable ID、原生对象布局和 provider，只读声明保持原入口；原先依赖
该新增宏隐式生命周期的代码需审计字段完整性后改为 `cmeta_reflect_value`，并为
自定义字段提供 storage。回滚时还原相应声明及匹配运行时，不将只读描述符强制
改为 v1。C11/C++17 的 [运行测试](tests/cmeta_data_reflect_cases.h) 覆盖只读能力、
嵌套指纹、非法元数据及跨 TU 一致性；[provider 测试](tests/cmeta_data_reflect_provider_cases.h)
覆盖字符串资源、固定数组、失败回滚和释放计数。

`cmeta_scope` owns an explicit finite resource set and runs a status-returning
body function. Native early returns from that function still reach generated
LIFO cleanup. Scope construction and cleanup use the same canonical DataDesc
construct ops through a generated native-typed static lifecycle accessor.
`cmeta_scope_checked` explicitly admits hand-written/runtime descriptors through
the existing checked lifecycle binding; it uses the same cleanup lowering.
There is no automatic fallback or second lifecycle registry. See
[structured scope and migration](LANGUAGE_REFERENCE.md#structured-scope).

Canonical lifecycle facts automatically select lowering for `(Type, value)`
rows: trivial storage has no callbacks or ops/live state; managed nofail storage
has no partial-init state; fallible storage uses nested rollback control flow.
Admitted lifecycle/invokable/field bindings
support repeated use without validating immutable metadata graphs again.
Data, ObjectRef and Plugin adapters share finite lexical cleanup obligations;
their existing resource authorities remain separate. See
[lifetime admission and lowering](LIFECYCLE_LOWERING.md).

Bindings are caller-trusted validated borrowed records, not unforgeable security
capabilities. Use only successful, unmodified bind/admit results and keep the
canonical provider/outer Plugin lease alive through all uses and cleanup.
`cmeta_scope_nofail` remains an explicit assertion using the same lowering;
ordinary scope already selects this path for declared INIT_NOFAIL types.
Body failures and C++ exceptions still perform LIFO cleanup.

`<cmeta/data_select.h>` provides `cmeta_data_of(pointer)` for builtin Data
descriptors and `cmeta_data_of_in(pointer, schema)` for explicit local schemas.
The pointer is an unevaluated type witness. Both C11 and C++17 select the same
canonical descriptor, reject unknown/volatile pointer types, and evaluate only
the selected descriptor expression once. See [typed selection](LANGUAGE_REFERENCE.md#schema-driven-data-selection).

Optional `CMeta::CMetaNative` specializes admitted `int(int)` calls and borrowed
`int(void *, int)` receiver bindings as bounded leaf thunks. Enable
`CMETA_BUILD_NATIVE_THUNKS` explicitly on x86-64 Windows/Linux; the default is OFF.
Redirection requires quiescence and never extends provider/Plugin lifetimes.
See [native ownership, qualification and benchmarks](NATIVE_THUNKS.md).

`<cmeta/bind.h>` generates receiver and ordinary parameter binding with explicit
scalar snapshots or borrowed pointers, exact native thunks, and canonical
Function projections. Captures stay within `CMETA_CAPTURE_INLINE`; projected
signatures use the existing unary/binary registry. See
[parameter binding](RECEIVER_OPERATIONS.md#生成-receivercapturebind976-2729).

Execution/runtime ownership is explicit. Atomics and RCU are owned by
Salts::Concurrency. Pool storage/lease policy is owned by Salts::Core and
thread-affinity/TLS policy by Salts::Platform. CMeta retains only explicit
lifecycle adapters where DataDesc semantics add value:
`cmeta_pool_type(Name, Type)` and `cmeta_local_type(Name, Type)`.
See [ownership, capacity, suspension and memory-order contracts](EXECUTION_PRIMITIVES.md).

## Semantic string and byte storage adapters

`cmeta/data.h` keeps STRING/BYTES meaning separate from native storage layout.
A full `cmeta_data_desc` may point at versioned `cmeta_data_buffer_ops` that
define semantic-zero construction and detection, bounded assignment,
no-fail move into a zero destination, restore-to-zero, and an
optional bounded borrowed read view. The checked facade validates adapter ABI,
storage identity plus exact physical layout, owned/borrowed agreement, the
provider's returned pointer/size pair, and the caller's byte ceiling before
publishing read outputs.

Assignment is single-owner and failure-atomic: the destination must start in
the provider's semantic zero state, and a failed assignment is restored to
that state. The adapter is format-neutral and does not interpret UTF-8 or
source-reader lifetimes. A format binder must enforce those rules before it
passes a byte slice to CMeta.

A successful read does not copy or extend ownership. Its view expires when the
source object is mutated, assigned, restored, destroyed, concurrently changed,
or otherwise invalidated by the provider. The v2 lifecycle requires both
`init_zero` and `move`; v1 providers are rejected. Read remains optional and
an otherwise complete provider without it returns `CMETA_TRAIT_MISSING`.

`CMETA_DEFINE_FIXED_BYTES` supplies this lifecycle for a complete inline byte
type, alongside the exact native-value provider and copy/move/destroy traits.
Its extent must equal the native type's size and must be nonzero. Assignment
requires exactly that extent (`CMETA_TYPE_MISMATCH` for another length) and
obeys the byte budget (`CMETA_CAPACITY_EXCEEDED`). Read borrows the entire
inline extent, including an all-zero value; semantic zero does not mean an
empty span. Copy produces independent storage, move clears the source, and
restore is idempotent. These operations allocate nothing and require exclusive
access to mutated objects.

`<cmeta/fixed_array.h>` provides `CMETA_DEFINE_FIXED_ARRAY` for an inline array
typedef. It exposes `CMETA_DATA_SEQUENCE` with a constant element count and
contiguous borrowed iteration, while preserving the existing C array ABI.
Every slot is live even in semantic zero. Element zero initialization must be
allocation-free and no-fail; copy, move and cleanup dispatch to the element's
canonical provider. Copy failure releases the accepted prefix and any partial
element; move leaves every source element in semantic zero. A collector accepts
exactly the declared count, rejects a short finish with `CMETA_TYPE_MISMATCH`,
and rejects excess input or an insufficient item budget with
`CMETA_CAPACITY_EXCEEDED`. There is no extra native count field, metadata
allocation or ownership registry. Element payload allocation remains governed
by the element provider's byte budget.

For example, a consumer linked with `CMeta::CMeta` can use:

```c
#include <cmeta/fixed_array.h>

typedef int readings[3];
CMETA_DEFINE_FIXED_ARRAY(readings_value, readings, int, 3u, &cmeta_data_int,
                         "example.Readings3", "Readings3");

int main(void) {
    readings source = {1, 2, 3};
    readings destination;
    const cmeta_data_desc *data = &readings_value_cmeta_data;
    if (cmeta_data_value_init_zero(data, destination) != CMETA_OK) return 1;
    if (cmeta_data_value_move(data, destination, source) != CMETA_OK) return 1;
    cmeta_data_value_destroy(data, destination);
    cmeta_data_value_destroy(data, source);
    return 0;
}
```

`cmeta_data_value_is_zero` queries the canonical lifecycle zero without
changing ownership. A fixed array's zero means zero elements in every live
slot, rather than an empty borrowed range. Record queries cover reflected
fields; a consumer still owns any unreflected presence/default overlay bytes.
Unsupported zero providers return an error and never infer zero from native
pointer or descriptor identity. These additions use optional size-versioned
collection callbacks; older provider prefixes retain their existing behavior.

Salts Core provides header-local `tstr` and `vstr` adapter metadata in
`cmeta_cmeta_data.h`. As with other header-generated CMeta metadata, descriptor
addresses may differ across translation units; use semantic type comparison.

## Unified Schema / Replay kernel

Framework authors can define row schemas with:

```c
#define MyRows(M) \
    Schema(M, \
        (ROW_A, 1), \
        (ROW_B, 2))

Replay(MyRows, SOME_MAPPER)
```

`Schema` owns parenthesized-row unpacking. `Replay` applies a named schema to a
mapper. `Enum`, `Struct`, `Traits`, and CFlow's structured operator metadata
reuse this kernel internally.

Application code does not need a separate schema or batch declaration to
instantiate several generic types; write one `cmeta_type(...)` declaration per
concrete type.

## Finite compile-time computation

CMeta can name finite type and integer-constant relations without introducing
C++ template syntax:

```c
TypeFunction(CommonArithmetic,
    (int, int, int),
    (int, double, double),
    (double, int, double),
    (double, double, double));

ValueFunction(TypeRank,
    (int, 1),
    (double, 2));

Predicate(Hashable,
    (int, 1),
    (opaque, 0));

typedef TypeEval(CommonArithmetic, int, double) result_type;
enum { double_rank = ValueEval(TypeRank, double) };
Require(Hashable, int);
```

`TypeFunction` and `ValueFunction` infer unary through ternary arity from the
first row. `TypeEval` and `ValueEval` infer it from their input count, so callers
never select a numbered entry point. `Predicate`, `Satisfies`, and `Require`
provide boolean queries and compile-time constraints. A missing or conflicting
row is a compile error; there is no default mapping. Numbered public spellings
are not exposed.

Function names and input keys must each be one stable preprocessor identifier.
Each declaration accepts 1 through 16 rows; repeat a declaration with the same
function name to add bounded fragments. The first row determines arity and all
rows in the declaration must match it. Values must be integer constant
expressions. Use a stable typedef name for types or declarators that contain
commas or other preprocessing syntax.

Existing schemas can also be folded as constant expressions:

```c
#define FeatureChecks(M) Schema(M, (1), (1), (0))

enum {
    feature_count = SchemaCount(FeatureChecks),
    every_feature = SchemaAll(FeatureChecks),
    some_feature = SchemaAny(FeatureChecks)
};
```

`SchemaCount` accepts any non-empty row shape. `SchemaAll` and `SchemaAny`
require exactly one integer constant expression per row.

### Finite DFA inference

The same row list can also project a bounded C relation for admission-time
inference:

```c
#define CommonRows \
    (TYPE_SMALL, TYPE_SMALL, TYPE_SMALL), \
    (TYPE_SMALL, TYPE_WIDE, TYPE_WIDE)

ValueFunction(CommonType, CommonRows);
InferenceRules(common_type_rules, CommonRows);
```

`cmeta_infer_dfa_build` converts the explicit relation to a deterministic
prefix trie using caller-owned state and transition arrays. A successful DFA
query is bounded by the declared arity (1 through 3). Missing, duplicate,
ambiguous, invalid, and capacity failures are distinct and never select a
default result.

The DFA is a control-plane mechanism: build it during validation, admission,
or plan compilation, then store only the inferred type/action in the execution
plan. Do not query it for every data item. CMeta performs no hidden allocation;
the relation borrows its static rows and the DFA borrows caller workspace.

## Finite generic routing

Libraries register a finite generic kind. Concrete type declarations use:

```c
cmeta_type(kind, generated_name, type_arguments...);
```

`cmeta_type(...)` accepts only registered generic kinds and routes directly to
their finite `CMETA_TYPED_` provider. It does not fall through to callable or
operator DSLs.

Callable/function DSLs use their own explicit entry points such as CFlow's
`cmeta_function(...)`; generic type routing never falls through to them.

There is no `Containers(...)` batch DSL and no container `implement(...)`
generation phase.

## Type identity and type universes

CMeta distinguishes two finite type sets:

```text
known types      -> descriptors / reflection / generic type identity
callable types   -> generated callable signature families
```

This avoids expanding every reflected generic application into the full
callable Cartesian product.

`cmeta_type_desc` may carry a semantic `cmeta_type_identity`. Built-in atoms,
pointers, const forms, and generic applications compare through stable semantic
identity rather than descriptor address. Header-generated descriptor addresses
are therefore not process-global type IDs.

Legacy projects that define only `CMETA_TYPE_LIST` keep the historical behavior:
that list acts as both known and callable type universes. New code should use
`CMETA_KNOWN_TYPE_LIST` and `CMETA_CALLABLE_TYPE_LIST` when the distinction
matters.

The built-in five type rows and finite `8/2/1` unary, binary, and generator
relations have one source of truth:
`formal/cmeta_cflow_calculus/CMetaCFlowCalculus/CMeta/BuiltinSignatures.lean`.
Lean validates that manifest and generates
`include/cmeta/generated/builtin_signature_manifest.h`; do not edit the
generated header by hand. Regenerate or verify it from
`formal/cmeta_cflow_calculus`:

```text
lake exe cmeta-signature-gen --write ../../cmeta/include/cmeta/generated/builtin_signature_manifest.h
lake exe cmeta-signature-gen --check ../../cmeta/include/cmeta/generated/builtin_signature_manifest.h
```

The checked-in header means ordinary CMake builds and installed CMeta headers
do not require Lean. Application-defined `CMETA_USER_TYPE_LIST` and
`CMETA_USER_*_RELATION_LIST` macros remain manual shared configuration.

Signature lowering exposes `CMETA_VALUE_SIGNATURES(U, B)` for unary and binary
value callables, while `CMETA_GENERATOR_SIGNATURES(G)` names the generator
protocol. Runtime invoke/generate dispatch uses these protocol-specific groups;
the complete ABI surface continues to use `CMETA_ALL_SIGNATURES(U, B, G)`.
This removes protocol-unreachable generated branches without changing signature
IDs or rejection results.

## Multi-TU model

Generated wrapper functions are TU-local `static inline`, and generated
metadata may also be translation-unit local. Consumers must compare descriptors
semantically rather than requiring pointer equality across translation units.

These views and all reachable callbacks are borrowed from their provider. A
dynamic module must remain loaded until every descriptor/interface/callable
consumer finishes, including copies retained by CFlow graphs/plans. Before
exchanging native descriptors, negotiate `CMETA_REFLECTION_ABI_VERSION`; the
runtime query `cmeta_reflection_abi_version()` checks linked-library agreement.
Reflection epochs do not version callable configurations or application vtables.
See [native module lifetime and ABI rules](LANGUAGE_REFERENCE.md#reflection-across-native-modules)
for the bootstrap and unload contract and its dynamic-library regression test.

## Range metadata

CMeta provides an allocation-free borrowed `cmeta_range` protocol with traits
including:

```text
SIZED
ORDERED
SORTED
UNIQUE
CONTIGUOUS
RANDOM_ACCESS
REUSABLE
```

Concrete libraries such as `container` may expose Range views through their own
typed adapters and descriptors.

Read-only reflection uses the narrower CMeta Data borrowed cursors. Collection
cursors return one source-owned element pointer; map cursors return source-owned
key/value pointers without allocating, copying, or exposing a native iterator.
When a provider supplies a generation callback, any mutation after `begin`
terminates traversal with `CMETA_GEN_MUTATED`. Pointers remain valid only while
the source remains alive and unmodified.

## Transactional collectors

`cmeta_collector` is the bounded, single-threaded collection protocol for an
adapter that constructs a caller-owned, zero-initialized output. A concrete
container descriptor may expose an optional value-oriented factory:

```c
cmeta_collector (*collector)(void *zero_output, size_t limit);
```

A provider may instead expose the optional `collector_init` tail callback.
`cmeta_data_collection_collector` initializes it directly in the final caller
slot, allowing fixed arrays to borrow the collector's own count without
allocating a separate context. Such a collector must not be copied or moved
before termination. Its output and slot remain alive until finish/abort;
discarding it before begin retains no resources. The older factory remains
available for providers using the established value-oriented construction.

`begin` receives the input descriptor and hard item limit. `accept` borrows one
value only until the callback returns; the adapter must copy, retain, or move it
within that call. `finish` commits the output, while `abort` releases temporary
values and restores the output's documented zero state.

The stable state vocabulary is `ZERO`, `BEGUN`, `ACCEPTING`, `COMMITTED`, and
`ABORTED`. The façade performs no allocation, I/O, logging, retry, or
synchronization policy.

## Interfaces

CMeta supports small protocol/vtable interfaces:

```c
interface(Source, SOURCE_METHODS);
implements(Source, file_source, capabilities,
    .next = file_source_next,
    .close = file_source_close);
```

This is a `{ self, vtable }` protocol mechanism, not a class hierarchy.

An Interface is a typed protocol/capability view, not the canonical runtime
identity/lifetime container for an arbitrary native object. Runtime-selected
identity and BORROWED/SHARED/OWNED authority live in `cmeta_object_ref`.
Projecting one object into an Interface therefore creates a borrowed capability
relation over the same provider; it must not copy the native object or infer
ownership from `self`, the vtable layout, or descriptor equality.

Interfaces containing `CMETA_INTERFACE_METHOD_OWNS_SELF` are ownership-bearing
capabilities. A borrowed ObjectRef-to-Interface projection must reject them
unless ownership is explicitly transferred; otherwise Interface destruction
could invalidate an object while its ObjectRef still claims to be live.
`cmeta_interface_desc_has_owning_method()` is the canonical semantic query for
that admission decision. Static compile-time-known Interface calls remain direct
typed vtable dispatch and do not route through ObjectRef.

Interface method schemas have two reflection levels. Historical `R0..R4`,
`V0..V4`, and `D0` rows define the exact dispatch ABI only; their method
reflection does not invent FunctionDesc semantics. Frameworks that need shared
function semantics use `F0..F4`, `FV0..FV4`, or `FD0` rows and explicitly
provide contract, return descriptor/carrier, and five-field parameter rows.
Those methods expose canonical `cmeta_function_desc` and
`cmeta_function_abi_desc` through the interface metadata, so TinyMock, service
binding, and execution-admission consumers do not maintain an interface-only
signature registry.

## Layering principle

```text
ordinary C
       ↓ when repeated declaration/metadata boilerplate appears
Application DSL
       ↓ for reusable compile-time row generation
Framework DSL
       ↓ for execution/storage/interoperation
Runtime Protocol
```

Prefer composing existing CMeta mechanisms and ordinary C over adding new
language vocabulary prematurely.

Typed tracepoint、一次性 fault point 和 disabled fast path 的使用与生命周期见 [TRACEPOINTS.md](TRACEPOINTS.md)。


Portable discovery and ABI contracts are documented in
[STATIC_MANIFESTS.md](STATIC_MANIFESTS.md), [FINGERPRINTS.md](FINGERPRINTS.md)
and [COMPONENT_MANIFESTS.md](COMPONENT_MANIFESTS.md). Runtime ownership and installed
Reflection-only qualification follow [BOUNDARIES.md](BOUNDARIES.md).
