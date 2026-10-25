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
