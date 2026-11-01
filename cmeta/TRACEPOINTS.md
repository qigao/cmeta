# Typed tracepoint 与 fault point（#926）

`cmeta/trace.h` 在 #923 的 static key 上提供 C11 声明和控制入口。Payload 的布局与字段 metadata 由既有 `cmeta_struct` 生成；manifest 可以直接借用 `StructMeta`，不建立另一份类型或所有权 registry。C++17 消费方通过所属 C 模块的 typed wrapper 和 opaque key 使用这些入口。

## 状态与生命周期协议

| 路径 | 事实源、所有权与容量 | 状态迁移、并发与失败 |
|---|---|---|
| Tracepoint | 一个 atomic key 决定是否启用，一个 exact typed atomic function pointer 保存 backend；一个同步栈上 payload，无队列和堆分配 | 初始化与销毁须 quiescent，bind/enable/disable/emit 支持 MPMC；NULL bind 和未绑定 enable 返回 `CMETA_INVALID_ARGUMENT`，保留原状态 |
| Fault point | 一个 atomic bool 保存至多一个未消费的许可 | 默认关闭；arm release 发布，hit acquire 读取后 exchange 消费同一 bool；并发消费者至多一个命中；重复 arm 合并，disarm 不撤回已消费的命中 |

每个 point 只在所属 C 模块中定义一次，初始化后的原子存储不可复制或移动。Tracepoint 在 key 关闭或尚未绑定时不求值 payload 参数；启用后的每个参数求值一次，参数之间的求值顺序遵循 C 语言规则。调用方不能依赖不同参数的求值顺序。

Backend 同步借用 `const payload *`，指针在 callback 返回时失效。Payload 中的指针字段也不产生 retain/move；来源必须活到 callback 返回，callback 不得保存这些借用或跨协程挂起使用。多个 emit 可同时调用同一个 backend，backend 自己负责共享状态同步；不同 emit 之间没有全局顺序保证。

关闭或替换不等待在途 callback，不清除已绑定的 backend。Provider 代码必须保持加载，直到绑定被替换且所有旧读者和在途 callback 结束；也可在整个 point 的独占销毁边界释放。宿主负责停止 emit、等待读者和 callback，再卸载 provider。这里不隐式保留 Plugin lease。

没有排队或扩容：backend 的执行时间和错误处理由 backend 所属模块负责，emit 同步返回，不重复记录日志。Disabled emit 与 fault hit 为 O(1)，enabled emit 的 payload 初始化为 O(字段数)，字段数及 payload 大小在声明时固定。

## C11 示例

```c
#include <cmeta/manifest.h>
#include <cmeta/trace.h>

cmeta_tracepoint(http_request,
    cmeta_field(uint64_t, request_id)
    cmeta_field(int, status));
SALTS_FAST_KEY(alloc_fail, false);

cmeta_registry(telemetry_manifest,
    cmeta_manifest_entry("http_request", CMETA_MANIFEST_TRACEPOINT,
        StructMeta(http_request_payload), UINT64_C(0), UINT32_C(0))
);

static uint64_t last_request;
enum { EXAMPLE_REQUEST_ID = 42, EXAMPLE_STATUS = 200 };
static void record_request(const http_request_payload *event) {
    last_request = event->request_id;
}

int main(void) {
    if (cmeta_trace_bind(http_request, record_request) != CMETA_OK ||
        cmeta_trace_enable(http_request) != CMETA_OK)
        return 1;
    cmeta_trace_emit(http_request, (uint64_t)EXAMPLE_REQUEST_ID, EXAMPLE_STATUS);
    if (cmeta_trace_disable(http_request) != CMETA_OK ||
        cmeta_fast_enable(&alloc_fail) != SALTS_OK)
        return 1;
    if (!cmeta_fast_key_consume(&alloc_fail) || cmeta_fast_key_consume(&alloc_fail))
        return 1;
    return last_request == EXAMPLE_REQUEST_ID && telemetry_manifest.count == 1u ? 0 : 1;
}
```

示例行为由 `cmeta_trace_test` 的正式 TinyTest 用例验证，另外覆盖失败和并发边界。独立示例需链接 `CMeta::CMeta` 与 `Salts::Platform`。Manifest 的零 fingerprint 表示未声明 fingerprint，不是新的算法或有效性检查。

## 控制入口

