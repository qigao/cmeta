# Static Component declarations

`<cmeta/component.h>` defines immutable CMeta component capability metadata for
the canonical Configurator and ServiceHost.

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

`CMETA_MANIFEST_COMPONENT` contains only the current immutable declaration
format. Runtime Provider bindings are separate, versioned native contracts.

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

Static composition passes an explicit CMeta manifest and component descriptor
to Configurator in `Salts::ACE`.

Dynamic composition admits the DSO through `Salts::Plugin`, holds a Plugin
lease over the borrowed component descriptor, and uses ServiceHost plus
Configurator in `Salts::ACE`. Provider declarations come from `Salts::Provider`.

## Canonical construction and ownership

`Salts::Provider` provides `<salts/configurator_abi.h>`: Configurator
status, dependency views and the version-2 ProviderBinding with exact create,
activate and deactivate callbacks. These callbacks construct objects; service
methods use the original typed CMeta Interface projection.

The same `Salts::Provider` target supplies `<salts/service_host_provider_abi.h>`.
Its reflected `salts_service_host_provider` Interface returns one borrowed
`salts_configurator_provider_binding`. Dynamic publication requires contract
`salts.service_host.provider.v2`, contract version 2 and binding ABI version 2.
Plugin leases cover descriptor admission, construction, invocation and teardown.
A mismatched contract is rejected before the construction callbacks run.

Inside `Salts::ACE`, Configurator owns one static dependency graph and
ServiceHost composes that graph with the existing Plugin Registry, generations
and Scopes.
`Salts::Plugin` remains the sole DSO loader and unload authority.

```c
#include <salts/configurator.h>
#include <salts/service_host.h>
#include <salts/service_host_abi.h>

salts_configurator_context context = SALTS_CONFIGURATOR_CONTEXT_INIT;
salts_service_host_runtime runtime = SALTS_SERVICE_HOST_RUNTIME_INIT;
salts_service_host_scope scope = SALTS_SERVICE_HOST_SCOPE_INIT;
```

Contexts must be initialized before first use. Live scopes and runtime bundles
stay at their original addresses; a scope is not copied or moved. Borrowed
Interface views expire when their enclosing scope is released. Publish N+1
changes admission for new scopes while old scopes retain N. Drain returns BUSY
until all retained scopes settle; object teardown precedes Plugin lease release.
A failed construction with unresolved ObjectRefs also keeps its module leases
until explicit repair and `salts_service_host_generation_recover()` settle it.

## Breaking SDK boundary

This source targets **Salts 4.0**, native runtime ABI **2.0**. All native hosts,
Provider DSOs, generated bindings and downstream libraries must be rebuilt from
one matching SDK. There are no compatibility headers, type aliases, old runtime
symbols or previous Provider contract fallback.

Use `find_package(Salts 4 CONFIG REQUIRED)` for the installed SDK. The library
exports ABI version/layout functions and `salts_service_host_abi_v2_0_required()`;
matching target names alone do not establish header/archive compatibility.

CMeta Reflection ABI 4, Plugin ABI 5, component metadata format 2 and CFlow ABI 5
remain independently versioned. This change does not silently reclassify their
historical CI results as acceptance of the new runtime ABI.

Original C11/C++17 graph, DSO generation/publication, concurrency, exclusive
resource fencing and initialization-failure tests execute from the canonical
build graph. Existing downstream applications qualify installed SDK use. No
standalone installer, consumer or SDK-probe project is part of this repository.
