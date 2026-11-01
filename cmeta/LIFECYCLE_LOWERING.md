# 静态生命周期与动态 admission（#980）

## 背景与选择

事实：CSTL 类型声明已经同时生成 native storage、concrete construct ops 和 DataDesc，
但原 scope 每次进入仍经 `cmeta_lifecycle_bind()` 检查完整描述符和存储契约。
本阶段将声明期已知的操作入口与外来描述符的 admission 分开，不改变 lifecycle authority。

`cmeta_scope` 使用类型声明提供的 `Type_cmeta_lifecycle(const Type *)`，直接借用
canonical construct ops；`cmeta_scope_checked` 明确通过 `Type_cmeta_data()` 和
`cmeta_lifecycle_bind()` 完整验证。没有按类型名称、callback 地址或验证结果自动选路。

保留所有 scope 的运行时验证不能完成静态入口目标；自动探测 accessor 并回退会隐藏
声明缺失；额外维护另一套生命周期回调会产生两个事实源。因此使用显式静态声明和
checked 入口，共享初始化、回滚、清理展开。

自动分类采用编译器常量条件和有限词法嵌套。继续要求调用点选择分类会重复事实；另建
分类 registry 会引入第二份 authority；在每项保存 live/ops 则保留了可消除的状态。
嵌套控制流以程序位置表示成功前缀，代价是最多 16 层词法结构和对应 C++ unwind 路径。
不据此宣称未经 benchmark 的吞吐或代码尺寸收益。

## 声明与证明边界

`CMETA_DEFINE_STATIC_LIFECYCLE(Type, ops, facts)` 只用于拥有 native Type 和 canonical ops
定义的本地声明/generator。它生成带 native type witness 的 accessor，校验 ops 的
精确 const 类型；scope 在编译期检查 accessor 的完整函数类型。ops 必须具有静态存储期，
与同一声明的 DataDesc.construct_ops 指向同一对象。CSTL 从同一生成器输出这个关联。
facts 必须来自初始化 ops.flags 的同一声明常量，不能在调用点重述。两个参数的形式发布
未知分类（零），保守执行 fallible lowering；不从 ops 地址或 callback 身份猜测分类。
缺少整个静态声明时编译失败，不回退到运行时查找。

这些证明覆盖 native 类型和声明关联，不推断回调的行为，也不证明任意手写描述符的
成员值正确。手写声明的所有者必须保证 ops 的 ABI、storage identity/layout、init/restore
契约。外来、Plugin 或运行时选择的描述符不得通过该宏冒充静态声明；使用 checked admission。

不为缺少 concrete construct ops 的 Struct 等类型猜测生命周期，也不按 C 类型拼写
推断 trivial/no-fail。分类由声明所有者显式提供，具体边界见下文。

## 所有权、状态与错误

- 数据单元为一个 native value；scope 独占自动存储，资源总数仍为 1–16。payload 容量由
  既有 provider 契约控制；不增加分配、队列、缓存、registry、线程或 module lease。
- construct ops 是唯一初始化/清理 authority。不可变元数据可共享，value 默认单线程、
  不可并发修改。provider/module 必须活到最后一次清理；scope 不延长其生命期。
- 每项按声明顺序初始化。失败项立即 restore 一次，跳过 body，先前成功项按 LIFO restore；
  未开始的项不清理。body 返回的状态继续传播，清理不覆盖状态。
- move 继续执行既有语义，源恢复 semantic zero；源在 scope 结束时仍可安全清理。
- C++ 的 managed/fallible `cmeta_scope` 和 `cmeta_scope_checked` 在 body 抛异常时，
  按相同 LIFO 路径清理已初始化资源，再原样重抛；不转换为 status。嵌套 scope 先内后外，
  moved-from 源仍按 semantic zero 清理。C 的结构化退出和 ABI 不变（#982）。
- body 只借用对象到返回为止，不允许保存悬空指针、挂起借用或跨函数 goto。
- checked admission 失败不调用 init/restore，保留明确的 INVALID_ARGUMENT、TRAIT_MISSING
  或 TYPE_MISMATCH；静态声明缺失或 native accessor 类型不符在编译期拒绝。

## 兼容性、验证与回滚

MED：手写类型只提供 `Type_cmeta_data()` 时，原 scope 调用须迁移到 `cmeta_scope_checked`；
本地声明所有者也可在保证上述契约后发布静态 accessor。CSTL 调用语法不变。
DataDesc、既有 construct ops 前缀、回调和 native 容器算法保持不变。
construct ops 末尾追加 size-versioned flags；旧前缀仍可 admission，分类为未知。
回滚只需一起恢复 scope 入口和消费者，持久化数据无迁移。
MED：此前手写 `Type_cmeta_lifecycle_flags` 再调用两个参数静态声明的 provider，应把
同一 facts 常量传给三个参数的声明入口，并移除重复枚举定义；生成器现在统一发布该常量。
原本未声明分类的两个参数调用保持保守路径。应用的普通两字段 scope 调用无需改动。

