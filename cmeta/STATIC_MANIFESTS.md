# Static manifests and ABI fingerprints

This is the portable metadata substrate for issue #926.

## Boundary

CMeta manifests are immutable tables compiled into the consumer or provider.
They do not own plugin instances, allocate registry nodes, run constructors, or
create a universal mutable process registry.

The architecture keeps three concepts deliberately separate:

```text
descriptor = identity and canonical semantic truth
manifest   = discovery: which descriptors participate in one explicit set
linker     = storage/aggregation backend for that set
```

Reflection descriptors therefore do not depend on a registry. Typed code reaches
its canonical descriptor directly (for example through the existing CMeta type
surface) and must not perform a process-global name lookup as the normal path.
A manifest may borrow descriptor pointers, but it does not become a second type,
ownership, or lifecycle authority.

Declaring a reflected type also does not implicitly publish it into a process-
wide type registry. Discovery is explicit and opt-in through a manifest. This
preserves dead stripping, private descriptor boundaries, static-library/DLL
composition, and Plugin lease ownership.

ELF, Mach-O, and COFF implementations may later aggregate manifest pointers with
qualified linker-section backends, but section names, attributes, pragmas, and
start/stop symbols are not part of the public contract. A generated array is the
portable reference representation.

```c
static const int json_codec = 1;
static const int xml_codec = 2;

cmeta_registry(codec_manifest,
    cmeta_entry(json_codec)
    cmeta_entry(xml_codec)
);
```

Entries retain declaration order and contain a borrowed descriptor pointer.
The manifest owns no referenced object and performs no lifetime operation.

## Descriptor tables versus manifest tables

Descriptor-local tables describe one canonical semantic object, for example:

```text
User descriptor
  -> fields[]
  -> lifecycle
  -> traits

Function descriptor
  -> parameters[]
  -> result

Enum descriptor
  -> values[]
```

These tables are immutable metadata owned by or referenced from the canonical
descriptor. They require no linker discovery.

Manifest tables solve the different problem of explicit discovery:

```text
codec manifest
  -> json codec descriptor
  -> xml codec descriptor

plugin manifest
  -> provider descriptor
  -> capability descriptor
```

Consequently, CMeta must not implement `cmeta_type(T)` by searching a global
manifest. The normal typed path is a direct/static descriptor reference.
Name-based manifest lookup, if later required by tooling or control-plane code,
is optional discovery and never the canonical Reflection path.

## ABI fingerprints

`CMETA_ABI_FINGERPRINT_VERSION` versions the byte-level algorithm independently
from package versions and `CMETA_REFLECTION_ABI_VERSION`. The v1 builder uses a
fixed FNV-1a 64-bit stream, fixed little-endian integer encoding, and
length-prefixed strings. Descriptor addresses and function addresses are never
hashed.

`cmeta_abi_fingerprint_type()` currently covers the stable type surface that is
already canonical in CMeta: name, size, alignment, kind, trait capability flags,
semantic type identity, and immediate pointee metadata. It intentionally hashes
trait capability flags rather than implementation function addresses.

Checked canonical type/struct/enum/function/interface projections now use the
same builder with a separate projection version and domain. The exact byte
contract, bounds, failure semantics and golden vectors are specified in
[FINGERPRINTS.md](FINGERPRINTS.md). Component/service composition is documented in [COMPONENT_MANIFESTS.md](COMPONENT_MANIFESTS.md);
fingerprints never duplicate semantic rows or retain providers.

A manifest entry should reference the canonical descriptor/fingerprint rather
than duplicate field, method, trait, ownership, or lifecycle schema.

## Linker backend rule

A linker-section backend may optimize aggregation, but it must preserve exactly
the same immutable manifest semantics as the portable generated-array backend.

In particular:

- no public ELF/Mach-O/COFF section spelling;
- no public `__start_*` / `__stop_*` contract;
- no constructor-driven mutable registration;
- no implicit registration of every reflected type;
- no hidden ownership or Plugin lease retention;
- no runtime Reflection lookup added to typed hot paths.

If a platform cannot preserve these semantics, it should use the portable
generated/static manifest representation instead of a reduced-safety fallback.

## Follow-up slices

- component/capability manifests consume these immutable tables while lifecycle
  remains with existing descriptors and explicit leases;
- typed tracepoints carry statically prepared payload metadata and use #923
  static-key branching when that substrate lands;
