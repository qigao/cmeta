# CMeta 0.1 — CMeta, ACE and CFlow

This repository owns the CMeta reflection/type system, ACE-style component and
service composition, Plugin lifecycle integration, and CFlow actor/reactive/
graph execution. Native platform and I/O implementations belong to
[`qigao/salts-core`](https://github.com/qigao/salts-core); cryptography and
ASN.1 belong to the Crypto repository.

## Canonical targets

The installed package exports:

- `CMeta::CMeta`
- `CMeta::ConfiguratorABI` and `CMeta::Configurator`
- `CMeta::PluginABI` and `CMeta::Plugin`
- `CMeta::ServiceHostProviderABI` and `CMeta::ServiceHost`
- `CMeta::CFlow`

SaltsCore supplies the imported shared owners `Salts::Core`,
`Salts::Platform`, `Salts::Concurrency`, `Salts::Coroutine` and
`Salts::NativeIO`. Each native function has one source and binary owner; this
repository does not retain compatibility copies or fallback implementations.

## Build

Install the matching SaltsCore 0.1 SDK, set `SALTSCORE_PREFIX`, then use the
versioned presets:

For the local LLVM build, set `CMETA_TARGET_TOOLCHAIN_FILE` to the product
compiler toolchain used for that Core SDK. CMeta requires only the installed
Core SDK; it does not find or link Crypto/GmSSL and needs no provider dependency
prefix. Core's private transport implementation is owned by its shared runtime.

Tests are ordinary executables in the project build graph, registered with
CTest. No test installs the SDK, configures another project, or invokes CMake
scripts to build/run a consumer. SDK installation and release assembly remain
normal build operations, separate from test execution.

```sh
cmake --preset user-release
cmake --build --preset release
ctest --preset release
```

The CI presets additionally take `CMETA_BINARY_DIR`, `CMETA_INSTALL_PREFIX`
and `CMETA_TARGET_TOOLCHAIN_FILE`. Android also takes `CMETA_ANDROID_ABI`.
The Android preset declares `SALTSCORE_PREFIX` as both the installed package
prefix and a target search root. Package lookup stays `ONLY` within the target
roots, so the NDK can resolve this ABI's Core SDK without searching host packages.
Consumers select the combined installed prefix and link the canonical targets:

```cmake
find_package(CMeta 0.1 CONFIG REQUIRED)
target_link_libraries(my_host PRIVATE CMeta::CMeta CMeta::ServiceHost)
```

Configurator and ServiceHost retain native ABI epoch 2.0, Provider contract v2
and Configurator binding v2. All hosts, generated bindings and Provider DSOs
must be rebuilt against one matching SDK. See
[ace/README.md](ace/README.md) for lifecycle and
ABI admission rules.

## Release 0.1.003

The immutable external tag is `v0.1.003`; CMake and NuGet use version `0.1.3`.
`CMeta.Native` contains CMeta, ACE and CFlow install trees for Linux, Windows,
macOS and Android, each on x64 and arm64. SaltsCore is published and restored
separately as `SaltsCore.Native`. This release consumes the flattened public
headers from SaltsCore 0.1.2 and exposes no compatibility `salts/` tree.

Desktop targets use the pinned `qigao/vcpkg-cache` LLVM setup in toolchain-only
mode. All targets build the Release profile with compiler obfuscation disabled;
the workflow rejects an action output or CMake cache that enables it. Android
uses NDK Clang for target compilation. The six desktop targets execute the
repository CTest suite on native runners; Android x64/arm64 are cross-build and
install checks and do not claim device execution.

Each SDK artifact records both the CMeta source commit and the exact SaltsCore
commit. The NuGet publisher combines those qualified artifacts without
rebuilding them, rejects an existing package version, then publishes the same
bytes to the GitHub Release.

Static source ownership policy: `python3 tools/check_ace_only_sources.py`.

## Optional CNet policy adapter

`<cmeta/cnet_policy.h>` composes the new CNet resource/policy contracts without
introducing a CMeta dependency into SaltsCore. This adapter requires the
matching SaltsCore `61d42472583e9923ca22ea2929c651b6a4c12450` SDK; the released 0.1.2
SDK does not contain these declarations. Build and test against that SDK's
installed prefix before qualifying a combined release.

The adapter provides read-only descriptors for `cnet_send_limits`,
`cnet_send_usage`, `cnet_backoff_input` and `cnet_pool_snapshot`. They support
inspection through the normal CMeta Data/Object APIs and grant no construction
or mutation authority over live I/O owners. Native policy callbacks and
providers remain explicit typed bindings supplied by the host.

For a caller-owned canonical slice, arm a cleanup obligation and bind it to
the existing C++ scope (C callers use `cmeta_with_cleanups`):

```cpp
cmeta_cleanup obligation = CMETA_CLEANUP_INIT;
if (cmeta_cnet_slice_cleanup(&obligation, &owned_slice) != CMETA_OK) {
    // Handle the error; the caller still owns the slice.
    return;
}
cmeta::cleanup_scope guard(obligation);
// Submit through CNet. Successful admission retains its own reference.
// Leaving this scope, including by exception, releases only owned_slice.
```

Keep the slice and cleanup record address-stable until scope exit. The buffer
adapter similarly accepts a `mem_buffer_t **` and clears the caller's slot on
release. Neither helper drives I/O nor destroys a pool. Explicitly stop, drain
and destroy CNet/Manager/Pool owners and handle their errors; these fallible
operations are not hidden in lexical cleanup.

The original `salts.native.cmeta_data_temp` and `salts.native.owner_cpp17`
tests cover descriptor inspection, error-return cleanup and exception unwind.