## Admitted binding 的信任契约（#980 P4）

这里的 admitted/validated binding 是经对应 `*_admit()` / `*_bind()` 验证的借用记录。
公开 C struct 不构成不可伪造的 security capability。调用方只可使用成功 admission 后
仍有效、未修改的记录，禁止伪造或修改后依赖 admitted fast path。
描述符/provider 仍是类型、语义和执行 authority；binding 或其地址不成为类型身份。
binding 不暗中持有对象、provider、回调或模块。外层 Plugin/provider lease 必须覆盖
全部调用及依赖清理；只有 owning subsystem 显式提供的 retain/release 才延长生命期。
不加入无法改变 C 信任模型的 cookie、magic 或伪 opaque 字段。

## 分类与清理义务（#976: 18、20–25）

`cmeta_lifecycle_admit(data, sizeof(Type), alignof(Type), &binding)` 完整验证
storage identity、布局、ABI 和回调；失败清空 binding。成功后的 `cmeta_lifecycle_init`、
`cmeta_lifecycle_move`、`cmeta_lifecycle_restore` 只做参数检查，不重新遍历描述符图。
init 失败自动 restore 部分对象一次，调用方不能再按成功对象登记清理；move 要求不同地址
且目标为已初始化 semantic zero，源在成功后保持可清理的 semantic zero。
binding 不拥有值、元数据或 provider；调用方不得伪造、修改或超过外层 Plugin lease 的寿命。

`cmeta_invokable_invoke_admitted()` 同样只接受成功 bind 后未修改的 borrowed binding。
它仍检查输出/参数 storage，保留 CALLBACK_ERROR；原 `cmeta_invokable_invoke()` 继续
完整检查手工组装/外来对象。原始 CFlow、Plugin、ObjectRef admission 没有因新增快入口而
被省略；它们仍是各自子系统的信任边界。

ObjectRef 的 `cmeta_object_field_bind(ref, name, &binding)` 验证对象/provider、字段及固定
布局后发布借用 binding；`read_admitted` 每次调用动态 read provider，不缓存会失效的
动态字段指针；`assign_admitted` 只接受与 `binding.field->value` 相同的 native storage，
只调用已有 assignment authority，缺失时返回 TRAIT_MISSING。固定布局地址由 admission
计算。binding 不能超过 object/provider/module 生命周期，旧按名字读写入口仍完整验证。

`CMETA_DEFINE_LIFECYCLE(Type, descriptor, init, restore, move, flags)` 从同一声明生成
精确 native callback 适配、canonical ops 和 `Type_cmeta_lifecycle_flags` 常量。
四个正向事实为 INIT_NOFAIL、TRIVIAL_ZERO、TRIVIAL_CLEANUP、MOVABLE；零表示未声明。
TRIVIAL_ZERO 表示 native `{0}` 即 semantic zero，必须同时声明 INIT_NOFAIL。
TRIVIAL_CLEANUP 表示不需要资源释放，不能由 restore 回调地址推断。回调行为承诺仍由
声明所有者负责；手工 descriptor 的 identity/布局继续在 admission 验证。
CSTL 生成器声明 nofail 初始化和 movable，payload 清理由原容器 destroy 负责。

`CMETA_DEFINE_TRIVIAL_LIFECYCLE(Type, descriptor)` 生成 native zero/reset/move 及上述
四个事实；只能用于不持有资源的平凡 native 值，C++ 还要求 `std::is_trivial`。
普通 `cmeta_scope(status, cmeta_autos((Type, value), ...), cmeta_body(expr))`
从 `Type_cmeta_lifecycle_flags` 自动选择，不要求应用再写 trivial/managed/nofail：

| canonical 分类 | 静态 lowering |
|---|---|
| TRIVIAL_ZERO + TRIVIAL_CLEANUP | native `{0}` 自动存储，无 ops/live 状态，无 init/restore 调用 |
| managed + INIT_NOFAIL | 直接 canonical ops，无逐值 partial-init 状态；违反 nofail 承诺立即终止 |
| managed + fallible / 未声明分类 | 直接 canonical ops，以现有 status 与嵌套控制流记录成功前缀；失败项及前缀逆序 restore |

