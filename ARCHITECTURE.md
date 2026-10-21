# Salts Canonical Architecture

**Status:** canonical repository and ecosystem architecture  
**Updated:** 2026-10-19

This document defines Salts module ownership, public target boundaries,
dependency direction, and the relationship between the Salts foundation and
higher-level ecosystem repositories. Detailed API/ABI/error contracts remain
authoritative in public headers, tests, and focused design specifications.

## Ecosystem ownership boundary

Salts is the **foundation**, not an umbrella repository for every higher-level
feature.

```text
Frameworks / applications
  TurboFlow · RulesForge · Flowie · TurboSCXML · Praktor
                         ↑
Domain infrastructure
  CHTTP · TurboDB · TinyTest
                         ↑
Extension layer
  salts-utils (including DataBind) · salts-net
                         ↑
Salts foundation
  CMeta · CSerde · CFlow · CSTL · Plugin
  NativeIO · Coroutine · Concurrency · CNet · Platform · Core
```

The dependency direction is upward only: higher layers may consume the Salts
foundation, while Salts does not depend on those domain/application projects.

### Salts

Salts owns the common systems semantics:

- CMeta type identity, metadata, traits, interfaces, contracts, and finite generic specialization;
- CFlow Graph/Stream/Reactive/Actor/Machine/Statechart execution;
- CSTL typed containers, algorithms, and ranges;
- CSerde canonical token primitives;\n- Plugin publication/loading/lease/lifecycle primitives;
- NativeIO, Coroutine, Concurrency, CNet, Platform, and Core;

NativeIO execution topology is orthogonal to higher semantic models. Its canonical contract defines Direct and Coroutine as current styles and Sharded/SMP as the planned shared-nothing style; CNet, CFlow Reactive, and CFlow Actor remain optional semantic consumers rather than mandatory layers. See [native-io/ARCHITECTURE.md](native-io/ARCHITECTURE.md).
- TinyTest as the lightweight testing component shipped with the foundation.

### salts-utils

salts-utils owns general higher-level utilities such as parser components,
QueryVM, DataBind schema/compiler/binding, crypto, filesystem/process adapters,
templates, Unicode, media helpers, and related tooling. It consumes an installed Salts SDK and does not make Salts
depend on the utility layer.

### salts-net

salts-net owns protocol/network tooling built on CNet/CMeta, such as
ICE/STUN/TURN, SNMP, LDAP, email protocols, proxying, and related adapters. It is
an extension sibling of salts-utils, not part of the Salts kernel.

### DataBind within salts-utils

DataBind is a SaltsUtils component for schema/compiler/native-dynamic binding.
CMeta remains the semantic source of native type identity; DataBind implements
schema overlay, validation, compiler/codegen, and binding/orchestration within
SaltsUtils. Its source, build, installation, and exports belong to SaltsUtils.

### CHTTP

CHTTP owns HTTP client/server, JSON-RPC, S3, WebSocket-facing service
infrastructure, and OpenAPI-related functionality. It consumes Salts networking
and runtime semantics through its installed package boundary. The historical
`HTTPServices` name is not the canonical repository boundary.

### TurboDB

TurboDB owns storage/database behavior and durable provider semantics. It may
reuse Salts typed/bounded execution primitives, but storage policy remains a
TurboDB concern.

### Framework/application repositories

TurboFlow, RulesForge, Flowie, TurboSCXML, Praktor, and similar projects build
on lower layers without pushing product semantics back into Salts.

In particular:

- TurboSCXML compiles W3C SCXML into **CFlow Statechart** execution;
- TurboFlow composes provider-neutral business graphs and durable workflows;
- RulesForge owns RETE rule execution;
- Flowie owns MQTT broker/client semantics.

> The diagrams below describe Salts's internal semantic/execution layering.
> Ecosystem repository layering is a separate concern from the public/private
> CMake dependency graph. The dependency matrix later in this document remains
> authoritative for concrete Salts targets.

## 1. Layered canonical architecture

