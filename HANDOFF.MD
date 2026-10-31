# Handoff

## 2026-10-31 repository cleanup

- GitHub Packages inventory requires `read:packages`, and deletion requires
  `delete:packages`; the default `gh` `repo` scope returns HTTP 403. Before
  deleting, filter user-scoped packages by `repository.full_name == qigao/cmeta`
  so packages owned by other `qigao` repositories remain untouched.
- CMeta CI is intentionally LLVM-only. Release qualification uses Clang on
  Linux/macOS, llvm-mingw on Windows, and NDK Clang on Android for both x64 and
  arm64. Do not restore GCC, MSVC, TCC or AppleClang release matrices without
  an explicit policy change.
- All local and remote Git tags were removed during this cleanup.

## 2026-10-31 release 0.1.000

- Release `v0.1.000` is represented as normalized `0.1.0` in CMake and NuGet;
  NuGet does not preserve the three-digit patch spelling.
- `vcpkg-cache` protected desktop Release actions require `obfuscator: true`
  and admission through `QIGAO_COMPILER_OBFUSCATION_ENABLED` in the actual
  CMake cache. Android remains target-owned by NDK Clang and a host cross-build
  must not be reported as device execution.
