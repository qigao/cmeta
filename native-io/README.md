# NativeIO

NativeIO 是 Salts 根目录下的原生 I/O 操作层。它只负责把有界操作提交给明确选择的 OS backend，并把终态完成批量交还给调用者；它不拥有 Actor、Reactor、CFlow Graph 或用户 socket。

> Canonical execution-style, endpoint and ownership contract: [ARCHITECTURE.md](ARCHITECTURE.md). Direct and Coroutine are current execution styles; Sharded/SMP is the frozen target contract tracked by #474/#475.

## 架构决策

原生 backend 留在 CFlow 会让 OS 完成状态依赖 Actor mailbox 和 acknowledge，无法单独测量或复用。放入 Platform 则会把现有 readiness 原语与网络操作、payload 生命周期混成一个职责。独立根模块保持以下单向依赖：

```text
CFlow / Actor / future adapters
              |
              v
         NativeIO operations
          /             \
         v               v
Coroutine pool       Platform errors/ABI
         |
         v
 vendor/minicoro
```

当前公开版本提供 Windows IOCP、Linux epoll/io_uring，以及 64 位 macOS/BSD kqueue driver；均支持 SOCK_STREAM connect/recv/send 和 UDP recv_from/send_to。`NATIVE_IO_OPERATION_STREAM_CONNECT`、`STREAM_RECV`、`STREAM_SEND` 是规范名称，原有 `TCP_*` 保持相同枚举值与 ABI 的兼容别名。上层可据此驱动 TCP，也可在 Linux epoll/io_uring 上驱动已 attach 的 AF_VSOCK stream。Windows IOCP 支持 overlapped byte-mode named pipe，Linux epoll 与 macOS/BSD kqueue 支持非阻塞 connected byte pipe；Linux io_uring 支持 blocking 或 nonblocking pipe/FIFO descriptor。工厂只初始化调用方明确选择的 backend，不做隐式 fallback。不满足平台/位宽要求时显式返回 `SALTS_ENOTSUP`。CFlow Actor 与 Reactive 可直接依赖 NativeIO；NativeIO 本身不依赖或拥有 CFlow/CNet 状态。

## NativeIPC 控制面

`<salts/native_ipc.h>` 只负责创建或接入 byte-pipe endpoint，不提交 payload I/O。Windows 提供固定容量、单 owner 驱动的 overlapped named-pipe accept server，以及不等待、不调用 `WaitNamedPipe` 的单次 client connect；POSIX 提供现有 FIFO 的 nonblocking open，不创建、不删除也不修改路径权限。`cmeta_ipc_pipe_capability_supported()` 是平台能力的唯一查询入口，未支持的控制面返回 `SALTS_ENOTSUP`，不会切换到线程或其他传输。

每个 `cmeta_ipc_pipe_endpoint` 是 move-only 所有权包装。C 赋值不会复制底层 handle/fd 的所有权；需要转交时逐字段移动并立即 `cmeta_ipc_pipe_endpoint_init()` 原对象。成功 rendezvous 返回的 `native_io_flags` 可原样传给 `native_io_backend_attach_pipe()`。`request_capacity` 只限制仍由 server 拥有的 pending/ready accept；成功回调转移 endpoint 后立即归还 slot，存活连接不占 accept 容量。Windows server 的关闭顺序为：停止 accept admission、`close` 请求取消、持续 `observe` 到 quiescent、处理或关闭 callback 收到的 endpoint，最后 `destroy`。

```c
#include <salts/native_io.h>
#include <salts/native_ipc.h>

cmeta_ipc_pipe_endpoint endpoint;
cmeta_ipc_pipe_endpoint_init(&endpoint);

#if defined(_WIN32)
int status = cmeta_ipc_named_pipe_connect(
    "\\\\.\\pipe\\salts-example", SALTS_IPC_PIPE_DUPLEX, &endpoint);
#else
int status = cmeta_ipc_fifo_open(
    "/tmp/salts-example.fifo", SALTS_IPC_PIPE_READ, &endpoint);
#endif

if (status == SALTS_OK) {
  /* attach_pipe 借用 native identity；release 后仍由 endpoint 负责 close。 */
  cmeta_ipc_pipe_endpoint_close(&endpoint);
}
```