```mermaid
flowchart TB

    subgraph BODY[" "]
        direction LR

        subgraph STACK[" "]
            direction TB

            subgraph L4["Layer 4 — User Models (Products / APIs)<br/>面向应用开发者"]
                direction LR
                STREAM["<b>Stream</b><br/>Java-aligned Stream API<br/>stream(&amp;users, &amp;s) → filter → map → to_list"]
                ACTOR["<b>Actor</b><br/>Actor Model · future"]
                STATECHART["<b>Statechart</b><br/>State Machine · future"]
                WORKFLOW["<b>Workflow</b><br/>Workflow / Pipeline · future"]
                REACTIVE["<b>IO / Reactive</b><br/>IO Pipeline / Reactive Style · future"]
                MORE["<b>...</b><br/>More Abstractions"]
            end

            subgraph L3["Layer 3 — CFlow Kernel (Execution Runtime)<br/>可独立使用的运行时产品"]
                direction TB

                subgraph KERNEL["Execution Kernel — Portable Semantic Execution Runtime"]
                    direction LR
                    RUN["<b>Run / Task</b><br/>run / resume<br/>state machine<br/>demand / backpressure<br/>cancel / error"]
                    SOURCE["<b>Source / Sink</b><br/>Source: VALUE / WAIT / DONE / ERROR<br/>Sink: value / error / done"]
                    WAIT["<b>Waitable / Waker</b><br/>WAIT → Waitable<br/>Waker.wake()<br/>OS event integration"]
                    EXEC["<b>Executor</b><br/>post(task)<br/>thread pool<br/>coroutine executor<br/>serial / manual"]
                    CLOCK["<b>Clock / Timer</b><br/>Clock<br/>Timer<br/>VirtualClock"]
                end

                subgraph KSVC["Kernel Services"]
                    direction LR
                    SCHED["Scheduler"]
                    TQ["TimerQueue"]
                    CANCEL["Cancellation"]
                    DEMAND["Demand / Backpressure"]
                    ERROR["Error Propagation"]
                    KTRACE["Metrics / Tracing"]
                end
            end

            subgraph L2["Layer 2 — CFlow Graph IR (Semantic Framework)<br/>框架内部语义模型与执行合同"]
                direction LR
                GRAPH["<b>Graph Model</b><br/>Graph / Subgraph<br/>Node / Edge / Relation"]
                OPS["<b>Operators (Relational)</b><br/>source · map · filter · flat_map · relation<br/>zip · merge · group_by · window · ..."]
                SEM["<b>Semantic Info</b><br/>Type / Trait — from CMeta<br/>Cardinality<br/>Effects / Purity<br/>Properties"]
                PROCESS["<b>Graph Processing</b><br/>Validation<br/>Optimization<br/>Lowering → Execution Plan"]
            end

            subgraph L1["Layer 1 — CMeta (Semantic Toolkit)<br/>语义工具 / DSL / 代码生成基础设施"]
                direction LR
                TYPE["<b>Type System</b><br/>type / struct / union / enum<br/>alias / generic<br/>const / volatile / ..."]
                TRAIT["<b>Trait & Property</b><br/>trivial / copyable<br/>comparable / hashable<br/>range / sized / sorted / ..."]
                INTERFACE["<b>Interface</b><br/>interface(...)<br/>vtable / impl<br/>capability / contract"]
                CALLABLE["<b>Callable</b><br/>function signature<br/>pure / side-effect<br/>noexcept / async / ..."]
                REFLECT["<b>Reflection & Schema</b><br/>type descriptor<br/>struct offset / size<br/>schema / reflection"]
                CODEGEN["<b>Codegen & Tools</b><br/>header / impl generation<br/>mock / stub generation<br/>tooling"]
            end

            subgraph PLATFORM["Platform / OS — Platform Abstraction (OS Primitives)<br/>基础设施"]
                direction LR
                THREAD["<b>Threading</b><br/>thread<br/>mutex / rwlock<br/>condvar"]
                EVENT["<b>Event / Waitable</b><br/>binary event<br/>wait / signal<br/>reset / wait_for"]
                POLLER["<b>Poller</b><br/>epoll / kqueue<br/>IOCP / poll<br/>native handle"]
                TIMER["<b>Timer</b><br/>sleep / timerfd<br/>CreateTimerQueue<br/>dispatch source"]
                IO["<b>I/O</b><br/>socket / pipe<br/>file / device<br/>async I/O"]
            end
        end

        subgraph CROSS["Cross-Cutting Capabilities"]
            direction TB
            LOG["<b>Logging & Tracing</b>"]
            METRICS["<b>Metrics & Monitoring</b>"]
            CONFIG["<b>Configuration</b>"]
            CSERDE["<b>CSerde</b><br/>Canonical Token Protocol"]
            PLUGIN["<b>Plugin</b><br/>Module ABI / Loader / Lease"]
            TESTING["<b>Testing & Simulation</b>"]
        end
    end

    STREAM -->|"Build on"| RUN
    ACTOR -.-> RUN
    STATECHART -.-> RUN
    WORKFLOW -.-> RUN
    REACTIVE -.-> SOURCE

    RUN -->|"Execute"| GRAPH
    SOURCE --> OPS
    WAIT --> SEM
    EXEC --> PROCESS

    GRAPH -->|"Define & Describe"| TYPE
    OPS --> CALLABLE
    SEM --> TRAIT
    PROCESS --> REFLECT

    TYPE -. "Describe Types & Contracts" .-> THREAD
    INTERFACE -.-> EVENT
    CALLABLE -.-> POLLER
    REFLECT -.-> TIMER

    RUN --- SCHED
    SOURCE --- TQ
    WAIT --- CANCEL
    EXEC --- DEMAND
    CLOCK --- ERROR
    ERROR --- KTRACE

    %% Cross-cutting ownership does not change target dependencies.
    CSERDE -. "canonical token transport" .-> SOURCE
    PLUGIN -. "publishes CMeta capabilities" .-> INTERFACE

    LOG -.-> RUN
    METRICS -.-> KTRACE
    CONFIG -.-> RUN
    TESTING -.-> GRAPH

    classDef layer4 fill:#fbf8ff,stroke:#a78bfa,color:#3b1c88,stroke-width:1px;
    classDef kernel fill:#f5faff,stroke:#60a5fa,color:#174ea6,stroke-width:1px;
    classDef graphStyle fill:#f7fbf4,stroke:#86b874,color:#176126,stroke-width:1px;
    classDef meta fill:#fff9f1,stroke:#f6a34a,color:#c74f00,stroke-width:1px;
    classDef platformStyle fill:#f7f9fc,stroke:#8296b3,color:#183a64,stroke-width:1px;
    classDef cross fill:#f4fbf8,stroke:#72b8a3,color:#075d4c,stroke-width:1px;
    classDef binding fill:#effaf5,stroke:#41a27c,color:#075d4c,stroke-width:1.5px;

    class STREAM,ACTOR,STATECHART,WORKFLOW,REACTIVE,MORE layer4;
    class RUN,SOURCE,WAIT,EXEC,CLOCK,SCHED,TQ,CANCEL,DEMAND,ERROR,KTRACE kernel;
    class GRAPH,OPS,SEM,PROCESS graphStyle;
    class TYPE,TRAIT,INTERFACE,CALLABLE,REFLECT,CODEGEN meta;
    class THREAD,EVENT,POLLER,TIMER,IO platformStyle;
    class LOG,METRICS,CONFIG,TESTING cross;
    class CSERDE,PLUGIN binding;

    style L4 fill:#fcfaff,stroke:#b7a2ef,stroke-width:1px,color:#38137a
    style L3 fill:#f4f9ff,stroke:#72aef5,stroke-width:1px,color:#124694
    style L2 fill:#f7fbf4,stroke:#91bb7b,stroke-width:1px,color:#176126
    style L1 fill:#fff8ef,stroke:#f1a452,stroke-width:1px,color:#c44e00
    style PLATFORM fill:#f6f8fb,stroke:#8798b0,stroke-width:1px,color:#183a64
    style CROSS fill:#f4fbf8,stroke:#72b8a3,stroke-width:1px,color:#075d4c
    style BODY fill:transparent,stroke:transparent
    style STACK fill:transparent,stroke:transparent
```

