# 精确原生调用特化（#981）

## 边界与选择

首个有限形状为 `int(int)`，以及将 `int(void *, int)` 的显式 borrowed
receiver 绑定为 `int(int)`。FunctionAbi 和 Function-owned receiver projection
负责 admission；生成器不从任意 Reflection 重建 ABI。不添加 MIR/libffi 依赖。
普通 C 生成的 callable 仍为默认路径，应用显式链接 `CMeta::CMetaNative` 后选择特化。

现有 `cmeta_mmap` 属于文件映射接口，没有匿名代码页与发布权限转换协议，不能直接
用来承载本功能。原生模块内部的平台适配器只负责有界代码页与 W^X 状态迁移。

相比固定 C 包装器，原生 thunk 可将 receiver 固定到稳定的调用地址，并允许控制面
重新绑定。代价是每个 thunk 一个 OS 分配单元与发布系统调用。相对于 MIR，有限形状降低
审计范围，但不支持任意参数、浮点、aggregate、变参或栈参数。AArch64 须另有消费者
与 benchmark 证据后增加；不提供猜测式降级。

## 所有权与状态协议

- 单元：一个 thunk、一个代码分配、一个 borrowed admitted binding；上限由创建时
  `max_code_bytes` 提供，Windows 计入 reservation granularity，Linux 计入整页取整。
  创建和销毁属于控制面，调用不分配内存。
- 唯一 owner 为调用方持有的 handle；不得复制或移动 live handle。描述符、target、
  receiver 和 Plugin lease 均不 retain，必须覆盖所有调用与重定向。
- 拓扑：单控制线程；READY 后可由多个线程调用，目标自身的并发规则不变。
  rebind/destroy 必须由调用方先关闭调用 admission 并等待全部调用退出。
- 状态：EMPTY → WRITABLE → READY。发布先写入，再转 RX 并同步指令缓存；从不 RWX。
  重定向保持地址稳定，但必须先撤销借出的入口，再将 RX 转 RW。
- 分配/预算失败保持 EMPTY；写权限切换失败保留原 READY 入口。发布失败不暴露新入口，
  handle 进入不可调用状态，允许显式重试 rebind 或 destroy，不自动执行旧代码。
- 销毁成功清空 handle；OS 释放失败保留资源并报告错误以便重试。无后台回收、隐式
  module retention 或并发重定向保证。
- 源 ABI/投影/所有权不匹配在修改代码前拒绝。成功 rebind 才发布新的 binding。

## 后端与验证

### ObjectRef 与 static-call 消费协议

ObjectRef 特化通过可选 `cmeta_native_object_provider` 关联原 operation provider，
不扩展 ObjectRef、FunctionDesc 或 provider 的核心 ABI。先复用
`cmeta_object_operation_invokable_bind` 验证对象、operation 成员资格和引用调用，再请求
同一 provider 的精确 native 绑定并重新 admission。provider 返回错误或不兼容投影时，
输出 token 保持空；没有代码分配、retain/release 或自动回退。生成器/provider 负责确保
两条执行路径具有相同语义，native 层不从 callable capture 或通用 invoke 指针反推 ABI。

`<cmeta/native/object.h>` 随 `CMeta::CMetaNative` 提供；
`<cmeta/native/static_call.h>` 的 C11 前端还要求链接既有 `Salts::Platform`。
`cmeta_static_thunk_call(name, default_function)` 声明有限 `int(int)` 槽，
`cmeta_static_thunk_update(name, &thunk)` 只借用 READY 入口；空/未发布句柄返回
INVALID_ARGUMENT，ABI 不匹配返回 TYPE_MISMATCH。

static-call 特化复用 `cmeta_static_call` 的
原子槽和 ABI 检查；失败时原目标保持不变。槽中仅借用已发布 thunk 的入口，不拥有
代码页。撤销时先发布普通目标，阻止新的 thunk 调用，等待旧调用退出，再 destroy；
在同一地址 rebind 仍要求调用方关闭 admission 并达到 quiescence。原子指针更新不能
代替这一步。正常调用沿用原 static-call 路径，没有新增 Reflection 查询。

两种 adapter 都只产生借用，额外状态固定有界。描述符/provider、对象、thunk、外层
Plugin lease 的销毁顺序由调用方明确安排：调用结束 → 撤销槽/借出入口 → destroy thunk
→ release ObjectRef → release lease。失败不会转移原对象或槽的所有权。移除 opt-in
adapter 即可恢复原引用调用，核心语义和数据布局无需迁移。

