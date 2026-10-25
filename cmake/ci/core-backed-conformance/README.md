# Core-backed Salts 3.0 — original source parity (Draft)

The first Core-backed profile [Salts #1313] installs **real**
`SaltsConfig.cmake` version 3.0 and canonical `Salts::` targets on
exact pinned standalone shared SaltsCore 0.1. Its four independent
C11/C++17 ACE/CFlow smoke consumers passed on Linux/macOS/Windows.

This stacked conformance candidate adds a **freshly configured independent
project** that only imports the *installed* `Salts 3.0` targets; no old
Salts Platform/NativeIO/Concurrency/Core/CNet source or native object is
compiled into its applications. It compiles original Salts *test*
translation units and two original real Provider fixture DSOs.

The 13 tests are:
- CMeta/Configurator C11 and C++17 against a dedicated provider DSO and
  original Logger → App CMeta test fixture (two M6 test translations).
- Original ServiceHost generation and concurrent publication/drain,
  plus C++17 ServiceHost ABI.
- Real PluginRegistry bounded/strict UTF-8 path rejection (M6 contract).
- Seven **unchanged original CFlow** test files: status, Stream Slice,
  ACE Pipes/Filters, Event Mailbox, NativeIO Pipe Adapter, Sharded
  NativeIO Adapter and Machine Instance.

The public ABI/implementation is always imported via canonical
`Salts::Configurator/ServiceHost/Plugin/CFlow/CMeta/TinyTest` and
`SaltsCore::Core/Platform/NativeIO/Concurrency`. No deprecated
`Salts::Core`, old `Salts::Platform`, or duplicate fallback target
is admitted. Actual Provider callbacks require quiescent lease release.

The three-OS CI builds these 13 tests *after* installing the real
Core-backed Salts 3.0 package, then relocates **both** installed SDK
prefixes, discards their former paths and separately recompiles/runs all
13 again. The first four independent ABI/symbol-owner smoke consumers
and the strict ELF/Mach-O/PE owner audit from the parent remain enabled.

This is still **not** a published 3.0 or an authorisation to mix the
legacy Salts 2.3 binary runtime in the same process. Additional native
cancellation, downstream rollout/rollback and ABI release gates remain.
