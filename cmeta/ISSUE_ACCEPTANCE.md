# #976 / #977 / #980 / #981 逐项验收记录

> 历史验收记录：下述 semantic 分组测试现已删除；相关运行结果仅描述当时提交，不代表当前测试覆盖。

核对日期：2026-10-07。代码基线：`8d19c98e1d0d208ae148bd1404fe59eca875f9ce`，
分支 `feat/cmeta-plugin-declarations-976-977`，[PR #979](https://github.com/qigao/salts/pull/979)。

需求来源为核对时的 [#976](https://github.com/qigao/salts/issues/976)、
[#977](https://github.com/qigao/salts/issues/977)、
[#980](https://github.com/qigao/salts/issues/980)、
[#981](https://github.com/qigao/salts/issues/981) 正文。四份正文最后更新时间均为 2026-10-06；
本次覆盖 #976 的全部 36 项能力及四个 issue 正文中的全部 191 个 checkbox，
包括阶段、迁移、平台与最终 Acceptance，保留原顺序和原文，便于逐项回查。

注意：以上基线和计数是 2026-10-06 issue 正文的历史快照，不代表此后扩展项已验收。
2026-10-07 更新的 #980 新增 P2 自动 lowering、P4 binding trust 及对应两项 Acceptance。
这四项的实现与验证单独记录在 [本轮生命周期补充](LIFECYCLE_LOWERING.md)；不能用下表
原来的“31/32”覆盖新增 merge gate。后续编译器实测也见 [审查单元与验证记录](REVIEW_STACKS.md)。

## 结论与判定口径

**事实：主要实现已具备；四个 issue 尚不能整体标记为全部验收完成。**
[#981 的有限 x86_64 范围](NATIVE_THUNKS.md) 已有实现、ABI/生命周期测试和收益证据；
#976/#977/#980 仍有编译器、Mach-O 运行验证和 CI 选例缺口，另有编译器分支集中化及旧文档收尾。

| Issue | 满足 | 待验证 | 未完全满足 | 条件范围 | Checkbox 总数 |
|---|---:|---:|---:|---:|---:|
| #976 | 85 | 2 | 1 | 0 | 88 |
| #977 | 39 | 2 | 0 | 0 | 41 |
| #980 | 31 | 1 | 0 | 0 | 32 |
| #981 | 28 | 0 | 0 | 2 | 30 |
| 合计 | 183 | 5 | 1 | 2 | 191 |

**计算：**按正文 `- [ ]` 条目逐行计数，88 + 41 + 32 + 30 = 191。
不同章节有重复要求；这些数字是核对条目数，**不是功能完成率**。
“满足”表示在下列已核对实现与实际测试范围内满足该项，不意味着每个编译器已运行每项测试；
跨编译器总资格由单独条目及 G1–G3 判断。禁止用局部“满足”覆盖总资格缺口。
“条件范围”表示 issue 本身规定的扩展触发条件尚未成立，不是已实现。

本次未改变生产实现、CI 选择规则、ABI 或 GitHub issue 状态。
工作区已有的 `.github/workflows/native-io-benchmarks.yml` 和 `.gitignore` 未提交改动
不属于本次验收，未纳入基线结论。CodeGraph 已同步，并以实际实现、调用点、正式测试及日志复核。

## 未关闭项

### G1 — MED：缺少当前提交的 Clang 原生语义运行资格

**事实：**当前 CI 的 Linux 原生 profile 使用 GCC，Windows 使用 MSVC，macOS 使用 GCC 15。
[编译器定义](../presets/Compilers.json) 将 AppleClang 用于 iOS；iOS/Android 交叉编译成功
不能替代 PP、RAII、Plugin DSO 与错误诊断的原生执行记录。
当前成功 run 未提供 Clang 原生 C/C++ 语义矩阵，旧的本地 Clang build tree 也不能证明当前提交。

**影响：**#976 平台矩阵及最终 compiler acceptance、#977 compiler acceptance、
#980 P6 三编译器 parity 尚未完成。并未据此推断 Clang 实现有 bug。

**最小补齐：**通过正式 user preset 构建并运行现有相关 CTest，包括 C/C++、负向诊断、
生命周期、接收者、Plugin 和 installed SDK；记录准确源码提交与编译器版本。
无需新增消费者工程或替代测试框架。

### G2 — MED：Mach-O 跨 TU Plugin 聚合缺少本次运行证据

**事实：**[linker 后端](../plugin/include/cmeta/plugin_linker.h) 已有 COFF/ELF/Mach-O 实现，
`正式测试`（已删除） 已设置各平台 dead-strip / section-GC。
Linux 和 Windows 的 Plugin suite 实际运行 `cmeta_plugin_linker_test` 和
`cmeta_plugin_scope_cpp_test`。本次 macOS 仅选择 execution / native suites，
[当时的筛选规则](https://github.com/qigao/salts/blob/8d19c98e1d0d208ae148bd1404fe59eca875f9ce/.github/workflows/native-tests.yml) 的 native 分支不包含这两项。
macOS build 成功不能证明加载 DSO 后的跨 TU 聚合、片段保留、计数错误拒绝和隔离行为。

**影响：**#977 P4 “qualified ELF/Mach-O/COFF” 只能确认 ELF/COFF 的运行结果。
#976 要求“明确三平台设计或分阶段范围”的设计项已具备，不能因此宣称 Mach-O 运行验收也完成。

**最小补齐：**在 macOS 正式 test matrix 选择现有 Plugin suite，执行 linker 与 lease 用例；
另外补齐 macOS installed SDK 运行记录，才能扩大 installed 跨平台结论。
后端结构无需因缺少测试记录而重写。

### G3 — MED：CI 筛选遗漏部分新增验收用例

**事实：**`测试注册`（已删除） 有 `cmeta_interface_arity_test/cpp_test`、
`cmeta_object_scope_cpp_test`、`cmeta_operation_test`、`cmeta_invokable_test`
以及 20 项 `cmeta_lowering_*_compile_fail`。
当前 native / execution / plugin 正则未直接选择这 25 项。
Linux installed suite 复用 ObjectRef RAII、operation、invokable 源码，提供了其中部分行为证据；
它没有补齐 Interface arity 与上述 20 项负向诊断。

**事实：**本次已在 MSVC Release 补跑这 25 项，25/25 通过，日志
`build/acceptance-local-msvc.log`。这是本地补充证据，并未修复未来 CI 的持续覆盖。

**影响：**“所有已选 tests/benchmark 通过”不能升级为“全部验收用例在所有平台通过”。
特别是声明期拒绝、最大 arity 和接收者桥接的回归可能不被现有筛选发现。

**最小补齐：**将上述正式测试纳入适当的 CI suite，并在 GCC、Clang、MSVC 执行。
沿用 configure 确定完整 build graph、完整构建、CTest 选例的现有边界。

### G4 — LOW：编译器条件仍有两处未集中

**事实：**[capture storage](include/cmeta/cmeta.h) 的 `cmeta_capture_storage` 仍直接按
`_MSC_VER` 选择对齐成员；[static-call header](include/cmeta/fastpath.h) 仍直接按
`_MSC_VER / __clang__` 选择 lock-free 断言。
新增的大部分设施已由 `compiler.h` 管理，但 #976 的“编译器设施集中在语义 DSL 之外”
按字面尚未完全满足。

**影响：**目前证据显示为维护与边界一致性问题，未发现由这两处分支导致的行为回归。
**最小补齐：**让编译器 owner 暴露明确的对齐/能力事实，保持 capture ABI 和 static-call
原有断言强度；以 C/C++ 布局与现有 static-call 测试验证，不能简单删除断言。

### G5 — LOW：Plugin 文档混用旧 ABI 数字

**事实：**[历史 PLUGIN_MANIFESTS.md](https://github.com/qigao/salts/blob/8d19c98e1d0d208ae148bd1404fe59eca875f9ce/cmeta/PLUGIN_MANIFESTS.md) 同时提及 ABI 5 admission、
“query 只接受 ABI 4”及“Reflection ABI 3 不变”。
当前 [Plugin ABI 常量](../plugin/include/cmeta/plugin.h) 为 5，
[Reflection ABI 常量](include/cmeta/abi.h) 为 4。

**影响：**跨 DSO 消费者可能按旧文档协商错误版本；运行时 exact admission 本身仍在。
**最小补齐：**更新旧版本说明，并区分 #926 CMeta discovery-array 范围与 #977 Plugin linker 范围。
这是文档一致性问题，不以修改或放宽运行时 ABI 检查解决。

## 实际执行证据

[CI run 37495434846](https://github.com/qigao/salts/actions/runs/37495434846)
对应上述完整 SHA，结论 success。下列数量是 CTest 注册测试数，并非每个测试内部断言数。

| 记录 | 实际结果 | 与本次验收有关的范围 |
|---|---|---|
| [Linux native](https://github.com/qigao/salts/actions/runs/37495434846/job/112383104660) | 67/67 | bind、cleanup、data_select、Function、native thunk、原生 benchmark |
| [Windows native](https://github.com/qigao/salts/actions/runs/37495434846/job/112383104804) | 70/70 | MSVC 公共头、Win64 native ABI、static-call、benchmark |
| [Linux ASan native](https://github.com/qigao/salts/actions/runs/37495434846/job/112383104507) | 67/67 | 对应宿主 C/C++ 路径在 sanitizer 下运行；不声称动态机器码本身被插桩 |
| [Linux Plugin](https://github.com/qigao/salts/actions/runs/37495434846/job/112383104786) | 57/57 | ELF linker、声明/诊断、lease、loader/lifecycle |
| [Windows Plugin](https://github.com/qigao/salts/actions/runs/37495434846/job/112383104657) | 58/58 | COFF linker、声明/诊断、lease、Windows dependency |
| [Linux execution](https://github.com/qigao/salts/actions/runs/37495434846/job/112383104511) | 47/47 | scope、CSTL identity/projection、lowering |
| [Windows execution](https://github.com/qigao/salts/actions/runs/37495434846/job/112383104629) | 47/47 | 同类 MSVC lowering 与 rollback 契约 |
| [macOS execution](https://github.com/qigao/salts/actions/runs/37495434846/job/112383104613) | 41/41 | GCC scope / lowering，非 Clang / Plugin linker 全资格 |
| [macOS native](https://github.com/qigao/salts/actions/runs/37495434846/job/112383104637) | 60/60 | GCC capability / fastpath / reflection，非 x86 native thunk 扩展 |
| [Linux projection + installed](https://github.com/qigao/salts/actions/runs/37495434846/job/112383104653) | 3/3 + 31/31 | CFlow projection、installed C/C++、Plugin linker/lease、native 消费者 |
| 本次本地 MSVC 补跑 | 25/25 | G3 列出的 5 项行为测试与 20 项编译失败诊断 |

run 中名为 `sdk-tests` 的发布相关 job 被跳过，不等于 installed tests 全部被跳过；
Linux projection job 确实执行 31 项 installed tests。反过来，也不能用这 31 项证明
所有系统、所有头文件组合和全部安装配置均已验收。

### 本次本地复验命令

先通过 `vswhere` 找到 Visual Studio，进入 `VsDevCmd.bat -arch=x64 -host_arch=x64`，
并将 `PROJECT_ROOT` 设为仓库根目录，再执行：

```powershell
cmake --build --preset win-cmeta-native-release-user --target cmeta_interface_arity_test cmeta_interface_arity_cpp_test cmeta_object_scope_cpp_test cmeta_operation_test cmeta_invokable_test
ctest --preset win-cmeta-native-release-user --no-tests=error --output-on-failure -R "^cmeta_(interface_arity_|object_scope_|operation_test$|invokable_test$|lowering_)"
```

这里按 target 构建的是本地已有正式测试；CI 仍须完整构建 configure 产生的 graph。
本次构建报告 `ninja: no work to do`，随后 CTest 完整执行所选 25 项，包括负向诊断构建。

## 证据索引

下列证据组由源文件、调用点、正式测试和上述实际运行结果组成。
仅看到实现或测试源码，不独立等价于运行验收；平台限制以 G1–G3 为准。

| ID | 实现与契约 | 正式用例 / 核对结果 |
|---|---|---|
| E1 | [pp.h](include/cmeta/pp.h)、[有限宏参考](LANGUAGE_REFERENCE.md#有限宏与精确调用声明) | `PP C 用例`（已删除）、C++ 共用用例、`现代零参数`（已删除）：0/16、pair、tuple、CAT/stringify、布尔/probe/unique；C11 显式计数，C++20 / finalized C23 按 capability 开放 |
| E2 | [compiler.h](include/cmeta/compiler.h)、[container_of.h](include/cmeta/container_of.h)、[语言能力说明](LANGUAGE_REFERENCE.md) | `原生类型`（已删除）、`container_of`（已删除）、`cleanup 能力`（已删除），通过 PP C/C++ 测试执行；MSVC C 不支持的推导能力明确为 0，显式类型入口仍有检查；残余集中化见 G4 |
| E3 | [interface.h](include/cmeta/interface.h) 的 decode → normalized row → shared map | `arity C 用例`（已删除） 及 C++ 共用：R/V、reflected/exact carrier、0–4；只保留输入形状归一化的有限分派，vtable/wrapper/call/metadata 主体已共享；本次 MSVC 2/2 |
| E4 | [function.h](include/cmeta/function.h)、[struct.h](include/cmeta/struct.h)、[enum.h](include/cmeta/enum.h)、[data_select.h](include/cmeta/data_select.h) | data_select C/C++ 与错误选择 compile-fail；`lowering 错误声明`（已删除） 及 C++：layout、callback、result flags/carrier；本次 MSVC 20/20 负向诊断含 capture 项 |
| E5 | [lifecycle.h](include/cmeta/lifecycle.h)、[scope.h](include/cmeta/scope.h)、[cleanup.h](include/cmeta/cleanup.h)、[object_scope.h](include/cmeta/object_scope.h)、[lowering 契约](LIFECYCLE_LOWERING.md) | cleanup C/C++、scope/逃逸 compile-fail、`ObjectRef RAII`（已删除）、[CSTL lifecycle](../cstl/tests/cstl_header_typed_test.c)：partial restore 一次、LIFO、move、nested、exception、busy/detach、trivial 无 live/ops；Linux installed 复用 |
| E6 | [Function projection](src/function.c)、[operation.h](include/cmeta/operation.h)、[invokable.c](src/invokable.c)、[ObjectRef](src/object.c)、[CSTL schema](../cstl/include/cstl/detail/typed_facade.h) | function_reflection、operation、invokable、object、cstl_generic_identity / semantic_projection；operation 只存 alias + ABI，ABI 拥有唯一 FunctionDesc；旧 receiver_method API 检索无残留；本次 MSVC operation/invokable 2/2，Linux installed 复用 |
| E7 | [invoke_decl.h](include/cmeta/invoke_decl.h)、[plugin_decl.h](../plugin/include/cmeta/plugin_decl.h)、[runtime admission](../plugin/src/plugin.c) | `Plugin declaration`（已删除）、C++ / empty passive+managed / compile-fail、contract / manifest / loader / lifecycle；void/nonvoid、0/nonzero、精确 carrier、显式发布、ABI 5 拒绝错误版本 |
| E8 | [plugin_linker.h](../plugin/include/cmeta/plugin_linker.h)、[plugin_scope.h](../plugin/include/cmeta/plugin_scope.h)、[Plugin 契约](../plugin/README.md) | `linker 编排`（已删除）、linker/lease C++：跨 C/C++ TU、GC、DSO 隔离、缺片段计数拒绝、move、exception、borrow 先结束；ELF/COFF 实跑，Mach-O 见 G2；无 constructor/可变 registry/隐式 lease |
| E9 | [admitted invocation](src/invokable.c)、[admitted field](src/object.c)、[CFlow admission](../cflow/src/function_projection.c)、[CFlow execution](../cflow/src/subscription.c)、[Plugin admission](../plugin/src/plugin.c) | invokable/object/bind、CFlow projection/execution、Plugin contract：raw 入口保留完整验证；已 admission 的 invocation/field 路径不重遍描述符图。CFlow 执行保留有限 callable flags/storage 检查，不把一切检查视为可删除开销；下游宏复用 CMETA_PP_* |
| E10 | [bind.h](include/cmeta/bind.h)、[Function projection](src/function.c)、`bind tests`（已删除） | C/C++ receiver bind、普通函数非相邻参数 bind、value 快照、borrow、padding、flags/carrier/map 拒绝；capture compile-fail 验证 32-byte 上限及类别，不隐式管理 OWNED/SHARED |
| E11 | [compiler presets](../presets/Compilers.json)、[当时的 CI 选择](https://github.com/qigao/salts/blob/8d19c98e1d0d208ae148bd1404fe59eca875f9ce/.github/workflows/native-tests.yml)、上文 run/job 链接 | GCC/MSVC C/C++ 有实际执行；macOS 为 GCC；Clang 当前运行资格见 G1，选例覆盖见 G3 |
| E12 | `installed CMake graph`（已删除） | Linux 31/31 复用正式测试并链接 Salts imported targets，无 source-tree 公共 include 注入；包括 PP、bind、cleanup、ObjectRef、operation、scope、Function、Plugin linker/lease/declarations、native |
| E13 | [native 契约](NATIVE_THUNKS.md)、[thunk API](include/cmeta/native/thunk.h)、[native 实现](native/thunk.c)、[code-memory owner](native/code_memory.c)、[Object adapter](native/object.c)、[static-call adapter](include/cmeta/native/static_call.h) | native_thunk C/C++、native_failure、native_object、native_static_call、Plugin native、installed native：Win64/SysV 寄存器映射、leaf tail-jump、负值/边界、重绑定、错误 ABI、预算、OS 失败、异常、先撤销借用后销毁；W^X、有界 caller owner，无隐式 retain |
| E14 | [native benchmark](benchmarks/cmeta_native_benchmark.c)、[Plugin native test](../plugin/tests/plugin_native_test.c)、[测量契约](NATIVE_THUNKS.md) | 同 TU 目标/相同操作与显式 lease；分别测量调用与创建/重绑定成本。当前 Linux/Windows Release 结果见下表；不以 sanitizer 差异计算生产收益 |

### #981 当前性能证据

以下为本次成功 run 中 TinyTest 报告的 steady-state ns/op；每组 25 次样本、
每样本 1,000,000 次操作。不是产品端到端吞吐承诺。

| 平台 | admitted generated receiver | native receiver | admitted ObjectRef | native ObjectRef |
|---|---:|---:|---:|---:|
| Windows Release / MSVC | 4.807 | 1.464 | 3.722 | 1.375 |
| Linux Release / GCC | 7.192 | 4.323 | 6.027 | 4.218 |

**事实：**这些有限形状在当前运行中有实际稳态收益，且已有真实 ObjectRef、static-call、
Plugin 消费路径。代码页建立/权限转换属于控制面，不能把上述差值当作单次创建后调用的净收益。
**范围：**只支持已声明的有限 `int(int)` / borrowed `int(void *, int)` 形状；
AArch64 与 non-leaf Win64 尚未实现，分别属于证据触发扩展和当前不存在路径的条件要求。

## #976 的 36 项能力核对

“具备”表示找到对应 owner、实现与用例；总平台资格和收尾仍受 G1–G5 限制。

| 项 | 能力 | 实现入口 / 边界 | 证据 |
|---:|---|---|---|
| 1 | finite pair map | CMETA_PP_PAIR_MAP_N 及逗号形式，0–16 | E1 |
| 2 | separator-aware map | MAP_COMMA_N / MAP_PREFIX_COMMA_N | E1 |
| 3 | PP boolean | BOOL / NOT / AND / OR / IIF / IF | E1 |
| 4 | token predicates | probe / token matching，有限 token 契约 | E1 |
| 5 | expansion-safe stringify | 两级 STRINGIFY / CAT 展开 | E1 |
| 6 | unique IDs | UNIQUE + compiler counter，TU 内唯一 | E1 / E2 |
| 7 | tuple algebra | UNPAREN / TUPLE_GET，有限宽度 | E1 |
| 8 | arity overload | OVERLOAD / NARG / 显式零计数 | E1 |
| 9 | modern zero arguments | VA_OPT capability，C11 不伪装 modern mode | E1 / E2 |
| 10 | expression assertions | CONST_REQUIRE / FLAGS_REQUIRE / LAYOUT_REQUIRE | E2 / E4 |
| 11 | native typeof / auto / once | NATIVE_TYPEOF / SAME_TYPE / AUTO，能力不足不提供弱替代 | E2 |
| 12 | type-safe container_of | 显式 member-type 与 capability-gated 推导，实参一次求值 | E2 |
| 13 | C11 Generic | type_select / data_select 指向 canonical descriptors | E4 |
| 14 | linker metadata | compiler section 设施 + Plugin 三格式后端；Mach-O 待运行 | E2 / E8 / G2 |
| 15 | cleanup / guard layer | attribute capability + portable semantic cleanup/scope | E2 / E5 |
| 16 | feature probing | HAS_BUILTIN / HAS_ATTRIBUTE / HAS_FEATURE 等；仍有 G4 收尾 | E2 / G4 |
| 17 | static lifecycle | canonical accessor，scope 不重验 graph | E5 |
| 18 | admitted lifecycle | admit 后使用同一 ops，borrowed capability | E5 |
| 19 | scope 简化 | UNIQUE 内部化，去用户 token，保留 rollback/LIFO | E5 |
| 20 | cleanup obligation | caller-owned 有界记录，arm/run/transfer/disarm | E5 |
| 21 | validate once / use many | admitted lifecycle/invokable/field；外部边界继续验证 | E9 |
| 22 | static invariants | layout、native callback、flags/carrier、capture 编译期拒绝 | E4 / E10 |
| 23 | lifecycle classification | trivial / nofail / managed/fallible 同一事实源 | E5 |
| 24 | ownership → cleanup | VALUE/BORROW/RELEASE/DESTROY；不把 CFG 状态写进 Reflection | E5 |
| 25 | ObjectRef RAII | 原 object_release authority，move 不 retain | E5 |
| 26 | Plugin lease RAII | 原 registry release，依赖借用先清理 | E8 |
| 27 | generated receiver bind | 精确 thunk + Function-owned receiver projection | E6 / E10 |
| 28 | explicit capture | value/borrow，32-byte 上限，无隐式 managed capture | E10 |
| 29 | generic parameter bind | 有序子集投影，普通函数及非相邻绑定保持契约 | E10 |
| 30 | Function owns receiver | RECEIVER param + Function projection validator | E6 |
| 31 | thin operation relation | alias + FunctionAbi，未重复 FunctionDesc 指针 | E6 |
| 32 | operation set | receiver type / rows / optional Generic owner | E6 |
| 33 | ObjectRef resolution | 先解析 canonical operation，provider 仅提供精确执行权 | E6 |
| 34 | Invokable convergence | 投影验证后 bind_data，无第二套方法语义 | E6 |
| 35 | CSTL migration | 同一 typed-facade schema 生成 Function / ABI / operation | E6 |
| 36 | obsolete layer removal | 旧 receiver_method 类型/API 无残留 | E6 |

## 原始 checkbox 逐项映射

下面 C01… 为各 issue 正文 checkbox 的顺序编号；“正文行”对应本次下载的正文快照，
不是仓库源码行号。原始要求中的旧 `SALTS_PLUGIN_ABI_VERSION` 名称保留以便核对，
实现按已授权重命名使用 `CMETA_PLUGIN_ABI_VERSION`。每行均给出结论与证据组。

### #976

| 核对项 | 正文行 | 原始要求 | 结论 | 证据 |
|---|---:|---|---|---|
| 976-C01 | 368 | vtable parameter emission; | 满足 | E3 |
| 976-C02 | 369 | wrapper declaration emission; | 满足 | E3 |
| 976-C03 | 370 | wrapper call-argument emission; | 满足 | E3 |
| 976-C04 | 371 | R/V result handling where token predicates provide an authoritative distinction; | 满足 | E3 |
| 976-C05 | 372 | required-method validation; | 满足 | E3 |
| 976-C06 | 373 | reflected parameter projection; | 满足 | E3 |
| 976-C07 | 374 | metadata arity extraction. | 满足 | E3 |
| 976-C08 | 382 | audit `function.h`; | 满足 | E4 |
| 976-C09 | 383 | audit Struct/Enum schema frontends; | 满足 | E4 |
| 976-C10 | 384 | audit reflection descriptor generation; | 满足 | E4 |
| 976-C11 | 385 | audit Plugin static metadata/registration; | 满足 | E7 / E8 |
| 976-C12 | 386 | audit scope/RAII helpers; | 满足 | E5 |
| 976-C13 | 387 | audit CFlow DSL macro families for duplicated arity/token mechanics. | 满足 | E9 |
| 976-C14 | 444 | statically known type path can bind lifecycle without repeated descriptor graph validation; | 满足 | E5 |
| 976-C15 | 445 | dynamic/foreign descriptor path still performs full admission; | 满足 | E5 |
| 976-C16 | 446 | both paths use the same canonical `cmeta_data_construct_ops` semantic authority; | 满足 | E5 |
| 976-C17 | 447 | no duplicated lifecycle semantics or fallback provider path. | 满足 | E5 |
| 976-C18 | 508 | partial-construction rollback; | 满足 | E5 |
| 976-C19 | 509 | LIFO cleanup; | 满足 | E5 |
| 976-C20 | 510 | semantic-zero restore; | 满足 | E5 |
| 976-C21 | 511 | moved-from cleanup safety; | 满足 | E5 |
| 976-C22 | 512 | nested scope correctness; | 满足 | E5 |
| 976-C23 | 513 | no cross-scope control-flow escape that bypasses cleanup. | 满足 | E5 |
| 976-C24 | 786 | return type; | 满足 | E6 |
| 976-C25 | 787 | result ownership/result flags; | 满足 | E6 |
| 976-C26 | 788 | effects; | 满足 | E6 |
| 976-C27 | 789 | properties; | 满足 | E6 |
| 976-C28 | 790 | remaining parameter names/types/flags; | 满足 | E6 |
| 976-C29 | 791 | receiver removal exactly once; | 满足 | E6 |
| 976-C30 | 792 | receiver type compatibility. | 满足 | E6 |
| 976-C31 | 988 | existing CSTL typed operation reflection remains semantically identical; | 满足 | E6 |
| 976-C32 | 989 | generic owner identity still uses canonical `cmeta_generic_desc` equality; | 满足 | E6 |
| 976-C33 | 990 | receiver TypeDesc matching remains exact; | 满足 | E6 |
| 976-C34 | 991 | current List/Map receiver-operation tests remain green; | 满足 | E6 |
| 976-C35 | 992 | no private CSTL method/reflection universe is introduced. | 满足 | E6 |
| 976-C36 | 1115 | no unsafe cast is used to pretend a mismatched receiver type is valid; | 满足 | E6 / E10 |
| 976-C37 | 1116 | generated callable preserves the receiver-elided FunctionDesc exactly; | 满足 | E6 / E10 |
| 976-C38 | 1117 | effects/properties/result semantics remain identical to the projected method; | 满足 | E6 / E10 |
| 976-C39 | 1118 | executable authority remains the exact generated thunk/callable, not Reflection lookup; | 满足 | E6 / E10 |
| 976-C40 | 1119 | no new runtime method-name invocation path is introduced; | 满足 | E6 / E10 |
| 976-C41 | 1120 | dynamic/foreign providers may continue to supply admitted exact bindings through the existing provider boundary. | 满足 | E6 / E10 |
| 976-C42 | 1166 | capture layout is generated through shared #976 tuple/map primitives; | 满足 | E10 |
| 976-C43 | 1167 | capture size overflow fails at compile time; | 满足 | E10 |
| 976-C44 | 1168 | trivial-value eligibility comes from canonical/generated semantics where needed, not type spelling guesses; | 满足 | E10 |
| 976-C45 | 1169 | borrowed captures remain bounded by their authoritative outer lifetime; | 满足 | E10 |
| 976-C46 | 1170 | Plugin-owned pointers/descriptors/callbacks remain bounded by the live Plugin lease; | 满足 | E10 |
| 976-C47 | 1171 | no implicit retain/release/malloc/free behavior is added to `cmeta_callable`. | 满足 | E10 |
| 976-C48 | 1319 | GCC; | 满足 | E11 |
| 976-C49 | 1320 | Clang; | 待验证 | G1 / E11 |
| 976-C50 | 1321 | MSVC; | 满足 | E11 |
| 976-C51 | 1322 | C mode; | 满足 | E11 |
| 976-C52 | 1323 | C++ inclusion mode where public headers promise it. | 满足 | E11 |
| 976-C53 | 1359 | all capabilities 1�C16 above have one documented CMeta owner and API; | 满足 | E1 / E2 |
| 976-C54 | 1360 | `pp.h` remains a small finite mechanics layer; | 满足 | E1 |
| 976-C55 | 1361 | compiler-specific facilities are centralized outside semantic DSL headers; | 未完全满足 | G4 / E2 |
| 976-C56 | 1362 | Interface no longer needs repetitive arity-specific implementation bodies for ordinary R/V dispatch; | 满足 | E3 |
| 976-C57 | 1363 | zero-arity and max-supported-arity cases are directly tested; | 满足 | E1 / E3 |
| 976-C58 | 1364 | tuple/pair mapping is directly tested; | 满足 | E1 |
| 976-C59 | 1365 | expansion-safe CAT/stringify/overload behavior is directly tested; | 满足 | E1 |
| 976-C60 | 1366 | linker-section registration has explicit ELF/COFF/Mach-O design or an explicitly staged platform scope; | 满足 | E8 |
| 976-C61 | 1367 | RAII/cleanup facilities preserve existing ownership rules; | 满足 | E5 / E8 |
| 976-C62 | 1368 | GCC/Clang/MSVC qualification is green for supported capability sets; | 待验证 | G1 / E11 |
| 976-C63 | 1369 | installed public headers expose the same semantics as in-tree builds; | 满足 | E12 |
| 976-C64 | 1370 | downstream DSLs do not introduce new private copies of these primitives. | 满足 | E9 |
| 976-C65 | 1374 | statically known CMeta types can bind canonical lifecycle without repeated raw descriptor validation on each scope entry; | 满足 | E5 |
| 976-C66 | 1375 | foreign/dynamic descriptors still require full lifecycle admission; | 满足 | E5 |
| 976-C67 | 1376 | admitted lifecycle/invokable/object capabilities do not become a second Reflection/type identity model; | 满足 | E5 / E6 / E9 |
| 976-C68 | 1377 | admitted capability lifetime never exceeds its descriptor/provider/Plugin lease lifetime; | 满足 | E5 / E8 / E10 |
| 976-C69 | 1378 | `cmeta_scope` no longer requires a user-supplied uniqueness token once #976 unique-ID support is available; | 满足 | E5 |
| 976-C70 | 1379 | current partial-init rollback, LIFO cleanup, nested-scope and moved-from cleanup tests remain green; | 满足 | E5 |
| 976-C71 | 1380 | trivial lifecycle values can lower without unnecessary per-value callback/live-state machinery when canonical metadata proves that optimization; | 满足 | E5 |
| 976-C72 | 1381 | managed lifecycle still uses the exact provider authority; | 满足 | E5 |
| 976-C73 | 1382 | Function OWNED/SHARED/BORROWED semantics map to cleanup/borrow obligations through the existing ownership calculus; | 满足 | E5 |
| 976-C74 | 1383 | lexical/CFG move and cleanup state remains compiler-private and is not added to Reflection descriptors; | 满足 | E5 / E10 |
| 976-C75 | 1384 | ObjectRef RAII discharges through `cmeta_object_release()` with existing BORROWED/SHARED/OWNED semantics; | 满足 | E5 |
| 976-C76 | 1385 | Plugin lease RAII discharges through existing Plugin registry authority and preserves dependent-borrow-before-lease-release ordering; | 满足 | E8 |
| 976-C77 | 1386 | repeated runtime descriptor validation is removed only after an explicit admission boundary proves the needed invariant; | 满足 | E9 |
| 976-C78 | 1387 | generated declarations move layout/ABI invariants to compile time where the source is statically authoritative. | 满足 | E4 |
| 976-C79 | 1391 | `FunctionDesc` owns all receiver callable semantics through canonical parameter metadata including `CMETA_PARAM_RECEIVER`; | 满足 | E6 |
| 976-C80 | 1392 | receiver projection validation is Function-owned and preserves result flags/ownership, return type, effects, properties and all non-receiver parameter semantics; | 满足 | E6 |
| 976-C81 | 1393 | the receiver operation relation does not duplicate the canonical FunctionDesc pointer when FunctionAbi already owns it; | 满足 | E6 |
| 976-C82 | 1394 | receiver operation sets remain thin indexes over canonical FunctionDesc/FunctionAbi plus receiver type and optional GenericDesc owner; | 满足 | E6 |
| 976-C83 | 1395 | CSTL typed facades generate receiver operations from the same schema that generates FunctionDesc/FunctionAbi; | 满足 | E6 |
| 976-C84 | 1396 | ObjectRef runtime resolution resolves canonical functions/operations and provider binding only supplies exact receiver execution authority; | 满足 | E6 |
| 976-C85 | 1397 | receiver binding converges into ordinary `cmeta_invokable_bind_data()` after projection validation; | 满足 | E6 |
| 976-C86 | 1398 | Interface Reflection remains independent and is not routed through receiver operations; | 满足 | E3 / E6 |
| 976-C87 | 1399 | Plugin function/interface exports remain based directly on canonical Function/Interface descriptors; | 满足 | E7 |
| 976-C88 | 1400 | obsolete `cmeta_receiver_method*` types/APIs are removed after migration; no permanent dual compatibility path remains. | 满足 | E6 |

### #977

| 核对项 | 正文行 | 原始要求 | 结论 | 证据 |
|---|---:|---|---|---|
| 977-C01 | 448 | exact invoke bridge; | 满足 | E7 |
| 977-C02 | 449 | individual descriptor/ABI pointer wiring; | 满足 | E7 |
| 977-C03 | 450 | export array count; | 满足 | E7 |
| 977-C04 | 451 | manifest boilerplate; | 满足 | E7 |
| 977-C05 | 452 | query ABI check. | 满足 | E7 |
| 977-C06 | 460 | define adapter generator from canonical Function declaration rows; | 满足 | E7 |
| 977-C07 | 461 | support zero/non-zero parameters; | 满足 | E7 |
| 977-C08 | 462 | support void/non-void result; | 满足 | E7 |
| 977-C09 | 463 | prove exact ABI behavior with existing fixtures; | 满足 | E7 |
| 977-C10 | 464 | no runtime ABI reconstruction. | 满足 | E7 |
| 977-C11 | 468 | function export rows; | 满足 | E7 |
| 977-C12 | 469 | interface export rows; | 满足 | E7 |
| 977-C13 | 470 | export array generation; | 满足 | E7 |
| 977-C14 | 471 | compile-time structural checks; | 满足 | E7 |
| 977-C15 | 472 | reuse #976 primitives only. | 满足 | E7 |
| 977-C16 | 476 | passive manifest declaration; | 满足 | E7 |
| 977-C17 | 477 | managed lifecycle declaration; | 满足 | E7 |
| 977-C18 | 478 | exact `SALTS_PLUGIN_ABI_VERSION` query; | 满足 | E7 |
| 977-C19 | 479 | preserve current public ABI/runtime validation. | 满足 | E7 |
| 977-C20 | 483 | align with #926 static manifest semantics; | 满足 | E8 |
| 977-C21 | 484 | portable generated-array reference backend; | 满足 | E8 |
| 977-C22 | 485 | qualified ELF/Mach-O/COFF aggregation only where semantics match; | 待验证 | G2 / E8 |
| 977-C23 | 486 | no constructor/global mutable registry fallback. | 满足 | E8 |
| 977-C24 | 490 | evaluate #976 cleanup/guard facilities for lease scopes; | 满足 | E8 |
| 977-C25 | 491 | preserve existing ownership calculus and teardown order; | 满足 | E8 |
| 977-C26 | 492 | no hidden lease ownership. | 满足 | E8 |
| 977-C27 | 496 | Plugin authoring has one canonical source for function parameter/result ABI shape. | 满足 | E7 |
| 977-C28 | 497 | Handwritten exact invoke adapters are unnecessary for ordinary canonical reflected functions. | 满足 | E7 |
| 977-C29 | 498 | Function exports reuse `FunctionDesc` and `FunctionAbi`; no duplicate descriptor universe. | 满足 | E7 |
| 977-C30 | 499 | Interface exports reuse `cmeta_interface_desc` and exact interface carriers. | 满足 | E7 |
| 977-C31 | 500 | Export/manifest/query boilerplate is generated from one explicit Plugin schema. | 满足 | E7 |
| 977-C32 | 501 | Merely reflecting a declaration does not implicitly publish it as a Plugin export. | 满足 | E7 |
| 977-C33 | 502 | Current exact Plugin ABI admission remains intact. | 满足 | E7 |
| 977-C34 | 503 | Runtime manifest validation remains intact. | 满足 | E7 |
| 977-C35 | 504 | Lease/generation/quiescence/unload invariants remain unchanged. | 满足 | E8 |
| 977-C36 | 505 | Descriptor/view lifetime remains bounded by the authoritative live lease. | 满足 | E8 |
| 977-C37 | 506 | No libffi, ABI guessing, method-name invocation or generic vtable interpretation. | 满足 | E7 / E8 |
| 977-C38 | 507 | No constructor-driven/global mutable registration. | 满足 | E7 / E8 |
| 977-C39 | 508 | Portable generated-array manifest semantics remain the reference backend. | 满足 | E7 / E8 |
| 977-C40 | 509 | GCC/Clang/MSVC and C/C++ public-header qualification remain green. | 待验证 | G1 / E11 |
| 977-C41 | 510 | Installed SDK headers expose the same Plugin declaration semantics as in-tree builds. | 满足 | E12 |

### #980

| 核对项 | 正文行 | 原始要求 | 结论 | 证据 |
|---|---:|---|---|---|
| 980-C01 | 374 | document C11 + compiler capability baseline; | 满足 | E2 / E5 |
| 980-C02 | 375 | define cleanup-obligation abstraction; | 满足 | E2 / E5 |
| 980-C03 | 376 | define static-vs-dynamic admission split; | 满足 | E2 / E5 |
| 980-C04 | 377 | define MSVC lowering contract. | 满足 | E2 / E5 |
| 980-C05 | 380 | generate/access canonical lifecycle directly for static types; | 满足 | E5 |
| 980-C06 | 381 | eliminate repeated scope-entry lifecycle validation; | 满足 | E5 |
| 980-C07 | 382 | classify trivial/no-fail/fallible/managed paths from canonical metadata. | 满足 | E5 |
| 980-C08 | 385 | remove user uniqueness token; | 满足 | E5 |
| 980-C09 | 386 | remove sentinel tricks where #976 primitives replace them; | 满足 | E5 |
| 980-C10 | 387 | minimize live/ops state; | 满足 | E5 |
| 980-C11 | 388 | preserve rollback/LIFO/move/nesting tests. | 满足 | E5 |
| 980-C12 | 391 | generated `_Generic` frontend; | 满足 | E4 / E9 |
| 980-C13 | 392 | declaration-time layout/type/ABI proofs; | 满足 | E4 / E9 |
| 980-C14 | 393 | admitted capabilities reused after trust boundaries. | 满足 | E4 / E9 |
| 980-C15 | 396 | move receiver semantics fully under canonical FunctionDesc; | 满足 | E6 |
| 980-C16 | 397 | migrate ObjectRef/provider resolution; | 满足 | E6 |
| 980-C17 | 398 | remove obsolete receiver-method descriptor layer. | 满足 | E6 |
| 980-C18 | 401 | MSVC RAII lowering tests; | 满足 | E5 / E11 |
| 980-C19 | 402 | COFF metadata registration tests; | 满足 | E8 |
| 980-C20 | 403 | GCC/Clang/MSVC semantic parity matrix; | 待验证 | G1 / E11 |
| 980-C21 | 404 | installed-header qualification. | 满足 | E12 |
| 980-C22 | 408 | #976 can finish without absorbing this issue's implementation scope. | 满足 | E1 / E5 |
| 980-C23 | 409 | static generated types avoid repeated raw descriptor validation in lexical RAII. | 满足 | E5 |
| 980-C24 | 410 | foreign/Plugin descriptors still validate fully at admission. | 满足 | E5 / E7 |
| 980-C25 | 411 | cleanup obligation is semantic; cleanup attributes are backend implementation details. | 满足 | E5 |
| 980-C26 | 412 | trivial values lower without unnecessary callback/live-state machinery. | 满足 | E5 |
| 980-C27 | 413 | managed/fallible values preserve exact partial rollback and LIFO cleanup. | 满足 | E5 |
| 980-C28 | 414 | typed Reflection frontends return/use canonical descriptors rather than parallel metadata. | 满足 | E4 |
| 980-C29 | 415 | receiver operations converge on canonical FunctionDesc/FunctionAbi. | 满足 | E6 |
| 980-C30 | 416 | MSVC provides equivalent CMeta semantics through its own lowering, not fallback behavior. | 满足 | E5 / E11 |
| 980-C31 | 417 | COFF registration is explicitly designed and tested. | 满足 | E8 |
| 980-C32 | 418 | no runtime ABI guessing, libffi-style reconstruction, or hidden provider/Plugin retention is introduced. | 满足 | E6 / E7 / E13 |

### #981

| 核对项 | 正文行 | 原始要求 | 结论 | 证据 |
|---|---:|---|---|---|
| 981-C01 | 283 | inventory exact consumers; | 满足 | E13 |
| 981-C02 | 284 | define thunk semantic boundary; | 满足 | E13 |
| 981-C03 | 285 | define executable-memory ownership; | 满足 | E13 |
| 981-C04 | 286 | benchmark generic current path to justify specialization. | 满足 | E14 |
| 981-C05 | 289 | exact receiver/context binding; | 满足 | E13 |
| 981-C06 | 290 | direct tail-jump; | 满足 | E13 |
| 981-C07 | 291 | redirectable target; | 满足 | E13 |
| 981-C08 | 292 | no stack frame; | 满足 | E13 |
| 981-C09 | 293 | ABI conformance tests; | 满足 | E13 |
| 981-C10 | 294 | ASan/UBSan-compatible host tests where applicable. | 满足 | E13 |
| 981-C11 | 297 | same semantic API; | 满足 | E13 |
| 981-C12 | 298 | target-specific register mapping only; | 满足 | E13 |
| 981-C13 | 299 | parity tests. | 满足 | E13 |
| 981-C14 | 302 | receiver/ObjectRef opt-in path; | 满足 | E13 |
| 981-C15 | 303 | static-call opt-in path; | 满足 | E13 |
| 981-C16 | 304 | Plugin opt-in benchmark; | 满足 | E14 |
| 981-C17 | 305 | prove no semantic/lifetime changes. | 满足 | E13 |
| 981-C18 | 308 | add only after real consumer/benchmark evidence. | 条件范围 | E13 |
| 981-C19 | 312 | native thunking is optional and does not change canonical CMeta semantics; | 满足 | E13 |
| 981-C20 | 313 | no arbitrary runtime ABI reconstruction exists; | 满足 | E13 |
| 981-C21 | 314 | no libffi-style generic invocation exists; | 满足 | E13 |
| 981-C22 | 315 | receiver/context binding can lower to a small exact thunk when profitable; | 满足 | E13 |
| 981-C23 | 316 | Win64 ABI is independently tested; | 满足 | E13 |
| 981-C24 | 317 | leaf/tail-jump is the default production subset; | 满足 | E13 |
| 981-C25 | 318 | any non-leaf Win64 path owns correct unwind metadata/registration; | 条件范围 | E13 |
| 981-C26 | 319 | executable memory has bounded explicit ownership and safe permission transitions; | 满足 | E13 |
| 981-C27 | 320 | thunks never retain hidden Plugin/provider/module lifetime; | 满足 | E13 |
| 981-C28 | 321 | #980 remains authoritative for RAII/cleanup semantics; | 满足 | E5 / E13 |
| 981-C29 | 322 | #977 exact generated C adapters remain the reference Plugin representation; | 满足 | E7 / E13 |
| 981-C30 | 323 | benchmarks demonstrate a concrete benefit before broader rollout. | 满足 | E14 |

## 验收关闭所需的剩余工作

1. G1：补齐当前提交的 Clang C/C++ 正式运行矩阵。
2. G2：在 macOS 运行现有 linker / lease / installed 用例，保留对应日志。
3. G3：使现有 CI 选择覆盖本次确认遗漏的正式测试；本地 25/25 不能替代持续回归。
4. G4：集中剩余编译器能力/对齐选择，保持公开布局与检查强度，并复验。
5. G5：同步 Plugin / Reflection ABI 文档。

**推论：**现有证据未暴露新的 HIGH 行为缺陷；这一结论仅覆盖本文所列实现、调用点与用例，
不等于对所有参数、并发调度、编译器版本或任意第三方 provider 的完整证明。
本次没有验证普通 C 的静态 borrow checker、任意运行时 ABI、AArch64 native 或 non-leaf thunk；
这些也不应被列为已经实现的能力。
