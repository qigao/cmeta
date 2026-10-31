# CMeta repository instructions

CMeta focuses on **CMeta / ACE**. Its component runtime implements
Configurator/ServiceHost/Plugin composition; CFlow supplies accompanying
CMeta-based execution integrations. New OS, concurrency, coroutine,
transport, Unicode/IDNA, native
FS/Process implementations belong to **qigao/salts-core**;
cryptographic code and ASN.1 belong to **qigao/crypto**. Source ownership is singular.

- This package exports only `CMeta::XXX` through `find_package(CMeta CONFIG REQUIRED)`.
  Do not add a version or EXACT to CMeta package lookups.
  Core, Crypto and Utils retain their own `Salts::XXX` targets; do not rename
  dependency-owned exports.
- No legacy interface, compatibility shim, alias, fallback, provider DSO
  for Crypto, copied native implementation or alternate build profile.
- No CMeta dependency is permitted inside the Core/Crypto repositories.
- Preserve bounded owner/lease/cancel/stop/unload behavior and exact-head
  source, installed SDK and native CI evidence.
- Never substitute one OS's successful tests for another's native backend.
- Run `python3 tools/check_ace_only_sources.py` before changes to the
  package layout. Verify installed C11/C++17 consumers and original typed
  runtime conformance after changes.

`ace/` builds the real static `CMeta::ACE` library and the interface-only
`CMeta::Provider` target. Provider declarations must not link the ACE runtime.

TinyMock and descriptor-driven SIMD execution belong to qigao/salts-utils;
do not restore those implementation trees or make CMeta depend on Utils.

This repository owns CMeta, CSTL reflected integrations, CFlow, ACE-style
Component/Plugin/Configurator, interceptors, scoped resources and
ServiceHost only. Core's `core/include/vstr.h` is a pure C11 borrowed view:
do not reintroduce `CMETA_STRUCT`, a reflected compatibility type or a
second `vstr` library here.

See `ARCHITECTURE.md` and the original module source/tests for functional
contracts. Never treat obsolete 2.x sample paths as authoritative.
