# Salts.Native

Immutable native SDK payloads published from `qigao/cmeta`. Release tag
`v0.1.000` maps to the normalized CMake and NuGet version `0.1.0`.

The package contains eight LLVM-built install prefixes:

- `sdk/linux-x64` and `sdk/linux-arm64`
- `sdk/windows-x64` and `sdk/windows-arm64`
- `sdk/macos-x64` and `sdk/macos-arm64`
- `sdk/android-x64` and `sdk/android-arm64`

Every prefix contains the pinned SaltsCore install plus the CMeta, ACE and
CFlow install, including headers, libraries, `lib/cmake/SaltsCore` and
`lib/cmake/Salts`. Desktop Release code is compiled with the pinned
`vcpkg-cache` LLVM setup and its required product-wide obfuscator pass. Android
uses the Android NDK Clang target toolchain; these cross-builds prove
compile/install only, not device execution.

Select the prefix matching the target OS, CPU and ABI, add it to
`CMAKE_PREFIX_PATH`, then use:

```cmake
find_package(Salts 0.1 CONFIG REQUIRED)
target_link_libraries(my_host PRIVATE CMeta::CMeta CMeta::ServiceHost)
```

Windows payloads use llvm-mingw with UCRT and are not ABI-compatible with
MSVC-built dependencies. Each prefix carries `salts-sdk-manifest.txt` with the
release version, CMeta source SHA, pinned SaltsCore SHA, RID and compiler
protection provenance.
