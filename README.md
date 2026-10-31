# Salts 0.1 — CMeta, ACE and CFlow

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

```sh
cmake --preset user-release
cmake --build --preset release
ctest --preset release
```

The CI presets additionally take `CMETA_BINARY_DIR`, `CMETA_INSTALL_PREFIX`
and `CMETA_TARGET_TOOLCHAIN_FILE`. Android also takes `CMETA_ANDROID_ABI`.
Consumers select the combined installed prefix and link the canonical targets:

```cmake
find_package(Salts 0.1 CONFIG REQUIRED)
target_link_libraries(my_host PRIVATE CMeta::CMeta CMeta::ServiceHost)
```

Configurator and ServiceHost retain native ABI epoch 2.0, Provider contract v2
and Configurator binding v2. All hosts, generated bindings and Provider DSOs
must be rebuilt against one matching SDK. See
[ace/README.md](ace/README.md) for lifecycle and
ABI admission rules.

## Release 0.1.000

The immutable external tag is `v0.1.000`; CMake and NuGet normalize the same
version to `0.1.0`. `Salts.Native` contains combined SaltsCore and CMeta install
trees for Linux, Windows, macOS and Android, each on x64 and arm64.

Desktop targets use the pinned `qigao/vcpkg-cache` LLVM setup. Protected
Release compilation enables its required product-wide LLVM pass and the
workflow rejects caches without `QIGAO_COMPILER_OBFUSCATION_ENABLED=ON`.
Android uses NDK Clang for target compilation. The six desktop targets execute
the repository CTest suite on native runners; Android x64/arm64 are cross-build
and install checks and do not claim device execution.

Each SDK artifact records both the CMeta source commit and the exact SaltsCore
commit. The NuGet publisher combines those qualified artifacts without
rebuilding them, rejects an existing package version, then publishes the same
bytes to the GitHub Release.

Static source ownership policy: `python3 tools/check_ace_only_sources.py`.
