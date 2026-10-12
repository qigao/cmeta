# Isolated Salts Component Runtime candidate

Tracking: [Salts #1154](https://github.com/qigao/salts/issues/1154) and [#1153](https://github.com/qigao/salts/issues/1153).
Branch: `feature/component-runtime-isolated` — **DO NOT MERGE / PUBLISH** until separate source, installed SDK and downstream gates pass.

This is a **Salts-owned** copy of the existing runtimes, not a SaltsUtils runtime. Original `component/` and `component-plugin/` source, exported ABI, CMake targets, default build, and applications remain unchanged. With `SALTS_ENABLE_COMPONENT_RUNTIME_CANDIDATE=OFF` (default) **none of these targets or tests are built**. The candidate does not install headers or export targets.

## Source provenance (baseline `c8a1a3d5`)

| Original source | Candidate source | Original blob |
| --- | --- | --- |
| `component/include/salts/component.h` | `component-runtime/include/salts/configurator.h` | `3c69f8ae5a6afdb5700e5f6a9b651e236bca6069` |
| `component/src/component.c` | `component-runtime/src/configurator.c` | `6bef730b093a3aef8cf1d76451b9f2e8926bc581` |
| `component-plugin/include/salts/component_plugin.h` | `component-runtime/include/salts/service_host.h` | `404280ded80a8d3069692ffb4cd7a4189918b4c6` |
| `component-plugin/src/component_plugin.c` | `component-runtime/src/service_host.c` | `6e71ac86972514f993a52a7c056e6041fc50c596` |
| `component-plugin/src/component_plugin_runtime.c` | `component-runtime/src/service_host_runtime.c` | `6b71a4db75d76b58eea5281bcc7114350dd74307` |

The implementation is mechanically copied/renamed. Shared `salts_component_provider_binding`, `salts_component_dependency`, `salts_component_status`, immutable CMeta semantic descriptors, and Plugin Provider Interface ABI remain in their **original provider-owned ABI**. Only host-side Configurator/ServiceHost records, enumerators, and functions are independently named; no duplicated provider ABI, no hidden ref-count and no new loader are introduced.

## Responsibilities

- `Salts::ConfiguratorCandidate`: static dependency resolution, exact instance construction/activation, reverse rollback and teardown. Uses `Salts::ComponentABI` but does not depend on Plugin.
- `Salts::ServiceHostCandidate`: copies existing generation publication, exclusive noncopyable scope, lifetime protection, BUSY drain, and concurrency, consuming the Configurator candidate and the **original** `Salts::ComponentPluginABI` + `Salts::Plugin`.
- `Salts::Plugin` remains the unique registry, loader, module lease and safe-unload authority; provider DLLs do not need a host runtime link.
- SaltsUtils [#620](https://github.com/qigao/salts-utils/issues/620) is a *subsequent code-generation adapter only*, fed by the existing canonical `salts-idlc` Contract IR. No new parser or runtime is implemented in SaltsUtils.

## Opt-in qualification

Configure the existing project preset with `-DSALTS_ENABLE_COMPONENT_RUNTIME_CANDIDATE=ON`. Build its configured graph and run candidate CTests (once added), then the existing baseline suites. Default/off is production parity. No auto fallback between old and new runtime. Never hand out a borrowed service after its enclosing scope is released, or allow a provider DSO to unload before its callbacks and ObjectRefs finish.

## Migration order

Copy/compile baseline -> static C11/C++ tests -> real loaded DSO and N-to-N+1 lifetime tests -> opt-in IDL generated adapter -> *one* downstream consumer per PR -> coordinated #1153 naming/SDK transition. Retire the old runtime only after migration and release gates; do not silently change historical artifacts or claim compatibility with an ABI-incompatible rename.

Status of this checkpoint: **copied source and opt-in CMake integration, no completed runtime CI evidence yet**.