- `cmeta_tracepoint(name, fields...)` 生成 `name_payload`、exact typed backend 和默认关闭的 key。字段列表沿用 `cmeta_struct` 的 1–16 字段与 `cmeta_field` 分隔约定。
- `cmeta_trace_bind(name, callback)` 要求 `void (*)(const name_payload *)`，不兼容签名在编译时拒绝；成功返回 `CMETA_OK`。生成的 `name_bind(NULL)` 返回 `CMETA_INVALID_ARGUMENT`，不改变现有 backend。
- `cmeta_trace_enable(name)` 成功返回 `CMETA_OK`，未绑定返回 `CMETA_INVALID_ARGUMENT`；`cmeta_trace_disable(name)` 关闭 key 并返回 `CMETA_OK`，保留 backend。
- `cmeta_trace_emit(name, values...)` 无返回值；关闭时无参数副作用，启用时同步调用 exact typed backend，不查询 Reflection。
- `SALTS_FAST_KEY(name, false)` 在 C 模块声明默认关闭的 Platform key；`cmeta_fast_enable(&name)` / `cmeta_fast_disable(&name)` 返回 `SALTS_OK` 或 `SALTS_EINVAL`。
- `cmeta_fast_key_consume(key)` 要求 live、非 NULL key，C/C++ 都返回是否消费一次许可。C++ 借用 C-owned opaque 存储；重复 enable 合并，不累积次数。

## 兼容性与验证

新增 facade 不改变既有 descriptor 布局、manifest 格式、fingerprint 版本或 Plugin ABI，CMeta core 保持无 runtime 依赖；consumer 显式选择 Platform。Disabled 路径保持调用方原有行为，开启 fault 后如何注入失败由所属模块显式决定。

采用已有 portable atomic gate，而不是在这里增加汇编、JIT 或全局注册器。替换 backend 所需的一次 typed 间接调用由 enabled-path benchmark 测量；disabled 路径在 backend load、payload 初始化和调用前返回。纯 fault 控制归 Platform；#957 删除无 metadata 价值的 CMeta 同义入口（HIGH source compatibility）。回滚须成套恢复调用点与 owner，不迁移数据。

正式测试覆盖默认关闭、参数不求值/只求值一次、精确 payload metadata、immutable manifest 借用、未绑定 enable、NULL bind 保留原后端、替换/关闭、并发发布与一次性 fault、C++ opaque 消费以及错误签名编译拒绝。五平台 gate 和 portable TSan 使用这些正式测试；Release benchmark 比较 plain branch、static key、tracepoint 和 fault point，不设置机器相关阈值。Component/capability manifest 和 canonical fingerprint 已通过同版本组合验证，契约见 [COMPONENT_MANIFESTS.md](COMPONENT_MANIFESTS.md) 与 [FINGERPRINTS.md](FINGERPRINTS.md)。

本地验证在 Visual Studio `VsDevCmd.bat -arch=x64 -host_arch=x64` 环境中使用版本化 user preset。设置该环境要求的 `PROJECT_ROOT`、`VCPKG_ROOT`、`VCPKG_WINDOWS_TRIPLET` 和 `VCPKG_WINDOWS_HOST_TRIPLET` 后，复验命令为：

```powershell
cmake --preset win-dev-user -DBUILD_TESTS=ON -DBUILD_BENCHMARKS=OFF -DBUILD_EXAMPLES=OFF
cmake --build --preset win-dev-user --target cmeta_trace_test cmeta_trace_cpp_test cmeta_fastpath_test cmeta_fastpath_cpp_test cmeta_manifest_test cmeta_header_cpp_test --parallel 4
ctest --preset win-dev-user --output-on-failure -R "^(cmeta_trace_|cmeta_fastpath_(test|cpp_test|.*compile_fail)$|cmeta_manifest_test$|cmeta_header_cpp_test$)"
```

Release 使用 `win-release-user`，configure 增加 `-DCMETA_BUILD_BENCHMARKS=ON`，build 增加 `cmeta_trace_benchmark`，并使用 `ctest --preset win-release-user -V -R '^cmeta_trace_benchmark$'` 查看 TinyTest benchmark。

2026-10-06 本地结果（事实）：MSVC 19.44 + ASan 与 Clang 21.1 各 9/9 通过；额外 C11 aggregate header/core/capabilities ASan 回归 3/3 通过；Release/native 正式测试与 benchmark 通过。MSVC Release `/O2 /Ob2`，25 × 1,000,000 ops 的 plain disabled branch/static key/tracepoint/fault point 均值为 0.468/0.475/0.472/0.474 ns，enabled tracepoint 为 1.271 ns。推论：该负载下 disabled facade 与 baseline 成本接近；此结果不构成跨机器的零开销保证。
