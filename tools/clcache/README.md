# Audited MSVC clcache CI candidate

This C11/Win32 source derives from [leon-bckl/clcache](https://github.com/leon-bckl/clcache),
commit \`bfdc1c6989b6a36c46e8fdc621a7b3c35f4416ac\` (MIT license in LICENSE).
The original repo's pinned \`xxhash/xxhash.h\` and \`build.bat\` are used solely to
build the **checked-in reviewed C source**. Do not follow a floating branch.

CI scope: Windows + MSVC + Ninja. Other platforms retain ccache.

Hardening compared with the upstream snapshot:

* Admit explicit Ninja \`/showIncludes\` commands and replay the dependency lines
  to Ninja on both cache hits and misses.
* Preserve original MSVC argument ordering, path flags, and source/object names
  in the full-command cache key; hash relevant implicit compiler environment.
* Correct split-list and dependency metadata buffer sizes.
* Validate cached dependency metadata, reject deleted dependencies and protect
  cache payloads with atomic publication (dependency record committed last).
* Ignore cache for non-deterministic date/time macros, \`/Zi\`, \`/ZI\`, and PCH
  compilation or consumers until independently qualified.
* Opt-in \`CLCACHE_TRACE=1\` emits \`clcache: HIT/MISS\` diagnostics for acceptance.

Known boundaries and remaining verification:

* Native MSVC Debug \`/Zi\` PDB and \`/Yc\` / \`/Yu\` PCH are **not cached**.
* Response-file commands that cannot be parsed by upstream clcache fall through
  to the real compiler and must be measured in real Ninja builds.
* The UTF-8 dependency parser assumes an English MSVC \`/showIncludes\` prefix.
  Hosted Windows CI should use English compiler output.
* Cache performance and full SDK/ABI behavior require Windows CI proof.
  This candidate must not be merged on static code inspection alone.
* This implementation is only for CI; it does not change the published Salts
  runtime, SDK ABI, or consumers.
