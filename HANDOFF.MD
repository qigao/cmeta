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
- Compiler obfuscation is forbidden for this release. The pinned
  `vcpkg-cache` actions must use `build-type: toolchain-only` with
  `obfuscator: false`; CMake still configures the product as Release. Admit the
  action output and reject any actual CMake cache with
  `QIGAO_COMPILER_OBFUSCATION_ENABLED=ON`. Android remains target-owned by NDK
  Clang and a host cross-build must not be reported as device execution.
- Keep both the per-RID SDK manifest and `packaging/nuget/feed_hygiene.py`
  aligned on `protection=none`; otherwise all platform builds can pass while
  the final fail-closed NuGet assembly rejects the correct artifacts.
- CI path filters admit only product source/public-header directories (and the
  formal Lean source directories). Workflow, build/configuration, test, tool,
  documentation and packaging files must not independently trigger CI. Tag
  publication remains event-driven and is not a file-path trigger.
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
- Product-wide obfuscation made macOS arm64 Release compilation exceed both
  150 and 240 minutes. Do not restore it. Keep the installed SDK graph in
  Release mode and qualify the same source with the separate LLVM Debug test
  graph (full build, CTest, and ABI owner audit).
- When pruning GitHub Actions runs, derive the current full SHA with
  `git rev-parse HEAD`; never hand-type it when deciding which run to cancel.
- Standalone installer/consumer CI and SDK-probe subprojects are forbidden.
  The old CMeta/CFlow/Plugin `tests/package_config` projects and their CTest
  registrations were removed; release staging install and NuGet assembly are
  retained inside the single SDK publication workflow.
- Run `tools/audit_native_owner.py` with the current five-path contract:
  `--core-tools`, `--core-prefix`, `--salts-prefix`, `--build-dir` and
  `--report`; legacy `--cmeta-prefix`/`--rid` arguments are not accepted.
- The former `component-runtime/` source path is now `ace/`. Public include
  paths are unchanged; do not restore a second compatibility directory.
- Repository-owned exported targets use `CMeta::` (including CMeta, Plugin,
  Configurator/ServiceHost, CFlow and retained CSTL targets). Only imported
  qigao/salts-core owners such as `Salts::Core`, `Salts::Platform` and
  `Salts::NativeIO` retain the `Salts::` namespace; do not add compatibility
  aliases for the old repository-owned names.
- Repository-owned public headers use `include/cmeta/`: ACE Configurator /
  ServiceHost and Plugin headers were moved from `include/salts/`. The
  `include/salts/` namespace is reserved for imported qigao/salts-core headers;
  do not restore forwarding headers or duplicate compatibility trees.
- The repository-owned release/package identity is `CMeta.Native`; its CMake
  package is `CMeta` under `lib/cmake/CMeta` and exports `CMetaTargets.cmake`.
  Per-RID release provenance is recorded in `cmeta-sdk-manifest.txt`.
  Do not confuse that identity with imported `SaltsCore` packages or restore
  `Salts.Native`, `find_package(Salts)` or `lib/cmake/Salts` compatibility
  aliases. The already-published `v0.1.000` release is immutable, so correcting
  its historical payload requires a new release version rather than retagging.
- Keep exactly three non-overlapping workflows: eight-RID LLVM SDK/release,
  LLVM sanitizers, and narrow Lean generated-contract verification. The
  redundant Linux source-cutover workflow was removed. Native workflow path
  filters must name files that actually enter their configured build graph;
  documentation, benchmarks, examples and local-only presets must not start
  the eight-RID matrix.

## 2026-11-01 Core value bridge and retained CSTL

- The Core `CValue`/fixed-width reflection bridge depends on CMeta descriptors,
  so it belongs here as `<cmeta/core_data.h>`, `<cmeta/core_fixed_width.h>` and
  `cmeta/src/core_data.c`; it must not be moved back into dependency-free
  SaltsCore or exposed under the old `cmeta_cmeta_data.h` name.
- The corrected split package is release `v0.1.001` / NuGet `0.1.1` because
  the historical `v0.1.000` tag is immutable. `CMeta.Native` must not bundle a
  second SaltsCore install tree; the release workflow builds against the exact
  Core commit in a separate prefix and packages only CMeta-owned files.
- When switching the Core prefix, reconfigure the selected preset with
  `--fresh`; an existing CMake cache can retain an older install. On Windows,
  use the llvm-mingw `llvm-test-ci` preset for an llvm-mingw Core SDK—native
  MSVC-ABI Clang links against a different CRT and is not equivalent evidence.
- Retained CSTL and CSTLStream are part of the CMeta build/export graph. Keep
  the root `add_subdirectory(cstl)` and use `BUILD_TESTING` for their formal
  CTest registration; omitting that subdirectory can make a package appear
  healthy while downstream container targets and metadata tests are absent.
- Source-ownership checks must validate the versioned `project(CMeta ...)`
  identity without hard-coding a patch release. When the pinned Core release
  advances, update both SDK and sanitizer workflows to the same immutable Core
  commit; otherwise a version-only release can fail before compilation.
- Raw instrumented/downstream Core configures must pass
  `SALTSCORE_BUILD_URI=ON`; Core's formal presets already do this, but omitting
  it from a direct `cmake -S` invocation fails before the sanitizer build.
