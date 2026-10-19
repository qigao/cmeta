# Static Component declarations

`<cmeta/component.h>` defines immutable CMeta component capability metadata for
the Component Configurator work tracked by #1003.

A declaration contains only:

```text
stable component/provider identity
+ optional typed config DataDesc
+ ordered provides Interface rows
+ ordered requires Interface rows
```

It does **not** load a module, select a provider, create an instance, own a
lease, or advance lifecycle. Dynamic module authority remains
`Salts::Plugin`.

## Declaration

```c
#define DATABASE_METHODS(X,I) \
    X(I,F0,int,version,value,&cmeta_type_int,CMETA_ABI_SCALAR)
#define TLS_METHODS(X,I) \
    X(I,F0,bool,ready,value,&cmeta_type_bool,CMETA_ABI_SCALAR)

CMETA_INTERFACE(DatabaseDriver, DATABASE_METHODS);
CMETA_INTERFACE(TLS, TLS_METHODS);

cmeta_component(PostgresDriver,
    cmeta_provides(DatabaseDriver)
    cmeta_requires(TLS));
```

The generated `cmeta_component_desc` is TU-local immutable metadata.
`stable_id` is the expanded declaration identifier and is semantic component
identity for explicit selection/diagnostics. Descriptor addresses are not
identity across TUs or DSOs.

The provides/requires rows borrow canonical `cmeta_interface_desc` objects.
No method schema, ownership contract, vtable, handle, callback, or Plugin lease
is duplicated in the component descriptor.

## Static discovery

Component declarations participate only through explicit manifests:

```c
cmeta_registry(drivers,
    cmeta_manifest_component_entry(
        "postgres", cmeta_component_meta(PostgresDriver)));
```

`CMETA_MANIFEST_COMPONENT` retains numeric kind 1 from the 2.x static Plugin
declaration API, but its public semantic name and descriptor layout are now
Component. Numeric equality does not make the old descriptor compatible. There
is no `CMETA_MANIFEST_PLUGIN` compatibility alias in the unmerged 2.3.0 candidate SDK.

Use:

```c
const cmeta_component_desc *component = NULL;
cmeta_manifest_get_component(&drivers, 0u, &limits, &component);
```

and inspect one exact-role capability with
`cmeta_component_get_capability()`.

Queries are bounded by caller-provided manifest limits and leave outputs
unchanged on failure.

## Fingerprint

`cmeta_contract_fingerprint_component()` fingerprints the declaration format,
optional canonical configuration contract, ordered provides/requires **shape**
and canonical Interface contracts in fingerprint domain 6.

The component `stable_id` is validated but intentionally excluded from the
shape digest. Identity and contract shape are separate admission facts:

```text
stable_id        -> provider/component identity
component hash   -> provides/requires Interface shape
Interface checks -> canonical protocol compatibility
```

Equal hashes do not prove provider identity or authenticity.

## Static versus dynamic providers

Static:

```text
cmeta_component_desc
    -> explicit CMeta manifest
    -> Salts::Configurator
```

Dynamic:

```text
DSO
    -> Salts::Plugin manifest/export
    -> live Plugin lease
    -> borrowed cmeta_component_desc
    -> Salts::ServiceHost
    -> Salts::Configurator
```

**Current runtime names and ownership (2026-10-19):** CMeta declares immutable
component/capability metadata; `Salts::Configurator` selects and runs one
static dependency graph; `Salts::ServiceHost` owns published generations,
Scope admission/drain and settlement of the Plugin leases held for dynamic
Providers; `Salts::Plugin` remains the only DSO loader/Registry/quiescence/
unload authority. Older ACE freeze documents and historical test names may say
`ComponentPlugin`; that historical name is not an installed runtime target or
a compatibility alias. The public runtime targets are `Salts::Configurator`
and `Salts::ServiceHost`.

A dynamic consumer must hold the Plugin lease through every descriptor,
Interface, callback, instance teardown, and dependent cleanup that can reach
provider-owned code or metadata.

## Migration from Salts 2.x