完整的 Windows accept/cancel/drain 与 POSIX FIFO reader/writer 示例见 `tests/native_ipc_test.c`。

## 数据与状态协议

- 数据单元：一个 `native_io_operation` 对应恰好一个 terminal `native_io_completion`。
- 事实源：backend 固定 endpoint/request 槽位；上层只持有带 generation 的句柄。
- endpoint 类型：attach 时从 `SO_TYPE` 记录 stream/datagram，分别只接受 STREAM/UDP operation；byte pipe 单独记录。TCP、IPv4/IPv6 与 AF_VSOCK 属于上层地址/传输适配维度，不扩张 NativeIO endpoint 类型，地址仍由 native `sockaddr` 表达。
- 所有权：backend 借用 socket；成功 submit 后借用 payload，直到 observe 返回对应 completion。
- 拓扑：除 `native_io_backend_wake()` 外，一个 backend 只由一个 owner 线程调用。一个 owner 可在同一 backend 上驱动最多 `endpoint_capacity` 个 TCP/UDP/Pipe endpoint；模块不创建线程、不内置任务队列。需要多核扩展时由上层创建多个 backend 并分片 endpoint，不能让多个线程并发驱动同一 backend。wake 是唯一允许从生产者线程调用的合并式控制边。
- 容量：endpoint、request 和 completion batch 均在 init 时固定；满额返回 `SALTS_ENOBUFS`。
- 顺序：每个 endpoint 的 read lane 与 write lane 分别按 FIFO 向内核发起操作，lane 之间不排序。request handle 与 `user_data` 用于关联；不同 endpoint 的 completion 顺序由内核决定。
- 连接：`STREAM_CONNECT`（以及兼容别名 `TCP_CONNECT`）独占尚未连接 stream endpoint 的 admission，重复 connect 返回 `SALTS_EALREADY`，连接终态被 observe 前提交 recv/send 返回 `SALTS_EBUSY`。readiness backend 要求该 socket 已由调用方设为 nonblocking；NativeIO 不改变其模式，也不决定 TCP/VSOCK 地址策略。
- 取消：cancel 只请求取消。IOCP 的 `ERROR_OPERATION_ABORTED`、io_uring 的 `-ECANCELED` 和 readiness 队列中尚未执行的请求进入 CANCELLED；已经完成的请求不会被改写成取消。
- 关闭：`close admission -> cancel/drain -> close native sockets -> release endpoints -> destroy`。
- 等待：`timeout_ms == 0` 为 poll，`UINT32_MAX` 为无限等待，其余值为相对毫秒 deadline；无终态返回 `SALTS_ETIMEDOUT` 且 count 为零。
- 唤醒：生产者先发布上层命令，再调用 wake。多个并发 wake 合并成一个有界 OS 控制信号；纯控制唤醒使 observe 返回 `SALTS_OK` 且 count 为零，不伪造 completion。close/destroy 前必须先停止 wake 调用者。

### Coroutine owner 路径

`native_io_backend_spawn_coroutine()` 提供同一 owner 线程上的可选结构化路径。entry 立即运行到返回或 `native_io_coroutine_await()`；await 成功提交后挂起，只有匹配的 terminal completion 被 owner observe 后才恢复。一次 observe 会先完成整批 terminal 的 request 归属解析，再恢复其中的 coroutine；因此恢复后的 entry 可以立即再次 await，不会复用仍被该批后续 packet 引用的 request 槽位。NativeIO 不使用 CoroNet context、TLS current-loop、隐式线程或第二套 request 状态机；request 槽位仍是唯一 I/O 事实源，coroutine 只保存执行位置。

coroutine task 数与 request 共用同一硬容量。frame 由 `Salts::Coroutine` 的有界池延迟创建并复用；池满返回 `SALTS_ENOBUFS`。取消 task 只转发为 request cancel，frame 必须等 `CANCELLED`/其他竞态终态被 observe 后才释放。带 ABI 版本与结构大小的 `native_io_coroutine_stats` 公开 `capacity`、`active` 与 `retained_frames`，但不改变既有 `native_io_backend_stats` 布局，也不暴露 minicoro handle。