这张图描述的是**分层产品模型**：Stream 是当前首要用户产品；Actor、Statechart、Workflow、
IO/Reactive 是可在同一 Kernel 上构建的后续 façade/model，不表示当前已经存在独立 public
runtime target。Layer 2/3 共同属于 CFlow；Layer 1 是 CMeta 语义工具层；Platform/OS 提供
底层执行原语。

CSerde 是 Salts 的 format-neutral token foundation。Native binding 位于
SaltsUtils DataBind，并通过 installed CMeta/CSerde 单向依赖 Salts。
Plugin 是独立的 module-lifecycle primitive，与 CFlow 无直接依赖。

### Canonical component lifecycle runtime

The installed Configurator/ServiceHost design has one ownership chain:

```text
CMeta
  immutable type / Interface / FunctionDesc / component capability metadata
      |
      +--> Salts::Configurator
      |      provider selection, dependency validation, start/rollback/stop
      |      for one static graph
      |
      +--> Salts::ServiceHost
      |      candidate generation, atomic publication, Scope admission,
      |      in-flight drain and per-generation Plugin lease settlement
      |              |
      |              +--> Salts::Plugin
      |                     sole DSO load, Registry, lease/quiescence,
      |                     stop and unload authority
      |
      +--> static provider bindings (in-process descriptor validation)
```

