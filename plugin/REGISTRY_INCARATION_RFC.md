# RFC: Registry-incarnation identity for CMeta Plugin Ref/Lease

Status: **design-only, NOT approved for implementation or merge**  
Tracking: [Salts #1156](https://github.com/qigao/salts/issues/1156)  
Related: [Draft public-lifetime documentation #1165](https://github.com/qigao/salts/pull/1165), [TurboAgent DSO conformance #114](https://github.com/qigao/TurboAgent/issues/114)

## 1. Decision to make

The current public Plugin ABI epoch is **5**, with exactly one supported publication layout. In that ABI a `cmeta_plugin_ref` contains only `{slot, generation}`; a `cmeta_plugin_lease` contains that Ref plus a lease slot and generation. Every new `cmeta_plugin_registry_init` starts slot and lease generations at 1. Within one live Registry, clearing a slot advances its generations. Across a *full* destroy and a fresh init, the same numeric token fields may reappear.

Current contract (documented in Draft #1165): Refs, Leases, and borrowed plugin-owned descriptors have authority only during one Registry incarnation; the Host must drain/release them before exclusive final teardown and must **never replay old token bytes into a newly initialized Registry**. The shape helpers only check for nonzero fields. The current ABI does **not** guarantee `CMETA_PLUGIN_STALE` across Registry incarnations.

**Question:** Must the Registry *actively reject* a stale token supplied after destroy→init, rather than relying on Host lifecycle discipline? Decide this from real Host/Actor/queue/durability ownership analysis. Merely adding an epoch to ComponentPlugin's generation ID does not resolve the Plugin Registry credential boundary.

## 2. Source-backed failure mode, not a production incident

In `plugin/src/plugin_registry.c`:
- `slot_for_ref_locked` checks only Ref slot and generation against the currently supplied Registry.
- `cmeta_plugin_registry_acquire` reserves the first available lease slot and returns its current generation.
- `cmeta_plugin_registry_release` checks Ref, lease-slot active bit and lease generation; it has no owning Registry incarnation comparison.

If Host code violates the current contract and presents historical Lease bytes to a new Registry that has a numerically matching *active* lease, the check cannot distinguish the historical token and could clear the new lease's active bit. This is a **conditional source-level ABA hazard**, not a demonstrated exploit and not a tested guarantee of the current ABI. Current-ABI tests must not invoke that invalid operation.

The related C++ `salts::plugin_lease_scope` owns a Lease, never the Registry, and requires a stable Registry address. Its destructor treats an unreportable release error as fatal. ComponentPlugin stores `cmeta_plugin_ref`/`cmeta_plugin_lease` in its generation modules; `plugin_ref_equal` currently compares only slot and generation. These are explicit migration surfaces if token identity changes.

## 3. Options

| Option | Contract | Advantages | Limitations |
| --- | --- | --- | --- |
| A. Existing ABI + strict Host drain | A token is authoritative only inside its origin Registry incarnation | No ABI disruption; bounded state and existing conformance already apply | An old token accidentally presented to a fresh Registry can be numerically indistinguishable |
| B. New exact ABI + Registry epoch | Ref/Lease carry a nonzero incarnation and every operation validates it | Fail-closed cross-incarnation stale-token rejection, including Lease release | Breaking public token layout; first-party ComponentPlugin and all SDK consumers must migrate together |

**Suggested criterion:** Choose B if any supported trusted Host path may queue, hand off, cache, or replay Plugin Refs/Leases across a Registry teardown/reinit boundary in a way that cannot be mechanically drained/fenced before publish. Otherwise A remains defensible, but must not be advertised as robust against cross-incarnation token misuse.

## 4. Candidate for Option B (requires separate approval)

One proposed C token representation, intentionally *not yet an API decision*:

```c
typedef struct cmeta_plugin_ref {
    uint32_t slot;
    uint32_t generation;
    uint64_t registry_incarnation;
} cmeta_plugin_ref;

typedef struct cmeta_plugin_lease {
    cmeta_plugin_ref plugin;   /* carries incarnation */
    uint32_t slot;
    uint32_t generation;
} cmeta_plugin_lease;
```

The Registry implementation retains the same `uint64_t` incarnation. A Ref is structurally valid only if slot, generation and incarnation are nonzero. Lease shape checking also requires a valid embedded Ref, but is **still not an active-lease authorization check**.

Mint a distinct nonzero incarnation each time a new Registry implementation is successfully published by `init`; the same Host C Registry object can be reused but never with the old incarnation. An implementation option is a **process-local bounded atomic identity allocator** using a CAS loop, initially at zero, incrementing to the next nonzero ID. If it reaches `UINT64_MAX`, reject additional new Registries rather than wrap/reuse. Unused IDs on failed allocation are permitted. The allocator is **only a uniqueness mechanism** and must own no DSO, lease or Registry resource. Document that identities/tokens are never shared across processes/fork or persisted to durable storage. Do not use pointer addresses, native DSO handles, wall clocks, random chance, or Host-provided duplicate epochs as proof of uniqueness.

Implementation ordering:
1. Allocate/initialize the Registry's internal slots and lock; mint incarnation before publishing `registry->impl`. A failed initialization cannot expose a partly live Registry. Concurrent initializations of distinct Registries must mint distinct IDs.
2. On `load`/`find`/`acquire`, emit the current incarnation in all returned public Refs and Leases. Validation must compare incarnation to the current Registry **before touching any slot/lease state**. For a live Registry, well-formed old credentials return `CMETA_PLUGIN_STALE`.
3. `release` checks incarnation before lease-slot active mask or generation and preserves the caller's token on rejection. A valid new lease remains active and releasable after old-token rejection.
4. `destroy` BUSY/UNLOAD_FAILED retains the **same** Registry implementation and incarnation for explicit Host recovery; no allocation of a new epoch and no implicit retry. Final successful destroy revokes that incarnation. `destroy` on an empty Registry remains idempotent.
5. Expired or absent Registry handles retain their existing invalid-argument behavior where applicable; do not promise that calls racing a final destroy are safe. Locking a freed implementation is not fixed by an epoch field.

## 5. ABI and consumer migration if Option B is authorized

- Current `CMETA_PLUGIN_ABI_VERSION` is 5. The new exact publication epoch would need an **explicit bump** (candidate 6, subject to coordination), not silent reuse of 5. `cmeta_plugin_query` must reject mismatched epochs before exposing manifest/descriptor pointers. No old-token adapter or compatibility fallback.
- Migrate all aggregate initializers, copy/compare helpers and public-size assumptions. In particular, `component-plugin/src/component_plugin.c::plugin_ref_equal` must compare incarnation as well as slot/generation; `salts_component_plugin_source` and `salts_component_plugin_module` retain the new token layout. Audit C++17 `salts::plugin_lease_scope`, C `cmeta_plugin_with_lease`, native loader tests and installed SDK headers.
- Add exact C11/C++17 struct size/alignment/offset checks on Windows MSVC, Linux GCC/Clang, macOS Clang, Android and iOS cross-compilers. Do not assume padding-free layout across targets.
- Update installed Native SDK/manifest exact pins and coordinated consumers (TurboAgent, TurboSCXML, CHttp, Turbo-Flow, etc.) only after a migration matrix is green. A historical build/TSan pass does not qualify the changed ABI.

## 6. Deterministic acceptance matrix for Option B

- [ ] Reuse the same Registry Host object after successful teardown: original/new `slot` and `generation` may be equal, but their incarnation differs; old Ref is `CMETA_PLUGIN_STALE`.
- [ ] With a **new valid lease currently active**, submit a copied historical Lease from a completed old incarnation; the old one returns `STALE`, does not clear the new active bit/count, and remains unchanged; then release the fresh lease exactly once.
- [ ] Two independent live Registries never share an incarnation even with equal Slot/Generation. Concurrent initialization uses deterministic synchronization rather than wall-clock timing.
- [ ] Failed/partial native close preserves the old Registry epoch and recoverable ownership until an explicit final Host destroy. An unrelated Registry remains unaffected.
- [ ] Same-Registry slot/lease generation fencing, zero-token rejection, exhausted allocator rejection and no hidden retry remain correct.
- [ ] Run full source-built first-party Linux TSan + ASan/UBSan, Windows/macOS installed public SDK consumer tests and exact ABI/release-pin audit; Android/iOS link-only results are not device-runtime claims.
- [ ] Negative compile/test gates reject old ABI-5 token layout and legacy struct assumptions; no dual-ABI fallback.

## 7. Boundaries / non-goals

- No implicit recovery from a failed native close, automatic settlement retry, or new hidden resource owner.
- No durable persistence of raw Registry implementation state, pointer values, Ref/Lease tokens or DSO handles.
- This RFC does not prove safety of callbacks after final Registry destruction; exclusive Host teardown and per-generation drain remain required.
- ComponentPlugin Generation ID and Registry-incarnation ID are **different identities**. Neither is a substitute for the other.
- **This document makes no ABI or behavior change.** Keep Salts #1156 open until the Option A/B decision is made explicitly; do not merge a runtime implementation on the basis of this RFC alone.