- named fault points use the same #923 gate and deterministic test control;
- qualified ELF/Mach-O/COFF aggregation is an optimization/backend concern, not
  a second public registry model.

Typed tracepoint 和默认关闭的 fault point 使用 #923 的 static key；payload metadata 可直接作为 manifest entry 借用。声明、控制、并发与 provider 生命周期见 [TRACEPOINTS.md](TRACEPOINTS.md)。Component/service declaration 及其 fingerprint 组合见 [COMPONENT_MANIFESTS.md](COMPONENT_MANIFESTS.md)。

## Typed consumption（#926 / #957）

`<cmeta/manifest_view.h>` 提供 type、struct、Function ABI、interface、trace payload、capability、enum domain、Component declaration 的 typed entry 与 getter。布局仍为 v1 的 generic manifest ABI；新增 kind 值只规定 descriptor 的类型契约。Generic、fault 与未支持的 descriptor 不会被推断或自动降级到另一种 kind。

`cmeta_manifest_type_entry` / `struct_entry` / `function_entry` / `interface_entry` / `trace_entry` / `capability_entry` / `enum_entry` / `component_entry` 在 C11 用 `_Generic`、C++17 用 typed constexpr pointer conversion 拒绝错误 descriptor 类型。它们不执行 constructor 或注册；fingerprint/flags 初始为零。高级显式 entry 可继续使用既有 `cmeta_manifest_entry`，但 provider 必须如实遵守 kind 契约。`FunctionMeta` / `FunctionAbi` 现在直接投影同一 canonical static descriptor，因此可用于静态初始化；生成的函数 getter 与 descriptor 内容不变。

```c
#include <cmeta/manifest_view.h>
cmeta_struct(ManifestRecord, cmeta_field(int, value));
cmeta_registry(records,
    cmeta_manifest_struct_entry("record", StructMeta(ManifestRecord))
);
int main(void) {
    const cmeta_manifest_limits limits = {CMETA_MANIFEST_DEFAULT_ITEMS,
        CMETA_MANIFEST_DEFAULT_DEPTH, CMETA_MANIFEST_DEFAULT_NODES};
    const cmeta_struct_desc *record = NULL;
    cmeta_status status = cmeta_manifest_get_struct(&records, 0u, &limits, &record);
    if (status != CMETA_OK) return 1;
    return record == StructMeta(ManifestRecord) ? 0 : 1;
}
```

链接 `CMeta::CMeta`。该场景由 `cmeta_manifest_view_test` 和 installed Reflection 测试覆盖。Getter 先验证格式、索引、exact kind 与 metadata，再发布原 descriptor 的 const 借用；失败保留输出。`CMETA_INVALID_ARGUMENT` 表示参数/descriptor 无效，`CMETA_TYPE_MISMATCH` 表示格式版本或 kind 不匹配，`CMETA_CAPACITY_EXCEEDED` 表示显式预算不足。Trace getter 验证 payload schema；capability getter 验证 interface schema，不授予接口 handle 或 Plugin lease。

查询限额由调用方显式传入：`max_items` 同时限制 manifest membership、字段/方法/参数/泛型 arity，`max_identity_depth` 与 `max_identity_nodes` 限制递归工作；depth 不能超过可配置的 `CMETA_MANIFEST_DEPTH_LIMIT`。Cycle 在 canonical recursive validator 执行前消耗预算并失败。没有动态分配、对象回调、模块 retain 或缓存。调用方须提供可读且 live 的 ABI-compatible descriptor/数组及 NUL-terminated 字符串；kind 校验不能证明一个任意裸指针的真实存储类型。跨 DSO 消费须先协商 Reflection ABI，并由 Plugin owner 保持 provider lease。这里只验证已知 C metadata，不解析不可信字节流。

`field.type_name` 仍仅用于显示；typed query 不把该字符串当作身份，也不通过全局字符串 lookup 找类型。Legacy ABI-only interface rows 仍按既有 validator 保留 dispatch shape，不伪造 Function metadata。Canonical struct/enum/function/interface fingerprint 见 [FINGERPRINTS.md](FINGERPRINTS.md)；Component service declaration 与 fingerprint 组合见 [COMPONENT_MANIFESTS.md](COMPONENT_MANIFESTS.md)；可选 linker aggregation 未实现。
