# Isolated installed SDK ABI probe — DO NOT MERGE / DO NOT PUBLISH

Stacked on Salts #1155 at `540e1da7c2448f15f04aeaaee1106f73a2fbcc7e`.
This is not a released SDK, not a stable ABI decision for Salts 2.3,
and not the full #1153 ComponentProviderABI naming cutover.

## Proposed contract

- Configurator/ServiceHost **new symbol ABI epoch 1.0**: runtime link/check
  `salts_{configurator,service_host}_abi_version() == 0x00010000`.
- Public import targets under the explicit probe only: `Salts::Configurator`
  and `Salts::ServiceHost`; static artifacts are named `salts_configurator`
  and `salts_service_host`. Candidate names are not exported.
- Existing `Salts::ComponentABI`, `Salts::ComponentPluginABI` and Plugin
  lifetimes remain unchanged for this additive stage. Final provider-facing
  `Salts::ComponentProviderABI` renaming is **not** implemented.
- A real released breaking naming cutover needs a separately approved SDK
  version/SONAME boundary (proposed next minor **2.4 / native 2.4**, not an
  unannounced 2.3 compatible patch). The probe is built on the 2.3 codebase
  strictly to evaluate packaging and linkage; **do not distribute it**.
- Default flags are both OFF for all published/native CI paths. The SDK
  probe requires `-DSALTS_ENABLE_COMPONENT_RUNTIME_CANDIDATE=ON` **and**
  `-DSALTS_ENABLE_COMPONENT_RUNTIME_INSTALLED_ABI_PROBE=ON`.
- Ordinary Configurator static clients do not link Plugin/ServiceHost; the
  ServiceHost static exported target propagates exact Plugin and Platform
  link-only dependencies through the installed CMake graph.

## External consumer proof

After configuring/building/installing the opt-in source into a *temporary*
`<prefix>` (not any released package), build only against its installed tree:

```sh
cmake -S component-runtime/sdk-probe -B build/sdk-abi-consumer \
  -DSALTS_INSTALLED_ROOT=<prefix>
cmake --build build/sdk-abi-consumer --parallel
ctest --test-dir build/sdk-abi-consumer --output-on-failure
```

The independent C11/C++17 consumers compile, link and test the version
functions and empty runtime init/destroy. Real two-DSO scope/generation/BUSY
drain tests remain in Salts candidate CTest and CHttp #249, but #249's
private staged archives **do not** qualify this installed export.

**Six-RID acceptance still requires**: the actual installed
`linux-x64`, `linux-arm64`, `windows-x64`, `macos-arm64`,
`android-arm64-v8a`, `ios-arm64` packages generated at one exact SHA;
matching header hashes, exported targets/import locations, C11/C++17 native
compile/link/run on the three host platforms, cross-compile/link on each
remaining RID, ABI mismatch rejection, strict real DSO lifetime, and
full-source sanitizer qualification. No partial success is called GREEN.
