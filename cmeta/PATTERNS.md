# CMeta pattern layer

See [ACE/CMeta functional freeze contract](ACE_FREEZE.md) for the bounded
ownership model, N-to-N+1 fencing, executable acceptance and release separation.
See [POSA2 17-pattern coverage matrix](ACE_PATTERN_COVERAGE.md) for the exact
CMeta composition evidence, already-reused runtimes and post-freeze gaps.

> **Post-freeze development in progress:** on `feature/cmeta-ace-patterns`,
> the five typed composition slices Interceptor, ACT, Strategized Locking,
> Thread-Safe Interface, and Monitor Object now have branch code and tests.
> They are **not** part of the separately qualified `02b741a` ACE freeze;
> latest-HEAD CI, packaging and ABI decisions are still outstanding.
> The Pattern interfaces are `<cmeta/ace_interceptor.h>` and
> `<cmeta/ace_synchronization.h>`; ACT is a thin typed adapter over
> NativeIO in `<salts/native_io_ace_token.h>`. Monitor uses an existing
> CMeta Interface with Platform mutex/condition instead of a new runtime.

> The next public Salts release is **2.3.0** (latest published: 2.2.0).
> The planned 3.0.0 Release was withdrawn; the temporary 4.0.0 proposal was
> superseded. Component/Configurator and ACE patterns remain under
> [#1012](https://github.com/qigao/salts/issues/1012) on long-lived Draft
> [PR #1013](https://github.com/qigao/salts/pull/1013): **DO NOT MERGE**.

## Purpose

CMeta's pattern layer is a composition discipline over existing canonical CMeta
semantics. It is not a second language, a parallel object model, or a runtime
framework.

The authoring rule is:

```text
macro declaration / finite replay
        +
typed inline facade
        +
canonical immutable descriptor
        =
pattern authoring
```

Runtime mechanism remains with its existing owner.

The current canonical runtime consumer is the ACE-style Configurator /
ServiceHost composition in `CMeta::ACE`: Configurator owns the static graph;
ServiceHost owns published generations, Scope admission/drain and Plugin lease
settlement. `CMeta::Plugin` remains the sole dynamic loader and
unload authority. The installed runtime target is `CMeta::ACE`; Provider
declarations use `CMeta::Provider`. CHttp, TurboFlow and
TurboSCXML remain downstream conformance profiles, not sources of
consumer-private component semantics.

### Interceptor native signature and FunctionAbi admission (post-freeze)

`CMETA_INTERCEPTOR_TYPE(Name, Request, Response)` declares an exact native
`Name_target_fn` and optional `Name_admit` accepting borrowed
`cmeta_function_abi_desc` expected/provider views. Admission reuses
`cmeta_function_abi_contract_compatible` and rejects mismatched types,
carriers, parameter flags, effects and result ownership without inventing
a dynamic invocation ABI. The native target pointer is independently
qualified by C11 and C++17 positive/negative compile tests.

A declared FunctionAbi is only trustworthy when the provider guarantees
it matches the executable native function. Descriptor equality alone cannot
prove that correspondence. Neither CMeta nor Interceptor retains a callable,
ObjectRef, data buffer or Plugin module: callers hold the corresponding Scope
through the entire invocation.

## Canonical decomposition

Patterns should reduce to the smallest existing semantic primitives.

### Strategy

```text
Strategy = CMeta Interface
```

Use the existing typed `{ self, vtable }` protocol and
`cmeta_interface_desc`. Do not add `cmeta_strategy` merely as a spelling
alias.

### Factory

```text
Factory =
  canonical FunctionDesc
  + FunctionAbi
  + explicit result ownership
```

A creator returning an owned component/object uses the existing
`CMETA_RESULT_OWNED` semantic contract. Factory semantics must not be inferred
from a pointer return type or from a function name.

No generic runtime ABI reconstruction is added by the Factory pattern.

### Adapter

```text
Adapter =
  exact typed Interface implementation mapping
  + compile-time native signature checking
```

Use `implements(...)` / exact generated vtables where they already express the
mapping. Add a new helper only when it validates or generates a relation that
cannot be represented by the existing Interface implementation surface.

Do not add a second generic adapter runtime.

### Extension Interface

```text
Extension Interface =
  one native object/provider identity
  + zero or more provider-authorized Interface projections
```

Use `cmeta_object_ref`, `cmeta_object_interface_provider`, and
`cmeta_interface_projection`.

A borrowed projection owns nothing. It must not expose independent destroy
authority for an Interface containing `CMETA_INTERFACE_METHOD_OWNS_SELF`.
Provider/module lifetime stays external.

### Lifecycle

Lifecycle metadata describes exact operations but does not own runtime state.

The component layer may compose:

```text
create
activate
deactivate
destroy
```

from exact typed functions. Cleanup authority remains the native provider and
outer resource/module owners.

### Scope

Pattern composition reuses existing ownership relations:

```text
value lifetime   -> DataDesc
object lifetime  -> ObjectRef/lifecycle
module lifetime  -> Plugin lease
lexical cleanup  -> CMeta scope/guard facade
```

A descriptor, Interface view, or callable never implicitly retains a Plugin
module.

### Static discovery

Use immutable explicit CMeta manifests.

```text
descriptor = semantic truth
manifest   = explicit static discovery set
linker     = optional private aggregation backend
```

Do not introduce constructor registration or a process-global mutable CMeta
registry.

## Canonical cmeta/component.h

The canonical `<cmeta/component.h>` declaration is static metadata only:

```text
stable component/provider identity
+ optional typed config DataDesc (valid native storage_type)
+ ordered provides Interface rows
+ ordered requires Interface rows
```

It does not load modules, resolve dependencies, select providers, own leases, or
advance lifecycle.

This metadata is the canonical component/provider capability declaration. Dynamic module publication and lease ownership remain under `CMeta::Plugin`. There is no second static Plugin declaration vocabulary in the 2.3.0 candidate.

## Patterns that do not belong in CMeta runtime

The following ACE patterns may consume CMeta metadata but their execution
mechanism remains outside CMeta:

| Pattern | Runtime owner |
| --- | --- |
| Reactor / Proactor | CNet / NativeIO |
| Acceptor / Connector | CNet |
| Active Object | Actor / CFlow |
| Message Queue | Concurrency / Actor / CFlow |
| Half-Sync/Half-Async | domain runtime + bounded handoff |
| Streams execution | CFlow / domain runtime |
| Plugin module lifecycle | CMeta::Plugin |
| Component resolution/lifecycle graph | CMeta::ACE |

CMeta may describe typed policies, messages, interfaces, factories and
lifecycle contracts for these mechanisms.

### ACE Acceptor–Connector composition conformance

The 2.3 ACE integration suite in
`cnet/tests/cnet_ace_acceptor_connector_test.c` uses a real loopback TCP
connection, not a simulated acceptor. `cnet_listener` and `cnet_client`
own listener/connection admission and deterministic callback progress.
A fully reflected `CMETA_INTERFACE` Strategy supplies borrowed typed
`on_state` dispatch, and the callback owner's storage remains live until
CNet stop/destroy. Closing the listener does not invalidate established
connections. Invalid admission fails without callback publication.

This qualifies a composition of existing primitives: **it does not authorize**
a `cmeta_acceptor`, `cmeta_connector`, hidden reactor, global registration
or implicit handler lifetime extension.

### ACE Active Object / Half-Sync-Half-Async conformance

The focused ACE Active Object suite in `cflow/tests/cflow_actor_test.c`
uses the real `cflow_actor` with its bounded typed Machine mailbox,
serial Executor, concurrent Scheduler and retained `cflow_actor_ref`.
The local test-only `cflow_ace_active_object_pattern.h` contains two fully
reflected CMeta Interfaces: an exact typed producer port and an action
Strategy. The borrowed Strategy is invoked only for admitted events by the
existing Actor action; CMeta does not own scheduling or queued work.

The test blocks an in-flight action to prove `ACCEPTED` then capacity
`FULL` without a hidden retry, exact FIFO typed observation, `STOPPING`
and `STOPPED` admission rejection, and `STALE` producer refs after Actor
destruction without invoking borrowed callbacks. The same Interface schemas
compile under C11 and C++17.

Actor owns state and bounded mailbox, Scheduler/Executor own progress, and
borrowed Strategy/provider storage outlives Actor destruction. No new
`cmeta_active_object`, hidden thread, Plugin lease, or second registry.

An independent installed consumer in
`cflow/tests/package_config/ace_actor` also builds through
`find_package(CMeta ... EXACT CONFIG REQUIRED)` and links only
`CMeta::CFlow`. Its executable creates a real Actor from installed headers,
invokes reflected CMeta producer/Strategy methods, observes one terminal
message and safely drains/destroys the Actor. This is run through the shared
installed-SDK CMake test harness, not an additional CI orchestration wrapper.

### ACE Pipes and Filters / Streams (2.3 candidate conformance)

The test-only `cflow/tests/cflow_ace_pipes_filters_test.c` composes
canonical CMeta `cmeta_function` FILTER and MAP declarations into
a real CFlow Surface Graph, normalized Graph and Subscription.
The source and sink are existing fully reflected `cflow_publisher` and
`cflow_subscriber` Interfaces; no second filter/stream DSL is introduced.

Demand is **downstream output demand**: with no request, no source item is
consumed; a request for one even value pulls two upstream integers through
FILTER before MAP produces its result. Incremental demand never triggers an
unbounded eager queue. Early cancellation stops delivery, and closing
Subscription destroys the moved Publisher exactly once before borrowed source
and Subscriber context storage can expire.

A separate real CFlow Channel with capacity two demonstrates explicit
producer-side `CFLOW_CHANNEL_FULL` rather than conflating Channel admission
with reactive demand. The Channel is owned outside the Subscription and is
closed/destroyed after its Publisher and Subscription are quiescent. Neither
CMeta nor CFlow automatically retries failed admission.

The installed-SDK CMake consumer under
`cflow/tests/package_config/ace_actor/pipes.c` compiles and executes the
same public FILTER → MAP, Subscriber, demand/cancel and bounded Channel APIs
against one exact installed `CMeta::CFlow`. It shares the existing out-of-tree
`find_package(CMeta ... EXACT)` and SDK install test with the ACE Actor
consumer; no second packaging workflow or fallback SDK is introduced.

### ACE Half-Sync/Half-Async over CNet and CFlow

The executable integration fixture
`cflow/cnet-adapter/tests/cflow_ace_half_sync_async_test.c` exercises a
real loopback TCP input and deliberately distinct progress owners:

```text
CNet / NativeIO TCP owner
  borrowed receive callback
    -> copied int via capacity-2 CFlow Channel (FULL is explicit)
    -> demand-driven CFlow Subscription / canonical Subscriber Interface
    -> retained Actor producer try_send (one-slot Mailbox, FULL explicit)
    -> CFlow Actor worker + serialized Machine action
```

CNet invokes only short, nonblocking callbacks under its poll owner; neither
Actor stop/wait nor CFlow Scheduler execution is reentered from the network
callback. Source bytes are borrowed only for that callback and become bounded
Channel-owned values on accepted admission. CFlow Subscription drives one
item at a time under explicit downstream demand, and a rejected Actor send is
reported through the existing Subscriber failure/terminal path, never
automatically retried or silently buffered.

The tests force both a Channel `FULL` and an Actor Mailbox `FULL` while
the Actor worker is stalled, plus a separate cancellation path. Teardown first
quiesces CNet/listener callbacks; then it closes Subscription and its moved
Publisher before Channel; finally it stops/destroys Actor and releases its
retained producer before destroying borrowed scheduler/executor/graph storage.
No new CMeta reactor, scheduling runtime, registry, Plugin lease, or fallback
queue is introduced.

The independent installed SDK consumer
`cflow/tests/package_config/ace_actor/half_sync.c` additionally exercises
public CNet TCP receive callbacks feeding the fixed-capacity CFlow Channel,
then an explicit-demand Subscription using a canonical Subscriber. It links
only exported `Salts::CFlowCNet` after exact `find_package(CMeta ... EXACT)`
and shares the same out-of-tree install verification harness as the ACE Actor
and Pipes/Filters consumers; no separate packaging workflow is added.

## Admission test for a new pattern helper

Add a new CMeta pattern-level macro/inline helper only if at least one condition
holds:

1. it generates a new canonical descriptor relation;
2. it proves a useful compile-time relation between existing descriptors or
   native signatures;
3. it removes repeated glue while preserving one existing semantic authority;
4. it provides a typed projection that cannot otherwise be expressed safely.

Do not add a helper solely to introduce a pattern name.

## Component Configurator integration

The composition follows CMeta Interface/Function/Object/Lifecycle declarations
into a Component descriptor with explicit provides/requires. `CMeta::Provider`
exposes the bindings. Configurator resolves the lifecycle graph and ServiceHost
owns generation Scopes within `CMeta::ACE`; `CMeta::Plugin` owns module leases
and unloading.

The Configurator adds only the runtime semantics not already owned by
CMeta:

- provider selection/resolution;
- dependency graph construction;
- instance lifecycle ordering;
- generation publication/drain.

If implementing the Configurator requires a second Interface, Factory,
lifecycle, ownership, or metadata model, the pattern decomposition is wrong.

## No-fallback rule

The unmerged 2.3.0 development branch may change before release. Published
Salts 2.2.0 is the prior binary baseline; when a new contract is
selected:

- migrate integration-branch consumers;
- delete superseded experimental spellings;
- do not retain alias/fallback paths merely because an earlier branch revision
  used them;
- do not merge partial duplicate semantic models to `master`.

Formal proof may be added later, but is not a prerequisite for engineering
work under #1012 or the already integrated #1005–#1009 foundation.
