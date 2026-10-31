# Salts Plugin

`Salts::Plugin` is the Salts-owned dynamic-module publication and lifecycle
runtime built directly on canonical CMeta semantics.

Plugin and CFlow are independent capabilities. Plugin owns module publication,
loading, registry, leases and quiescent unload; it does not own graph/execution
semantics and does not link CFlow.

## Windows 显式路径的依赖搜索

带目录分隔符的 UTF-8 插件路径先转换为绝对路径，再使用
`LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS`。
依赖可来自插件目录、宿主程序目录、System32 和宿主显式注册的目录；
不会把普通当前目录或 PATH 条目追加进这次搜索。裸文件名继续使用既有平台名称搜索语义，
需要限定搜索范围的宿主应传入显式路径。Windows 自身已加载模块、KnownDLLs、
API set 和重定向规则仍适用，这些标志不提供模块隔离或签名验证。
语义依据：[Microsoft LoadLibraryExW](https://learn.microsoft.com/en-us/windows/win32/api/libloaderapi/nf-libloaderapi-loadlibraryexw)。

**HIGH／兼容性**：原先依赖当前目录或 PATH 才能找到的 DLL，需要部署到受信目录，
或由宿主在启动阶段显式注册依赖目录。插件目录中的私有依赖继续受到支持。
平台不支持这些标志或依赖不可解析时返回 `CMETA_PLUGIN_LOAD_FAILED`，不扩大搜索范围重试。
Plugin/CMeta ABI、registry 状态归属、lease 和停止／卸载顺序保持不变。
相对主插件路径仍以调用时的当前目录解析；宿主仍负责验证主插件路径和目录来源。

正式测试 `cmeta_plugin_windows_search_test` 放在 `plugin/tests`，由 `BUILD_TESTS`
和 Windows 平台条件控制，不依赖可选 CMetaNative。真实测试 DLL 覆盖同目录依赖、
绝对／相对主插件路径，以及 `AddDllDirectory` 显式注册的独立依赖目录。
测试通过 canonical Function ABI 验证后调用依赖函数，并按 lease → stop → quiescent → unload
清理；注册目录仅在该测试进程存活期间使用，卸载后移除。

**验证状态（2026-10-11）**：[草稿 PR #1119](https://github.com/qigao/salts/pull/1119) 的
源码提交 `e1a26b89ffaccd4cfad7dd8680020ffc35370f3b` 已通过
[Salts CI 38131873603](https://github.com/qigao/salts/actions/runs/38131873603)。
Windows 使用 `win-release-ci` 完整构建；本测试 3 个用例、47 条断言全部通过，无跳过或过滤。
Windows native/plugin CTest 38/38、execution CTest 97/97 通过，包含既有 ServiceHost
DSO、generation、scope、publication、runtime failure 和 concurrency 回归；这两组有重叠，不相加为独立测试数。
其余七个平台构建任务也通过；Android/iOS 结果仅代表交叉构建。
未本地构建 SDK、未合并或发布。这三项正向用例不构成完整搜索边界验收；
宿主／系统目录、目录边界拒绝及搜索策略加载失败清理仍需正式 Windows qualification。
Retro 仍消费已发布 rc.11，并保留其 Windows 迁移门槛，等待包含修复的发布包和正式验收证据。
回滚仅还原源码补丁及对应测试；不得据此移除消费方的搜索策略要求。

## 显式生成 Plugin 声明

Plugin 的公开宏和枚举常量统一使用 `CMETA_PLUGIN_*` 前缀，类型和函数使用
`cmeta_plugin_*` 前缀。宿主和 provider 必须同步迁移源码并重新编译；动态库入口
为 `cmeta_plugin_query`，不查找旧入口。manifest 布局与 ABI epoch 5 保持不变。

`<salts/plugin_decl.h>` 复用 CMeta 精确 Function 声明和 Interface carrier。
普通反射声明不会自动发布任何 export。完整 C provider 示例：

```c
#include <salts/plugin_decl.h>

FunctionInvokeDecl(value, int, twice, (int, input, CMETA_PARAM_IN));
int twice(int input) { return input * 2; }

#define MATH_EXPORTS(X) \
    X(function, twice, "math.twice", "math", 1u, 1u)

CMETA_PLUGIN_DECLARE(math_plugin, "example.math", (1u,0u,0u),
    MATH_EXPORTS, CMETA_PLUGIN_PASSIVE());
```

以普通 DSO target 编译该源文件并链接 `Salts::PluginABI`。安装 SDK 与 in-tree
使用相同头文件和声明。每个 DSO 在一个 TU 中声明一次 query。

`CMETA_PLUGIN_DECLARE(name,id,version,exports,lifecycle)` 的参数为：本地符号前缀、
Plugin ID、`(major,minor,patch)` 三元组、非空 X-list、生命周期 tuple。
函数行是 `X(function,native_symbol,export_id,contract_id,contract_version,capabilities)`；
Interface 行是 `X(interface,(InterfaceType,&instance),export_id,contract_id,contract_version,capabilities)`。
Interface instance 必须是静态存储期的可变精确 `{self,vtable}` carrier。
同一个 native function 每个列表出现一次；不同导出别名使用显式 wrapper。
ID、版本、capabilities 均为显式声明，不从 Reflection 推断。

无导出的 provider 使用 `CMETA_PLUGIN_DECLARE_EMPTY(name,id,version,lifecycle)`。
它复用同一个 manifest/query 生成器，生成 `exports = NULL`、`export_count = 0`，
不会创建零长数组或占位 export；passive 和 managed provider 都适用。例如独立源文件：

```c
#include <salts/plugin_decl.h>
CMETA_PLUGIN_DECLARE_EMPTY(empty_plugin,"example.empty",(1u,0u,0u),CMETA_PLUGIN_PASSIVE());
```

`CMETA_PLUGIN_PASSIVE()` 将 self 与四个回调设为 NULL。
Managed provider 使用 `CMETA_PLUGIN_LIFECYCLE(&state,start,request_stop,is_quiescent,destroy)`，
四个回调填写具有 `CMETA_PLUGIN_CALL` 和现有精确签名的函数标识符。
生成层检查非零 uint32 contract version、三段 uint32 semantic version（各段允许零）、
非负 uint64 capabilities、Interface carrier 类型、callback 类型与 export 上限；
版本与 capabilities 必须是整数常量表达式，越界、负值和浮点输入均拒绝，避免静默截断。
缺字段、错误行形状或缺失精确 thunk 也在编译期失败。
ID 格式、capability admission、重复 ID、state 指针与其余外来数据约束仍由
`cmeta_plugin_manifest_validate()` 校验，不能绕过。

函数入口拒绝非 NULL context，使用无 capture 的精确 native thunk；
其存储、错误与所有权契约见 [CMeta 声明说明](../cmeta/LANGUAGE_REFERENCE.md#有限宏与精确调用声明)。
生成的 `cmeta_plugin_query()` 仅在 ABI 完全相等时返回 const manifest，否则返回 NULL。
ABI epoch、struct_size、lease/generation、STARTED admission、停止、quiescence 与 unload 规则不变。

C 使用静态初始化的 const export 表。C++17 使用显式 CMeta `AsAbi` 声明；
由于 C++17 无法以 designated initializer 选择 union 的非首成员，函数 export 的 const 表项
通过 TU-local 值构造辅助函数初始化。该初始化不注册对象，不改写 manifest，不持有 lease；
不得在其他 TU 的静态初始化期间提前消费此表。
没有 constructor 驱动的注册表、全局可变发现列表或 linker 失败回退。
跨 TU 聚合通过可选 `<salts/plugin_linker.h>` 显式启用；lease 作用域入口见下文。
普通声明继续以生成的静态数组为参考表示。

正式验证包含 `cmeta_plugin_declaration_test`、C++ 同源用例、空导出的 passive/managed 用例、
C/C++ 编译失败用例、已有 loader/lifecycle 回归，以及 `cmeta/tests/installed`
内的已安装 SDK 声明测试。

本地复验使用仓库 preset。在 VS x64 开发环境中设置 README 要求的两个 Windows triplet
环境变量后执行以下命令，使用 MSVC：

```powershell
cmake --preset win-dev-user -DBUILD_TESTS=ON -DBUILD_BENCHMARKS=OFF -DBUILD_EXAMPLES=OFF
cmake --build --preset win-dev-user
ctest --preset win-dev-user -R "(^(cmeta_(pp|const|flags|layout|interface)_|cmeta_plugin_)|^cmeta_(header_cpp|function_(admission|reflection|header)|tinymock_(interface|function_auto_mock|cflow_clock_interface))_test$|^cflow_interface_reflection_test$)" --output-on-failure
```

安装验证先使用 `install-win-release-user` 构建 preset，再令 `CMETA_PACKAGE_ROOT` 指向
该 profile 的安装前缀，在 `cmeta/tests/installed` 中依次执行 configure、build、test
的 `installed-win-release` preset。负例属于 CTest 的正式编译失败测试，预期诊断出现才算通过。

## 跨 TU linker 聚合（#977）

`<salts/plugin_linker.h>` 提供两个声明宏：

- `CMETA_PLUGIN_EXPORT_FRAGMENT(name,exports)`：在一个 TU 内将非空 X-list 发布为分片。
  `name` 是 DSO 内唯一的外部 C 符号，`exports` 使用与普通声明完全相同的 Function/Interface 行。
- `CMETA_PLUGIN_DECLARE_LINKER(name,id,version,count,lifecycle)`：每个 DSO 恰好在一个 TU 声明根。
  除 `count` 外，参数与 `CMETA_PLUGIN_DECLARE` 相同；`count` 是预期导出总数的整数常量，
  范围为 `1..CMETA_PLUGIN_MAX_EXPORTS`。空 provider 使用 `CMETA_PLUGIN_DECLARE_EMPTY`。

分片必须作为对象文件直接参与链接，或由构建系统显式抽取所需 archive member；
section 的保留标记不负责从静态库抽取未引用的成员。根声明保存的预期数是完整性约束，
query 在返回 manifest 前检查实际区间字节数。ABI 不匹配、缺少分片、计数错误或布局填充
均返回 NULL，loader 报 `CMETA_PLUGIN_QUERY_REJECTED`；若平台缺少 section 边界，链接直接失败。
运行时仍验证每个 export 的 struct_size、ID、版本、capabilities 和 canonical descriptor。
数量检查不能证明集合身份；同数量但错误的分片仍需正常 contract admission 检出。

每个 DSO 拥有一个独立发现集合，静态根 manifest 的 exports/count 从不在 query 中改写。
导出按 ID 查找；linker 后端的枚举次序由链接器决定，调用者不能依赖跨 TU 顺序。
若顺序本身属于消费者契约，继续使用单一显式数组。C++17 分片和数组后端一样，
须等 DSO 初始化完成再查询；没有构造函数驱动的注册、进程全局发现表或隐藏 lease。

`CMETA_PLUGIN_HAS_LINKER_EXPORTS` 表示编译器具有相应 section 原语；
它不替代具体 compiler/linker/优化组合的测试资格。当前后端为：

| 格式 | 保留与边界策略 | 必须通过的验证 |
|---|---|---|
| COFF/MSVC | 排序 section、完整行哨兵、`/INCLUDE`；可写 section 支持 C++17 的值初始化 | C/C++ 多 TU、`/OPT:REF`、Debug ASan、Release |
| ELF | `used` + `retain`，隐藏的 DSO-local 边界符号 | C/C++ 多 TU、`--gc-sections`、DSO 隔离 |
| Mach-O | `used`，`section$start/end` 边界 | C/C++ 多 TU、`-dead_strip`、DSO 隔离 |

编译器探测与 attribute spelling 归 `cmeta/compiler.h`；平台 section 名称及组装归 Plugin。
不支持的编译器不定义 linker 声明宏，调用者必须显式选择数组后端；不自动切换。
本次本地资格覆盖 Windows x64/MSVC，其余平台须由对应平台运行同一组正式测试确认。
平台机制依据：[MSVC allocate](https://learn.microsoft.com/en-us/cpp/cpp/allocate?view=msvc-170)、
[/INCLUDE](https://learn.microsoft.com/en-us/cpp/build/reference/include-force-symbol-references?view=msvc-170)、
[ELF section GC](https://lld.llvm.org/ELF/start-stop-gc.html)、
[Apple linker](https://github.com/apple-oss-distributions/ld64/blob/main/doc/man/man1/ld-classic.1)。

设计取舍：显式数组最可移植，但要求集中列出全部导出；linker 分片允许声明靠近实现，
代价是平台资格、链接顺序约束和一个预期总数。选择不可变 root + 固定区间，避免 query
懒初始化的同步与可变状态，也不引入指针表到现有连续 ABI 的运行时复制。
query 的时间/额外空间都是 O(1)，既有 manifest validation 的复杂度不变。
容量仍由 `CMETA_PLUGIN_MAX_EXPORTS` 限定；固定 8 字节 section 对齐只接纳满足布局断言的 ABI。
没有新依赖或 ABI 变化。迁移仅替换发布声明；回滚时将 X-list 集中到
`CMETA_PLUGIN_DECLARE`，不需要数据迁移、registry 或 loader 改动。

## Lease 作用域（#977）

宿主包含 `<salts/plugin_scope.h>` 并链接 `Salts::Plugin`。
C 使用 `cmeta_plugin_with_lease(registry,ref,body,context)`：
`registry` 和 `ref` 指定已有 provider，`body` 接收借用的 const manifest 与原样 context。
空 body 返回 `CMETA_PLUGIN_INVALID_ARGUMENT`；acquire 失败原样返回状态且不调用 body；
成功时调用 body 一次，释放后返回 body 状态。body 中提前 return 或返回错误均释放，
但不能通过 `longjmp` 离开作用域，也不能在 body 返回后继续持有借用对象。

C++17 使用 `salts::plugin_lease_scope`。默认对象为空；
`acquire(registry,ref)` 返回既有 acquire 状态，持有 lease 时再次 acquire 返回
`CMETA_PLUGIN_INVALID_STATE`。`manifest()` 返回借用指针，空对象返回 nullptr；
显式 bool 判断是否拥有 lease。类不可复制，可以 noexcept 移动构造，移动后源对象为空；
不提供移动赋值，避免覆盖活跃 lease 时隐藏释放失败。
`close()` 成功后清空所有权，重复 close 成功；失败保留所有权及 manifest，允许显式处理后重试。
析构在正常返回、提前返回与异常展开时调用既有 `cmeta_plugin_registry_release`。

核心状态仍由 registry 拥有：scope 只独占一个 lease token 并借用 registry 的固定地址，
不会转移 registry、增加隐藏引用或将 lease 放入 descriptor。registry 存储必须比 scope 长寿，
不得复制、移动、重置其 handle；registry 运行时继续负责同步，scope 本身要求单 owner 访问。
移动 scope 不改变 active_leases。依赖 provider 的值、view 和回调对象必须在 scope 之后声明，
在 close/析构之前销毁；scope 无法通过 C/C++ 类型系统阻止裸指针逃逸。
停止仍关闭新 acquire；已有 lease 阻止 unload，最终释放后才可 quiesce/unload。

两种入口均无额外分配、容量和等待路径，acquire 的容量/状态错误不变。
自动清理无法向已退出的调用者报告释放错误；在有效 registry 和私有 authoritative lease
契约下释放应成功。违反契约导致释放失败时，C 入口 abort，C++ 析构 terminate；
需要检查状态的 C++ 调用者先显式 close。该策略不静默丢弃 lease，也不自动重试。
消费者可随时退回显式 acquire/release，无公开 C ABI 或生命周期迁移。

`cmeta_plugin_linker_test` 与 `cmeta_plugin_scope_cpp_test` 同时注册在 in-tree 和
installed SDK 测试图，覆盖分片缺失、错误计数、ABI 拒绝、并存 DSO 隔离、精确调用、
提前返回/异常、移动、停止 admission、显式释放失败和 unload 门禁。

## Targets

```text
Salts::PluginABI
    -> <salts/plugin.h>
    -> Salts::CMeta

Salts::Plugin
    -> Salts::PluginABI
    -> Salts::Core
    -> Salts::Platform
    -> platform dynamic loader
```

`Salts::PluginABI` is publication-only and carries no loader/registry runtime.
Plugin providers can therefore publish CMeta Function/Interface exports without
linking the host runtime.

## One ABI

Plugin supports exactly one current ABI. There is no ABI negotiation, fallback,
readable-prefix compatibility, or retry of older layouts.

Moving package/repository ownership does not change the binary ABI by itself.
A Plugin ABI bump is required when the manifest/export/query binary contract
changes **or when a transitive CMeta Reflection layout published by an export
changes incompatibly**. A host must reject the older Plugin epoch before it
dereferences reflected Function/Interface descriptors.

## Semantic boundary

Plugin publishes canonical CMeta capabilities:

```text
operation/service capability -> CMeta Function + FunctionAbi + exact adapter
stateful provider capability -> CMeta Interface + {self,vtable}
```

It does not define a second function, interface, callable or execution model.

## Lifecycle

```text
load
  -> validate current manifest ABI
  -> LOADED
  -> start
  -> STARTED
  -> acquire/release leases
  -> request_stop
  -> STOPPING
  -> plugin quiescent + leases == 0
  -> QUIESCENT
  -> unload
```

Everything borrowed from a Plugin DSO remains valid only while a live lease
keeps the module loaded.

That rule applies to the complete reachable reflection graph, not only the
top-level export pointer:

```text
Function / Interface export
    -> FunctionDesc / InterfaceDesc
    -> TypeDesc / TypeIdentity
    -> GenericDesc
    -> DataDesc / lifecycle providers when referenced
```

Descriptor copies and semantic-equality matches do not retain the module.
Cross-DSO type/generic comparison uses CMeta semantic identity; descriptor
addresses are never module-independent identity. Consumers must drop every
borrowed descriptor/view and destroy values whose lifecycle callbacks live in
the provider before releasing the final Plugin lease.

Standalone TYPE/DATA/GENERIC exports are not part of the current Plugin ABI.
Reachable Function/Interface descriptor graphs are the canonical publication
path unless a real consumer proves independent type discovery is required.

## CFlow composition

There is no Plugin -> CFlow or CFlow -> Plugin dependency.

An application that projects a Plugin Function into CFlow uses the ordinary
CMeta -> CFlow projection and keeps the Plugin lease alive for the whole
borrowed-code/reflection lifetime. Any helper for this choreography is
consumer/helper-level code, not a PluginCFlow subsystem.
