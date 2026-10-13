# CFlow cross-DSO reflection owner comparison (Salts #1180)

**Test-only.** Stacked on CSTL [Draft PR #1169](https://github.com/qigao/salts/pull/1169).
No production Salts CMake, C11 ABI, release tag, or SDK package changes.

The baseline exactly checks out Salts `v2.3.0-rc.15` at
`21bdb1145fbb2b39ee3fa644173ec037adba7b87`; each mode builds
and installs a real Salts SDK with ASan+UBSan and no suppressions. Two
**independent** native DSOs, using only installed `Salts::CFlow`, each
reference `cflow_type_executor_protocol_stats_ptr` from the actual
`cflow/src/executor.c` translation unit. A C11 loader checks both load
orders using `dlopen/dlsym/dlclose`.

| CFlow mode | Expected native result | CSTL baseline |
| --- | --- | --- |
| `static-default` | **Expected ODR rejection**, never a "safe" pass | test-only static-hidden |
| `static-hidden` | Independent private descriptor addresses, **semantic shape equal**, clean ASan | test-only static-hidden |
| `shared-single` | One canonical shared descriptor address and `libcmeta_cflow.so.5` DT_NEEDED by both DSOs | source-built shared-single |

Each accepted mode must verify both CFlow and CMeta descriptor shape
while each DSO is **still loaded**; it returns value snapshots only, never
retains/dereferences borrowed reflection pointers after `dlclose`.
The comparator audits the actual compiled `executor.c.o` sanitizer flags,
ELF defined/dynamic symbols and real linker dependencies, DSO image SHA256,
runtime outcome and loader diagnostics. A negative test passing only means
the default-static collision was precisely recognized, not production safety.

**Not covered:** cross-DSO callable ownership/lifetime or dynamic reload
under Windows/macOS SDK, CHttp request lifecycle, shared CRT policy, or a
production static-vs-shared decision. These remain independent gates.
Use [TurboAgent #123](https://github.com/qigao/TurboAgent/issues/123) for
real downstream CHttp/Provider DSO tests; do not infer their exact released
ASan acceptance from this candidate topology.
