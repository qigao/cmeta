# CMeta x ACE / POSA2 — 17-pattern coverage matrix

> **Post-rc.5 LF implementation (2026-10-10):** the opt-in CPU backend now lives in
> [Salts::Concurrency](../concurrency/src/thread_pool_lf.c), with a versioned
> [public creator/stats contract](../concurrency/include/salts/thread_pool.h),
> [formal lifecycle tests](../concurrency/tests/thread_pool_lf_test.c) and a
> [small public-API benchmark](../concurrency/benchmarks/thread_pool_lf_benchmark.c).
> See [design and validation scope](../concurrency/LEADER_FOLLOWERS_DESIGN.md).
> The matrix and release ledgers below describe their historical source checkpoints.
> This implementation is separate from the published rc.5; it does not establish
> FlowMQ integration, DSO provider qualification or 17/17 final release acceptance.

> **Status:** design/coverage inventory, **not** 17/17 implementation or release qualification.
> **Baseline:** `feature/cmeta-ace-patterns`; ACE functional-freeze code SHA [`02b741a`](https://github.com/qigao/salts/commit/02b741a7f2ebceaeed68c681898d09072c5323d1), qualified by [CI #37775060929](https://github.com/qigao/salts/actions/runs/37775060929). This document is documentation-only: it does not reopen or supersede the frozen source contract in [ACE_FREEZE.md](ACE_FREEZE.md).
> **Change policy:** [#1012](https://github.com/qigao/salts/issues/1012), draft [PR #1013](https://github.com/qigao/salts/pull/1013) **DO NOT MERGE / DO NOT PUBLISH**; [#1018](https://github.com/qigao/salts/issues/1018) independently owns 2.3.0 package, ABI and downstream release qualification.

> **Runtime naming note (2026-10-19):** the matrix preserves historical source
> checkpoints and CTest names, some of which use `ComponentPlugin`. The
> currently integrated lifecycle targets are `Salts::Configurator` and
> `Salts::ServiceHost`; Plugin remains the sole loader/Registry/quiescence/
> unload authority. Current SDK and installed-consumer qualification is tracked
> in [PR #1260](https://github.com/qigao/salts/pull/1260).

## Goal and counting rule

Target: **all 17 canonical patterns in POSA2 expressed and usable through CMeta semantics composed with existing Salts runtime owners**. This is **not** a request for 17 new classes, 17 new public `cmeta_*` aliases, or 17 runtimes.

Primary pattern catalog: [Schmidt/Stal/Rohnert/Buschmann POSA2 table of contents](https://www.cs.wm.edu/~dcschmidt/POSA/POSA2/). Additional GoF/ACE idioms (Strategy, Factory, Adapter, Observer, Pipes and Filters, etc.) are composition tools **outside** this canonical 17-item count.

Distinguish three independent questions per pattern:

1. **Mechanism:** is actual behavior already present in an authoritative Salts runtime?
2. **CMeta composition:** can canonical Type/FunctionDesc/FunctionAbi/Interface/ObjectRef/DataDesc/lifecycle + ordinary typed C11 glue represent the roles and constraints?
3. **Conformance:** is the intended POSA2 pattern behavior, including failure and lifetime, proven by an appropriate executable test (and installed consumer when a public interface is required)?

Status vocabulary:

- **QUALIFIED:** focused executable CMeta+runtime composition is included in frozen ACE regression evidence. Not a 2.3 SDK release claim.
- **REUSE:** runtime pattern is already implemented and directly exercised; no duplicate CMeta implementation is justified. Link its existing evidence.
- **IMPLEMENTED / POST-FREEZE TESTED:** typed implementation and executable branch evidence exist after the functional freeze; final-head/full SDK acceptance is separate.
- **COMPOSED / PARTIAL CONFORMANCE:** Platform primitives and some real pattern tests exist; some negative/lifecycle/SDK checks remain.
- **TEST-LOCAL PROTOTYPE:** a bounded reference fixture exists without a named real consumer; it is explicitly not a product runtime or qualified SDK pattern.
- **PRIMITIVES / GAP (historical):** retained only when describing the original audit, not the current table.

## All 17 POSA2 patterns

| # | POSA2 family / pattern | Existing authoritative mechanism | Canonical CMeta representation | Status | Exact remaining work (no duplicated runtime) |
|---:|---|---|---|---|---|
| 1 | Service — **Wrapper Facade** | Platform / NativeIO / CNet | typed inline façade, Interface, FunctionAbi, explicit status/borrow | **COMPOSED / PARTIAL CONFORMANCE** | Qualify a complete narrow native-resource façade: types, errors, close/borrow, C11/C++17 and out-of-tree consumer. No generic wrapper runtime. |
| 2 | Service — **Component Configurator** | Salts::Configurator + Salts::ServiceHost + Salts::Plugin | immutable `cmeta_component_desc`, manifests, typed provider, ObjectRef, leased Interface projections | **QUALIFIED** | Preserve [frozen Component/Plugin contracts](ACE_FREEZE.md); downstream/ABI remains [#1018](https://github.com/qigao/salts/issues/1018), not a new pattern engine. |
| 3 | Service — **Interceptor** | domain callback/dispatch owners | exact FunctionDesc/FunctionAbi + typed Interface roles + borrowed context | **IMPLEMENTED / POST-FREEZE TESTED** | Typed before/after/error/short-circuit chain; result/error ownership, effect/ordering, reentrancy and ServiceHost Scope admission. No generic dynamic ABI reconstruction or hidden invocation registry. |
| 4 | Service — **Extension Interface** | existing provider/ObjectRef/Plugin lifetime | `cmeta_object_ref`, `cmeta_object_interface_provider`, `cmeta_interface_projection` | **QUALIFIED** | Preserve non-owning projections and explicit owning-self rejection; no second provider identity. |
| 5 | Event — **Reactor** | NativeIO epoll/kqueue readiness; CNet event callbacks | typed callback / Interface over existing owner-bound dispatch | **REUSE** | Existing NativeIO/CNet model, ownership and callback tests are evidence; **no second Reactor or CMeta scheduler**. |
| 6 | Event — **Proactor** | NativeIO IOCP/io_uring completion; CNet completion-driven semantics | typed callback / Interface over existing completion contract | **REUSE** | Existing NativeIO/CNet request/completion and terminal tests are evidence; **no second Proactor**. |
| 7 | Event — **Asynchronous Completion Token (ACT)** | NativeIO request `{slot,generation}` + `user_data` and terminal completion | typed request-to-context association + lifecycle / borrowed or owned context contract | **IMPLEMENTED / POST-FREEZE TESTED** | Demonstrate match of exact token to completion, cancel/terminal races, stale reuse, in-flight borrow, teardown and exactly-once context settlement; do not create a second request registry. |
| 8 | Event — **Acceptor-Connector** | CNet listener / client, accepted stream and owner handoff | exact typed Handler Strategy / Interface; explicit borrowed owner lifetime | **QUALIFIED** | Real TCP loopback ACE fixture and invalid-admission/close contracts already qualified. |
| 9 | Sync — **Scoped Locking** | Platform mutex / RW lock, CMeta structured lifecycle scope | `cmeta_scope` / canonical DataDesc construct-and-restore + typed guard adapter | **COMPOSED / PARTIAL CONFORMANCE** | Prove actual **lock acquire and unlock** around a critical section, early error and reverse cleanup; generic CMeta resource scope alone does not prove Scoped Locking. No lock owned by CMeta. |
| 10 | Sync — **Strategized Locking** | existing mutex / RW lock and Concurrency | type-safe Lockable Interface + selected policy/Guard | **IMPLEMENTED / POST-FREEZE TESTED** | Demonstrate at least two locking policies with identical critical-section contract, acquire/release symmetry, immutable policy binding, invalid mode rejection; no virtual call on unrelated network hot paths. |
| 11 | Sync — **Thread-Safe Interface** | Platform/Concurrency locking and owner-affine execution | typed public Interface vs internal nonlocking methods; explicit threading/admission contract | **IMPLEMENTED / POST-FREEZE TESTED** | Prove lock-once public boundary, internal calls without recursive self-deadlock, concurrent callers, owner-only violations, shutdown/reentrancy and no false synchronization promise. Generic `FunctionDesc.effects` is not a replacement for this execution contract. |
| 12 | Sync — **Double-Checked Locking Optimization** | Platform `cmeta_once`; Concurrency C11 atomics | typed init/once façade + explicitly safe publication/order | **COMPOSED / PARTIAL CONFORMANCE** | Compare safe once vs C11 acquire/release DCL where warranted; test publication and teardown. **Never implement historic unsynchronized, data-racing DCL**; a safe once solution may qualify the intent without adding a public DCL macro. |
| 13 | Concurrency — **Active Object** | CFlow Actor, typed bounded Mailbox, Executor/Scheduler | reflected producer / action Strategy Interface | **QUALIFIED** | Preserve bounded admission, FIFO, stale refs and no hidden worker/queue through existing ACE tests. |
| 14 | Concurrency — **Monitor Object** | Platform mutex/condition + domain state | typed Interface + scoped guard + condition predicate/protocol | **IMPLEMENTED / POST-FREEZE TESTED** | Actual synchronized method entry, condition-loop waiting, signal, nonrecursive internal calls, cancellation/close and no lost wake or early destruction; a mutex alone is not a Monitor Object. |
| 15 | Concurrency — **Half-Sync/Half-Async** | CNet TCP → bounded CFlow Channel → Subscription → Actor | typed Channel/Subscriber/Action interfaces and explicit transfer | **QUALIFIED** | Existing real-loopback ACE fixture proves explicit two-level FULL and shutdown; no extra queue. |
| 16 | Concurrency — **Leader/Followers** | existing Platform thread/mutex/condition primitives; CNet SG has fixed owners | explicit typed leader election/handoff and follower execution roles, if selected | **TEST-LOCAL PROTOTYPE (NOT QUALIFIED)** | `platform_ace_leader_followers_test` and `_cpp_test` exercise bounded copied CPU-event dispatch, explicit successor election, 1/2/4 workers, FIFO, concurrent producers, safe cancellation/close, and exact callback typing. No named non-SG consumer, runtime export, real DSO lease or installed SDK; no owner migration. |
| 17 | Concurrency — **Thread-Specific Storage** | Platform `SALTS_THREAD_LOCAL` / thread affinity; `cmeta_local_type` lifecycle adapter | typed per-thread value + explicit init/get/destroy and ownership | **COMPOSED / PARTIAL CONFORMANCE** | Prove per-thread isolation, creation/exit, cleanup, nested calls and no borrow crossing thread exit/migration. `cmeta_local_type` is thread-*affine* storage, not automatic TLS allocation by itself. |

**Current evidence snapshot (2026-10-09):** QUALIFIED 5 / REUSE 2 / IMPLEMENTED POST-FREEZE 5 / COMPOSED PARTIAL 4 / TEST-LOCAL PROTOTYPE 1. These are **evidence categories**, not a release percentage. See the exact-SHA ledger below; neither existence of code nor a prior CI success automatically qualifies a final SDK.

## Qualification dimensions and evidence discipline (2026-10-09)

**Do not treat the status column as a release gate.** Independently assess each pattern across five dimensions; the source SHA, concrete CTest case and GitHub Actions run URL must support each claimed PASS. A run at an older SHA is historical evidence, not exact-head qualification. Use `N/A` only with a reason; never upgrade to release qualified based solely on code presence.

| Dimension | Evidence required | Current disposition |
|---|---|---|
| Implementation | Native typed contract and authoritative runtime source/commit | 16 have implementation/reuse/composition; #1064 has a test-local prototype, not a consumer executor |
| Behavioral conformance | Positive, invalid-admission, lifecycle/terminal and negative tests with CTest name | See pattern fixtures and exact-source ledger below; gaps must stay explicit |
| Sanitizers | Selected ASan+UBSan / TSan results tied to exact commit and selected tests | [#37877974693](https://github.com/qigao/salts/actions/runs/37877974693) at `c8c27cc`; not final HEAD/release |
| Installed SDK | Out-of-tree C11/C++17 compile+run, ABI/SONAME and wrong-minor rejection | Some installed ACE consumers exist; immutable candidate verification remains #1018 |
| Release/downstream | Same immutable 2.3 candidate: host/device, CHttp/TurboFlow/TurboSCXML, explicit go/no-go | **OPEN**, owned by #1018 |

**Representative case IDs (not an exhaustive proof of each column):** `cnet_ace_acceptor_connector_test` (real TCP); `cmeta_ace_patterns_installed_test` (external C11/C++17 consumer); `cmeta_native_component_plugin_act_iocp_test` (Windows real IOCP/DSO); `cmeta_native_component_plugin_publication_test` (real Plugin scope and Interceptor); shared C11/C++17 `cmeta_ace_sync_cases.h` (locking, monitor, TLS/once). Verify actual CTest selection and SHA before promoting a particular dimension.

**Release discipline:** #1013 stays Draft / DO NOT MERGE / DO NOT PUBLISH. #1018 documents the deliberate 2.3 native ABI break as an explicit compatibility exception; test-only exact SDK pins do not justify production dependency version pins. #1064 is not a 2.3 release blocker without a genuine non-SG consumer.

## Grounded implementation / conformance evidence

- **Canonical pattern primitives:** [PATTERNS.md](PATTERNS.md), [Interface source](include/cmeta/interface.h), [Function metadata](include/cmeta/function.h), [Object Interface](include/cmeta/object_interface.h), [pattern C11/C++17 conformance](tests/cmeta_pattern_cases.h).
- **Component:** [declaration](include/cmeta/component.h), [runtime](../component/include/salts/component.h), [ComponentPlugin scopes](../component-plugin/include/salts/component_plugin.h); freeze tests and generation/epoch evidence in [ACE_FREEZE.md](ACE_FREEZE.md).
- **Reactor / Proactor:** [NativeIO public model](../native-io/include/salts/native_io.h) + [backend implementation](../native-io/src/native_io.c) + [native tests](../native-io/tests/native_io_test.c); [CNet](../cnet/README.md). Backend-kind distinction is explicit: epoll/kqueue = readiness; IOCP/io_uring = completion. Do not create another loop.
- **Acceptor-Connector:** [real TCP ACE conformance](../cnet/tests/cnet_ace_acceptor_connector_test.c).
- **Active Object / Half-Sync:** [Actor conformance](../cflow/tests/cflow_actor_test.c), [TCP → Channel → Actor fixture](../cflow/cnet-adapter/tests/cflow_ace_half_sync_async_test.c), [ACE_FREEZE.md](ACE_FREEZE.md).
- **Scoped lifetime / TLS / synchronization primitives:** [CMeta scope](include/cmeta/scope.h), [thread-local CMeta façade](include/cmeta/local.h), [Platform mutex/condition/once/TLS](../platform/include/salts/thread.h), [C11 atomics](../concurrency/include/salts/atomic.h), [execution primitives contract](EXECUTION_PRIMITIVES.md). None alone certifies the entire corresponding ACE pattern.
- **ACT starting point:** [NativeIO request, operation and completion identity](../native-io/include/salts/native_io.h) and [NativeIO tests](../native-io/tests/native_io_test.c); `user_data` is a copied association value, **not** a lease or ownership transfer.

## Acceptance contract for each gap

Every missing or unqualified pattern must specify: **Intent → ACE participants → CMeta native type/Interface/Function relation → authoritative runtime owner → ownership/borrows → concurrency/execution rules → error/cancel/terminal behavior → composition edges → positive and negative conformance**.

- The smallest complete executable slice wins. Use existing APIs, typed C11 functions, finite macros and explicit immutable descriptors.
- Only introduce a CMeta public helper when it proves a previously unexpressed native type/ownership relation or removes demonstrably unsafe repeated glue. **No pattern-named aliases solely for aesthetic coverage.**
- Qualified C11 and C++17 compiled tests for public typed declarations; compile-negative mismatch checks for exact FunctionAbi / Interface mappings. Cross-platform tests and installed-SDK consumer if an exported contract changes.
- Must preserve CNet/NativeIO I/O and terminal authority, CFlow scheduling authority, Concurrency/Platform mutex/TLS authority, Component composition authority and Plugin sole module lease/loader authority.
- All bounded admission, no implicit retries, no hidden queues/thread pools, no global mutable CMeta registry. Do not dispatch every network packet through pattern reflection.
- Documentation-only evidence changes do not reopen ACE functional freeze. **Any new source/public ABI work is post-freeze and requires separate approval, a precise issue/regression test, and the appropriate CI/SDK gates; do not silently merge into the frozen 2.3 candidate.**

## Post-freeze implementation candidates — not yet qualified

The five patterns below have **actual source plus executable fixtures** on the
existing ACE branch. All remain **GAP** in the qualification totals until the
*latest source head* compiles and the acceptance gates are reviewed. Existing
frozen SHA `02b741a` does **not** qualify these new commits.

| Pattern | Source / conformance | Remaining gate |
|---|---|---|
| Interceptor | [typed finite chain](include/cmeta/ace_interceptor.h) + C11/C++17 [pattern cases](tests/cmeta_pattern_cases.h) | Exact native FunctionAbi alignment, callback/Plugin lifetime and latest-head CI |
| ACT | [NativeIO typed token](../native-io/include/salts/native_io_ace_token.h) + [real read/cancel tests](../native-io/tests/native_io_test.c) + [C++ test](../native-io/tests/native_io_header_cpp_test.cpp) | Further close/race, installed consumer and latest-head CI |
| Strategized Locking | [fully reflected Lockable Interface](include/cmeta/ace_synchronization.h) + [mutex/RW tests](tests/cmeta_ace_sync_cases.h) | Additional policy-admission/lock ordering checks, CI |
| Thread-Safe Interface | [typed guarded public call](include/cmeta/ace_synchronization.h) + [concurrent counter](tests/cmeta_ace_sync_cases.h) | Negative native ABI and thread-safety contract/TSan, CI |
| Monitor Object | [real guarded monitor fixture](tests/cmeta_ace_sync_cases.h), using Platform condition | More cancellation/timeout/fairness cases, CI |

No new Reactor, Proactor, Actor, worker pool or CMeta scheduler was created.
Leader/Followers remains an optional, non-SG **test-local only** topology under #1064.

### ACT / Plugin lease test (post-freeze)

[`cmeta_native_component_plugin_publication_test`](../component-plugin/tests/component_plugin_publication_test.c)
now combines the existing real Plugin DSO + Component generation with a
**synthetic** typed ACT terminal identity. It demonstrates that closing a
generation does not release the DSO while the caller still holds a Scope;
a stale completion cannot settle the token, and terminal settlement alone
does not implicitly release the Scope or its module lease. After explicit
Scope release, generation drain frees the sole module lease. Real NativeIO
read/cancel completions are tested separately under NativeIO; this test does
not claim a synthetic completion is real kernel I/O.

### Fully reflected Thread-Safe Interface and Monitor contracts (post-freeze)

`cmeta_ace_sync_cases.h` now declares both user-facing typed Interface ports
with **canonical FR/FV method metadata** (FunctionDesc + FunctionAbi), including
an explicit enum status carrier and borrowed output-parameter semantics.
The test asserts every method's reflection validity in both C11 and C++17.
These are test-local declarations over Platform's existing mutex/condition;
there is no new synchronization runtime or global Interface registry.

### Real NativeIO × ACT × Plugin DSO integration (post-freeze)

[Real native terminal conformance](../component-plugin/tests/component_plugin_native_io_act_test.c)
exercises **Linux epoll** and **Darwin kqueue** over a true nonblocking OS
pipe with NativeIO request/observed completion. The same fixture loads a
real Plugin DSO, publishes ComponentPlugin generation N, admits and borrows
a live Scope, then closes admission before I/O terminal observation.
It verifies that both successful read and explicit cancellation preserve
the provider lease through terminal observation and exactly-once ACT
settlement; final DSO teardown occurs only after the **application** releases
the Scope. This is not simulated NativeIO progress and not a second
request/Plugin registry. Windows IOCP owns its separate native regression
path; do not infer IOCP + DSO end-to-end coverage from these POSIX tests.
The current HEAD must pass full native CI before this can be claimed qualified.

### Windows IOCP × ACT × Plugin DSO integration (post-freeze)

[Windows IOCP conformance](../component-plugin/tests/component_plugin_native_io_iocp_act_test.c)
uses **actual overlapped named pipes** through the existing NativeIO IOCP
backend. Two explicit cases observe successful PIPE_READ and cancellation,
match exactly one ACT terminal, reject a stale-generation token, and prove
that closing ComponentPlugin admission / completing NativeIO does **not**
reclaim the borrowed Plugin Scope. Only explicit Scope release permits DSO
generation drain and Plugin unload. This Windows-only fixture complements
the existing Linux epoll / macOS kqueue nonblocking-pipe fixture; it does
not add a Reactor/Proactor, hidden queue, or duplicate Plugin lease.
Status: newly submitted, **requires native Windows CI evidence**.

### Real Plugin Scope × typed Interceptor integration (post-freeze)

[ComponentPlugin publication conformance](../component-plugin/tests/component_plugin_publication_test.c)
also runs a **native-typed CMeta Interceptor** whose target calls a real
ComponentPlugin DSO provider via an explicitly acquired Scope. The generation
has stopped new admission, yet in-flight intercepted work remains authorized
until the Scope is released. CMeta's existing FunctionAbi admission checks
the native signature; before/after/error and short-circuit paths verify
exact callback ordering, unchanged rejected results and no double invoke.
The real provider module cannot unload while the borrowed Scope remains
live, and CMeta does not create or silently release a Plugin lease.
This uses the existing CTest case, already selected by the ACE sanitizer
regression matrix; the current commit requires exact-head CI acceptance.

### Thread-Safe Interface vs owner-affine Interface negative contract (post-freeze)

The same [C11/C++17 concurrent-pattern fixture](tests/cmeta_ace_sync_cases.h)
now contrasts two **different** contracts: a public Thread-Safe Interface
serialized through the selected real Lockable Strategy, and an independently
reflected owner-affine Interface that checks the canonical Platform thread
token. The foreign-thread attempt must be rejected **without mutating
unprotected state**, while the original owner can still call afterwards.
Thread ownership is a runtime admission invariant, **not** evidence that an
owner-affine receiver is thread-safe. No new scheduler, global thread registry
or implicit cross-owner dispatch is introduced. Requires latest-head CI.

## Active pattern-gap tracking

Umbrella: [#1058 — POSA2 17-pattern coverage and conformance](https://github.com/qigao/salts/issues/1058). Missing semantic/composition slices:

| Pattern | Issue | Scope |
|---|---|---|
| Interceptor | [#1059](https://github.com/qigao/salts/issues/1059) | Exact FunctionAbi, before/after/error and borrowed callback lifetime |
| Asynchronous Completion Token | [#1060](https://github.com/qigao/salts/issues/1060) | Typed terminal association and once-only context settlement |
| Strategized Locking | [#1061](https://github.com/qigao/salts/issues/1061) | Typed lock policy plus scoped guard |
| Thread-Safe Interface | [#1062](https://github.com/qigao/salts/issues/1062) | Synchronized entry/private operations and self-deadlock prevention |
| Monitor Object | [#1063](https://github.com/qigao/salts/issues/1063) | Predicate/condition waiting, synchronized access and teardown |
| Leader/Followers | [#1064](https://github.com/qigao/salts/issues/1064) | Genuine leader handoff on an optional non-SG execution topology |

Primitive-only conformance (Wrapper Facade / Scoped Locking / safe Once-DCL / Thread-Specific Storage) is separately tracked in #1058. **Neither this issue list nor documentation changes the frozen CMeta source ABI.**

## Proposed investigation / implementation order

1. **Typed asynchronous boundaries:** Interceptor and ACT (FunctionAbi, callback borrow, cancellation/settlement).
2. **Synchronization semantics:** Thread-Safe Interface + Strategized Locking + Monitor Object, composed with actual Scoped Locking.
3. **Thread topology:** Leader/Followers, tested on separate opt-in executor topology; retain fixed CNet SG owner.
4. **Complete the primitive-only acceptance:** Wrapper Facade, Scoped Locking, safe once/DCL, Thread-Specific Storage.
5. **Pattern language smoke:** at least one realistic composed host exercise uses Component Configurator + Extension Interface + Interceptor + existing Reactor/Proactor/Acceptor-Connector + ACT; one synchronization/concurrency exercise uses typed Guard + Thread-Safe Interface + Monitor Object or Active Object. Never force all 17 patterns into one contrived runtime.

## Cross-cutting non-POSA2 patterns

Strategy = canonical Interface; Factory = FunctionDesc/FunctionAbi + ownership; Adapter = exact typed Interface mapping; Observer = callback Interface; Pipes and Filters = CFlow Graph. These remain essential **composition elements**, but do **not** increase the POSA2 17-pattern count.


## Phase B exact-source checkpoint — 2026-10-09

Latest completed ordinary [Salts CI #37875659581](https://github.com/qigao/salts/actions/runs/37875659581)
**SUCCESS** at commit [`0524e216`](https://github.com/qigao/salts/commit/0524e2169c096d18d674278e91f3e29626d4f99c).
The shared C11/C++17 ACE sync fixture now executes:

- **Thread-Specific Storage:** real `SALTS_THREAD_LOCAL` per-worker isolation, nested access and explicit thread-exit cleanup (initial regression `cdb0a50`).
- **Safe Once / DCL intent:** four concurrent Platform worker threads observe one safely published value via `cmeta_once`, with no data-racy historical DCL (`568feb3`).
- **Scoped Locking:** an outer guard and failing inner typed synchronized gate, reverse lifecycle and duplicate-release rejection (`388a19c`).
- **Wrapper Facade:** typed open/use/close around a real Platform mutex, invalid arguments, unchanged output on closed resource and explicit reopen (`0524e216`).

This is **executable in-tree host conformance**, not proof of full installed-SDK,
cross-thread borrow prevention, automatic TLS destructors, asynchronous
use-after-close safety, or sanitizer freedom. The wrapper is deliberately
quiescent-only, requiring joined users before close. The earlier table is a
historical inventory; no pattern automatically becomes 17/17 QUALIFIED on
the strength of this note.

**Next qualification:** the existing opt-in `[ACE-SAN]` selector is used
to run targeted ASan+UBSan/TSan against the latest branch head. The results
must be checked against this documentation commit and the selected CTest
scope; a normal host matrix is not a sanitizer run. Draft PR #1013 stays
**DO NOT MERGE / DO NOT PUBLISH**, and all immutable SDK/downstream release
gates remain independent under #1018.


## Current qualification ledger — 2026-10-09 (post-freeze, not release)

This ledger supersedes *only* the outdated **evidence status** in the historical
17-row inventory above; it does not redefine the canonical 17 patterns or
retroactively change frozen commit `02b741a`.

| Patterns | Actual observed evidence | Remaining qualification |
|---|---|---|
| Component Configurator, Extension Interface, Acceptor-Connector, Active Object, Half-Sync/Half-Async (5) | Frozen ACE host conformance plus later branch regressions | Final immutable 2.3 SDK, downstream and device release gates #1018 |
| Reactor, Proactor (2) | Existing NativeIO/CNet readiness/completion runtimes, intentionally REUSE | No second runtime; exact backend evidence at release candidate |
| Interceptor, ACT (2) | Real loaded ComponentPlugin DSO invocation and real NativeIO request/cancel association; full host [#37869377431](https://github.com/qigao/salts/actions/runs/37869377431) at `6185834`; selected sanitizer suites [#37877974693](https://github.com/qigao/salts/actions/runs/37877974693) on later source | No implicit lease; final candidate SDK, full composition and downstream |
| Strategized Locking, Thread-Safe Interface, Monitor Object (3) | Reflected C11/C++17 lockable/synchronized/condition tests, wrong-owner rejection and selected ASan+UBSan/TSan [#37877974693](https://github.com/qigao/salts/actions/runs/37877974693) | Exact final installed SDK and closure of any outstanding specific issue acceptance |
| Wrapper Facade, Scoped Locking, safe Once/DCL, Thread-Specific Storage (4) | Shared real Platform primitives, negative lifecycle tests and native threads; host [#37875659581](https://github.com/qigao/salts/actions/runs/37875659581), latest TLS worker-lifetime host [#37877642307](https://github.com/qigao/salts/actions/runs/37877642307), and selected sanitizer [#37877974693](https://github.com/qigao/salts/actions/runs/37877974693) at `c8c27cc` | Wrapper requires quiescent close, TLS scalar cleanup is explicit (not automatic destructor); cross-platform exact-head/installed SDK release gates remain |
| Leader/Followers (1) | **TEST-LOCAL PROTOTYPE / NOT QUALIFIED**: explicit pre-handler successor election, real mutex/condition workers, bounded copied FIFO and typed C11/C++17 callback contract in `platform_ace_leader_followers(_cpp)_test`; see [#1064](https://github.com/qigao/salts/issues/1064) and [commit `70c3f24`](https://github.com/qigao/salts/commit/70c3f249d1eff06122f35549a0bf8e27f7d68f72) | Named non-SG consumer, DSO provider scope, production ownership and installed SDK (only if exported), cross-platform/sanitizer qualification, comparison with SG if applicable |

**Count:** 16 patterns have an implemented mechanism or executable composition
at different qualification levels (including Reactor/Proactor REUSE);
**Leader/Followers has only a test-local prototype, not a consumer runtime**. This is explicitly **not** 16/17
or 17/17 fully qualified for release. In particular the earlier five
`GAP` rows now have post-freeze implementation evidence and should not be
read as unimplemented. All run numbers above certify only the particular
source and test selection they ran.

**Admission decision:** no named non-SG consumer for #1064 has been
established. A CNet SG owner lane is not a Leader/Followers executor. Do not
add a production queue/pool or relabel SG. Keep #1064 open; the bounded
fixture is an isolated prototype, not a shipped execution topology.

**Release separation:** #1018 still requires immutable installed 2.3.0
candidate, actual SONAME/package digest verification, Windows/macOS DSO
consumers, downstream CHttp/TurboFlow/TurboSCXML, device runtime and final
go/no-go. No merge, stable package or GitHub Release is authorized by these
tests.

## #1064 Leader/Followers exact CMeta ABI checkpoint — 2026-10-09

- [`b7c72ca3`](https://github.com/qigao/salts/commit/b7c72ca36574554470f8759aec8cb3dfe4b0f4f4) adds C11/C++17 **canonical CMeta FunctionDesc/FunctionAbi** tests for the actual test-local `enum lf_result (*)(void *, int)` CPU callback, borrowed context, enum terminal, and typed role/stop metadata. This does not create a scheduling or Plugin-lease runtime.
- [Exact-source ACE-MATRIX #37886770939](https://github.com/qigao/salts/actions/runs/37886770939) passed **257/257 Linux GCC/Clang**, **247/247 Windows MSVC**, **250/250 macOS GCC/Clang**, plus Linux ARM64, Android and iOS cross-build and Lean. All four LF C11/C++17 test targets executed on native hosts.
- [`0c2551a5`](https://github.com/qigao/salts/commit/0c2551a5ffac45e7f408a541d77ad199cacb85fc) passed [ACE-SAN #37886359217](https://github.com/qigao/salts/actions/runs/37886359217), **24/24 ASan+UBSan and 24/24 TSan**, for the earlier Platform LF executor pair only. The newly introduced CMeta ABI tests require a separate latest-source ACE-SAN qualification.
- **Disposition:** test-local **PROTOTYPE**, still **NOT QUALIFIED** as a consumer feature. Non-SG CPU-only workload must be explicitly selected and shown useful; do not relabel CNet SG, add default worker pools, publish an SDK or merge draft #1013.
