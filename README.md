# Salts 4 — CMeta, ACE and CFlow

Salts owns **CMeta reflection, typed interfaces and RAII, Component/Plugin
contracts, Configurator/ServiceHost and CFlow execution**. It does not own
native platform, networking, cryptography or string-runtime implementations.

## Source ownership

| Repository | Modules and canonical link targets |
| --- | --- |
| [salts](https://github.com/qigao/salts) | `Salts::CMeta`, `Salts::ConfiguratorABI`, `Salts::PluginABI`, `Salts::Plugin`, `Salts::ServiceHostProviderABI`, `Salts::Configurator`, `Salts::ServiceHost`, `Salts::CFlow` |
| [salts-core](https://github.com/qigao/salts-core) | `Salts::Core`, `Salts::Platform`, `Salts::Concurrency`, `Salts::Coroutine`, `Salts::NativeIO`, and optional CNet, SCTP, URI, Unicode, IDNA, CSerde, TinyTest, FS, Process |
| [salts-utils](https://github.com/qigao/salts-utils) | Higher-level utilities, including `Salts::TinyMock` and static `Salts::SIMD`; destination [Draft #732](https://github.com/qigao/salts-utils/pull/732) still needs complete SDK qualification |
| [crypto](https://github.com/qigao/crypto) | Exactly one **static** `Salts::Crypto` archive; GmSSL, hash/signature algorithms, ASN.1 BER/DER, TLS/X.509 and CipherBIO |

Every native function has a **single source and binary owner**. No
`SaltsCore::`, `Crypto::`, old target aliases, compatibility headers,
runtime fallback or duplicated GmSSL builds are provided. Link only
`Salts::XXX`. Core and Crypto never depend on Salts/CMeta.

The unbuilt `utils/` monolith and old native entry points have been physically retired; FS/Process belong to Core and ASN.1 to Crypto. No old APIs or compatibility targets are retained.

The retained `cstl/` tree is CMeta-specific and remains outside the installed
production graph until explicitly qualified. TinyMock and descriptor-driven
SIMD execution belong only to `qigao/salts-utils`; their source trees are no
longer retained here. CMeta continues to own vector/mask reflection metadata.

## Build

Install the matching **SaltsCore** SDK first, using
`find_package(SaltsCore 0.1 CONFIG REQUIRED)`, then:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH=/path/to/installed/salts-core
cmake --build build --parallel
cmake --install build --prefix /path/to/installed/salts-ace
```

Consumers use `find_package(Salts 4 CONFIG REQUIRED)` and link the
canonical `Salts::CMeta`, `Salts::CFlow` and other listed targets.
There is no 2.x build configuration or native implementation bundled here.

The existing native CI builds the production graph and runs its original
C11/C++17, real Provider DSO, generation/publication and CFlow tests directly.
Use `-DBUILD_TESTING=OFF` for a package-only build. Test builds require the
installed Core TinyTest target. There are no separate consumer/probe projects.

Configurator and ServiceHost now use native ABI **2.0**, Provider contract v2
and Configurator binding v2. Rebuild every host, generated binding and Provider
against this SDK; no previous API aliases or contract fallback are supplied.
See [the runtime contract](component-runtime/README.md) and
[component declarations](cmeta/COMPONENT_MANIFESTS.md).

## Merged source and remaining SDK gates — 2026-10-31

- [#1332](https://github.com/qigao/salts/pull/1332) merged as
  `a2b3be42854c969131159fae1c0673fed7e085cb`: remove the old Component
  runtime API, headers and targets; require Configurator binding v2,
  ServiceHost Provider contract v2 and native ABI 2.0. Plugin ABI remains 5.
- [#1333](https://github.com/qigao/salts/pull/1333) merged as
  `31206d353d14dbbe2f119c2035804984853345fc`: remove all 28 TinyMock/SIMD
  donor files. Their complete headers, implementations and original tests
  move to SaltsUtils; CMeta keeps vector/mask reflection metadata and Core
  keeps TinyTest. Salts does not depend on Utils.
- The final production tree was tested at `2e2aba039aeceb29c0410c7a2e896d2deb8b31e2`:
  [Linux/macOS/Windows native CI](https://github.com/qigao/salts/actions/runs/41175204059)
  passes **25/25 on each host**;
  [ASan/UBSan/LSan and TSan](https://github.com/qigao/salts/actions/runs/41175204063)
  pass **25/25 in each profile**. The merge has the same source tree.
- [Orbit Draft #23](https://github.com/qigao/Orbit/pull/23), head
  `3ba66fd5d48187e6c8551b3bd73bdb37fc05ec63`, independently passes its
  [Linux/Windows real generated Provider/DSO CI](https://github.com/qigao/Orbit/actions/runs/41172450938)
  against the installed Salts 4 runtime. Other downstreams still require
  their own migration and rebuild; see [#1153](https://github.com/qigao/salts/issues/1153).

**The source merges do not publish Salts.Native 4.0.0 or qualify a six-RID
release.** SaltsUtils [#732](https://github.com/qigao/salts-utils/pull/732)
remains Draft: its [latest CI](https://github.com/qigao/salts-utils/actions/runs/41175206347)
fails restoring the exact 4.0.0 package before compilation. The original four
moved module tests pass locally (53 cases / 367 assertions, normal and
ASan/UBSan with local leak detection off), but the complete Utils build,
installation and platform tests remain pending. Track the actual dependency
closure in [Utils #699](https://github.com/qigao/salts-utils/issues/699) and
the immutable SDK migration in [Utils #684](https://github.com/qigao/salts-utils/issues/684).

Earlier Salts [#1330](https://github.com/qigao/salts/pull/1330) /
[#1331](https://github.com/qigao/salts/pull/1331) and Core
[#95](https://github.com/qigao/salts-core/pull/95) /
[#96](https://github.com/qigao/salts-core/pull/96) candidates contain a conflicting
Core SIMD owner/descriptor ABI. They cannot be merged unchanged with this
Utils ownership decision. Core candidate cleanup, affected downstreams and
the frozen source/API migration obligations remain open.

Static source policy: `python3 tools/check_ace_only_sources.py`.
