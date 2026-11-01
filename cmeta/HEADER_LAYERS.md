# CMeta public header layering (#957)

本表是公开 header 分类的事实源。Core 包括 canonical metadata、生命周期、
metadata carrier/protocol，以及这些声明所需的有限编译期 machinery；这些协议
本身不拥有 scheduler、allocator、thread 或 Plugin runtime。

| 层 | Header |
|---|---|
| Core：基础类型、身份、构造 kernel | `abi.h`, `cmeta.h`, `types.h`, `type_identity.h`, `type_select.h`, `type_traits.h`, `status.h`, `pp.h`, `signatures.h`, `generated/builtin_signature_manifest.h` |
| Core：canonical metadata 与生命周期 | `data.h`, `data_select.h`, `declared_type.h`, `enum.h`, `flags.h`, `struct.h`, `variant.h`, `lifecycle.h`, `function.h`, `interface.h`, `method.h`, `object.h`, `object_interface.h`, `manifest.h`, `manifest_view.h`, `fingerprint.h`, `plugin.h` |
| Core：typed carrier、projection 与协议 | `collector.h`, `compute.h`, `container.h`, `contract.h`, `entry.h`, `fixed_array.h`, `generic.h`, `infer.h`, `invokable.h`, `policy.h`, `range.h`, `relations.h`, `value.h`, `vector.h` |
| Core aggregate | `meta.h` |
| Structured-C | `scope.h`；`struct.h` 内的 intrusive projection 只增加静态 owner/member/type 检查，不拥有容器运行期 |
| Optional metadata/type facade | `local.h`, `pool.h`, `fastpath.h`, `trace.h` |


`<cmeta/meta.h>` 聚合 Core；Structured-C scope 和四个 Optional adapter 必须显式
include。Core archive 不链接 Platform、Concurrency、Coroutine、Core、CFlow 或 Plugin。
`generated/builtin_signature_manifest.h` 是生成的 Core schema。

Atomic 与 RCU 直接属于 Concurrency，使用 `<atomic.h>` 的
`SALTS_ATOMIC_TYPE` 和 `<rcu.h>` 的 `SALTS_RCU_TYPE`。
Pool 的 storage/lease/BUSY 归 Core `object_pool_owner_state`；Local affinity/TLS
归 Platform `cmeta_thread_affine_state`；CMeta 只增加 canonical DataDesc 生命周期 binding。
Platform `<fastpath.h>` 拥有 C 原子 fast key 与一次性消费，
CMeta static-call/tracepoint 增加 Function ABI/typed payload metadata。
纯 fault 控制直接使用 Platform API，不提供另一套 CMeta spelling。

迁移影响、所有权协议、验证与回滚见 [BOUNDARIES.md](BOUNDARIES.md)。
不得添加 compatibility aggregate、forwarding alias、隐式 fallback 或第二个 runtime owner。
