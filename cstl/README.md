# CSTL

**Typed C11 containers and algorithms with native compiled C implementations.**

CSTL is Salts's standard-library-style collection layer. CMeta supplies finite
typed facades and metadata, while CSTL owns the concrete data-structure
algorithms. The generated typed surface is thin; vector growth, hashing, tree
balancing, allocation, and other core algorithms remain ordinary compiled C.

This is a central example of the Salts model: modern typed syntax at the API
surface while preserving explicit ownership and native C execution underneath.

**Tags:** C11 · containers · generic-programming · algorithms · vector · hashmap · btree · range · stream

Link the installed CMake target and include the typed header (the aggregate
header includes it as well):

```cmake
target_link_libraries(my_target PRIVATE Salts::CSTL)
```

```c
#include <cstl/typed.h>
```

## One declaration is enough

For a single typed container:

```c
cmeta_type(List, IntList, int);
```

For several:

```c
cmeta_type(Vec, IntVec, int);
cmeta_type(List, IntList, int);
cmeta_type(HashSet, IntSet, int);
cmeta_type(HashMap, IntLongHashMap, int, long);
cmeta_type(Map, IntLongMap, int, long);
cmeta_type(BTree, IntTree, int, long);
```

No `implement(...)`, `DeclareContainers(...)`, or `ImplementContainers(...)` call is required or exposed for typed containers.

## Importing a C facade from a DLL

Windows C cannot put addresses of DLL-imported data into static initializers.
This affects element metadata in collection, range and method descriptors.
For all 13 standard container kinds, `cstl_typed_import` replays the ordinary
typed methods while borrowing the original module's immutable metadata:

```c
/* Producer: use the normal descriptor-explicit cmeta_type declaration, and
 * export functions returning its container, data and receiver-operation set. */
cstl_typed_import(Vec, SharedValues, Value,
                  shared_values_container(), shared_values_data(),
                  shared_values_methods());
cstl_typed_import(Map, SharedLookup, Key, Value,
                  shared_lookup_container(), shared_lookup_data(),
                  shared_lookup_methods());
/* Heap has no DataDesc or whole-container lifecycle provider. */
cstl_typed_import(Heap, SharedPriority, Value,
                  shared_priority_container(), shared_priority_methods());
```

| Kinds | Import arguments after kind/name | Capabilities |
|---|---|---|
| Vec, Deque, List, Stack, Queue, Set, HashSet | element C type, container/data/method queries | Typed methods, range, collector, reflection and container lifecycle |
| Map, HashMap, MultiMap, BTree, BPlusTree | key/value C types, container/data/method queries | Typed methods, key/value/entry ranges, collector, reflection and container lifecycle |
| Heap | element C type, container/method queries | Typed methods, range, collector and reflection, matching the local Heap declaration |

All query expressions must return non-NULL descriptors for the same wrapper
name, layout, kind and argument providers, produced by the normal explicit
declaration without an additional wrapper identity argument (Heap uses its
normal two-argument declaration). The producer must
finish any graph initialization before returning them. Metadata and provider
code remain borrowed: keep the module loaded until every value, range,
collector and metadata borrow has ended. This is a linked-library contract,
not admission of an unchecked dynamically loaded plugin.

Typed methods, ranges, collectors, lifecycle traits, structured construction
and method-reflection accessors retain their normal results and errors. Use
`Name_cmeta_descriptor()`, `Name_cmeta_data()` and
`Name_receiver_operation_set()` to query imported metadata; no header-local
collection/map descriptor objects are emitted. Map entry ranges use the
producer's entry TypeDesc. Existing `cmeta_type` declarations are unchanged.
Importing does not add missing argument traits: hash keys still need HASH and
EQUAL, ordered keys need COMPARE, and managed elements need their normal value
lifecycle. Existing capacity errors and failed-collector cleanup also apply.
Local and imported facades replay the same kind rows and reflection filters,
including both Deque insertion ends and all three List insertion names.

For a C++ consumer, declare the same wrapper with `cstl_typed_decl` and expose
the producer's descriptor query functions with `extern "C"` linkage. The
layout-only declaration owns no metadata; use the producer's DataDesc,
container and operation callbacks to manage its values. The shared-library
tests cover C and C++ consumers for all 13 kinds, including wrapper and map
entry layout, owned-value copy/move, range lifetimes and collector rollback.
Heap retains its range/collector contract without whole-container copy/move.

