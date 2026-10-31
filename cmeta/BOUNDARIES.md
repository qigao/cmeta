# CMeta 分层与机制归属（#926 / #957）

## 决策与影响

CMeta 保存 canonical descriptor、type identity、生命周期语义、immutable manifest 与
address-independent fingerprint。机制独占在既有 owner：Concurrency 管理 Atomic/RCU，
Core 管理 bounded pool storage/lease，Platform 管理 thread affinity/TLS 与 C 原子 fast key，Plugin 管理模块与 lease。公开 header 的完整分类只维护在
[HEADER_LAYERS.md](HEADER_LAYERS.md)。

`CMeta::CMeta` 不链接上述 runtime，也不编译 fastpath 汇编；`meta.h` 不包含 optional
adapter 或 scope。Static-call/trace consumer 显式 include 对应 header 并链接
`CMeta::CMeta` 和 `Salts::Platform`。Pool 链接 Core；Local 链接 Platform。
选择既有 owner 加 metadata projection，避免创建第二套 allocator/并发/模块状态。

兼容性（HIGH）：#957 要求删除错误归属的 spelling，不保留 forwarding alias。
Atomic/RCU 使用 `SALTS_ATOMIC_TYPE` / `SALTS_RCU_TYPE`；生命周期 adapter 使用
`cmeta_pool_type` / `cmeta_local_type`。Fast key 与纯 fault 使用 `SALTS_FAST_KEY`、
`cmeta_fast_enable` / `cmeta_fast_disable` / `cmeta_fast_key_consume`，机制返回 SALTS 状态；
CMeta typed static-call 与 trace 返回 CMeta 状态。Fast key 与 typed static-call
统一使用 C 原子实现。消费者须更新 include、link target 与状态检查；跨模块公开 owner 布局变化要求全量重编译。

此次整合复用当前主线 owner 布局和 API，新增 Core begin/end 与 external destination
校验；不引入替代 owner 或 generic kind。Pool/Local adapter 通过 owner command 管理
BUSY，不直接写 owner 状态；canonical lifecycle callback 只执行一次。初始化失败时
restore_zero 后结束 BUSY 并归还 slot/reset affinity；cleanup 失败向上传播。

## 状态、失败与关闭

Pool 为单创建线程 owner，容量固定；lease 地址稳定且禁止复制。成功 acquire 保留一个
slot，payload 借用在 release 失效。每次 begin 恰好对应一次 end，callback 不得挂起、
修改 native storage/state 或递归执行 owner command。BUSY 返回明确错误，不重试。
Move destination 必须是外部 live semantic-zero 存储，不能是池内任何 byte，包括 free
slot/interior；source 仍须 release。destroy 拒绝未归还 lease。

Local affinity 与 BUSY 归 Platform；仅创建线程可访问，不能复制实例或将 borrowed
payload 越过 destroy、线程退出或可能迁移线程的 suspension。线程退出前必须 destroy。

每个 fast key/call 的事实源是一个原子存储；初始化/销毁要求 quiescent，MPMC 更新
复用 acquire/release 协议。替换或关闭不 drain callback；provider 必须活到全部旧读者
与在途 callback 结束。Metadata 控制面查询不改变 owner 状态，不 retain Plugin lease。
详见 [EXECUTION_PRIMITIVES.md](EXECUTION_PRIMITIVES.md)、[FASTPATH.md](FASTPATH.md)
与 [TRACEPOINTS.md](TRACEPOINTS.md)。

Manifest 只借 canonical descriptor。Fingerprint 从 canonical struct/enum/function/
interface/component rows 计算，不 hash 地址、padding、路径、时间戳。无 mutable registry、
constructor registration 或隐式模块 retention。Portable immutable table 是 ELF/Mach-O/
COFF 的共同参考；未实现 optional linker aggregation，无需 backend-specific 对等证明。
详见 [STATIC_MANIFESTS.md](STATIC_MANIFESTS.md)、[FINGERPRINTS.md](FINGERPRINTS.md)
与 [COMPONENT_MANIFESTS.md](COMPONENT_MANIFESTS.md)。

## 验证与回滚

正式 TinyTest 覆盖 owner 单独消费、C/C++、跨 TU/DSO 固定 fingerprint 向量、原子 publication 和并发替换、Pool/Local callback 重入/失败恢复、trace disabled 参数语义，
以及 Plugin 原 lease 的 unload/BUSY。组合测试通过同一 Type/DataDesc 的 manifest
查询，把 owned payload 从 Pool 移至 Local，检查 source release、析构恰好一次、
fingerprint 前中后不变，以及资源关闭后 static metadata 仍可查询。

`tests/installed` 只通过本次安装的 `CMetaConfig.cmake` 链接 `CMeta::CMeta` 与 TinyTest。
C11/C++17 实际查询 descriptor、Function ABI、manifest 与 fingerprint，并以 header
guard 拒绝 runtime 经 aggregate 引入。Find_package 禁用默认路径，防止消费其他 checkout。

在 VS x64 环境，配置根目录 release user preset 后：

```powershell
cmake --build --preset install-win-release-user --parallel 4
$env:CMETA_PACKAGE_ROOT = "$env:PROJECT_ROOT/external/pkgs/salts/release"
cd cmeta/tests/installed
cmake --preset installed-win-release
cmake --build --preset installed-win-release --parallel 4
ctest --preset installed-win-release
```

Linux CI 使用 `installed-linux-release`。回滚须成套恢复调用点与 owner，不保留两套
namespace/ownership 同时运行；此次变化不迁移运行期数据、不引入外部依赖。
