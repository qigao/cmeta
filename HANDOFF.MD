# Handoff

## 2026-10-31 repository cleanup

- GitHub Packages inventory requires `read:packages`, and deletion requires
  `delete:packages`; the default `gh` `repo` scope returns HTTP 403. Before
  deleting, filter user-scoped packages by `repository.full_name == qigao/cmeta`
  so packages owned by other `qigao` repositories remain untouched.
- CMeta CI is intentionally LLVM-only. Native and sanitizer builds run on
  Ubuntu with `CC=clang` and `CXX=clang++`; the Lean job uses the same compiler
  family for generated native code. Do not restore GCC, MSVC, TCC, Windows, or
  macOS compiler matrices without an explicit policy change.
- All local and remote Git tags were removed during this cleanup.
