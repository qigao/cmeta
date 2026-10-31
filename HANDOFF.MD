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
- The pinned salts-core checkout uses the module-owned layout (`core/src`,
  `platform/src`, `cnet/src`, and peers). Source-ownership gates must follow
  that public layout and must also prove the standalone `asn1/src/reader.c`
  owner; the pre-migration `src/<module>` paths are stale.
- Windows platform checks must use `_WIN32`, not `_MSC_VER`, when selecting the
  UCRT `_aligned_malloc`/`_aligned_free` pair. llvm-mingw does not define
  `_MSC_VER`; sending it through `posix_memalign` both fails compilation and
  would break the required allocator/deallocator pairing.
- The exact Core release pin includes the cross-compiler Windows DLL identity
  fix: CMake must publish `saltscore_<module>-0.1.dll` without llvm-mingw's
  default Unix-style `lib` prefix. CMeta applies the same top-level policy to
  its own provider/plugin DLLs. Keep the ABI audit fail-closed rather than
  accepting compiler-dependent filenames.
- Protected macOS arm64 Release compilation exceeded 150 minutes at Ninja
  parallelism 2 without a compiler or test failure. The release matrix uses
  parallelism 3 for that RID and a 240-minute fail-closed timeout; do not
  disable obfuscation or omit targets to hide the throughput cost.
