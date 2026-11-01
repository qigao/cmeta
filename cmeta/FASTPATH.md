# Typed static call over Platform fastpath (#923 / #963)

## 决策与边界

Salts::Platform 提供 C11 原子 bool gate；CMeta 提供精确类型的原子函数指针槽与
Function ABI 校验。读取使用 acquire，更新使用 release。`FunctionDecl` 同时生成
函数指针 typedef，槽复用它；调用路径不做反射查询。槽与 descriptor 的布局、
相等查询、状态归属和所有权契约保持一致。

候选方案包括 C 原子、固定汇编 load、运行时 direct-call patch。当前统一采用
C 原子：固定汇编入口增加外部函数调用，本地 Windows 测量未显示收益；运行时
patch 还会引入可执行内存、指令缓存同步和并发代码修改的额外协议。
这里借用 Linux static-key 的控制面/数据面分离，不承诺指令 patch 的零 load 成本。

**兼容性（HIGH）**：删除 `SALTS_PLATFORM_NATIVE_FASTPATH` 构建选项及显式 native
fastpath 接口，不保留别名。原 `cmeta_fast_key_read_native` 调用改用
`cmeta_fast_key_read` 或 `cmeta_fast_branch`；原 `cmeta_static_native_invoke` /
`cmeta_static_native_invoke0` 改用 `cmeta_static_invoke` / `cmeta_static_invoke0`。
直接使用原始 target-load 接口的调用方应通过 typed slot 的 `slot_load` 获取精确
类型的函数指针。消费者移除旧配置参数并重新编译；无运行期数据迁移或新依赖。
可选 `CMETA_BUILD_NATIVE_THUNKS` 属于独立机制，见 [NATIVE_THUNKS.md](NATIVE_THUNKS.md)。

## 状态与生命周期协议

每个 key 的事实源是一个 `atomic_bool`；每个 call 的事实源是一个精确类型的
原子函数指针。无队列、动态容量或堆分配，空间 O(1)，load/store O(1)。初始化、
销毁以及存储所在模块卸载要求 quiescent；运行期间允许 MPMC 更新和读取，最后
一次原子修改决定当前值。invoke acquire 一次，然后
执行该次读出的目标；它可能在后续 update 前后继续执行。

发布使用 release。只有 acquire 读到对应发布（或 C 原子规则允许的 release
sequence）才建立对此前数据的可见性；bool 反复切换不是版本或完成通知。
关联可变数据仍需自己的同步或不可变快照协议，不能靠 gate 获得互斥。

目标代码由调用方所属模块持有。ABI metadata 只在控制面校验期间借用，不保存
进槽；candidate metadata 可在 update 返回后释放，default metadata 在每次更新
时必须仍有效。update 不 retain、drain、取消或卸载旧目标；所有旧代码目标必须
存活到读者和在途调用结束。停机顺序是停止
接收调用、等待所有读者/调用结束，再释放槽或卸载提供者。替换不引入第二份
状态，不依赖 #927 未合并的 guard 实现。

## 公开使用

`<fastpath.h>` 暴露 Platform-owned static key；`<cmeta/fastpath.h>` 显式暴露 CMeta typed static-call projection。`<cmeta/meta.h>` 不暴露这些 optional runtime/control-plane 能力。C11 的
`SALTS_FAST_KEY(name, initial)` 定义原子 gate；`cmeta_fast_branch(&name)`
读取，`cmeta_fast_enable` / `cmeta_fast_disable` 发布新值，`cmeta_fast_key_set` 可显式
指定状态。NULL 更新返回 `SALTS_EINVAL`；读取要求非 NULL 活对象。
C++17 只借用 C 定义的 opaque key，通过同名读取和控制 API 访问。

`cmeta_static_call(slot, default_function)` 必须位于 C 文件作用域；在一个 TU
定义槽，不能放进被多个 TU 包含的头中重复定义。默认目标需先有 `FunctionDecl`
或 `Function0Decl`。`cmeta_static_invoke(slot, args...)` 调用精确签名；零参数用
`cmeta_static_invoke0(slot)`，避免 C11 空 variadic 参数扩展。

`cmeta_static_update(slot, target_function)` 从同一声明取 pointer 和 ABI，编译期
拒绝签名不匹配的符号。生成的 `slot_set` 是高级控制接口：调用方必须如实关联
精确类型 pointer 与 metadata。NULL slot/target、无效 metadata 返回
`CMETA_INVALID_ARGUMENT`；不兼容返回 `CMETA_TYPE_MISMATCH`。失败保留原目标。

`cmeta_function_abi_contract_compatible` 忽略函数名和参数名，要求有效且显式的
ABI carrier、语义类型、参数方向/所有权、结果 flags、effects 和 properties 精确
一致；UNSPECIFIED carrier 不兼容。它只用于控制面，含 descriptor 校验的时间为
O(n²)，生成声明最多支持既有的 16 个参数。它不证明提供者代码履行了声明的契约。

完整可运行示例见 `tests/cmeta_fastpath_test.c` 与
`tests/cmeta_fastpath_coroutine_test.c`，包含声明、实现、切换和错误检查。

## 验证与测量

正式 TinyTest 覆盖 gate publication、MPMC 替换、失败不修改、跨 TU metadata、
精确 C 签名、C++ opaque 消费及 coroutine yield/resume。保留原有测试验证 acquire /
release、错误状态和 ABI；benchmark 比较普通 branch、C 原子 key、直接调用和
C 原子槽调用。`CMETA_BUILD_BENCHMARKS=ON` 可单独启用 CMeta benchmark graph。
结果有正确性断言，不设跨机器性能阈值。

删除前在提交 `467dd000` 上使用 Windows x64 / Ryzen 9 7940HX / MSVC 19.44
Release `/O2 /Ob2`，固定一个逻辑 CPU，连续测量 5 轮，每轮 25 samples ×
1,000,000 ops。下表是每轮平均 ns/op 的中位数，属于该机器上的测量事实：

| 路径 | 普通 C | C 原子 | 已删除的汇编入口 |
| --- | ---: | ---: | ---: |
| disabled key | 0.443 | 0.445 | 1.121 |
| enabled key + callback | 1.125 | 1.126 | 2.041 |
| call | 1.353 | 1.350 | 2.580 |

计算：汇编槽调用的耗时比为 `2.580 / 1.350 ≈ 1.91`；其五轮平均值范围为
2.222–3.016 ns/op。此结果不代表其他平台，也未用 profiling 分离具体开销来源。
当前实现可在 Visual Studio x64 环境中通过 user preset 复验：

```sh
cmake --preset win-release-user -DENABLE_TESTS=ON -DBUILD_TESTS=ON -DBUILD_BENCHMARKS=OFF -DCMETA_BUILD_BENCHMARKS=ON
cmake --build --preset win-release-user --target cmeta_fastpath_benchmark
ctest --preset win-release-user --no-tests=error -V -R '^cmeta_fastpath_benchmark$'
```

若需回滚，应成套恢复汇编实现、公开入口、构建配置与测试；原子槽及其存储格式
不需要迁移。不会把仅恢复旧声明当作可用后端。

参考：[Linux static keys](https://cdn.kernel.org/doc/html/latest/staging/static-keys.html)、
[GCC atomic memory models](https://gcc.gnu.org/onlinedocs/gcc/_005f_005fatomic-Builtins.html)。