生成器用有限 forward/reverse replay 打开和关闭词法块，不保存逐值 live 布尔量，也不在
静态路径缓存 ops 指针。所有 native 变量仍声明在同一 scope，重复变量名继续编译失败。
回滚不需要第二份状态数组、counter 或堆记录。C++ 同样按嵌套块逆序处理异常。
checked 路径必须先验证 descriptor/layout/flags；成功后才用 runtime facts 自动选路，
每项只保留已验证 ops 的借用指针，没有额外 live 状态。无效 metadata 不调用 lifecycle。
同一 native 拼写可有不同 provider 分类，不根据拼写、地址或命名推断。

显式 `trivial` 行和 `cmeta_scope_nofail` 仅保留为分类断言及资格测试入口，与普通 scope
共享 lowering；它们不再是应用获得简化路径的前提。显式断言不符仍失败，不能静默降级。
CSTL 的 semantic-zero nofail 初始化由容器声明发布一次，普通 scope 自动采用；body 中
payload 分配仍可能失败，返回的错误不影响清理。

INIT_NOFAIL 是本地 provider 的行为承诺；init 返回失败说明该承诺被破坏，立即终止。
初始化回调通过 status 报告可恢复失败，不得抛 C++ 异常；异常属于 provider 契约违例，
同样立即终止，不转入另一种 lowering。C++ body 抛异常时先逆序 restore 再传播；
restore 按既有生命周期契约不得失败或抛异常。body 同样必须使用函数表达式，不能用
跨作用域跳转或 longjmp 绕过清理。嵌套、body 错误、C++ 异常和已移动资源分别由
`cmeta_cleanup_test`、`cmeta_cleanup_cpp_test` 与 `cstl_header_typed_test` 验证。

清理记录 `cmeta_cleanup` 仅拥有“一次调用释放 authority 的义务”，不定义新的 Reflection
destroy trait。其存储由调用方提供，容量是数组长度，单 owner、无分配、无增长。arm 对已
登记项返回 BUSY；run 在回调前清空记录，保证重入和重复 discharge 不二次释放；transfer
只能转移到空记录。资源/authority/记录地址在释放完成前保持稳定，不得复制 armed 记录。
`cmeta_with_cleanups(records, count, body, context)` 保留 body 状态并逆序 discharge；
C++ `cmeta::cleanup_scope` 在异常展开时执行同一义务。耗时 O(count)，额外空间 O(1)。

- Data adapter 使用 admitted binding 的 restore_zero，binding 和存储必须活到清理结束。
- ObjectRef adapter 调用 `cmeta_object_release()`；BORROWED 清空视图，SHARED release，
  OWNED destroy。`cmeta::object_scope` 的 take/move 不 retain，拒绝 busy 目标且保持双方
  原状；close 幂等，detach 显式转出释放义务。metadata/provider/module 仍由外层 owner 保活。
- Plugin adapter 位于 `cmeta/plugin_scope.h`，调用原 registry release。把 lease 放在依赖
  记录之前，逆序清理时先结束借用；释放失败代表既有 lease 契约被破坏，按原 RAII 规则终止。

### 自动清理的 no-fail 契约（#983）

自动 discharge 只释放本地所有权，必须没有可恢复失败，也不得抛出 C++ 异常。
`cmeta_cleanup_fn` 保持 `void` 返回值；签名只是必要条件，不能证明函数的行为。
适配器作者必须引用 canonical authority 的 no-fail 承诺，并保证资源、provider、binding
和外层 lease 在 discharge 前有效。C 结构化清理与 C++ guard 遵循同一契约。

flush、commit、协议 close/ack、数据库 settlement 等可恢复失败必须通过显式操作返回
status，由调用方决定传播、重试或放弃；随后自动清理仅释放剩余本地所有权。
不得把该操作强转为 cleanup callback、忽略其 status、隐藏重试或自动补偿。
CMeta 不定义文件、socket、数据库或 allocator 的领域语义。

检测到 no-fail 前提被破坏时立即终止进程，不把错误转换成成功或继续下一种清理方案。
Plugin 的 C 适配器调用 `abort()`，C++ lease guard 调用 `std::terminate()`；二者均不允许
恢复执行。显式 `plugin_lease_scope::close()` 仍返回原错误并保留 lease，允许调用方修正
前置条件后显式重试。Data restore 与 ObjectRef release/destroy 的 no-fail 承诺由原
provider 负责；不能包装一个可恢复失败的 authority 来伪造这一承诺。

