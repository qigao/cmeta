# Installed Configurator / ServiceHost SDK qualification

This directory is an **out-of-tree consumer** for an installed Salts SDK. It
does not build a parallel runtime, use source-tree implementation archives, or
require candidate-only CMake flags. The same fixture is used by the release
workflow to qualify the exact staged SDK selected for each RID.

## What it proves

- CMake resolves `SaltsConfig.cmake` from the exact supplied install root,
  without falling back to another host or sysroot package.
- An ordinary C11 and C++17 consumer links and checks the Configurator and
  ServiceHost ABI version functions, the required ABI epoch link marker, and
  the public context/Scope layouts.
- A static-only consumer can link `Salts::Configurator` without importing the
  Plugin loader or ServiceHost archive.
- On native hosts, two distinct Provider DSOs are compiled against only the
  installed `Salts::ComponentProviderABI` / `Salts::PluginABI` imports. The
  installed `Salts::ServiceHost` consumer exercises generation N to N+1,
  rejection of an invalid provider, Scope lifetime, BUSY drain, and release of
  the original Plugin lease before final module unload.
- On Windows, dependent DLLs are copied only from the chosen SDK install next
  to the test executables. The test does not widen DLL lookup to PATH or cwd.
- Android/iOS cross-builds verify compile/link contracts only; they do not claim
  native execution on a device.

## Run against one installed SDK

Set `SALTS_INSTALLED_ROOT` to a single completed install tree, then run:

```sh
cmake -S component-runtime/sdk-probe -B build/servicehost-sdk-abi-consumer \
  -DCMAKE_BUILD_TYPE=Release \
  -DSALTS_INSTALLED_ROOT=/absolute/path/to/installed-sdk
cmake --build build/servicehost-sdk-abi-consumer --parallel
ctest --test-dir build/servicehost-sdk-abi-consumer --output-on-failure
```

Do not configure from a source/build tree and do not substitute privately staged
archives for the imported SDK libraries. Use the native or cross toolchain
arguments from the release workflow for the selected RID.

## Release rule

All six RIDs must come from the same exact source commit and contain matching
ABI headers, the canonical `Salts::Configurator`, `Salts::ServiceHost` and
`Salts::ComponentProviderABI` exports, and the matching runtime libraries.
Native installed C11/C++17 DSO tests must pass on Linux, Windows and macOS.
Release acceptance additionally requires the exact tag-scoped native, sanitizer,
formal, benchmark and package gates. A successful source build or one-RID probe
alone is not a release qualification.

The only supported runtime names are the canonical Configurator/ServiceHost
names. The retired `Salts::Component`, `Salts::ComponentPlugin` and
`Salts::ComponentPluginABI` runtime targets and headers must not be exported.
