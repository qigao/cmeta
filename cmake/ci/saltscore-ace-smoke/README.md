# Salts M6 — installed Core consumer conformance (Draft)

This isolated project compiles the original **Salts CMeta and ComponentABI
CMake targets** and the unchanged **Configurator C11 implementation** against
a separately installed SaltsCore runtime. It does not compile or link legacy
Salts::Core, Salts::Platform, Concurrency, Coroutine, NativeIO or CNet.
The SaltsCore checkout is pinned to the exact PR #22 candidate SHA.

C11/C++17 tests check the original reflection and Configurator ABI,
an empty Configurator init/resolve/start/stop cycle, and a separate ACE
provider shared module. The executable and the module must share
mem_global() and same-thread cmeta_thread_current_token() identities.
Retained mem_slice_t buffers cross the image boundary in both directions.

This is test-only qualification, not a full Salts target switch. The
ServiceHost/Plugin Registry and the historical monolithic Salts::Core
are not built. An eventual M6 production PR must remove old binary owners,
retarget real package dependencies, run all downstream CMeta/CFlow/ACE
tests, and qualify the ABI epoch/SONAME and reload/rollback rules.

Tracking: qigao/salts#1287 and qigao/salts-core#20. Keep Draft.

## M6 ABI 0.1 runtime admission

This qualification now pins SaltsCore Draft #23 head `36ff9a4f`.
Both the real Salts CMeta/Configurator C11/C++17 consumer and its separate
ACE provider DSO must link the exact
`salts_core_runtime_abi_0_1_required()` marker, compare runtime ABI
version 0.1 and independently assert memory pool, buffer and slice layouts.
The final ELF/Mach-O/PE binary-owner audit also requires all these ABI
symbols to be exported only by the real installed SaltsCore::Core image.
This does not grant coexistence with original Salts::Core binary owners.

## Real non-empty Configurator graph (M6.1)

The original Salts `configurator_test_fixture.h` provides genuine CMeta
Interface and Component declarations, configuration-bearing Provider
bindings and owned ObjectRefs. The isolated C11 consumer now resolves a
two-component Logger → App dependency graph, verifies deterministic
activation order and typed Interface projection, reads the configured result
(7 + 5 = 12) and proves one deactivate/destroy/resource release per
provider. The test does not call the old monolithic Salts runtime.

The Core pin is now `38512062`, which requires the physical
Windows ABI-suffixed `-0.1` DLL names as well as the ELF/Mach-O loader IDs.
This is a test-only Configurator graph, not ServiceHost/PluginRegistry rollout.

## M6.2 — real PluginRegistry and ServiceHost acceptance

Original Salts `plugin/`, `component-plugin/` and
`component-runtime/` CMake targets now build against installed
SaltsCore #24 (`80394167`) with explicit
`SALTS_PLUGIN_USE_SALTSCORE` qualification. Production Salts retains
its original vstr/Salts::Core owner until coordinated migration.

Original ServiceHost generation tests run against two genuine Plugin
Provider DSOs, and a separate path gate probes overlong UTF-8,
surrogates, out-of-range and truncated paths without weakening
CMETA_PLUGIN_PATH_MAX. No runtime fallback or legacy binary is linked.

## M6.3 — independent installed `SaltsM6` qualification package

The real original CMeta/ComponentABI/Plugin/ServiceHost targets are
exported into a **test-only** package `SaltsM6`, not the production
`SaltsConfig` package. Its generated CMake config requires the
real installed `SaltsCore 0.1` owner. C11 and C++17 consumers now
configure from an unrelated build directory using only these installed
package files, invoke PluginRegistry/ServiceHost and verify the exact
Core ABI epoch and strict UTF-8 rejection. Any unexpected legacy
`Salts::Core/Platform/NativeIO/CNet` target fails configuration.

This remains Draft / no ABI release promotion and does not delete
old Salts binaries or authorize their coexistence with SaltsCore.

## M6.4 — scoped real Plugin/ServiceHost sanitizers

A separate Linux workflow instruments both the exact installed
SaltsCore #24 shared foundation and the original production Salts
CMeta/PluginRegistry/ServiceHost and dynamically loaded Provider DSOs.
ASan/UBSan with LeakSanitizer and TSan are mutually exclusive builds;
all original generation, strict UTF-8 and C11/C++17 tests are repeated
three times with fail-on-report, without suppressions. This does not
duplicate expensive GmSSL/vcpkg CNet provider CI on unrelated changes.
Passing ordinary CTest is not a substitute for both sanitizer results.

