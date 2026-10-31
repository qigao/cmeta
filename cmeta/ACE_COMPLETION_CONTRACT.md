> Historical evidence below applies only to each recorded commit and CI run.
> Current API names describe the corresponding owner; these results do not
> qualify Salts 4.0 / native runtime ABI 2.0.

# ACE/POSA2 completion contract — opt-in Leader/Followers and primitive conformance

Status: **post-freeze design + isolated test-local prototype** on `feature/cmeta-ace-patterns`. This is not a productized executor, fully qualified conformance, a new Salts 2.3 public ABI, or release approval. See #1058, #1064 and #1018.

**Current ownership:** `CMeta::ACE` implements Configurator static graph
selection/activation and ServiceHost generation publication, Scope admission,
drain and settlement of Plugin leases. `CMeta::Plugin` remains the sole loader,
Registry, quiescence and unload authority. The earlier
[SDK audit #1260](https://github.com/qigao/salts/pull/1260) is historical evidence;
current native ABI 2.0 requires independent exact-source acceptance.

## Non-negotiable architectural split

| Concern | Authority | CMeta role |
|---|---|---|
| Readiness/completion, request generation, terminal status | NativeIO / CNet | typed callback/request descriptors only |
| Connection owner, shard execution and handoff | CNet Sharded Graph | borrowed handler and immutable policy metadata |
| Worker creation, mutex, condition variable, TLS, once | Platform/Concurrency | strongly typed role/strategy declarations |
| Actor scheduling and bounded mailbox | CFlow | typed producer/action/receiver contracts |
| DSO load/unload, generation scope and leases | ServiceHost | typed borrowed invocation, never implicit retention |

An owner-affine CNet SG lane **cannot** become a Leader/Followers pool by changing a pattern label. Existing Reactor/Proactor already implement their ACE semantics; no duplicate I/O loop is permitted.

## Leader/Followers (#1064): optional CPU event executor, **not** an SG I/O topology

Only implement this pattern if an actual consumer requests an event source where any equivalent worker may process any event. No owner-affine endpoint, TLS-bound callback, borrowed stack buffer, or live NativeIO request may be transferred through this path.

```text
bounded event ingress (owned message copies)
     |
     v
single elected leader --take one event--> elect successor under mutex
     |                                      |
     v                                      v
process event outside mutex              next leader waits for ingress
     |
     v
exactly-one settlement + owned payload release
```

### Role and state contract

- Caller constructs an explicit pool with `capacity > 0`, worker count in `{1,2,4}`, and one fixed typed event contract. This is an opt-in consumer facility; **do not install in CMeta, CNet or NativeIO by default**.
- The producer retains ownership on rejected admission. On acceptance, the pool owns the copied event until exactly one completion/cancellation settlement; no retry, hidden resubmission or unbounded queue.
- Synchronization authority is one existing Platform mutex and condition variable. Under that lock keep `leader_id`, `closing`, `queued`, `active` and one bounded FIFO. Only a designated leader removes an event. The leader elects a waiting follower (or preserves its role if no follower is available) **before** processing that event outside the lock. A successor cannot take the same event.
- Stop under the mutex sets `closing`, rejects subsequent submission, broadcasts all waiters, and drains accepted work by a documented policy (either finish or explicit cancelled settlement). Destruction is forbidden while `queued + active > 0` or workers remain live.
- Worker cancellation/error cannot abandon leadership: release or elect a successor while holding the mutex and wake a follower. Never invoke application handlers, Plugin callbacks or CMeta reflection while holding the election mutex.
- CMeta encodes a canonical, compile-time typed `Event -> Result/Status` Callable or Interface. Provider and borrowed callback lifetimes must cover *all* admitted events, and ServiceHost generation scope must be held externally until pool shutdown. This declaration does not create scheduler or ownership semantics.
- No fairness claim stronger than eventual handoff with runnable followers and an unblocked event source. No migration of CNet endpoint owner, duplicate backend polling or replacement for SG.

### Executable admission before marking qualified

1. Deterministic 1/2/4-worker tests: one active leader at any instant, baton exchange while the previous worker processes, and exactly one settlement for every admitted event.
2. Queue-full rejection preserves caller payload; FIFO dequeue within the single event ingress; callback errors and worker cancellation leave capacity and counts consistent.
3. Close during blocked wait and active processing: wake all, reject late admission and join before storage/provider teardown; explicit generation-scope busy/drain test with a real DSO.
4. C11 and C++17 exact typed Callable declarations, compile-negative wrong signature, ASan+UBSan and TSan where supported.
5. Independent installed-SDK consumer **if** anything public is exported, plus a workload-specific comparison against ordinary SG. No speedup claims without measured data.

**Decision gate:** the `platform_ace_leader_followers(_cpp)_test` fixtures now exercise a bounded CPU-event prototype with explicit pre-handler successor election and type checks. Until a concrete non-SG consumer is identified and lifetime/DSO/release gates pass, #1064 stays TEST-LOCAL ONLY / NOT QUALIFIED, not a fake 17/17 completion.

## Remaining primitive-to-pattern obligations (#1058 phase B)

| Pattern | Actual contract to prove | Required failure/negative test |
|---|---|---|
| Wrapper Facade | Typed external/native resource open/use/close; status propagation; borrowed handle invalid after close | double-close, operation after close, failed-open leaves outputs unchanged; separate C11/C++17 consumer |
| Scoped Locking | Existing Platform mutex/RW lock acquired before guarded call and released at **every** return / C++ unwind | acquisition failure, early return, reversed nested release, no copyable active guard |
| Safe Once / DCL intent | Platform once or portable acquire-release published initialization; no historical data-racy DCL | simultaneous first-use, failed initialization semantics, stale/non-published reads |
| Thread-Specific Storage | Real `SALTS_THREAD_LOCAL` per-thread construction/use/teardown with typed CMeta view | cross-thread pointer borrow rejected, thread exit releases own data, address reuse not identity |

For all four: type exactness, owned/borrowed lifetimes, C11/C++17 conformance and installed consumer when public. Presence of `cmeta_scope` or `cmeta_local_type` **alone** does not count as qualification.

## Phase A gap acceptance and actual CI authority

- Interceptor (#1059): actual loaded DSO and borrowed ServiceHost Scope at `6185834`, full CI run 37869377431 passed; pending new fixture sanitizer and final candidate.
- ACT (#1060): genuine epoll/kqueue NativeIO plus Plugin Scope and Windows IOCP evidence exist on the branch; exact-head and targeted sanitizer acceptance remain distinct.
- Strategized Locking (#1061), Thread-Safe Interface (#1062), Monitor (#1063): tested reflected ports, actual mutex/RW/condition and concurrent fixtures exist; exact HEAD, negative cases and sanitizer completion must be independently verified.
- Leader/Followers (#1064): **test-local prototype, not qualified or exported**; commit `70c3f24` adds 1/2/4-worker FIFO/baton/concurrent-producer/close/worker-failure tests with C11/C++17 strict signatures. A named non-SG production consumer and DSO/SDK qualification remain open.

## Ordering

1. Close Phase A tests at their exact source SHA. Distinguish host success, sanitizer success and published installed SDK.
2. Implement Phase B *only* as thin CMeta composition over existing Platform primitives and concrete resource operations. Avoid pattern-named API aliases.
3. Decide #1064 via a named non-SG consumer before implementing any workers or queue.
4. Update `ACE_PATTERN_COVERAGE.md` only when the corresponding executable evidence exists; keep #1013 Draft and #1018 release admission independent.


## TLS worker-lifetime qualification checkpoint (2026-10-09)

The shared C11/C++17 regression now tests sequential native worker lifetimes
and strict owner-affinity rejection: [`37fd7e3`](https://github.com/qigao/salts/commit/37fd7e3ca222b108563b64488940f10196c854dd)
checks a foreign worker fails admission before mutating borrowed state; 
[`cd46728`](https://github.com/qigao/salts/commit/cd467281e0378f61c6cf511c6c68eb6bf105631e)
checks that a new worker begins with independently zero-initialized TLS after
a previous worker has exited. The latter passed exact-head ordinary Linux
[CI #37877642307](https://github.com/qigao/salts/actions/runs/37877642307).

**Destructor decision:** `SALTS_THREAD_LOCAL` for scalar test state does not
imply a general-purpose native TLS destructor callback contract. We will not
introduce a second TLS runtime or claim automatic destruction for arbitrary
CMeta borrowed values. Resource-owning thread-specific values must have an
explicit owner-managed stop/join/destroy path, or use an independently
specified Platform lifecycle API in a future dedicated issue. Never borrow
thread-local addresses across owner migration or after exit.

**Next evidence gate:** rerun the existing opt-in `[ACE-SAN]` ASan+UBSan/TSan
selection on the latest test source, then restore the Draft PR title. This
qualification does not include packaging, immutable 2.3 SDK acceptance or
production non-SG Leader/Followers execution.

## #1064 Leader/Followers exact CMeta ABI checkpoint — 2026-10-09

- [`b7c72ca3`](https://github.com/qigao/salts/commit/b7c72ca36574554470f8759aec8cb3dfe4b0f4f4) adds C11/C++17 **canonical CMeta FunctionDesc/FunctionAbi** tests for the actual test-local `enum lf_result (*)(void *, int)` CPU callback, borrowed context, enum terminal, and typed role/stop metadata. This does not create a scheduling or Plugin-lease runtime.
- [Exact-source ACE-MATRIX #37886770939](https://github.com/qigao/salts/actions/runs/37886770939) passed **257/257 Linux GCC/Clang**, **247/247 Windows MSVC**, **250/250 macOS GCC/Clang**, plus Linux ARM64, Android and iOS cross-build and Lean. All four LF C11/C++17 test targets executed on native hosts.
- [`0c2551a5`](https://github.com/qigao/salts/commit/0c2551a5ffac45e7f408a541d77ad199cacb85fc) passed [ACE-SAN #37886359217](https://github.com/qigao/salts/actions/runs/37886359217), **24/24 ASan+UBSan and 24/24 TSan**, for the earlier Platform LF executor pair only. The newly introduced CMeta ABI tests require a separate latest-source ACE-SAN qualification.
- **Disposition:** test-local **PROTOTYPE**, still **NOT QUALIFIED** as a consumer feature. Non-SG CPU-only workload must be explicitly selected and shown useful; do not relabel CNet SG, add default worker pools, publish an SDK or merge draft #1013.
