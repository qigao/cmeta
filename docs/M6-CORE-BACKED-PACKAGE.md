# Unreleased Salts 3.0 Core-backed SDK candidate

This branch introduces the **opt-in actual Salts package** build mode,
`SALTS_CORE_BACKED_PROFILE=ON`, for the source split described in
qigao/salts#1287 / qigao/salts-core#20. The historical
`SALTS_CORE_BACKED_PROFILE=OFF` Salts **2.3.0** build and its dependencies
remain unchanged. Nothing is merged or released by this candidate.

## ABI choice and explicit scope

The candidate uses **project and package version 3.0.0**, with native
`SALTS_NATIVE_SOVERSION=3.0`, as an *unreleased distinct major ABI*.
`find_package(Salts 2.3 EXACT)` must reject the 3.0 package, so a legacy
application cannot silently resolve a reduced set of exported targets.
The chosen final published version and compatibility contract still
require release review.

The profile retains the unchanged original production implementation
for `Salts::CMeta`, `Salts::ComponentABI`, `Salts::PluginABI`,
`Salts::Plugin`, `Salts::ComponentProviderABI`,
`Salts::Configurator`, `Salts::ServiceHost`, and `Salts::CFlow`.
TinyTest is explicitly **not** exported or installed by this profile.
The independent original test suite builds original TinyTest from source
in its own test-only target, never making it a production dependency. All components use the
canonical `SaltsTargets` installation; this is **not** an alias for
the previously qualified temporary `SaltsM6` package.

The following historical state owners are deliberately *not built*,
not aliased and not exported in the 3.0 candidate:
`Salts::Core`, `Salts::Platform`, `Salts::Concurrency`,
`Salts::Coroutine`, `Salts::NativeIO` and `Salts::CNet`.
The former `utils/` monolith and other not-yet-qualified modules,
including Unicode/IDNA, ASN.1, CSerde and CSTL, are outside this
narrow first package profile. This is not a full-featured Salts 2.3
drop-in replacement.

Instead, a **real installed** SaltsCore 0.1 shared runtime is mandatory.
The new package config calls `find_dependency(SaltsCore 0.1 CONFIG REQUIRED)`;
both CMake and the final executable native-symbol auditor must reject a
duplicate static/Foundation owner. PluginRegistry's strict UTF-8 calls
the installed Core implementation; CFlow uses the imported
NativeIO/Platform/Concurrency. Provider and ServiceHost still run
original C11 source with real Plugin lease quiescence. There is no
runtime fallback.

## Qualification profile

A separate three-platform workflow installs an exact pinned,
independently built SaltsCore 0.1, configures this real Salts 3.0
profile with all unsupported legacy source-tree tests/features OFF,
installs it, and independently configures four C11/C++17 ACE/CFlow
consumers from `find_package(Salts 3.0 CONFIG REQUIRED)`. It compiles
an original test Provider DSO against only the installed public
Component/Plugin ABI and checks lease-lifetime/stop/unload behavior.
`find_package(Salts 2.3 EXACT)` is tested as an **expected rejection**.

Both installed prefixes are then moved, the old directories asserted
absent, and a new external C11/C++17 build must pass again. A
fail-closed ELF/Mach-O/PE audit checks the sole owner and native ABI
identities for Core/Platform/Concurrency/NativeIO plus the actual
dynamic imports of all executable consumers.

## Explicit non-goals

This profile does not remove the old production Salts 2.3 binary
graph, reconcile every downstream consumer, publish 3.0, or
authorize mixed old/native DSOs. Full Unicode/IDNA/ASN1, FS/Process,
nonquiescent unload, dependency closure and release/rollback policy
are later gated engineering tasks. **PR stays Draft, no merge/release.**

## Original source conformance on real SaltsConfig 3.0

The independent `core-backed-original-tests` project now imports
only the installed **real** `Salts 3.0` and `SaltsCore 0.1` CMake
packages. It compiles original Configurator C11/C++17, ServiceHost
generation/concurrency/C++, seven CFlow source test suites, and the
strict PluginRegistry UTF-8 rejection fixture (13 tests total).
The only additional DSOs are the original *test-only* Component
Provider fixtures; no historical Salts Platform, Core, NativeIO,
Concurrency, or CNet production sources are compiled.

All 13 tests run on Linux, macOS, Windows before **and after both
installation prefixes are moved**, with old locations absent.
The separate four-process installed consumer ABI and legacy Salts
2.3 admission-rejection gates remain mandatory. This qualifies a
reduced, unreleased Salts 3.0 ABI candidate, not a 2.3 upgrade.

## Linux sanitizers on the real SaltsConfig

A separate Linux workflow configures the exact imported **shared**
SaltsCore 0.1 and **actual installed Salts 3.0 CMeta/ACE/CFlow**
candidate under two independently instrumented modes:
ASan+UBSan with LeakSanitizer, and TSan.

The original 13 test sources and two real Provider DSO fixture
binaries are instrumented with the same flags as both native SDKs.
The workflow checks the exact 13 CTest registrations and repeats
each original suite three times, failing on any sanitizer report.
It does not substitute the earlier SaltsM6 test-only package,
skip OS tests, suppress failures, or embed old Salts::Core.

## Fail-closed negative ABI qualification

Three additional independent CMake configurations must fail *for the
expected reason*, on Linux/macOS/Windows:

- A deliberately malformed `SaltsCore 0.1` package advertises one
  `STATIC IMPORTED SaltsCore::Core`; the real Salts 3.0 profile must
  reject it as not an imported **SHARED** native owner.
- A test-only caller creates a legacy `Salts::Platform` before adding
  the actual production Salts source tree; Plugin/ServiceHost refuses
  the mixed old/new owner graph. No old Platform source is compiled.
- An independent client discovers only real installed `Salts 3.0`
  with discovery of `SaltsCore` disabled; `SaltsConfig` must reject
  it because Core is a mandatory `find_dependency`.

These tests capture CMake output and assert an owner-specific diagnostic,
so an unrelated configure failure does not count as a successful
negative ABI gate. The existing legacy `find_package(Salts 2.3 EXACT)`
rejection also remains mandatory. Malformed packages and injected
legacy target aliases exist **only in the negative test fixtures**;
the actual released package candidate contains neither.

## Test-only dependency excluded from real SDK

The reduced real Salts 3.0 package now exports exactly the eight
CMeta/ACE/CFlow production targets; `Salts::TinyTest` is explicitly
rejected in the public package. The full original test suite still
uses original TinyTest, but builds it **only inside the independent
test project**, preserving ABI behavior without shipping a native
test runtime or bringing its additional state into the SDK.

## Exact canonical production export set

Both independent C11/C++17 application and original 13-suite
conformance projects inspect the **actual imported target list**
created by `SaltsTargets.cmake`. The Core-backed Salts 3.0 SDK
must export exactly eight production `Salts::` targets:
CMeta, ComponentABI, PluginABI, Plugin,
ComponentProviderABI, Configurator, ServiceHost and CFlow.
Unexpected additions—including TinyTest, old Core/NativeIO/CNet,
or unqualified utilities—are a fatal configure error.

`SaltsCore::Core/Platform/Concurrency/Coroutine/NativeIO`
remain a separate imported shared dependency, not additions to
the Salts export set. The default legacy 2.3 package is unaffected.
