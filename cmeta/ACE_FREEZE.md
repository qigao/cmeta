> Historical evidence below applies only to each recorded commit and CI run.
> Current API names describe the corresponding owner; these results do not
> qualify Salts 4.0 / native runtime ABI 2.0.

# Salts 2.3 — ACE / CMeta functional freeze contract
> **Post-freeze development notice (2026-10-09):** the existing long-lived
> branch now contains **additional unqualified** Interceptor, ACT, locking,
> Thread-Safe Interface and Monitor code *after* the frozen `02b741a` SHA.
> This historical freeze evidence certifies only that exact code snapshot;
> subsequent commits require new CI and acceptance. PR #1013 remains Draft,
> **DO NOT MERGE / DO NOT PUBLISH**.


**Status:** **ACE FUNCTIONALLY FROZEN** at source HEAD
[`02b741a7f2ebceaeed68c681898d09072c5323d1`](https://github.com/qigao/salts/commit/02b741a7f2ebceaeed68c681898d09072c5323d1),
2026-10-08. The exact-source [full CI #37775060929](https://github.com/qigao/salts/actions/runs/37775060929)
completed **SUCCESS** (Linux GCC/Clang, Windows MSVC, macOS GCC/Clang,
Linux ARM64, Android/iOS cross-build and Lean). Later documentation-only
commits do not silently change this frozen code baseline; substantive
changes must requalify the affected contracts.
Long-lived `feature/cmeta-ace-patterns` [Draft PR #1013](https://github.com/qigao/salts/pull/1013)
remains **DO NOT MERGE. DO NOT PUBLISH.** Feature freeze is **not** 2.3.0
native SDK, downstream or stable release qualification.

**Current ownership mapping:** the graph below uses the current public owner
names for the corresponding responsibilities. `Salts::Configurator` owns static
graph resolution and activation; `Salts::ServiceHost` owns generation
publication, Scope admission and drain, and Plugin lease settlement.
`Salts::Plugin` remains the sole DSO loader and unload authority. Frozen test
names and run links below apply to their historical source only. They do not
qualify the current Salts 4.0 / ABI 2.0 implementation.

```text
CMeta: immutable Type / Interface / FunctionAbi / DataDesc / ObjectRef /
       Lifecycle / Manifest / typed macro + inline declarations
    |
    +--> Salts::Configurator: bounded static selection, graph/lifecycle
    |        |
    |        +--> Salts::ServiceHost: generation publication + scope admission
    |                    |
    |                    +--> Salts::Plugin: sole DSO load/unload + module lease
    |
    +--> CNet / NativeIO: transport, accept/connect, Reactor/Proactor owner
    |
    +--> CFlow / Actor: bounded mailbox, scheduler, Graph, Channel, Stream
```

**No new public ACE pattern DSL, runtime scheduler, dynamic service locator,
reflection registry, module loader or automatic retry.** Strategy is the
canonical typed `Interface {self,vtable}`; Factory is FunctionDesc/FunctionAbi
plus explicit result ownership; Adapter is exact typed Interface mapping;
Extension Interface borrows one ObjectRef with provider-authorized
projections. A macro/inline may be admitted *only* if it proves a new native
relation or prevents a real ownership error, not merely to rename an ACE
pattern. Public Component struct/layout is frozen for this 2.3 candidate
after test qualification; intentional changes restart ABI gates.

## Context, owned callbacks and borrowed views

```text
Context ZERO -> READY -> RESOLVED -> ACTIVE -> STOPPED
                                \-> FAILED (rollback complete)
```

- First use: `salts_configurator_context context =
  SALTS_CONFIGURATOR_CONTEXT_INIT;` (C11 and C++17). Reading an indeterminate
  uninitialized stack context is outside the API contract; no unsafe
  "automatic initialization" compatibility path.
- Reinit in READY/RESOLVED/ACTIVE fails **without changing storage or losing
  ObjectRef destroy authority**. STOPPED or fully rolled-back FAILED reuse
  requires exclusive caller ownership and expired borrowed views.
- A failed `create()` that returned a valid owned ObjectRef is settled once;
  on `activate()` failure the current instance is destroyed but NOT
  `deactivate()`d. Provider activation must undo partial side effects itself
  or have them covered by owned ObjectRef destruction.
- Previously activated dependencies are deactivated/released in reverse
  topological order. On post-activation PROVIDES failure, the failed
  instance *is* deactivated before ObjectRef release.
- An Interface/descriptor/callback/service view borrows its containing
  ObjectRef/Component scope/Plugin lease and may not escape their lifetime.

## Generation publication, leases and Scope

```text
ZERO -> BUILT -> PUBLISHED -> DRAINING -> STOPPING -> DRAINED
                         \        \-> FAILED (explicit bounded recovery)
                          \-> new admission on next published generation
```

- Complete candidate graphs are built before publication. Publication,
  admission/release, close and drain claims are protected by the existing
  runtime mutex. Generation IDs strictly increase and at most **two**
  generations may remain attached; there is no third-generation fallback.
- A live `salts_service_host_scope` is **noncopyable, address-stable,
  thread-exclusive**. A shallow copy cannot release or borrow; stale and
  double release cannot decrement the original generation's scope count.
  Scope release is *fallible* and therefore is not silently cast to a
  no-fail lexical cleanup callback.
- `runtime_close` withdraws current admission. It is *not* the same as
  final `runtime_destroy`; a later explicitly published higher generation
  may reopen admission subject to capacity.
- A scope pins its generation's provider callback/storage until release,
  not an exclusive external socket/lock. Drain waits for all scopes, stops
  Components and releases owned ObjectRefs **before** Plugin module leases;
  unload is allowed only after Plugin reports quiescence. No second DSO
  owner/refcount is created.
- `cmeta_cleanup`, `cmeta::object_scope` and ObjectRef/result cleanup
  discharge only the ownership they were explicitly given; they **never**
  manufacture a Plugin lease or authorize long-lived borrowed views.

## Exclusive external resource, N -> N+1

A socket listener, writer, file lock or equivalent singleton resource
**belongs to the domain owner**, not either Component generation. It must
not be bound twice simply because a candidate is built before publication.

1. Domain binds the stable exclusive resource once; Generation N borrows the
   capability. Its domain fencing token is epoch N.
2. N+1 builds/activates using borrowed provider storage without acquiring a
   second listener/writer. Candidate failure cannot close the domain resource
   or revoke N.
3. ServiceHost publishes N+1, but the domain still decides when its
   resource epoch changes. A new N+1 scope is initially **not admitted for
   exclusive I/O**. After the domain explicitly switches the fencing epoch,
   N is no longer admitted for exclusive I/O although its old scopes still
   pin teardown/code lifetime.
4. N drains after its scopes are released; draining N **must not** close
   the domain resource now used by N+1. After close/drain of N+1 and all
   borrowed work, the domain closes the resource exactly once.

The epoch check and the associated resource operation must be serialized or
made atomic by the **domain** where concurrent I/O is possible. The
deterministic `component_plugin_exclusive_fence_test` proves the
state/ownership contract, not a lock-free socket-fencing algorithm.
A real CNet/CHttp listener transfer still requires consumer-specific
concurrency qualification; no hidden authority is added to CMeta.

## Qualified executable contracts / freeze gate

| Contract | Fixture |
| --- | --- |
| Canonical Strategy / FunctionAbi Factory / Extension Interface | `cmeta_pattern_test`, `cmeta_pattern_cpp_test` |
| Correct and incorrect Interface result ABI (C11, C++17) | configure-time `SALTS_ACE_*_COMPILES` checks |
| Owned/Borrowed result, exactly-once cleanup, noncopyable C++ scope | `cmeta_pattern_cases.h`, `cmeta_pattern_cpp_test.cpp` |
| Context states and activation rollback | `cmeta_native_component_test`, C++ header fixture |
| Plugin lease cleanup before DSO unload; owned ObjectRef discharged first | `cmeta_native_component_plugin_generation_test` / DSO tests |
| Copied/stale scope, 2-generation capacity, close/reopen | `cmeta_native_component_plugin_publication_test` |
| Exclusive domain resource fencing and single close | `cmeta_native_component_plugin_exclusive_fence_test` |
| Real CNet Acceptor/Connector and Handler lifetime | `cnet_ace_acceptor_connector_test` |
| Typed CFlow Active Object, bounded mailbox and stale producer | `cflow_actor_test` / installed Actor consumer |
| FILTER -> MAP demand, channel FULL, cancellation | `cflow_ace_pipes_filters_test` / installed Pipes consumer |
| Real TCP -> Channel -> Subscription -> Actor | `cflow_ace_half_sync_async_test` / installed CFlowCNet consumer |

**Functional freeze admission:** all applicable above tests must compile,
execute and pass in the branch's selected Linux complete CTest/Lean gate,
and no unresolved P1 correctness issue may contradict these invariants.
The last source edit must be the commit actually tested. Host-matrix
results improve confidence; older-green runs do not qualify a newer HEAD.

**Separately OPEN for 2.3 release** (not permission to add new CMeta
primitives): immutable SHA-attested package, native SONAME/ABI evidence,
final sanitizer and Windows/macOS downstream conformance, CHttp request
scope/deferred mount, TurboFlow durable-plan ownership, TurboSCXML
session/invocation generations, Android/iOS device runtime if required,
and the explicit release/merge decision in
[#1018](https://github.com/qigao/salts/issues/1018).

## Functional qualification evidence (exact source SHA)

[Full matrix #37775060929](https://github.com/qigao/salts/actions/runs/37775060929)
executed the final ACE regression source `02b741a7f2ebceaeed68c681898d09072c5323d1`.

| Execution profile | Result |
| --- | --- |
| Linux GCC | **249/249** full CTest, including the new epoch-fenced exclusive owner, CMeta Pattern C11/C++17 and DSO/Context tests |
| Linux Clang | **249/249** full CTest |
| Windows MSVC | **240/240** full CTest |
| macOS GCC | **242/242** full CTest, including the CNet half-close terminal contract |
| macOS Clang | **242/242** full CTest |
| Linux ARM64 | portable native build and targeted contracts passed |
| Android arm64 / iOS arm64 | cross-compilation passed; **not** device-runtime execution |
| Lean | build and tests passed |

Every native host's compiler probe admitted the canonical C11/C++17
Interface method type and rejected the intentionally incorrect return
signature. The new C11/C++17 owned-result tests, ObjectRef/Plugin lease
destruction order, and one-bind-per-Scope exclusive-epoch fencing fixture
ran in the full CTest suites. The unique external writer/listener scenario
is a **deterministic domain-owner contract fixture**; actual real-world
socket/writer epoch-switch atomicity remains with CNet/CHttp and must
be qualified in the relevant downstream execution owner, not introduced
as a second CMeta runtime.

In-repository `salts_configurator_context_init` call sites were audited:
all direct Component C11/C++17 and independently installed consumers use
`SALTS_CONFIGURATOR_CONTEXT_INIT` before first use; the ServiceHost
generation initializes its embedded context before calling init. Domain
repositories' final exact-2.3 consumer qualification remains in #1018.

### Remaining work is release and consumer acceptance, **not new ACE APIs**

- SHA-attested exact 2.3.0 installed native SDK and SONAME verification
- final sanitizer runs, independent package integrity and supported devices
- CHttp request/deferred, TurboFlow ExecutionPlan/durable, TurboSCXML
  session/invocation N-to-N+1 consumer qualification
- explicit release and merge decision, which has **not** been given

## Freeze change control

After functional freeze admission, accept only corrections accompanied by
an exact negative/regression test and an issue explaining the violated
invariant. New pattern convenience APIs, alternative schedulers, registries,
legacy aliases, speculative incremental graph rebuilds and runtime-state
persistence are outside this freeze. All subsequent source changes require
re-running the affected contract gate. **PR #1013 stays Draft and
DO NOT MERGE regardless of test results.**
