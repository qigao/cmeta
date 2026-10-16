# Configurator / ServiceHost — canonical native runtime

The only installed lifecycle implementations are **Salts::Configurator** and
**Salts::ServiceHost**, with no Component/ComponentPlugin runtime aliases.
Salts::ComponentABI describes declarative CMeta Component metadata;
Salts::ComponentProviderABI exposes the borrowed ProviderBinding contract.

Configurator owns the bounded static graph and activation/rollback. It
does not load Plugin DSOs. ServiceHost uses the **existing** Salts::Plugin
Registry, validates complete borrowed FunctionDesc/FunctionAbi before
construction and owns generation publication, Scope admission, draining,
module lease settlement and final unload. Dynamic legacy R0, undefined ABI
carrier/direction/ownership and owning-self Interfaces fail closed; there
is no runtime ABI invocation fallback or second Plugin loader.

Generation, storage and Scope addresses remain stable. A failed N+1 does
not withdraw N. Existing borrowed N Scope stays valid during N+1 publish;
drain returns BUSY until original Scope release, then DSO lease settles.
At most two attached generations. No copied or moved live Scopes.

Installed C11/C++17 consumers use exactly one published Salts release tag:
`find_package(Salts CONFIG REQUIRED)`, link `Salts::Configurator` or
`Salts::ServiceHost`; Provider DSOs link `Salts::ComponentProviderABI`
and `Salts::PluginABI`. Public headers are `salts/configurator.h`,
`salts/service_host.h`, `salts/component_provider_abi.h` and
`salts/component_runtime_abi.h`. The installed epoch-1 ABI marker is
verified on all six SDK RIDs, and real native DSO tests execute on
Linux/macOS/Windows. Android/iOS installed consumers compile/link only.

Release requires six-RID installed ABI, source Sanitizer and Provider DSO
acceptance. No dependent may pin the implementation to a review SHA/branch
or reconstruct old Component/ComponentPlugin runtime naming.
