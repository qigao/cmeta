# Salts 4 — CMeta, ACE and CFlow

Salts owns **CMeta reflection, typed interfaces and RAII, Component/Plugin
contracts, Configurator/ServiceHost and CFlow execution**. It does not own
native platform, networking, cryptography or string-runtime implementations.

## Source ownership

| Repository | Modules and canonical link targets |
| --- | --- |
| [salts](https://github.com/qigao/salts) | `Salts::CMeta`, `Salts::ConfiguratorABI`, `Salts::PluginABI`, `Salts::Plugin`, `Salts::ServiceHostProviderABI`, `Salts::Configurator`, `Salts::ServiceHost`, `Salts::CFlow` |
| [salts-core](https://github.com/qigao/salts-core) | `Salts::Core`, `Salts::Platform`, `Salts::Concurrency`, `Salts::Coroutine`, `Salts::NativeIO`, and optional CNet, SCTP, URI, Unicode, IDNA, CSerde, TinyTest, FS, Process |
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

Static source policy: `python3 tools/check_ace_only_sources.py`.