The import path uses the existing descriptor ABIs. Runtime queries were chosen
over mutable lazy descriptor copies or nullable static metadata because those
would add publication races or lose object-independent inspection. Upgrade the
SDK headers and rebuild consumers to adopt it; reverting requires regenerating
or changing consumers back before removing the new macros.

## Owning nested values

The descriptor-explicit `cmeta_type` forms attach whole-container COPY, MOVE and
DESTROY traits to the wrapper's own TypeDesc. These traits call the canonical
`Name_cmeta_data()` lifecycle; they introduce no separate copying algorithm or
ownership registry. Pass that exact TypeDesc and DataDesc as the outer element
or value provider. Copy constructs independent inner storage, a failed copy
destroys its partial construction without publishing an element, and move
leaves the source in the provider's semantic zero. Each wrapper owns its raw
storage and delegates each retained element's lifetime to its declared provider.

Vec and Map also accept a final `const cmeta_type_identity *` argument after
their explicit providers. Supply an APPLY identity using `stl_vec_generic_desc`
or `stl_map_generic_desc` and the exact argument providers' semantic identities.
The wrapper TypeDesc, Range metadata, and DataDesc then share that identity.
Construct any descriptor-owned argument references before publishing the graph,
from inner to outer, under the application's once initialization or while the
graph is exclusively owned. Identities and argument arrays must outlive all
values and remain immutable after publication; item operations do not initialize
or repair metadata. The declaration does not add comparison or hashing traits
for whole containers, so an outer Set or map key still requires a provider that
actually supplies its required operation traits.

```c
static const cmeta_type_identity *int_vec_args[1];
static const cmeta_type_identity int_vec_id =
    CMETA_TYPE_ID_APPLY_INIT(&stl_vec_generic_desc, int_vec_args);
cmeta_type(Vec, OwnedInts, int, &cmeta_type_int, &cmeta_data_int, &int_vec_id);
static const cmeta_type_identity *const matrix_args[] = {&int_vec_id};
static const cmeta_type_identity matrix_id =
    CMETA_TYPE_ID_APPLY_INIT(&stl_vec_generic_desc, matrix_args);
cmeta_type(Vec, Matrix, OwnedInts, &OwnedInts_cmeta_type,
           &OwnedInts_collection_data, &matrix_id);

/* Call exclusively during startup, before exposing either descriptor. */
static bool prepare_matrix_metadata(void) {
    int_vec_args[0] = cmeta_type_identity_of(&cmeta_type_int);
    return cmeta_data_desc_valid(Matrix_cmeta_data()) &&
           cmeta_data_value_copy_supported(Matrix_cmeta_data()) &&
           cmeta_data_value_move_supported(Matrix_cmeta_data());
}
```

This extends declaration metadata without changing wrapper or descriptor ABI.
Storage-only declarations remain storage-only; callers must provide complete
canonical data providers to declare owning nested values. Missing lifecycle or
ordering capability is an error at the admitting boundary. Generated consumers
must regenerate and rebuild against the SDK providing this capability. The
alternative of wrapping an inner container in a synthetic record or privately
copying its raw handle would introduce a second lifetime authority and is not
used. Reverting the feature requires reverting its dependent generated consumers
before rebuilding the SDK; live owned objects must be drained before replacement.

## Self-describing raw handles

PR #53's declaration and expression initializers remain supported:

```c
Vec(int, declared_values);
Map(int, long, declared_scores);

vec_t values = VecOf(int);
map_t scores = MapOf(int, long);
cstl_stack_t pending = StackOf(int);
```

`cstl_stack_t` is the canonical raw Stack handle. CSTL does not define a
`stack_t` alias because that identifier belongs to platform/system namespaces
on POSIX-derived targets. Portable code must use `cstl_stack_t` or the
`Stack(T, name)` declaration facade.

These forms bind CMeta descriptors to erased CSTL handles without
allocating storage or generating a concrete C type. They are raw-handle
initializers, not additional CMeta Generic instantiations. Initialize them with
the ordinary raw operations and destroy them after use:

