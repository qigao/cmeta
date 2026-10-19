# Configurator / ServiceHost — canonical native runtime

This directory contains the single canonical native component lifecycle runtime.
The public runtime names are **Salts::Configurator** and **Salts::ServiceHost**.
There is no second Component/ComponentPlugin lifecycle implementation or
compatibility target.

## Ownership

- **CMeta** owns immutable type, Interface, FunctionDesc/FunctionAbi, DataDesc
  and Component capability metadata. Descriptors do not load modules or own
  runtime instances.
- **Salts::Configurator** owns bounded provider selection, dependency
  resolution, activation order, instance creation, rollback and reverse stop
  for one static graph. It does not load DSOs or own generation publication.
- **Salts::ServiceHost** composes Configurator with the existing Plugin
  Registry. It owns generation construction, publication, Scope admission,
  in-flight generation protection, draining and settlement of the Plugin
  leases held for each generation.
- **Salts::Plugin** remains the sole DSO loader, Registry, plugin lifecycle,
  quiescence and unload authority. ServiceHost does not implement a second
  loader or independently invoke Plugin stop/unload operations.

A Plugin reference supplied to ServiceHost must already refer to a loaded and
started Plugin in the supplied Registry. ServiceHost acquires leases before
reading provider-owned exports or descriptors, and releases those leases only
after the component objects have stopped and been released. The application or
Plugin manager remains responsible for requesting Plugin stop, observing
quiescence and unloading a module when no other owners or leases remain.

## Graph and generation lifecycle

Configurator resolves an explicit provider selection. Missing or ambiguous
providers and dependency cycles fail closed; there is no implicit ranking or
fallback. The graph is activated in dependency order and rolled back in reverse
order if creation, activation or Interface projection fails.

ServiceHost builds a complete candidate before publication. A failed N+1 build
does not withdraw the current N. Publication atomically changes admission for
new Scopes; existing Scopes keep the old generation alive. Drain returns
`SALTS_SERVICE_HOST_BUSY` while a generation still has active Scopes, and
stopping its components precedes release of the generation's Plugin leases.
At most two generations may remain attached to one runtime.

Runtime, generation bundles and caller-provided storage must remain alive and
address-stable until their lifecycle contracts are complete. A live Scope is
single-owner and non-copyable/non-movable by contract; the owner-address check
is defensive misuse detection, not a memory-safety boundary. Each Scope is used
by one thread at a time, while the provider defines its own method-level
concurrency semantics.

## ABI admission

Dynamic Provider admission validates the borrowed provider contract and its
complete borrowed-service FunctionDesc/FunctionAbi requirements while the
Plugin lease is live and before construction callbacks run. Static deployments
use the Configurator's descriptor and binding validation in the same process;
they do not cross the dynamic DSO trust boundary. Dynamic legacy/undefined
carriers, directions, ownership and owning-self Interface shapes fail closed.
There is no arbitrary runtime native-call reconstruction or legacy ABI fallback.

The runtime uses native ABI epoch 1.0. Check the version functions and the
link-time epoch marker against the installed library; a matching header or
CMake target name alone is not proof that the linked binary has the same ABI.

## Installed consumers and release acceptance

Use one immutable published Salts SDK for the host and all native providers:

```cmake
find_package(Salts CONFIG REQUIRED)
target_link_libraries(my_host PRIVATE Salts::ServiceHost)
# A static-only composition client can link Salts::Configurator instead.
```

Provider DSOs use `Salts::ComponentProviderABI` and `Salts::PluginABI`.
The public headers are `salts/configurator.h`, `salts/service_host.h`,
`salts/component_provider_abi.h` and `salts/component_runtime_abi.h`.

The SDK workflow must verify the installed exports and ABI header for all six
RIDs: Linux x64/arm64, Windows x64, macOS arm64, Android arm64 and iOS arm64.
Native installed C11/C++17 Provider DSO lifecycle tests execute on Linux,
Windows and macOS. Android/iOS checks are compile/link checks, not claims of
device execution. The final immutable package must independently contain the
canonical headers, imported CMake targets and corresponding libraries. A green
ordinary master build or a staged source-tree archive is not a substitute for
release-tag and package-provenance acceptance.

Do not pin implementation sources to a review SHA/branch, restore the retired
runtime names, or add fallback loaders/ABI invocation paths.
