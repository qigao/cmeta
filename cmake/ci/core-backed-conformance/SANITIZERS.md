# Salts 3.0 installed package native sanitizers (Draft)

This branch is stacked on the real SaltsConfig 3.0 original-source
conformance PR. It does not build the temporary SaltsM6 SDK or any
historical Salts::Core/Platform/NativeIO binary. Instead it compiles and
installs **both real packages** using matched instrumentation:

- pinned standalone `SaltsCore 0.1` shared Core/Platform/NativeIO/
  Concurrency/Coroutine binary owners;
- original Salts 3.0 retained CMeta/Component/Plugin/Configurator/
  ServiceHost/CFlow source implementations and real installed
  `SaltsConfig/SaltsTargets`;
- all thirteen original-source CMeta/ACE/ServiceHost/CFlow tests and
  real Provider DSO fixtures, compiled separately through the installed
  public target graph.

Linux ASan+UBSan with LeakSanitizer and Linux TSan run as mutually
exclusive, fully instrumented builds, with fail-on-report semantics
and **three repetitions** of all thirteen original-source parity
suites. Tests include actual native Pipe and Sharded IO cancellation,
concurrent mailbox and ServiceHost generations, typed CMeta
Configurator and Provider lease/unload. No tests or leak/race reports
are suppressed, skipped, or replaced by a noninstrumented fixture.

A preflight runs the canonical SaltsCore CI CTest regex validator
against the *actual* workflow to block missing shell quotes or
unanchored test selectors. This prevents a build-only green being
mistaken for sanitizer qualification. No production release or 2.3
runtime coexistence is authorized.
