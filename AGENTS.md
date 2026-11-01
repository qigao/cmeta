# Salts repository instructions

Salts focuses on **CMeta / ACE**. The `ace/` module implements
Configurator/ServiceHost/Plugin composition; CFlow supplies accompanying
CMeta-based execution integrations. New OS, concurrency, coroutine,
transport, Unicode/IDNA, native
FS/Process implementations belong to **qigao/salts-core**;
cryptographic code and ASN.1 belong to **qigao/crypto**. Source ownership is singular.

- Repository-owned public CMake targets use `CMeta::XXX`; imported native
  owners from qigao/salts-core keep their canonical `Salts::XXX` names.
- Repository-owned public headers live under `include/cmeta/`. Imported
  qigao/salts-core headers use their published flat or functional paths; no
  `include/salts/` or `include/salts_core/` namespace exists.
- No legacy interface, compatibility shim, alias, fallback, provider DSO
  for Crypto, copied native implementation or alternate build profile.
- No CMeta dependency is permitted inside the Core/Crypto repositories.
- Preserve bounded owner/lease/cancel/stop/unload behavior and exact-head
  source, installed SDK and native CI evidence.
- Never substitute one OS's successful tests for another's native backend.
- Run `python3 tools/check_ace_only_sources.py` before changes to the
  package layout. Verify original typed runtime conformance after changes.
- Standalone installer/consumer CI, SDK-probe projects, and their source trees
  are forbidden. Release staging install and package assembly remain part of
  the single native SDK publication graph, not separate product projects.

TinyMock and descriptor-driven SIMD execution belong to qigao/salts-utils;
do not restore those implementation trees or make Salts depend on Utils.

This repository owns CMeta, CSTL reflected integrations, CFlow, ACE-style
Component/Plugin/Configurator, interceptors, scoped resources and
ServiceHost only. Core's `include/vstr.h` is a pure C11 borrowed view:
do not reintroduce `CMETA_STRUCT`, a reflected compatibility type or a
second `vstr` library here.

See `ARCHITECTURE.md` and the original module source/tests for functional
contracts. Never treat obsolete 2.x sample paths as authoritative.
