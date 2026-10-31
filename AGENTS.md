# Salts repository instructions

Salts is exclusively the **CMeta / ACE / Component Runtime / CFlow**
repository. New OS, concurrency, coroutine, transport, Unicode/IDNA, native
FS/Process and ASN.1 implementations belong to **qigao/salts-core**;
cryptographic code belongs to **qigao/crypto**. Source ownership is singular.

- Every public CMake link target is `Salts::XXX`, irrespective of repository.
- No legacy interface, compatibility shim, alias, fallback, provider DSO
  for Crypto, copied native implementation or alternate build profile.
- No CMeta dependency is permitted inside the Core/Crypto repositories.
- Preserve bounded owner/lease/cancel/stop/unload behavior and exact-head
  source, installed SDK and native CI evidence.
- Never substitute one OS's successful tests for another's native backend.
- Run `python3 tools/check_ace_only_sources.py` before changes to the
  package layout. Verify installed C11/C++17 consumers and original typed
  runtime conformance after changes.

TinyMock and descriptor-driven SIMD execution belong to qigao/salts-utils;
do not restore those implementation trees or make Salts depend on Utils.

This repository owns CMeta, CSTL reflected integrations, CFlow, ACE-style
Component/Plugin/Configurator, interceptors, scoped resources and
ServiceHost only. Core's `include/vstr.h` is a pure C11 borrowed view:
do not reintroduce `CMETA_STRUCT`, a reflected compatibility type or a
second `vstr` library here.

See `ARCHITECTURE.md` and the original module source/tests for functional
contracts. Never treat obsolete 2.x sample paths as authoritative.