## Installed Provider DSO lifecycle conformance

SaltsM6 also installs its real original Provider fixture DSOs. A separate
installed C11 executable derives the absolute Provider path from the
resolved `SaltsM6_DIR` and refuses missing files. It uses the installed
PluginRegistry to load, start, acquire a live lease, verify the manifest,
request stop, reject premature unload with `CMETA_PLUGIN_BUSY`, release,
observe quiescence, unload and destroy. No build-tree Provider references or
fallback loaders are admitted. This extends the clean-prefix proof beyond
headers/ABI calls to real DSO lifecycle at the installed path.

## M6.5 — concurrent original ServiceHost generations

The original `service_host_concurrency_test.c` is now compiled and run
against the installed SaltsCore owner with the genuine dynamically loaded
Provider fixture. It exercises four concurrent readers, 32 bounded
publication/drain rounds, borrowed scope lifetimes, and generation fences.
The ASan/UBSan/LSan and TSan workflow also repeats this exact test
three times alongside the normal PluginRegistry generation test. No
stand-in scheduling model or skipped race cases are substituted.

## M6.6 — move both installed SDKs before external consumer acceptance

CI moves both the SaltsCore 0.1 and test-only SaltsM6 installation
prefixes to unrelated paths; their original prefixes MUST be absent.
It then configures, builds and runs external C11/C++17 ServiceHost/
PluginRegistry consumers again, using the *relocated* Provider DSO for
real registry start/lease/stop/unload, not a build-tree substitute.

The relocation ABI report records hashes, dynamic Core/Platform imports,
native loader 0.1 identities and sole exports for mem_global,
mem_buffer_release, mem_slice_release, cmeta_thread_current_token,
salts_core_runtime_abi_0_1_required and salts_core_utf8_valid.
Missing images/tools/symbols fail closed; upload one JSON per OS.
This is NOT a release or switch of the old Salts package.

## M6.7 — installed CFlow Graph and reactive ACE pipes

The original `Salts::CFlow` target now belongs to the test-only
`SaltsM6` export set. Independent installed C11/C++17 consumers
exercise CFlow Graph/take/status and the actual SaltsCore NativeIO
shared ABI without old Salts::Core/NativeIO/Concurrency owners.
These external consumers also rerun after both SDK roots are moved.
Source tests from unmodified CFlow status, stream-slice and reflected
ACE Pipes/Filters suites are repeated in the scoped Linux ASan/UBSan
with LeakSanitizer and TSan gates. No test is skipped to qualify
migration, and production CFlow remains unchanged unless explicitly
opted in by the isolated qualification build.

## M6.7 final-image native-owner audit

The relocated package scanner now includes **both standalone CFlow C11/
C++17 executables**, the NativeIO/Concurrency/Platform/Core shared
runtime binaries and the installed real Plugin DSO. The CFlow consumers
invoke `native_io_backend_init()` on a deliberately invalid config
(`SALTS_EINVAL`) and query `cmeta_thread_current_token()` to force
direct dynamic NativeIO and Platform imports rather than allowing linker
`--as-needed` to elide dependencies behind header-only checks.

ELF/Mach-O/PE scans require exact native 0.1 identities, one definition
each of `native_io_backend_init`, `cmeta_readiness_reactor_init_kind`,
`cmeta_readiness_backend_supported`, `cmeta_deadline_queue_init`
and the existing memory/thread ABI functions, and the appropriate
direct dynamic imports from the external CFlow executables.

## M6.9 — original NativeIO/Actor/Machine lifecycle conformance

The next stacked candidate compiles and executes **four unchanged
original CFlow C11 suites**: typed Event Mailbox with concurrent
producers, native Pipe Publisher/Subscriber cancellation and
acknowledgment, sharded NativeIO cross-owner routing/capacity and
drain, and resumable Machine Instance with cancellation/transaction
arbitration. These are linked to the actual imported
`SaltsCore::NativeIO/Concurrency/Platform` shared libraries
and original Salts CMeta/CFlow, never a legacy static owner.

All four run on Linux, macOS and Windows source CTests; the Linux
ASan/UBSan/LeakSanitizer and TSan jobs repeat them three times
alongside the qualified CMeta/ServiceHost/CFlow tests. Full
installation/relocation and single-owner ABI checks remain mandatory.
This is a new test-only stacked Draft, not a production cutover.