Configurator does not load DSOs or publish generations. ServiceHost does not
implement another loader: dynamic Provider discovery starts from an already
loaded/started Plugin in the supplied Registry, acquires a lease before using
borrowed Provider metadata, and releases that lease only after component
teardown. CMeta descriptors describe contracts; they do not own runtime
instances or retain callbacks.

The supported installed targets are `Salts::Configurator`,
`Salts::ServiceHost`, `Salts::ComponentProviderABI` and `Salts::PluginABI`.
Historical ACE freeze documents and test names may retain the former
`ComponentPlugin` label; it is not a supported installed target or a fallback
path. The current six-RID SDK and installed-consumer release gates are tracked
in [ServiceHost SDK audit PR #1260](https://github.com/qigao/salts/pull/1260),
and the unpublished-generation cleanup failure contract is tracked in
[issue #1261](https://github.com/qigao/salts/issues/1261).

`tinytest/`、vendor、build tools 与 Lean/formal generation 属于测试、构建或验证平面，
不进入上面的 runtime ownership 图。设备采集、串口、QueryVM 与格式/协议 parser 由
SaltsUtils 所有；Salts 不再导出对应 targets，唯一例外是底层 `Salts::UriParser`。

## 2. Single sources of truth

### CMeta — type and semantic truth

`Salts::CMeta` 是整个体系最底层的类型与语义事实源，负责：

- type identity、type traits、Enum/Struct reflection；
- callable / relation / interface / contract 元数据；
- semantic data descriptors；
- generic/declared type 与 container descriptor；
- Range、Collector 等跨容器协议。

CMeta 不实现容器算法、不解析具体数据格式，也不拥有 CFlow runtime。

### CSerde — canonical data-event truth

`Salts::CSerde` 定义 format-neutral canonical token、reader/writer contract 和 view
lifetime 语义。它当前没有 Salts public target 依赖，也不拥有 JSON/YAML/XML/CSV
parser。

具体格式由 parser/codec adapter 将 native syntax/events 投影为 CSerde，而不是在 CSerde
内部建立第二套 parser。

### Binding boundary — SaltsUtils DataBind

Salts 1.8 does not own a public native-binding engine. Salts owns the
foundation facts consumed by binding:

```text
CMeta  = native C semantic/storage truth
CSerde = canonical token protocol truth
```

The single logical/native binding engine is SaltsUtils `Salts::DataBind`.
Reusable scalar/struct/buffer/variant/container lifecycle mechanisms are
converged there. Salts does not export `Salts::CBind`, forwarding headers,
source copies, or compatibility aliases.

```text
SaltsUtils DataBind -> installed Salts::CMeta + Salts::CSerde
Salts -/-> SaltsUtils
```

Legacy CBind-only paths are deliberately not retained as fallback:
borrowed parser-source output is outside DataBind publication ownership, legacy
enum storage is superseded by canonical enum-bits metadata, and legacy
container layout recovery is superseded by CMeta collection/map providers.

### CFlow — execution truth

`Salts::CFlow` 是 typed structured graph/dataflow compiler and runtime。其 public
依赖为 CMeta 与 Platform；公开 readiness API 直接暴露 Platform 类型。Concurrency 是
private execution substrate。

CFlow 不拥有容器算法、不解析 serialization format，也不把 raw `cserde_token` 当作可
任意 `filter/map` 的业务 `Stream<T>`。Parser/DataBind/CFlow 的组合边界位于完整
semantic/native value 上。

### CSTL — container truth

`Salts::CSTL` 是标准容器算法和实例 metadata 的事实源，并通过 CMeta container
contract 暴露类型、Range、Collector 与 construction 能力。

CFlow 本身不依赖 CSTL。`Salts::CSTLStream` 是显式 INTERFACE composition target，
只把 `Salts::CSTL + Salts::CFlow` 组合给需要 container stream API 的使用者。

### Platform / Concurrency — execution substrate

`Salts::Platform` 提供最底层 platform abstraction，并公开依赖 CMake 的
`Threads::Threads`。`Salts::Concurrency` 在其上提供 thread pool、Disruptor 等并发
基础能力，并公开依赖 Platform。

CFlow 通过公开 readiness API 消费 Platform，并私有消费 Concurrency。因此 CFlow 的 public
target dependency 是 `CFlow -> CMeta + Platform`；执行语义仍由 CFlow 自身拥有，而不是由
Platform 定义。

### Core — general utility layer

`Salts::Core` 保留字符串、文件、日志、正则、进程、内存及其他通用工具。它公开依赖
CMeta、Platform、Concurrency，并私有消费 STL/CFlow；Core 不应重新成为 container、
metadata、data binding 或 execution semantics 的第二事实源。

### UriParser — low-level URI syntax primitive

`Salts::UriParser` 是 Salts 唯一拥有的 parser target。它提供 URI 词法/结构解析 primitive，
供 `Salts::CNet` 直接消费。UriParser 不依赖 SaltsUtils；Salts package 也不查找或链接
SaltsUtils，因此 package graph 保持单向 `SaltsUtils -> installed Salts`。

QueryVM 与其他格式/协议 parser 不属于 Salts package；它们由 SaltsUtils 构建与导出，并在需要
URI 能力时通过 installed `Salts::UriParser` 消费该 primitive。

## 3. CFlow internal architecture

CFlow 当前存在两个互补入口：dataflow graph/stream pipeline 与 typed Machine pipeline。
两者共享 CMeta semantics 和 CFlow runtime，但 Machine 不是从 Normalized Graph 派生的
第二种表示。

```mermaid
flowchart LR
    Surface[Surface API<br/>Stream / Graph / Operators]
    Lower[Lower]
    Normalized[Normalized IR]
    Analysis[Effect / Property / Verify]
    Optimize[Optimize]
    Plan[Primitive IR / Execution Plan]
    Direct[Direct executor]
    Runtime[CFlow Run<br/>Runtime / Scheduler / Reactive]

    MachineDef[Typed Machine Definition]
    MachineBuild[Validate / Normalize]
    Machine[Immutable Machine IR]
    MachineRuntime[Machine Runtime]

    Surface --> Lower --> Normalized
    Normalized --> Analysis --> Optimize --> Plan
    Plan --> Direct
    Plan --> Runtime

    MachineDef --> MachineBuild --> Machine --> MachineRuntime
    MachineRuntime -. source adapter / demand .-> Runtime
```

等价职责流：

```text
Surface Graph / Stream / Operators
        ↓ lower
Normalized semantic IR
        ↓ analyze / verify / optimize
Primitive IR / compiled execution plan
        ├── direct execution
        └── runtime / scheduler / reactive execution

Typed Machine definition
        ↓ transactional normalize / validate
Immutable Machine IR
        ↓ instance / Event mailbox / executor
Machine Runtime
        ↓ optional Source adapter / Run demand
CFlow Run
```

CMeta 为这些层提供 type/callable/relation/interface semantics；CFlow 负责 execution
semantics。两者不复制彼此的事实源。

## 4. Data and parser integration

Canonical data path 固定为：

```text
native format syntax
        ↓
SaltsUtils parser/event model
        ↓ format projection
CSerde canonical values + CMeta semantic shape
        ↓
SaltsUtils DataBind
        ↓
native C value
        ↓ optional composition
CFlow Stream<T> / Graph / Machine
```

反方向写出遵守同一分层：SaltsUtils DataBind 根据 CMeta 将 native value
映射到 CSerde writer contract，具体 serializer 再把 canonical events 转成目标 syntax/wire。

关键禁止项：

- `Salts -> SaltsUtils`；
- `Plugin -> CFlow`；
- `CFlow -> Plugin`；
- `CFlow -> CSTL`；
- `CSerde -> concrete parser`；
- 把 `Stream<cserde_token>` 暴露为可任意 `filter/map` 的业务 stream；
- 在 DataBind/TBE/CFlow 中维护第二套通用 type/semantic/binding truth。

SaltsUtils 是独立 package，拥有 QueryVM 与 JSON/XML/YAML/CSV/INI/TLV/LTV 等格式/协议
parser component targets。它只通过已安装 Salts package 使用 `Salts::Core`、`Salts::CSTL`、
`Salts::CSerde`、`Salts::UriParser` 等底层能力；Salts 不反向查找 SaltsUtils。具体 parser 与
CSerde 的组合只能位于显式 adapter target；例如 `Salts::JsonCSerdeAdapter` 组合
`Salts::JsonParser` 与 `Salts::CSerde`，而不会把 CSerde 传播给 JsonParser。

## 5. Public target dependency matrix

| Target | Public dependencies | Private / composition dependencies | Canonical ownership |
| --- | --- | --- | --- |
| `Salts::CMeta` | none | none | type / semantic metadata |
| `Salts::CSerde` | none | none | canonical token protocol |
| `Salts::PluginABI` | `CMeta` | none | plugin publication ABI |
| `Salts::Plugin` | `PluginABI` | `Core`, `Platform`, system loader | module loading / lease / lifecycle |
| `Salts::Platform` | `Threads::Threads` | platform implementation | platform abstraction |
| `Salts::Concurrency` | `Platform` | none | concurrency substrate |
| `Salts::CFlow` | `CMeta`, `Platform` | `Concurrency` | graph/dataflow execution |
| `Salts::CSTL` | `CMeta` | none | container algorithms |
| `Salts::CSTLStream` | `CSTL`, `CFlow` | INTERFACE composition | container stream integration |
| `Salts::Core` | `CMeta`, `Platform`, `Concurrency` | `CSTL`, `CFlow` plus utility vendors | general utilities |
| `Salts::UriParser` | none | generated URI lexer | low-level URI syntax primitive |

该表只描述 canonical Salts public/runtime target graph；SaltsUtils-owned parser/query targets、
具体第三方 vendor 与 build/test target 不属于此表的 ownership 语义。

## 6. Architectural invariants

1. **依赖只向基础事实源收敛。** CMeta/CSerde 不因上层使用场景反向依赖
   DataBind、CFlow、CSTL 或 SaltsUtils；Salts 不依赖 SaltsUtils。
2. **同一语义只保留一个 truth。** 类型与 semantic shape 属于 CMeta；
   canonical events 属于 CSerde；native binding 属于 SaltsUtils DataBind；
   execution 属于 CFlow；module lifecycle 属于 Plugin；container algorithms 属于 CSTL。
3. **repo ownership 与 link dependency 分离。** CSerde/UriParser/Plugin 属于 Salts；
   DataBind 与 language bindings 属于 SaltsUtils，并通过 installed Salts targets
   消费 foundation capabilities。
4. **组合能力位于 adapter/composition layer。** Parser + DataBind、Plugin + CFlow、
   CSTL + CFlow 不通过反向依赖污染底层 kernel。
5. **raw structural transport 不是业务 stream。** CSerde token grammar 必须完整保留；CFlow
   pipeline 从完整 semantic/native value 边界开始。
6. **PUBLIC 与 PRIVATE dependency 不混淆。** 公开头若暴露 Platform 类型，则 Platform 必须是
   public target dependency；仅供 execution implementation 使用的 Concurrency 保持 private，
   且 target 可见性不改变各模块的 semantic ownership。
7. **canonical 边界不得静默漂移。** 如果 public target ownership 或依赖方向改变，先更新
   本文，并在对应专项 spec 中明确 migration 与验证方式。
8. **Salts 不反向依赖 SaltsUtils。** UriParser 留在 Salts 是为保持 `CNet -> UriParser` 的底层
   dependency；其他 parser/query capability 上移后只能形成 `SaltsUtils -> Salts`。

## 7. Detailed design references

以下文档保留各专项的详细 reasoning。若历史描述与当前公开 target/CMake/test 不一致，
以当前公开接口、测试和本文的 repo-level ownership 为准：

- `cflow/README.md` — CFlow public surface、lower/IR/runtime/Machine；
- `docs/superpowers/specs/2026-08-21-container-cmeta-cflow-design.md` — CSTL/CMeta/CFlow 分层来源；
- `docs/superpowers/specs/2026-08-22-cmeta-cflow-calculus-v1-design.md` — CMeta/CFlow calculus 与 operator policy；
- `docs/superpowers/specs/2026-08-22-cflow-execution-foundation-design.md` — execution substrate；
- `docs/superpowers/specs/2026-08-23-serialization-data-binding-design.md` — historical CMeta/CSerde/CBind/parser data architecture；
- `docs/superpowers/specs/2026-08-24-cflow-machine-runtime-design.md` — typed Machine runtime；
- SaltsUtils `docs/superpowers/specs/2026-09-16-parser-capability-ownership-migration.md` — current parser package cutover.

## WASI datagram controls and address streams

Additive APIs reuse CNet datagram and c-ares owners for unbound/bind/native peer
association, lossless endpoints, actual options, terminal receive pause and
ordered bounded address streams. Existing UDP init/config layouts stay stable.
Empty UDP sends are real messages; only UDP send admits zero NativeIO length.

Queries retain stable callback slots through actual terminals, including logical
cancellation/drop. Accepted results win cancellation; deadlines latch before
progress and overflow never produces a truncated success. Numeric IPs bypass DNS.

CNet DNS deliberately accepts only ASCII LDH hostnames, pre-encoded A-labels,
and numeric IPv4/IPv6 input; non-ASCII input fails closed. The full Unicode
17.0.0 module migration plus UTS #46 IDNA support are future Salts work in
[#1088](https://github.com/qigao/salts/issues/1088). There is no ICU link,
IDNA library fallback, or Salts -> SaltsUtils dependency in the 2.3 SDK.

Canonical cross-platform vcpkg triplets remain in `qigao/vcpkg-cache`.
Darwin UDP disconnect uses `disconnectx` rather than `connect(AF_UNSPEC)`
when the latter mutates socket state before failing.

Qualification covers IPv4/IPv6, empty/oversized messages, peer replacement,
terminal pause, numeric/ASCII/A-label acceptance, Unicode/invalid-name rejection,
ordered results, bounded failures
and stale/cross-owner handles. Additive callers relink; old callers retain their
behavior. Disabling the optional adapter is the rollback boundary.

## Root-relative filesystem mutation for C guests

WASI command guests need atomic exclusive creation, rename and append flag
changes on existing handles. Ambient `cmeta_fs_rename` and reopening a path
cannot preserve the admitted-root authority or open-file identity. The secure
root API therefore adds `SALTS_FS_ROOT_MUTATION_VERSION`, the root-only
`SALTS_FS_ROOT_O_EXCL` flag, `cmeta_fs_root_rename` and
`cmeta_fs_root_file_set_append`. Existing API layouts remain unchanged; new
consumers require relinking against a matching SDK.

POSIX rename walks both parents using the existing no-follow descriptor walker,
then calls renameat. Windows retains both directory HANDLEs and invokes native
FileRenameInformation, moving the final entry without resolving it as a path.
There is no path-string or copy/delete fallback. Across filesystems, failure is
EXDEV. All acquired parent/source handles are released on every exit path.

POSIX append mutation uses F_GETFL/F_SETFL. Windows records append state in the
private file owner and uses WriteFile's atomic EOF offset for each append write.
Open access rights are retained when flags change. Calls use the existing
single-owner file contract; no independent cache of file positions is added.
Exclusive creation maps to O_EXCL or FILE_CREATE and never truncates an existing
entry. Errors remain negative errno; successful rename retains existing open
file identities. Tests cover exclusive collision, append on/off, replacement,
retained handles and invalid destination paths.