正式验证：`cmeta_cleanup_test` / `cmeta_cleanup_cpp_test` 覆盖显式 finalize 失败后保留
资源、自动释放一次及拒绝 fallible callback 类型；`cmeta_object_scope_cpp_test` 覆盖
ObjectRef；Plugin scope 测试覆盖 close 失败保留所有权和自动清理的终止边界。

`cmeta_result_cleanup_classify` 使用 Function result flags 区分 VALUE、BORROW、RELEASE、
DESTROY，UNKNOWN 返回 TRAIT_MISSING，冲突 flags 返回 INVALID_ARGUMENT。
`cmeta_cleanup_object_result` 只有在 canonical flags 与 ObjectRef lifetime 一致时登记
OWNED/SHARED 义务；BORROWED 不登记。VALUE 的 native 存储生命周期由 Data adapter 负责。
consume/move 成功后由生成器 transfer/disarm 源义务；失败不得提前移除源义务。
这对应 [ownership calculus mapping](../formal/cmeta_cflow_calculus/OWNERSHIP_COMPILER_MAPPING.md)，
不增加 descriptor 内的 LIVE/MOVED/CFG 状态，也不声称提供普通 C 的跨分支 borrow checker。

MED：新声明会在编译期拒绝不匹配的 callback、packed Struct 布局、Function/Interface
返回 carrier 和冲突 ownership flags；手写外来描述符继续返回 runtime 错误。迁移需修正
声明事实，不能绕过诊断。回滚时一并恢复生成器、头文件和调用方，无持久化格式变化。
验证覆盖 C11/C++17、部分初始化回滚、move、旧 ABI 前缀、逆序/重入/异常清理、Plugin
依赖顺序，以及每类声明错误的 compile-fail CTest；SDK 消费者复用同一批行为测试。

managed setup/cleanup 为 O(N) 次回调、O(N) 有界词法嵌套。静态路径除 native value 和
调用方 status 外无逐值运行时 bookkeeping；checked 路径另加原描述符校验和借用 ops 指针。
本阶段验证移除重复 admission，不声明未经测量的吞吐或延迟收益。
正式测试覆盖静态/checked 同一 ops 身份及行为、失败回滚、拒绝错误 accessor、动态坏元数据、
CSTL 资源释放、C++ 头文件和 installed SDK。

本次 P2/P4 验收复用正式 C/C++ cleanup/scope、CSTL 和 installed SDK 测试，新增全部
三类混合、16 项边界、trivial 回调消除、不同位置 partial failure、body status、异常展开、
runtime-only metadata 自动分类及坏 metadata 拒绝。公开 binding 布局和函数 ABI 不变。
回滚时同时恢复 lifecycle 声明生成器、scope lowering 和 CSTL facts 发布，避免声明与消费
不一致；不改变持久化数据，也不添加 ABI 兼容路径。

## 新增 merge gate 对照（2026-10-07）

| #980 条目 | 实现与验收面 |
|---|---|
| P2：canonical facts 自动选择 | lifecycle 声明统一发布分类；普通两字段 scope 和 CSTL consumer 自动选择；C/C++ 混合分类与未知分类保守测试 |
| P4：validated borrowed binding 信任契约 | lifecycle、ObjectRef field、invokable 公共头明确成功 admission、禁止伪造/修改、外层 lease；不改变 struct 布局或加入 cookie |
| Acceptance：调用点不重复优化分类 | 普通 scope 覆盖 trivial、nofail、fallible；16 项边界、partial rollback、LIFO、move、嵌套、异常及 nofail 违例终止测试 |
| Acceptance：binding 不被描述为不可伪造能力 | 本节及公共头明确 C caller trust；canonical descriptor/provider 是 authority，无隐藏 retain 或第二份身份模型 |

事实：实现提交 `d51b7b13` 的 MSVC 完整构建通过，语义 CTest 190/190、相邻消费者
30/30；远端 GCC 12.2 和 Clang 14.0.6 完整构建通过，语义各 189 通过、1 项 C23 能力
检测跳过，消费者各 29/29。Clang installed SDK 50/50，包含同源 C/C++ scope 回归和
no-fail fatal 测试。普通 Linux configure 的 native/fastpath 均为 OFF。
补充提交 `be37997c` 仅新增保守分类测试及说明；MSVC/GCC/Clang 的 cleanup C/C++
均重新通过 2/2，installed SDK 重新通过 50/50。
日志位于本地或 `root@eu:/root/dev/salts-clang-976-977` 的
`build/issue980-{msvc,gcc,clang}-{semantic,consumers}.log` 和 `build/issue980-sdk-tests.log`。
macOS GCC/AppleClang 本轮结果仍需 CI 验证；此处不据三种编译器实测宣称所有平台已通过。