```c
if (vec_init(&values, 8u) != STL_OK)
    return 1;
vec_destroy(&values);
```

The `*Of(...)` compound literals also remain valid in assignments, returns,
and function arguments. Once an initialized raw handle owns storage, it must
not be copied by value.

## What is header-only?

Only the generated typed layer:

```text
IntList wrapper type
IntList_* thin forwarding functions
container descriptor
Range factories / traits
```

The forwarding functions are `static inline`, and descriptors are header-local metadata. The actual algorithms remain compiled C in CSTL sources:

```text
vector growth
list allocation/linking
hash probing
heap operations
B-tree/B+tree balancing
```

So "header-only typed facade" does not mean "header-only container algorithms".

## Supported kinds

```text
Vec        Deque      List
Stack      Queue      Heap
Set        HashSet
HashMap    Map        MultiMap
BTree      BPlusTree
```

Ordered kinds obtain comparison from the registered CMeta type descriptor;
their typed declarations do not take a parallel comparator token. `Map` is an
ordered red-black-tree map and requires key `COMPARE`. `HashMap` is an independent
open-addressed hash table and requires key `HASH` and `EQUAL`. `List` is an
independent node-based doubly-linked list with stable iterators across insertion.

## Typed operations

A `cmeta_type(...)` declaration creates one concrete C type and its ordinary
`Type_*` API:

```c
cmeta_type(List, IntList, int);

IntList values = {0};

IntList_init(&values, 100u);
IntList_add(&values, 10);
IntList_push_back(&values, 20);
IntList_destroy(&values);
```

The same API is used from every ordinary `.c` translation unit. CSTL does not
provide receiver syntax or a generic operation namespace. Operations stay
direct, static-inline typed C calls over the compiled CSTL algorithms.

Canonical ownership transfer is available through CMeta DataDesc:

```c
IntList destination = {0};

if (cmeta_move(IntList, &destination, &values) != CMETA_OK)
    return 1;
```

A successful move leaves `values` in canonical semantic-zero, so it remains
safe to destroy or reinitialize. No source translator or compiler-private
LIVE/MOVED state is required for lifecycle correctness.

## TinyTest equality bridge

`<cstl/tinytest.h>` supplies pointer-based TinyTest assertions for a finite
set of explicitly declared typed containers. It never copies a storage-owning
CSTL handle. Define a comparator for each test type, then pass its type name to
`check_cstl_equal`:

```c
#include <cstl/typed.h>

cmeta_type(Vec, IntVec, int);

#include <cstl/tinytest.h>

CSTL_TINYTEST_DEFINE_SEQUENCE_EQUAL(IntVec, int)

/* ... after IntVec values have been initialized ... */
check_cstl_equal(IntVec, actual, expected);
```

`CSTL_TINYTEST_DEFINE_SEQUENCE_EQUAL` requires an ordered Range and the
element `EQUAL` trait. `CSTL_TINYTEST_DEFINE_ORDERED_MAP_EQUAL` compares both
keys and values as ordered pairs, so it supports `Map`, `MultiMap`, `BTree`,
and `BPlusTree` when their key and value types provide `EQUAL`; duplicate keys
remain significant by traversal order. `HashMap` is unordered and is rejected
by these comparators. The type arguments must be the same types used in the
`cmeta_type(...)` declaration; the bridge validates CMeta type identity when it is
available and always validates destination size and alignment before reading a
Range value.

## CFlow bridge

Link `Salts::CSTLStream` for the optional modern container-operation facade.
Its purpose is to make filtering, transformation, aggregation, and typed
collection convenient for CSTL containers without introducing another
container implementation. Familiar fluent names improve readability, but no
external Stream API defines this facade's lifecycle, reuse, terminal, ordering,
or parallel-execution semantics.

The facade is a reusable typed CFlow Graph bound to a borrowed container Range,
not a single-use traversal object. Operators append nodes to that Graph;
evaluation creates fresh runtime and cursor state. A `REUSABLE` Range may be
evaluated again while its container remains alive and unmodified. `reduce` is a
Graph stage and collection remains an explicit terminal. Capacity arguments are
hard resource bounds: exceeding one fails the collection transaction instead of
silently truncating the result.