若 entry 未经 `native_io_coroutine_await()` 直接挂起，则违反 NativeIO coroutine 协议：spawn/resume 返回 `SALTS_EPROTO`，该 suspended frame 被销毁，task slot 立即归还，不把 backend 留在无法 drain 的活动状态。

```text
spawn -> RUNNING -> await/submit -> SUSPENDED
                                   |
                      OS terminal completion
                                   |
                                   v
                              RUNNING -> returned -> pooled
```

请求槽位状态机为：

```text
FREE --submit accepted--> PENDING --observe terminal--> FREE(next generation)
                              |
                              +--cancel request--------+
```

每次成功 submit 恰好产生一个可观察终态。submit 原生失败在返回前回滚到 FREE，不产生 completion。destroy 在 admission 未关闭、请求未 drain 或 endpoint 未释放时返回 `SALTS_EBUSY`，并保留所有权供调用者修复。

## 性能边界

### 后端原生推进与显式批处理

决策背景：串行 echo 的 Linux trace 显示 epoll 每 RTT 重复注册/移除读监听，
io_uring 每个 SQE 单独 enter。公共 operation/completion 契约不要求相同的内部推进算法。
这里保留旧 `submit` 的立即启动/错误返回行为，新增 `prepare`、`flush` 和
`native_io_coroutine_await_prepared`。不把旧 submit 隐式改成延迟接收，不添加后台线程或新依赖。

- **epoll**：非阻塞 I/O 快路径不变；第一次 would-block 时注册对应方向的 ET 监听，
  监听保留到 endpoint release，新增方向才 MOD。没有请求时不读取 payload；新请求先尝试
  syscall，已有 FIFO lane 则排队，避免越过旧请求读取。处理完现有请求后即停止，不为 drain
  到 EAGAIN 而越过调用方的 buffer/demand。即使边沿已被消费，后续请求仍先尝试 I/O，
  不依赖“再来一个边沿”。kqueue 保持既有注册策略，不强行继承 epoll ET 规则。
- **io_uring**：prepare 将有界描述符放入原 request 槽位；只有每个 read/write lane 的 head
  进入待提交链，flush 批量发布 SQE。observe 在等待或交回 completion 前提交 eligible heads。
  wake eventfd 由 ring 内部的 `IORING_OP_POLL_ADD` 观察；支持 EXT_ARG 的内核直接用
  `io_uring_enter(GETEVENTS)` 完成等待，不再额外 `poll(ring_fd, wake_fd)`。CQ 推进后的新
  lane head 在同一 owner 轮次合并，普通 submit 的空闲 lane 仍立即提交。Linux 构建与内核
  能力允许时，ring 以 `SINGLE_ISSUER | DEFER_TASKRUN | TASKRUN_FLAG` 表达既有单 owner
  契约，并把 completion task-work 收敛到 owner 的 GETEVENTS progress 边界。若内核不支持
  TASKRUN_FLAG，则退到 `SINGLE_ISSUER | DEFER_TASKRUN`；不支持 DEFER 时再退到
  SINGLE_ISSUER，最后才退到无 setup flag 的同一 io_uring backend，绝不隐式切换 epoll。
  DEFER + TASKRUN_FLAG 模式下，`observe(0)` 先消费可见 CQE、提交 staged work 并再次检查
  CQ；只有 SQ flags 出现 `IORING_SQ_TASKRUN` 或 `IORING_SQ_CQ_OVERFLOW` 时才执行
  非阻塞 `GETEVENTS(min_complete=0)`。完全 idle 时直接返回 `SALTS_ETIMEDOUT`，不进入
  内核；旧内核没有 TASKRUN_FLAG 时仍保留原来的保守 GETEVENTS 行为。
