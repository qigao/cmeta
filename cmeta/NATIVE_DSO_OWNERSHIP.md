# Salts 2.3 native DSO metadata ownership

This is the **source-level product policy** selected for Salts's current
static CMeta/CFlow/CSTL dependency graph (Salts #1167, #1180). It does not
change the public C11 struct layouts, published tags, binary ABI, or ownership
mechanism. The immutable `v2.3.0-rc.15` binary is not retroactively repaired.

## Single execution owner; image-local static metadata

When `Salts::CSTL` and `Salts::CFlow` are static archives, their reflected
CMeta descriptor objects are **private to the image that embeds the archive**.
The compiler and linker must not export their data symbols from an unrelated
`libsalts`, `libcnet`, CHttp Client/Server, or Provider DSO. This is the
existing production CMake policy introduced by merged Salts PR #1187.

There is **no process-global reflected-descriptor registry**, no attempt to
collapse private static descriptor objects into pointer-identical instances,
and no lazy compatibility fallback. Identically shaped descriptors in two
images can have *different pointer addresses*. CMeta stable type IDs and
semantic comparison APIs (such as `cmeta_type_equal`) are the cross-image
comparison mechanism, with both providers **loaded and alive**. Semantic
equality is **not** authority to dereference, mutate, free, or invoke through
a descriptor owned by another image.

A container value, its backing allocation, the descriptor graph and the
callbacks or native code used to operate or destroy it belong to **one
owning provider image**. Passing a raw container handle to an unrelated
image's compiled CSTL algorithm, copying raw runtime storage, or retaining
descriptor pointers after the provider unloads is **not supported**.
For example, CSTL B+Tree typed semantic-zero admission compares its canonical
descriptor pointer to that image's own
`stl_bplus_tree_container_desc`; the pointer is not a process-global
cross-DSO token. No hidden foreign-pointer upgrade or ownership inference
is permitted.

## Supported native consumer boundary

When another DLL/DSO needs typed container services, use the existing
`cstl_typed_import` protocol: query the **producer's original**
immutable container/DataDesc/receiver-operation tables and route type-specific
operations through the producer-owned functions. The provider must remain
loaded through all values, ranges, collectors, metadata, callbacks,
destructors, outstanding owner work, and final cleanup. The C11 and C++17
import tests exercise all thirteen container kinds. `cstl_typed_import`
is explicit borrowing of the *same producer's* API, not a second CSTL
implementation or a way to transfer a raw handle to a different allocator.

For CFlow, Graph/Plan/Actor and generated RPC callables may copy admitted
*value records*, but descriptor pointers, callable function bodies,
`self` context, executable callbacks and outer Plugin image remain
borrowed. A ServiceHost Generation/Supplied Provider lease must prevent
`dlclose`/`FreeLibrary` until the final admitted execution, cancellation,
terminal callback and reverse cleanup settles. A CMeta semantic type match
does not extend that lease or authorize invoking foreign unloaded code.
Preserve the existing one-Owner, bounded queue/storage and exactly-once
settlement rules.

## Explicit shared-library alternative

`BUILD_SHARED_LIBS=ON` requests an **independent** shared-single
CFlow/CSTL topology; the static image-local visibility rule does **not**
force-harden or automatically switch it. A shared-single product release
requires its own versioned CMeta **data** import/export ABI (including
Windows MSVC DLL/.lib consumers and the #1185 history), SDK target and
manifest changes, installed SDK ABI/runtime qualification, external downstream verification, and
downstream CHttp/DataBind/Provider agreement. Do not fall back at runtime
between private-static and shared-single descriptors; they are different
link/ABI choices made before packaging.

## Required regression and release evidence

- Ordinary native CTest `salts_static_owner_exports_test` directly loads
  **real** `Salts::Core` and `Salts::CNet` dynamic libraries on Linux,
  Windows and macOS. It requires public `cmeta_log_str` and
  `cnet_client_init` exports while rejecting the data symbols
  `stl_bplus_tree_container_desc`, `stl_bplus_tree_generic_desc`,
  and `cflow_type_executor_protocol_stats_ptr` in both images.
  It is only registered for the default static CSTL+CFlow policy and
  must execute in native CI, not be marked passed by compilation alone.
- Keep the installed C11/C++17 `cstl_imported_metadata` tests (all thirteen
  kinds), CMeta generic semantic identity tests and exact component
  lease/drain tests alongside this negative export gate.
- The historical original-release Linux ASan ODR positive/negative topology
  comparison remains [#1167](https://github.com/qigao/salts/issues/1167)
  and [#1180](https://github.com/qigao/salts/issues/1180); later source
  producer fixes were merged through #1187/#1193 and separately checked
  with TurboAgent full-source Provider DSO ASan+UBSan.
- **Publishing is separate.** A new immutable `Salts.Native 2.3.0-rc.N`
  must be built from the chosen producer source, packed as a complete
  six-RID SDK, re-pinned by downstream consumers, and admitted by
  actual installed SDK tests and appropriate ASan/UBSan/TSan.
  The release owner is [Salts #1081](https://github.com/qigao/salts/issues/1081),
  not these historical test-only branches. Old binary tags are immutable.
