# CMeta

[![CI](https://github.com/qigao/cmeta/actions/workflows/native-sdk-release.yml/badge.svg?branch=master)](https://github.com/qigao/cmeta/actions/workflows/native-sdk-release.yml)
[![License: Apache 2.0](https://img.shields.io/badge/License-Apache_2.0-blue.svg)](LICENSE)

**Reflection, component composition and structured execution in C11 and C++17.**

## Features and modules

| Module | Responsibility |
| --- | --- |
| [CMeta](cmeta/) | Type identity, reflection, callable ABI and object/data contracts |
| [ACE](ace/README.md) | Component configuration and service lifecycle |
| [Plugin](plugin/) | Loading, ABI admission, registries and leases |
| [CFlow](cflow/) | Typed graphs, streams, actors and reactive execution |
| [CSTL](cstl/) | Typed containers and stream integration |
| [TinyMock](tinymock/README.md) | Reflected mocks using Base TinyTest |

## Quick start

```sh
git clone https://github.com/qigao/cmeta.git
cd cmeta
```

Start with [.env.example](.env.example), restore matching upstream SDKs and
load your local `.env` into the parent shell. Follow the
[build instructions](#build-and-tests) for your compiler's CMake user preset.
CMake does not load dotenv files or restore SDKs implicitly.

## Salts ecosystem

- [SaltsBase](https://github.com/qigao/salts-base): native primitives, NativeIO, parsers and foundational utilities.
- [CMeta](https://github.com/qigao/cmeta): reflection, components, CFlow and containers.
- [Salts CNet](https://github.com/qigao/salts-cnet): networking, TLS/DTLS and cryptography.
- [SaltsUtils](https://github.com/qigao/salts-utils): templates, IDL/Schema, DataBind and language bindings.

## Upstream dependency versions

CMeta 0.1.7 selects SaltsBase 0.1.8 and vcpkg-cache 0.1.3.
The formal native test graph uses SaltsCNet 0.1.12 with the same Base SDK;
the existing `SALTSCORE_ROOT` input selects the `SaltsCNet` package prefix.
The tests link the shared `Salts::CNet` target, so they do not resolve the
unused static Crypto target's GmSSL dependency. Production CMeta does not
depend on the network SDK. Public CMeta APIs and ABI layout are
unchanged; the API compatibility floor remains Base 0.1.4.

The released CMeta 0.1.6 archives require Base 0.1.6. Their package configs and
release assets remain immutable. Build 0.1.7 against Base 0.1.8 to generate a
matching exact dependency in `CMetaConfig.cmake`, then rebuild downstream
hosts/plugins together. SDK publication follows successful qualification of the complete CI matrix.

Select the upstream SDK release through `VCPKG_CACHE_VERSION` in the parent
environment (for example, by loading the local `.env`). Use the same version
for the `VCPKG_CACHE_REPOSITORY_ROOT` checkout. Values contain the version
without the `v` prefix; missing variables fail rather than choosing `latest`.
The shared `resolve-release` entry point is required (not present in v0.1.2).

The per-triplet files in `cmake/dependencies/` are release descriptors. Their
historical `.lock.json` suffix is retained; `source.kind=github-release`
distinguishes them from generated literal URL/hash locks. Resolve the selected
release into the build directory before restoring it:

```sh
python "$VCPKG_CACHE_REPOSITORY_ROOT/scripts/restore-native-dependency.py" resolve-release \
  "cmake/dependencies/$VCPKG_TARGET_TRIPLET.lock.json" \
  --output-lock "build/dependencies/$VCPKG_TARGET_TRIPLET.lock.json"
python "$VCPKG_CACHE_REPOSITORY_ROOT/scripts/restore-native-dependency.py" locked \
  "build/dependencies/$VCPKG_TARGET_TRIPLET.lock.json" \
  --destination "build/dependencies/$VCPKG_TARGET_TRIPLET-$VCPKG_CACHE_VERSION" \
  --payload vcpkg_installed
```

Set `VCPKG_INSTALLED_DIR` to the reported root for the selected preset. The
resolver requires the published asset's SHA-256 digest; the existing restore
checks downloaded bytes and preserves immutable installations. Changing a
version never overwrites an old SDK. CMake does not load `.env` or download
the dependency SDK implicitly. Existing host-tool package locks remain exact.

CI uses the same named repository Actions variables and selects local shared
actions from the chosen infrastructure checkout. SaltsBase and SaltsCNet
prepare jobs resolve once, then distribute generated locks and verified blobs
to all matrix workers. Set the CI variables before enabling these changes;
the selected infrastructure release must contain `resolve-release`.
For 0.1.7, CI variables select `SALTS_BASE_VERSION=0.1.8`,
`SALTS_CORE_VERSION=0.1.12` and
`SALTS_CORE_REF=a29cea5465b5423e512cb63a17d5e76b84f2fdad` (CNet's published
0.1.12 source); `VCPKG_CACHE_VERSION` remains `0.1.3`. Activate these alongside
the source update so the source-selected CNet test SDK uses the same Base.

This keeps consumer version policy in the environment, asset digests in the
upstream release, and generated restore locks in build inputs. Updating a
release no longer requires editing a digest table for every platform. SDK
manifests, compiler/ABI selection and product tests remain mandatory. To roll
back, choose the previous exact release and restore into its own directory.


This repository owns CMeta reflection and metadata, ACE component/service
composition, Plugin lifecycle, CFlow execution and CSTL containers. The local
checkout is `cmeta/` (formerly `salts/`). Native primitives, parsers, coroutines
and I/O belong to [SaltsBase](https://github.com/qigao/salts-base); networking
and TLS belong to [Salts CNet](https://github.com/qigao/salts-cnet).

## Targets and dependency ownership

The package exports `CMeta::CMeta`, `CMeta::Provider`, `CMeta::ACE`,
`CMeta::PluginABI`, `CMeta::Plugin`, `CMeta::CFlow`, `CMeta::CSTL`,
`CMeta::CSTLStream` and `CMeta::TinyMock`. TinyMock uses Base's TinyTest runtime;
TinyTest itself is unchanged.

Production imports `Salts::Core`, `Salts::NativeIO` and `Salts::Coroutine`
from the exact installed Base SDK. Former Platform/Concurrency targets are
consolidated into Core. CFlow uses the public `threadpool_is_current()` to
check callback identity before joining a pool. Base retains the sole authority;
there is no private declaration or duplicate thread-local state in CFlow.
Base 0.1.4 or newer is required.

CMeta owns its existing fast-key implementation used by tracing and fault
controls. Native UUID storage follows Base's `uuid_t` byte array; CMeta retains
its stable `salts.uuid` metadata identity and 16-byte value layout.

## Build and tests

Copy `.env.example` to the ignored local `.env`, set machine paths, and import
those entries into the invoking shell. CMake does not read dotenv files.
`SALTS_BASE_VERSION` selects the exact installed SDK at `SALTS_BASE_ROOT`.
Its compiler, architecture and runtime must match the user preset. Missing
inputs and incompatible SDKs stop configuration.

The full test graph additionally uses `SALTS_CORE_VERSION` and `SALTSCORE_ROOT`
for existing CNet composition cases. These cases require a current split Core
SDK; the historical monorepo SDK with the same CMake version is insufficient.
The CMeta product package does not export this test dependency.

CFlow readiness fault injection requires `SALTS_BASE_SOURCE_ROOT` at the
selected Base release. Configuration compares the source and release Git trees
and rejects modified readiness headers. This explicitly approved test-only
exception supplies private declarations; installed NativeIO owns all runtime
state. Fake backends and contract fixtures belong to `cflow/tests/`.

With the environment loaded, Windows LLVM-MinGW full verification is:

```sh
cmake --fresh --preset native-release-user
cmake --build --preset native-release-user
ctest --preset native-release-user --output-on-failure --no-tests=error
```

Other native presets include `llvm-native-release-user`, `gcc-release-user`
and `msvc-release-user`. Sanitizer presets require matching SDKs. Each module
owns its tests and registration; there is no root test aggregator or nested
consumer verification project.

A product build needs only the Base SDK:

```sh
cmake --fresh --preset llvm-mingw-x64-release-user
cmake --build --preset llvm-mingw-x64-release-user
cmake --build --preset install-llvm-mingw-x64-release-user
```

The install prefix defaults to `build/<preset>/stage`. This normal installation
is the CMeta SDK consumed by salts-utils. Use `--fresh` after SDK version changes.

```cmake
find_package(CMeta CONFIG REQUIRED)
target_link_libraries(my_host PRIVATE CMeta::ACE)
target_link_libraries(my_provider PRIVATE CMeta::Provider)
```

Configurator and ServiceHost retain native ABI epoch 2.0, Provider contract v2
and Configurator binding v2. Rebuild hosts, generated bindings and provider
DSOs against one matching SDK. See [ACE lifecycle rules](ace/README.md).

The historical immutable `v0.1.003` tag denotes the earlier monorepo SDK. This
migration does not move or republish it; the split SDK requires its own release
qualification.

## CI qualification

CI downloads and verifies the selected Base/cache releases once, then passes
the same locked blobs and infrastructure snapshot to all ten published Base
profiles: Windows x64 MSVC/LLVM-MinGW, Linux x64 and ARM64 Clang/GCC, Linux x64
Clang TSan, macOS ARM64 Clang, and Android x64/ARM64. The former Windows ARM64
and macOS x64 lanes require Base SDKs that are not published in 0.1.4 and are
outside this matrix. ASan/UBSan presets remain available for explicit runs.

`SALTS_CORE_REF` is a required immutable source commit for the test-only CNet
SDK, whose normal product preset builds and installs the full configured
graph before CMeta's native tests. This declared source-SDK input is not a
fallback after a release restore failure. Production uses the released Base
SDK exclusively; Base source supplies only the approved white-box test headers.
All native jobs execute the complete CTest suite; Android jobs cross-build and
install without claiming device tests. SDK TAR artifacts retain manifests and
formal test diagnostics. This CI does not republish the historical NuGet tag;
a new release requires a new immutable version and qualified artifacts.

## Optional CNet policy adapter

`<cmeta/cnet_policy.h>` composes the new CNet resource/policy contracts without
introducing a CMeta dependency into SaltsCore. This adapter requires the
current split SaltsCore SDK; the historical 0.1.002 release
does not contain these declarations. Build and test against that SDK's
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

## Contributing

Open an issue for bugs or design proposals. Keep pull requests focused and
include reproduction steps and relevant CTest results. Follow
[AGENTS.md](AGENTS.md), keep tests with their owning module, and update API
documentation when behavior changes.

## License

First-party code is licensed under [Apache 2.0](LICENSE). Third-party code
retains its upstream licenses; see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