The most recent published Salts Release is **2.2.0**. The 3.0.0 Release
was withdrawn and the next release identifier is **2.3.0**, not 3.0.0 or
the former interim 4.0.0 proposal. The static `<cmeta/plugin.h>`
declarations shipped in 2.2.x are replaced with Component metadata in the
2.3.0 candidate. This is an intentional **source and native ABI break
within major 2**; no legacy aliases or runtime fallback are provided.

| Salts 2.2.x static metadata | 2.3.0 candidate replacement |
| --- | --- |
| `<cmeta/plugin.h>` | `<cmeta/component.h>` |
| `cmeta_plugin`, `cmeta_plugin_empty`, `cmeta_plugin_meta` | `cmeta_component`, `cmeta_component_empty`, `cmeta_component_meta` |
| `cmeta_plugin_desc`, `cmeta_plugin_capability`, `cmeta_plugin_role` | `cmeta_component_desc`, `cmeta_component_capability`, `cmeta_component_role` |
| `CMETA_PLUGIN_DECLARATION_VERSION` (1) | `CMETA_COMPONENT_DECLARATION_VERSION` (2) |
| `CMETA_PLUGIN_PROVIDES`, `CMETA_PLUGIN_REQUIRES` | `CMETA_COMPONENT_PROVIDES`, `CMETA_COMPONENT_REQUIRES` |
| `CMETA_MANIFEST_PLUGIN`, `cmeta_manifest_plugin_entry` | `CMETA_MANIFEST_COMPONENT`, `cmeta_manifest_component_entry` |
| `cmeta_manifest_get_plugin`, `cmeta_plugin_get_capability` | `cmeta_manifest_get_component`, `cmeta_component_get_capability` |
| `cmeta_contract_fingerprint_plugin` | `cmeta_contract_fingerprint_component` |

`cmeta_provides` and `cmeta_requires` retain their spellings for
Component roles. Dynamic `<salts/plugin.h>` and `<salts/plugin_decl.h>`
remain the sole module-load/lease authority.

Migrate the generated declaration header, macro, metadata accessor and
manifest entry together. Manual format-1 descriptors must be rebuilt for
format 2: `name` becomes `stable_id`, `count` becomes
`capability_count`, and optional `config` borrows a canonical typed
DataDesc with a valid native `storage_type`. Kind-only Sequence/Set/Map
schema descriptors cannot be admitted as Component configurations. Never
cast a format-1 record into a format-2 record.

Regenerate all stored contract fingerprints. Format 2's domain-6 digest
includes config and declaration-format semantics, while stable identity
is validated independently. A 2.2.x fingerprint is not a 2.3.0 token.

CMake package admission uses `SameMinorVersion`; even a non-EXACT
`find_package(Salts 2.2 CONFIG)` request against the 2.3 SDK must
fail. SDK-owned shared targets use SONAME epoch `2.3` rather than the
released 2.2 epoch. CFlow's independently versioned ABI and negotiated
Reflection/Plugin protocol epochs are not tied to SDK SemVer. Rebuild
each consumer, provider DSO, host and contract cache with one immutable
2.3.0 candidate; do not mix 2.2 and 2.3 headers, layouts or binaries.

Rollback restores the entire matching 2.2.x deployment and its caches.
There is no in-place conversion or raw runtime-state migration.

## Explicit sanitizer qualification for #1018

The long-lived Draft PR may temporarily use `[ACE-SAN]` in its title to run
two independent Linux debug host profiles without packaging or benchmarking:
`linux-dev-ci` enables ASan+UBSan using the canonical `cmake/Sanitizers.cmake`,
and `linux-tsan-ci` runs TSan separately (never combined with ASan).
Both build complete targets using CMake presets and execute the focused
Component, ComponentPlugin DSO/Scope, Actor, Pipes/Filters, CNet TCP and
Half-Sync/Async CTest contracts. No new test runner, orchestration wrapper,
global state or automatic retry is introduced. `[ACE-SAN]` and
`[ACE-MATRIX]` are mutually exclusive.

Sanitizers instrument the branch-built host/runtime, not Android/iOS or
prebuilt vcpkg dependencies; a sanitizer GREEN is *not* a 2.3 binary
compatibility or provider package release. Document any incompatible runner
or unsupported TSan environment as such rather than declaring unexecuted
tests passed. Remove the temporary marker to return to Linux-only daily CI.

## Draft-only full native qualification for #1018

