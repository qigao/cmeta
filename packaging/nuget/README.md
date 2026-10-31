# CMeta.Native

Package metadata for CMeta 4 / ACE / CFlow install trees. No CMeta.Native SDK
has been published by this change, and no publisher workflow is enabled.
The repository is currently qigao/salts; the manual rename to qigao/cmeta is
pending.

A native payload uses `sdk/<os>-<arch>/` for Linux, macOS or Windows and x64
or arm64. Each prefix contains headers, libraries and
`lib/cmake/CMeta/CMetaConfig.cmake`. Consumers use:

```cmake
find_package(CMeta CONFIG REQUIRED)
target_link_libraries(my_host PRIVATE CMeta::ACE)
target_link_libraries(my_provider PRIVATE CMeta::Provider)
```

The prefix must match the target OS, CPU, compiler and CRT. In particular,
llvm-mingw/UCRT SDKs cannot reuse or relabel MSVC SDKs. Host tools are restored
separately from target libraries. Android/iOS payloads are outside the native
six-platform CI qualification here.

`feed_hygiene.py` retains its exact historical Salts.Native 2.3 quarantine
contract and old archive paths. It does not inspect, publish or qualify a
CMeta.Native 4 package.