`Salts::CSTLStream` is a compiled bridge target. Its `stream(...)` helpers
bind the container Range and explicitly inject CSTL's bounded HashSet and
Vec state backends; the core `Salts::CSTL` target still has no CFlow
dependency.

Element predicates are already represented by CMeta callables. The callable
owns the typed signature and declared contract; CFlow owns traversal, predicate
invocation, value lifetime, and Graph execution. For example:

```c
cmeta_function(filter, value, bool, keep_even, (int value)) {
    return value % 2 == 0;
}

cflow_stream pipeline = {0};
stream(&values, &pipeline)
    ->filter(&pipeline, keep_even)
    ->distinct(&pipeline, 64u)
    ->sorted(&pipeline, 64u)
    ->skip(&pipeline, 1u)
    ->take(&pipeline, 10u);
```

Captured predicates use `lambda(filter, ...)` or `cmeta_bindable(filter, ...)`
and enter the same Graph as immutable callable values. Filtering should not be
reimplemented separately for `Vec`, `List`, `Set`, or other container kinds.

`skip(n)` and `take(n)` are positional CFlow Graph nodes rather than terminal
capacity options. They count values at their exact pipeline position and keep
encounter order. `take(n)` short-circuits upstream after its bound;
`take(0)` does not pull the source. Every evaluation owns fresh counters, so a
reusable unchanged Range can execute the same sliced Graph again. Interpreted
collection supports managed element traits. Direct compilation currently
accepts slice-only trivial-value graphs; mixing a slice with callable nodes
fails explicitly until the plan executor can preserve lazy short-circuit and
error semantics.

`distinct(max_unique)` keeps first-encounter order. The nonzero argument limits
unique retained values, not source items. A duplicate is filtered without
growing state; the next new value after the bound returns
`CFLOW_STATUS_CAPACITY_EXCEEDED`. Element descriptors must provide `HASH` and
`EQUAL`; managed elements are copied into the HashSet and destroyed when the
Run closes. Direct/compiled plans reject this stateful operator explicitly.

`sorted(max_elements)` uses a bounded CSTL Vec plus the stable
`O(n log n)` sort implementation. It buffers the complete upstream, requires
`COMPARE` plus lifecycle traits, preserves encounter order among equal values,
and emits no partial result if the hard element bound is exceeded. Managed
elements remain independently owned throughout collection and Run teardown.
Direct/compiled plans reject this materializing operator explicitly.

New convenience operations follow the same ownership boundary:

- An element operation such as a new transform or selection rule belongs in
  CFlow as a typed operator accepting a CMeta callable.
- A result operation such as counting, matching, or finding belongs in the
  terminal/collector layer, with explicit short-circuit, ownership, error, and
  capacity semantics.
- CSTL remains responsible for container storage and Range/collector
  adapters; it does not duplicate the CFlow algorithm for each container kind.

Names alone do not create API. A new operation is public only after its Graph or
terminal semantics, managed-value lifetime behavior, failure contract, and
tests are all present.

The common result operations are available through prefixed helpers so they do
not collide with generated container methods:

```c
size_t selected = 0u;
bool has_even = false;
cstl_find_result first = {0};
const char *error = NULL;

cstl_stream_count(&pipeline, &selected, &error);
cstl_stream_any_match(&pipeline, keep_even, &has_even, &error);
cstl_stream_find_first(&pipeline, &first, &error);
cstl_find_result_destroy(&first);
```

Structured variants such as `cstl_stream_count_result` and
`cstl_stream_any_match_result` return the shared allocation-free
`cstl_status_result`. `cstl_status_result_message` returns canonical
library-owned static text; existing `bool + out_error` helpers remain available
for detailed compatibility diagnostics.

`cstl_stream_all_match` and `cstl_stream_for_each` complete the same
terminal family. Empty input produces count zero, any false, all true, no first
value, and no visitor calls. Matching and finding short-circuit the current
execution; count and visiting require source completion. `find_first` owns an
independent copy, including for managed element types. Predicate values retain
their CMeta typed-callable validation, while a `for_each` visitor borrows each
element only for its callback duration.

Sequence/set wrappers derive Range information in the same declaration, so
CFlow can bind an initialized typed object directly:

```c
cflow_stream s;
stream(&values, &s);
```