The normal `feature/cmeta-ace-patterns` PR checks only Linux full CTest and
Lean. During a deliberate branch integration checkpoint, add `[ACE-MATRIX]`
to the **Draft PR title**, which selects complete host CTest on Linux GCC/Clang,
Windows MSVC and macOS GCC/Clang, the portable Linux arm64 contract subset,
and Android/iOS compile profiles. This uses the exact PR HEAD and does not
publish, package or authorize any new SDK version. CNet/NativeIO transport
benchmarks remain excluded from this ACE branch mode; they require their own
performance qualification. Remove the marker to restore Linux-only iteration.

No host test result substitutes for mobile device execution or downstream
exact-package qualification, both of which remain open in #1018.

## Unreleased Salts 2.3.0 native SDK candidate migration

**Status: Draft integration branch only; not published.** The next
Salts release is 2.3.0, following the published 2.2.0. The withdrawn 3.0.0
release and interim 4.0.0 proposal are superseded. The new Component,
Configurator, Context and noncopyable ComponentPlugin Scope contracts will be
qualified as a coherent 2.3.0 candidate. No 2.3 tag, release or deployment is
approved by Draft PR #1013.

For native consumers migrating from 2.2.0:

1. Rebuild each host, provider DSO and dependent library against the exact
   immutable 2.3 SDK. Pin generated CMake package identity, NuGet SHA and
   digest; do not combine 2.2 records or fingerprints with 2.3 binaries.
2. Initialize contexts before use:
   `salts_component_context context = SALTS_COMPONENT_CONTEXT_INIT;`.
   READY/RESOLVED/ACTIVE reinitialization is rejected without losing object
   ownership. Exclusive STOPPED or fully rolled-back FAILED reuse is allowed
   after dependent borrowed views expire.
3. Keep a live `salts_component_plugin_scope` at its original address.
   Copies and moves are not independent owners; they cannot release the
   generation or borrow services. Release the original once all callbacks,
   tasks and service views have quiesced.
4. `find_package(Salts 2.3.0 EXACT CONFIG REQUIRED)` must succeed.
   `find_package(Salts 2.2 CONFIG)` must fail; CMake's
   `SameMinorVersion` rule rejects older minor versions. SDK-owned native
   shared libraries use SONAME 2.3 (rather than 2.2), while the independently
   versioned CFlow ABI remains unchanged. No 2.2 fallback is provided.
5. Negotiated Reflection ABI 4, ordinary Plugin ABI 5 and
   ComponentProvider contract v1 remain independent epochs. Do not change
   them merely because the package is named 2.3.0.
6. CHttp request/mount/deferred, TurboFlow ExecutionPlan/durable and
   TurboSCXML Session/invocation lifetimes remain domain-owned. Their
   workflows must pin the *same* immutable 2.3.0 candidate and record
   source SHA, version, NuGet hash and executed test results. Previous
   3.0.0 prerelease candidate results are historical, not 2.3 acceptance.

Rollback replaces the entire SDK, provider binaries, host and contract
cache with the matching previous 2.2.x set. No mixed-epoch binary linking,
in-place runtime-state conversion or automatic settlement retry.

### Opt-in 2.3.0 ACE candidate (not a release)

Draft PR #1013 stages `.github/workflows/ace-candidate-native-sdk.yml`.
Only a same-repository PR from `feature/cmeta-ace-patterns` into master
that is still Draft and explicitly titled
`DO NOT MERGE ... [ACE-CANDIDATE] ...` may execute the candidate.
Ordinary PR commits do not publish. The job validates exact HEAD, matching
2.3.0 CMake/vcpkg versions, full Linux CTest, and independently
compiled/linked installed Component and Scope consumers, including rejection
of a 2.2 package-version request.

The only eligible package is immutable Linux-x64
`Salts.Native 2.3.0-ace.sha<FULL_COMMIT_SHA>` with source manifest
and SHA256 checksum. It cannot create a stable tag or GitHub Release, merge
PR #1013, update master, or replace Windows/macOS, sanitizer, mobile device
runtime or pinned downstream qualification. A skipped workflow is not
evidence of a released or accepted SDK.

## Component Context initialization contract (2.3 development branch only)