Win64 使用寄存器参数与原调用方的 shadow store；SysV 使用自己的参数寄存器映射。
两者都只使用 volatile 寄存器并 tail-jump，不调整栈、不嵌套 call，因此没有新增
栈帧或 non-leaf unwind 元数据。将来扩大形状必须重新评审 ABI/unwind 边界。

基线与特化用同一外部 TU target、相同操作数和 TinyTest/CTest benchmark 测量；
单独报告创建/重定向成本，不将控制面成本藏进 steady-state 收益。测试覆盖负数、
边界整数、重复重定向、错误 ABI、容量不足、C++ 异常展开、生命周期与安装后的消费端。
独立链接的测试 OS adapter 注入分配/写权限/发布/释放失败，验证状态和释放义务；生产库
不包含故障开关。Plugin 测试使用同一 DSO 中的 generated receiver 与 native receiver，
记录唯一显式 lease，先销毁 thunk 再 release/stop/quiesce/unload，不改变 loader 状态机。
默认构建不开启特化；移除 `CMETA_BUILD_NATIVE_THUNKS` 即可回退到应用原有显式 C 路径，
不会自动改变已经选择的执行方式或公共 Reflection 布局。

可编译消费者与验证入口：

- [`cmeta_native_thunk_test.c`](tests/cmeta_native_thunk_test.c)：admit、预算、创建、调用、
  同地址重定向和销毁；其 `int(int)` 直接重定向也是静态调用方的显式 opt-in 示例。
- [`cmeta_native_thunk_cpp_test.cpp`](tests/cmeta_native_thunk_cpp_test.cpp)：跨 leaf thunk
  的异常展开；不引入 non-leaf 路径或 unwind table 所有权。
- [`cmeta_native_object_test.c`](tests/cmeta_native_object_test.c)：共享/拥有对象、跨 TU
  provider、能力成员资格、错误传播，以及引用/特化路径的结果和释放计数一致性。
- [`cmeta_native_static_call_test.c`](tests/cmeta_native_static_call_test.c)：同一原子槽中的
  native 发布、普通目标恢复、单次求值及 ABI 拒绝；错误形状另由 compile-fail CTest 覆盖。
- [`plugin_native_test.c`](../plugin/tests/plugin_native_test.c)：显式 lease 下的 DSO
  消费和 benchmark。fixture offer 属于测试协议，不是新增 Plugin 公共 ABI。
- `cmeta_native_benchmark` 分别测调用、quiescent rebind、create/destroy；
  `cmeta_plugin_native_benchmark` 测同一个已启动 DSO 的借用调用。

Windows 本地使用 `win-cmeta-native-dev-user` / `win-cmeta-native-release-user`
configure/build/test presets。CI 在 Linux Release、Linux ASan/UBSan 与 Windows Release
启用此选项；其它平台保持默认 C 实现。配置接入不等于对应平台已通过运行验证。

### 本地测量（2026-10-06）

事实：MSVC 19.44、x64 Release、同一外部 TU/DSO target，25 个 sample，调用路径每个
sample 一百万次。CTest `cmeta_native_benchmark` 测得 generated receiver 为
6.841 ns/op，native 为 2.871 ns/op；Plugin 同一 DSO 分别为 6.490 与 2.883 ns/op。
控制面每个 sample 一千次，rebind 为 2.224 µs/op，create+destroy 为 4.670 µs/op。

计算：调用节省约 3.970 ns/op，单次创建/销毁成本的粗略摊销点为
4670 / 3.970 ≈ 1177 次调用，未计入代码页占用和调用方 quiescence 成本。
这是单机微基准，不代表真实业务收益；只有重复调用足以摊销控制面和内存成本时才适合
显式 opt-in。扩大形状或平台仍需分别测量，不能从此结果推断 AArch64 收益。

同日接入消费者后的独立测量（同一配置、sample/操作数）：ObjectRef admitted reference
为 5.115 ns/op，native 为 2.564 ns/op；普通绑定 native 为 2.582 ns/op，经过既有
static-call 槽为 2.579 ns/op。后两者差值仅 0.003 ns/op，不据此声称槽带来加速。
ObjectRef 的两个路径调用同一外部 TU target，计时外断言结果、provider 调用次数与唯一
显式 retain/release；native 不新增 owner 或 lease。基准入口仍为 `cmeta_native_benchmark`。

依据：[Microsoft x64 ABI](https://learn.microsoft.com/en-us/cpp/build/x64-calling-convention)、
[指令缓存同步](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-flushinstructioncache)、
[x86-64 SysV psABI](https://gitlab.com/x86-psABIs/x86-64-ABI)。