- **IOCP**：prepare 使用既有 overlapped submit；没有待提交 SQ，flush 无额外工作。
  不把 readiness/io_uring 的“取消一个本地排队请求后继续使用 socket”保证扩展到 Winsock。
  Microsoft 明确说明取消未完成 overlapped I/O 后继续使用 socket 的行为未定义，
  应关闭 socket，见 [Winsock overlapped I/O](https://learn.microsoft.com/en-us/windows/win32/winsock/overlapped-i-o-and-event-objects-2)。

状态协议：`FREE -> admitted/queued -> staged -> in-flight -> terminal -> FREE`。
request 槽位是唯一所有权事实源，staged 链只是槽位内的调度索引，没有独立 payload 副本。
单 owner 修改所有阶段，wake 仍是唯一跨线程入口；read/write lane 内 FIFO、lane 间不排序。
prepare 成功即开始 borrow，直到 observe 对应终态才结束；取消 staged 请求不进入内核，
取消 in-flight 请求仍等待内核终态。close 只停止 admission，已接收请求仍可 flush/drain。
所有阶段合计不超过 request_capacity；满额拒绝，不扩容，不分配新的热路径存储。

flush 对部分 enter 使用实际 SQ head 的消费前缀确认所有权，已消费项绝不重放。
无 SQPOLL/第二 submitter 时，未消费后缀可重新准备；flush 原生失败立即返回错误，并为
尚未提交的受影响请求发布 FAILED 终态。调用方仍须观察全部已接收请求，不能因 flush
失败提前释放 buffer。每个请求恰好一个终态，generation 防止旧 completion 关联新槽位。
调度索引增删 O(1)，flush O(本批 eligible 请求数)，不扫描全部 endpoint。

迁移范围：CNet owner、CFlow NativeIO adapter 和网络 benchmark 显式采用 prepare/observe；
coroutine 通过新 await 入口选择批处理，旧 await 不变。公开结构体布局与枚举值不变，
但新调用方需要包含新增符号的 NativeIO 库。不同后端不保证 prepare 后尚无外部副作用。
回滚可将上述调用点改回 submit/旧 await；epoll 注册策略可独立回退，不改变数据格式。

验证范围包括 staged/内核取消、FIFO、容量、环绕复用、部分提交错误、close/drain、
ET 无 pending 时到达数据及后续读、wake 和 coroutine completion 路由；收益需要同时看
串行 RTT、多连接并发、饱和吞吐和 CPU，不能仅凭 syscall 减少宣称最优。

内核语义参考：[epoll ET](https://man7.org/linux/man-pages/man7/epoll.7.html)、
[io_uring_enter](https://man7.org/linux/man-pages/man2/io_uring_enter.2.html)。

完整批处理用例见 `tests/native_io_test.c` 的 `native_io_test_prepared_fifo`：
同一 owner prepare 多个请求，再显式 flush 或通过 observe 隐式 flush。
无论 flush/observe 是否返回错误，已接收请求都必须观察终态后才能释放 buffer。
benchmark 中 prepare 计入 admission 阶段，真正 SQ 提交计入 observe/flush 阶段；
阶段耗时不能与重构前的 submit/observe 分界直接对比，总 RTT 仍可比较。

若 byte pipe 的首要目标是最低框架延迟或最高吞吐，并且调用方能够自行管理
operation、completion、容量与关闭顺序，应直接使用 NativeIO Pipe 接口。
Actor 与 CFlow Reactive 适用于需要状态隔离、确认协议、demand 或 Graph 操作的
场景；它们提供额外语义，也会引入相应控制面成本。不要仅为传输数据而把
NativeIO Pipe 强制包装成 Actor 或 Reactive。

direct backend 初始化时预分配 endpoint/request/native event storage，之后 direct submit/observe 不分配内存。coroutine owner 的 task free stack、request 路由表与 completion batch 在首次 spawn 时一次性延迟创建；frame 随后按同一硬上限延迟创建并复用：

- IOCP：stream connect 使用 `ConnectEx`，socket 数据 submit 直接调用 `WSARecv`/`WSASend`，named-pipe submit 直接调用 overlapped `ReadFile`/`WriteFile`，observe 统一读取 completion port。
- epoll/kqueue：connect 使用 nonblocking `connect` 与 `SO_ERROR`；其余 submit 先以单次非阻塞 syscall 尝试，仅在 would-block 时进入每 endpoint 的 FIFO lane，并由 owner 在 observe 中直接等待 readiness 和继续 syscall。
- io_uring：connect 使用 `IORING_OP_CONNECT`，pipe/FIFO read/write 使用 `IORING_OP_READ`/`IORING_OP_WRITE`。每个 endpoint 的 read/write lane 各保持至多一个内核 in-flight SQE，其余已接受描述符保留在固定 request 槽位中；observe drain CQ 后推进 lane。ring 由模块映射，但没有 worker、mutex、callback、payload copy 或跨线程 mailbox。

readiness 的 kernel interest 是监听策略的镜像，不是请求事实源；epoll ET 允许它跨空 lane
保留，kqueue 按 pending lane 更新。endpoint/request/terminal storage 和 native event batch 均有固定上限。

socket 的关闭顺序仍是停止提交、drain 所有请求、由调用方关闭 socket，最后
`release_socket`。epoll 的 socket release 只释放 endpoint 元数据，不对已经关闭的
fd 调用 `epoll_ctl`：该数字可能已经属于其他 owner。内核管理 watch 生命周期；
存在指向同一 open file description 的重复 fd 时，watch 可能继续存在，旧事件由
slot/generation 校验拒绝（见 [epoll 的关闭与重复 fd 语义](https://man7.org/linux/man-pages/man7/epoll.7.html)）。
仍打开的 drained pipe 则在 `release_pipe` 时显式移除监听，允许同一 fd 重新 attach。
`native_io_readiness_lifetime_test` 覆盖关闭后跨线程复用 fd 与仍打开的 pipe 重接入；
并发竞争需在 `linux-tsan-ci` 下验证，普通运行不提供数据竞争检测。

从未 spawn coroutine 或当前没有活动 coroutine 时，`native_io_backend_observe()` 直接进入所选 OS driver，不经过 completion 路由缓冲或 coroutine context switch。纯 direct 使用也不会创建 coroutine owner/pool。因此 direct benchmark 仍测量原生 NativeIO 路径；只有显式 spawn coroutine 的调用方承担 owner storage 与 suspend/resume 成本。

## 最小示例

```c
#include <salts/native_io.h>

native_io_backend backend = {0};
native_io_backend_config config = {
#if defined(_WIN32)
    NATIVE_IO_BACKEND_IOCP,
#elif defined(__linux__)
    NATIVE_IO_BACKEND_EPOLL, /* 或 NATIVE_IO_BACKEND_IO_URING */
#else
    NATIVE_IO_BACKEND_KQUEUE,
#endif
    16u, 64u, 16u};

int status = native_io_backend_init(&backend, &config);
if (status != 0)
  return status;

/* Attach an already-created socket, submit operations, then call
   native_io_backend_observe() from this same owner thread. Windows sockets must
   have been created for overlapped I/O. */
```

完整可运行的通用 stream/TCP 与 UDP loopback 用法位于 `tests/native_io_test.c`；Linux VSOCK 的地址、listener 与运行时跳过策略由 CNet 测试覆盖。

网络性能比较位于 CNet 的 `cnet_io_benchmark`，由依赖 NativeIO 的上层 target 统一比较 libuv、NativeIO 与 CNet，避免 NativeIO 反向依赖 CNet。libuv 只链接 benchmark executable，不进入 NativeIO 的公开依赖或生产链接面。

`native_io_pipe_benchmark` 在 Windows IOCP 上比较 raw overlapped named-pipe completion 与 NativeIO，在 Linux epoll 和 macOS/BSD kqueue 上比较 raw POSIX pipe 调用与 NativeIO。每个样本执行 256 次单向 transfer，覆盖 1/4/8/16/32/64 KiB；应用 payload 每次只计一次，不把读端和写端重复计算为两倍流量。fixture、buffer、handle/descriptor 与 backend 初始化位于计时区外，输出独立的 p50/p95 延迟、吞吐以及 raw submit、NativeIO submit/observe 阶段表。Linux io_uring 在对应 pipe backend 实现前不生成伪基线。

`cnet_io_benchmark` 的 Linux release CI 分别创建 epoll 与 io_uring 作业，并通过
`CNET_IO_BENCHMARK_BACKEND` 显式选择 NativeIO direct、NativeIO coroutine 与
CNet 共用的 backend；非法或当前平台不支持的值会立即失败，不会回退到 epoll。
Actor 与 Reactive 有各自的线程、mailbox/demand、背压和 terminal 语义，必须在
CFlow 中按发送端与接收端分别设计 workload，不与 NativeIO/CNet/libuv 的 transport
数据面结果横向比较。CFlow NativeIO adapter 的常规测试只验证集成正确性。