The long-term `feature/cmeta-ace-patterns` development branch under #1012/#1014
requires a **defined first-use state** for a static Component context:

```c
salts_component_context context = SALTS_COMPONENT_CONTEXT_INIT;
/* ... */
salts_component_context_init(&context, /* deployment/storage arguments */);
```

The earlier unpublished Component prototype accepted uninitialized stack
storage, but the 2.3 candidate requires a defined first-use state and reads
`context.state` to reject reinitialization
from READY, RESOLVED or ACTIVE **before** overwriting any live ObjectRef.
The caller must therefore explicitly initialize storage in C11/C++17. After
normal stop or fully rolled-back failure, exclusive reuse is allowed; any
borrowed Interface/service views must have expired. No legacy admission path
or global registry is introduced.

**Compatibility:** this Component model was not part of the published
2.2.0 SDK. All new 2.3 consumers must honor the explicit first-use rule and
be rebuilt as one compatible set. #1014 tracks this ownership invariant,
and #1018 tracks package admission. Draft PR #1013 remains unmerged.

## Scope ownership and re-publishing after close (development branch only)

On `feature/cmeta-ace-patterns` (umbrella #1012, follow-up #1017), each live
ComponentPlugin scope has an owner-address check. A shallow copy is **not**
another reference: scope release and service lookup reject it without affecting
the generation's scope count. Live scopes must remain address-stable. This is
defensive misuse detection, not a C memory-safety/security boundary.

`runtime_close()` removes the current published generation and closes new
admission, but it does not destroy the runtime. Existing admitted work remains
pinned to its generation; an explicit, higher-ID `runtime_publish()` may later
re-open admission within the same two-attached-generation bound. The final
`runtime_destroy()` requires all attached generations to be fully drained.

The owner-address field changes Scope layout compared with an unpublished
interim prototype. The 2.3 candidate layout must be consumed from its exact
installed SDK; native SONAME 2.3 and SameMinorVersion admission isolate it
from the published 2.2 binary epoch.

## Generation concurrency and resource admission

`Salts::ComponentPlugin` builds a complete candidate before publication. Its
runtime mutex serializes publication, scope admission/release, close and the
drain claim. A scope pins one immutable generation: publication changes only
new admissions, and drain returns BUSY while any scope still pins that
generation. Close rejects subsequent acquisitions; existing scopes can still
use their services until release. Component stop and ObjectRef cleanup finish
before module leases are released. At most two generations are attached.

Runtime initialization returns `SALTS_COMPONENT_PLUGIN_RESOURCE_ERROR` if the
platform mutex cannot be created. The runtime stays zero/uninitialized and can
be retried; failed initialization never enables admission without a lock.

Keep the runtime, registry, generation bundles and caller-owned storage alive
and address-stable until scopes are released and all attached generations are
drained. A live scope has one owner and must not be copied or used concurrently
with its release; service views expire at release. Provider methods follow their
own concurrency contract. Runtime init/destroy and generation build/discard or
reuse require exclusive access to their objects. Do not read mutable runtime
or generation fields while another thread is changing them.

Formal CTest qualification includes a platform-adapter mutex failure/retry
test, DSO-backed readers holding old/new scopes across publication and close,
repeated acquisition races with publication, and concurrent drain claims that
release the provider lease exactly once. These tests do not replace a
ThreadSanitizer run when a supported runner is available.

CMeta itself owns no loader or dynamic service registry.

## Current integration scope

Format 2 contains stable identity, optional typed config, and provides/requires.
Typed configuration reuses canonical `cmeta_data_desc` identity directly.
External YAML/JSON/XML/CLI parsing remains outside CMeta. Factory and lifecycle
execution authority are deliberately not embedded in the descriptor; they
belong to the Component runtime binding under #1008.

The Component descriptor and canonical Configurator/ServiceHost runtime
are integrated into master source; the latest published Salts release remains
2.2.0, so installed 2.3 SDK/package and downstream acceptance are separate
from source integration. The current ServiceHost SDK audit is tracked in Draft
PR #1260. No stable release, tag or downstream cutover is authorized by that
PR. The older ACE freeze and coverage records retain their source-SHA scope and
historical `ComponentPlugin` terminology; consult the canonical runtime docs
above for supported targets and ownership.