Associative types use explicit descriptor views:

```c
stream_keys(&map, &s);
stream_values(&map, &s);
stream_entries(&map, &s);
```

Collection names its output type and retains an explicit capacity bound:

```c
IntList output = {0};
cstl_collect_result result =
    to_list_typed(&s, IntList, &output, 100u);
```

`collect_typed(&s, Type, &output, limit)` has the same typed terminal contract.
The generated public collector takes `Type *`, so a mismatched output wrapper
is diagnosed before CFlow receives its erased collector context.

Self-describing raw outputs retain the three-argument terminal:

```c
list_t raw_output = ListOf(int);
cstl_collect_result raw_result = to_list(&s, &raw_output, 100u);
```

Trivial-copy streams can instead return an owned bounded byte result:

```c
cflow_result bytes = {0};
cstl_status_result byte_status =
    to_array_result(&s, 100u, &bytes);
if (cstl_status_result_is_ok(byte_status))
    cflow_result_destroy(&bytes);
```

`to_array_result()` distinguishes capacity, unsupported lifecycle, allocation,
source admission, and Runtime failure. The legacy `to_array()` remains its
`bool` compatibility projection. Both clear the result on failure.

The erased form validates its descriptor at runtime; the typed form provides
the stronger compile-time output pointer check. Collection is transactional.
On failure, a generated wrapper is reset to zero. An erased raw handle releases
its storage while retaining the descriptor and type binding supplied by its
declaration or `*Of(...)` initializer, so it can be initialized again.
`cstl_collect_result.flow_status` classifies Range admission and CFlow
Runtime failures; `status` remains the exact CMeta Collector status, and
`count` remains the accepted transaction count. For example, a bounded
`sorted()` Runtime overflow reports `CFLOW_STATUS_CAPACITY_EXCEEDED` together
with the Collector's abort status, while output-collector overflow reports
`CMETA_CAPACITY_EXCEEDED` directly. `error` remains a borrowed diagnostic and
must not be freed or retained beyond its documented source lifetime.
The core `Salts::CSTL` target does not depend on CFlow.

### Worker-scheduler terminal

`collect_async_typed()` submits the complete pipeline to an existing CFlow
worker scheduler. CSTL does not create or own another thread pool. Multiple
independent handles may share that scheduler; one handle remains ordered and
its Collector is never invoked concurrently. This is pipeline-level
concurrency, not implicit per-element parallelism.

The input container, output container, and scheduler are borrowed through
execution destruction. Cleanup order is therefore execution, scheduler,
output, Stream Graph, then input. Capacity is still a hard transaction bound:
failure or cancellation aborts the output instead of publishing a partial
container.

```c
#include <cstl/stream.h>

cmeta_type(List, AsyncIntList, int);

int main(void) {
    AsyncIntList input = {0};
    AsyncIntList output = {0};
    cstl_stream_t pipeline = {0};
    cstl_stream_execution_t execution = {0};
    cflow_stream_execution_snapshot snapshot = {0};
    cflow_scheduler workers = {0};
    int exit_code = 1;

    if (AsyncIntList_init(&input, 2u) != STL_OK ||
        AsyncIntList_push_back(&input, 10) != STL_OK ||
        AsyncIntList_push_back(&input, 20) != STL_OK ||
        stream(&input, &pipeline) == NULL ||
        !cflow_scheduler_worker_init(&workers, 2u))
        goto cleanup;

    if (collect_async_typed(&execution, &pipeline, &workers,
                            AsyncIntList, &output, 2u) !=
            CFLOW_STREAM_EXECUTION_OK ||
        cflow_stream_execution_wait(&execution) !=
            CFLOW_STREAM_EXECUTION_OK ||
        !cflow_stream_execution_get_snapshot(&execution, &snapshot) ||
        snapshot.state != CFLOW_STREAM_EXECUTION_COMPLETED)
        goto cleanup;
    exit_code = 0;

cleanup:
    (void)cflow_stream_execution_destroy(&execution);
    if (cflow_scheduler_valid(&workers))
        cflow_scheduler_destroy(&workers);
    AsyncIntList_destroy(&output);
    cstl_stream_destroy(&pipeline);
    AsyncIntList_destroy(&input);
    return exit_code;
}
```
