# Salts 3 — CMeta, ACE and CFlow

Salts owns **CMeta reflection, typed interfaces and RAII, Component/Plugin
contracts, Configurator/ServiceHost and CFlow execution**. It does not own
native platform, networking, cryptography or string-runtime implementations.

## Source ownership

| Repository | Modules and canonical link targets |
| --- | --- |
| [salts](https://github.com/qigao/salts) | `Salts::CMeta`, `Salts::ComponentABI`, `Salts::PluginABI`, `Salts::Plugin`, `Salts::ComponentProviderABI`, `Salts::Configurator`, `Salts::ServiceHost`, `Salts::CFlow` |
| [salts-core](https://github.com/qigao/salts-core) | `Salts::Core`, `Salts::Platform`, `Salts::Concurrency`, `Salts::Coroutine`, `Salts::NativeIO`, and optional CNet, SCTP, URI, Unicode, IDNA, ASN1, CSerde, TinyTest |
| [crypto](https://github.com/qigao/crypto) | Exactly one **static** `Salts::Crypto` archive; GmSSL, hash/signature algorithms, TLS/X.509 and CipherBIO |

Every native function has a **single source and binary owner**. No
`SaltsCore::`, `Crypto::`, old target aliases, compatibility headers,
runtime fallback or duplicated GmSSL builds are provided. Link only
`Salts::XXX`. Core and Crypto never depend on Salts/CMeta.

The retained `cstl/`, `simd/` and `tinymock/` trees are CMeta-specific.
They are not part of the installed production targets until explicitly
qualified against the single Core SDK.

## Build

Install the matching **SaltsCore** SDK first, using
`find_package(SaltsCore 0.1 CONFIG REQUIRED)`, then:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH=/path/to/installed/salts-core
cmake --build build --parallel
cmake --install build --prefix /path/to/installed/salts-ace
```

Or set `SALTSCORE_PREFIX` to the installed Core SDK path and use
`cmake --preset native-release` and `cmake --build --preset release`.

Consumers use `find_package(Salts 3 CONFIG REQUIRED)` and link the
canonical `Salts::CMeta`, `Salts::CFlow` and other listed targets.
There is no 2.x build configuration or native implementation bundled here.

The native, installed, independently relocated C11/C++17 consumer and
original CMeta/ServiceHost/CFlow acceptance suite is in
`.github/workflows/ace-only-source-cutover.yml`. Static source policy:
`python3 tools/check_ace_only_sources.py`.
