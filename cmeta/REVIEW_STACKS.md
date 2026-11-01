# #979 的语义审查与合并边界（#984）

原组合 PR [#979](https://github.com/qigao/salts/pull/979) 保留原历史。
核心 PR [#986](https://github.com/qigao/salts/pull/986) 使用新分支
`review/cmeta-core-984`，通过隔离提交区分 A–D/G、公共命名迁移和 F；它不包含 #981
的 native thunk 实现、构建选项或 installed native 测试。可选 E 使用其上的
[`review/cmeta-native-981` / PR #987](https://github.com/qigao/salts/pull/987)，单独审查、构建与验收。以下表格定义语义依赖；网络、runner
与 benchmark 的成功不能替代 CMeta 语义验收。

| 单元 | 契约与主要文件 | 依赖 | 正式验收面 |
|---|---|---|---|
| A：#976 声明基础 | `include/cmeta/{pp,compiler,function,interface,struct}.h` 及声明生成器；有限 PP、编译器能力、精确类型证明 | 无新 runtime authority | `cmeta_pp_*`、`cmeta_lowering_*`、`cmeta_interface_arity_*`、compile-fail、C/C++ installed SDK |
| B：#980/#982/#983 生命周期 | `include/cmeta/{lifecycle,scope,cleanup,object_scope}.h`；static/checked admission、partial rollback、LIFO、move、exception、no-fail discharge | A；ObjectRef 使用 C 的唯一模型 | `cmeta_scope_*`、`cmeta_cleanup_*`、`cmeta_object_scope_*`、`cmeta_pool_cpp_test`、Plugin scope/fatal tests |
| C：receiver 收敛 | `include/cmeta/{operation,function,invokable,object}.h` 及对应 `src/`；FunctionAbi 是唯一 callable 事实源 | A；与 B 的 ObjectRef 适配一起验收 | `cmeta_operation_test`、`cmeta_invokable_test`、`cstl_semantic_projection_test`、`cflow_function_projection_test`、旧 epoch 拒绝 |
| D：#977 Plugin | `plugin/include/cmeta/plugin*.h`、`plugin/src/`；精确 adapter、manifest/query、跨 TU 聚合与 lease | A、C；scope 适配使用 B | `cmeta_plugin_*`、ELF/Mach-O/COFF linker tests、installed SDK |
| E：#981 可选 native | `cmeta/native/`、`include/cmeta/native*.h`、相关 Platform executable-memory backend | A–D admission 已完成后才可执行 specialization | `cmeta_native_*`、W^X/ABI/失败回滚、独立 benchmark；不作为 B 的前提 |
| F：独立修复 | CNet、NativeIO、runner、benchmark workflow | 各模块自身契约 | 各模块 CTest 与独立 benchmark jobs |
| G：#985 TinyMock 消费者 | [`qigao/salts-utils/tinymock`](https://github.com/qigao/salts-utils/tree/move/tinymock-simd-from-salts/tinymock)（当前唯一源码归属）；history/action/return 只拥有行为状态，复用 CMeta 类型和 ownership | A–D；不依赖 E | `cmeta_tinymock_*`、TinyTest 独立链接、C/C++ installed SDK |

表中 `include/` 和 `src/` 默认相对 `cmeta/`。B/C 共享 ObjectRef 和 Reflection epoch，
必须一起满足公开契约，不能为了机械拆分恢复 `receiver_method`、旧布局或兼容 alias。

## 实际提交隔离

共同基线是 `c9da3ab1`。原 `4be93148` 在新历史中拆为：

| 提交 | 审查范围 | 依赖与验证 |
|---|---|---|
| `9874fe0b` | 机械 `salts_` → `cmeta_`、`salts_coro` → `coro` 及对应文件名迁移 | 不改算法或布局；消费者同步重建 |
| `58ee2440` | F：CMake helper/调用点、Windows 版本资源、模块 export/header 整理、CNet 和正式测试迁移 | 完整 build graph 和模块 CTest；不作为语义实现证据 |
| `b6712237` | B/C/D：Data 合并、类型选择、nofail scope、Plugin 宏迁移及对应回归 | 核心语义门禁和 CSTL/CFlow consumers；无 native 后端 |
| `review/cmeta-native-981` | E：原 `4be93148` 的 native 部分及 `39d255e1` 的消费者集成 | 仅依赖完整 core；native/fastpath 测试、benchmark、installed SDK |

A 的 `0c72f4ab`、`e516171b`、`ec56fd27`、`f8d3e731`、`bb6ae9b6`，
C 的 `f102cfc9`，D 的 `cb7e1efe` / `40ed9743` 保留原提交。后续重要语义提交为：

- B：`db43085b`（#982 exception）、`e1fb7676`（#983 no-fail obligations）、
  `1a5a6a3d` / `015336e8`（#980 canonical lifecycle lowering）。
- G：`8332ae18`（#985 TinyMock admission 与 ownership）。
- 收尾：`425fc04b`（#976 编译器条件归属）、`3f28ce9a`（#977 ABI 文档）。
- F：`4231cbea`（vcpkg）、`f8af504b`（NODELAY）、`cf8e13a0`（Clang/benchmark 报告）、
  `fe889bfc`（CNet benchmark）、`2d837e80` / `0064fce1` / `a71ad507`（构建可移植性）。

这采用 #984 允许的“隔离提交 + 可选后端 stacked PR”，不声称 A–G 都有独立 PR。
B/C 与 D 的迁移仍在同一核心合并单元完成，保留唯一 Function/receiver 模型和明确的
Reflection 4 / Plugin 5 cutover；不存在临时旧布局、别名或 ABI fallback。
审查核心实现可按上表选取语义提交，不必从 F 的网络/runner diff 推断语义行为。

隔离前后的代码核对使用 Git 文件树，而非读取源码 marker 的 CMake 测试：

```sh
git diff --stat 3f28ce9a 9d506b96
git diff --exit-code 9d506b96 review/cmeta-native-981 -- . ':(exclude)cmeta/REVIEW_STACKS.md'
```

第一条仅列出 34 个 native 实现及集成文件；第二条要求重组后的最终源码、测试、
build 和 workflow 与原分支完全一致，只有本审查说明更新。E 对 lifecycle/scope/
ObjectRef/admission 的实现没有 diff；从核心分支不应用 E 即可移除后端。

## CI 门禁

CI 仅保留 Release 构建与运行配置。`<profile> / native` 在 GCC、
Linux Clang、MSVC、macOS GCC、AppleClang 上统一运行原 native 与 plugin 筛选规则
的并集，重叠用例只执行一次；不再设置独立的 plugin 或 semantic 矩阵条目，也不再
注册或选择语义专项 CTest 标签。原专项测试范围不额外并入合并组。
`projection`、`execution` 与 benchmark jobs 保留各自的资格结论；任一分组通过
不能被拿来宣称平台或性能全部通过。

核心栈完全没有 native thunk 源码或 configure 入口。E 自己增加
`CMETA_BUILD_NATIVE_THUNKS`：普通 PR profile 默认关闭；release 准备包含原支持
平台的 native SDK 产物。所有 profile 完整构建各自 configure graph，不通过
`--target` 裁剪 CI 构建，也不直接执行测试程序。

每个合并单元需要对应的实际 compiler/平台输出；源码检查、Windows 的通过或 Linux
Clang 的通过都不能替代 macOS Mach-O qualification。远端 Linux 的单项 NativeIO
失败应单独追踪，不能以 CMeta 测试通过掩盖。

## 隔离资格与历史证据

事实：2026-10-07，核心代码 `3f28ce9a` 与 native 实现 `c50a4996` 的资格如下。
其后本文件更新不改变被验证的源码、测试或构建文件。

| 合并单元与环境 | 完整构建及实际 CTest 结果 |
|---|---|
| Core，Windows MSVC Release，全新 build tree | 全量 405/405；当时的语义专项分组 190/190 |
| Core，root@eu GCC 12.2，fastpath OFF | 完整 build graph 通过；语义 189 通过、1 跳过；消费者 29/29 |
| Core，root@eu Clang 14.0.6，fastpath OFF | 完整 build graph 通过；语义 189 通过、1 跳过；消费者 29/29；installed SDK 50/50 |
| E，Windows MSVC，native/fastpath ON | 完整 build graph 通过；native/fastpath 测试及相关 benchmark 17/17 |
| E，root@eu Clang 14.0.6，native/fastpath ON | 完整 build graph 通过；native/fastpath 测试及相关 benchmark 17/17；installed SDK 54/54 |

Linux 跳过项为 `cmeta_pp_zero_c23_test`，由编译器能力决定。Windows E 使用原分支
`9d506b96` 的相同源码树构建，通过前述 Git diff 核对与 `c50a4996` 一致。
Core 和 E 的 Linux SDK 使用不同的新安装前缀 `stage/issue984-core/clang` 与
`stage/issue984-native/clang`，没有让残留 native SDK 产物参与核心验收。

日志：核心隔离 worktree 的 `build/issue984-isolated-msvc-{configure,build,semantic,regression}.log`；
原 Windows worktree 的 `build/issue984-native-msvc-{configure,build,tests}.log`；
远端隔离 worktree `/root/dev/salts-clang-976-977` 的
`build/issue984-isolated-{gcc,clang}-{configure,build,semantic,consumers}.log`、
`build/issue984-isolated-sdk-tests.log`、`build/issue984-native-clang-{configure,build,tests}.log`
及 `build/issue984-native-sdk-tests.log`。

这不是新分支全平台 CI 全部通过的声明。#986 / #987 的 macOS 和其他矩阵仍以各自
运行结果为准，不得以 Windows/Linux 或原历史提交的通过替代。

当前合并组使用正式 preset 复验，核心不传不存在的 native thunk 开关。
以下筛选对应当前 Unix native/plugin 组，范围与上述历史语义专项分组不同：

```sh
cmake --preset linux-clang-release-ci -DENABLE_TESTS=ON -DBUILD_TESTS=ON -DBUILD_EXAMPLES=OFF -DBUILD_BENCHMARKS=OFF
cmake --build --preset linux-clang-release-ci --parallel 2
ctest --preset linux-clang-release-ci --no-tests=error --output-on-failure --timeout 60 -R '^(platform_fastpath_|cmeta_meta_header_test$|cmeta_trace_|cmeta_fastpath_|cmeta_function_reflection_test$|cmeta_abi_test$|cmeta_coroutine|coro_executor|cmeta_capabilities|cmeta_core_test$|cmeta_(bind|cleanup|data_select|native|lowering|interface_arity|object_scope)_|cmeta_(operation|invokable)_test$|cmeta_data_test$|cmeta_enum_bits_test$|cmeta_header_cpp_test$|cmeta_plugin_|cmeta_(pp|const|flags|layout|container)_|cmeta_fingerprint_|cmeta_manifest_)'
```

历史事实：原 `b9734108` 的 [CI run 37515892751](https://github.com/qigao/salts/actions/runs/37515892751)
中 macOS GCC / AppleClang 的 semantic 和 Plugin jobs 已通过；总运行含取消的 benchmark
jobs，不能称整轮 CI 全部通过。该结果属于原提交，新分支以自己的 CI 结果为准。
此前 Clang 全量测试的 `native_io_uring_batch_test` 两个内部用例失败也仍是独立模块
问题，不能用 CMeta semantic 通过替代其资格。

## 合并与回滚

先合并核心栈，再合并 E。原 #979 作为组合版本对照，不能与替代核心 PR 重复合并。
不强推原共享分支。核心提交中的前缀、Reflection epoch 和 Plugin epoch 迁移要求
host、provider、SDK 消费者同步重建；回滚这些契约必须一起回滚消费者。

E 只有一个方向的依赖：先完成 canonical admission，再使用可选 native specialization。
回滚 E 的提交连同其构建入口、tests 和 SDK 注册即可恢复已验证的核心树，不改变 #980
cleanup authority。F 的独立提交按各模块契约验收，不把其构建或网络结果当作 B/C/D
语义结论。
