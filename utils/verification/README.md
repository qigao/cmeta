# Salts 队列与内存协议验证

这些模型用于验证 Salts 并发原语的有限状态协议。模型抽象了 payload 内容、指针地址和 C11 内存模型，重点检查 claim/publish/observe/release、容量、所有权、关闭和唤醒语义。

## 模型范围

| 模型 | 覆盖内容 |
|---|---|
| `disruptor_worker.pml` | worker-pool 单次 claim、单次 release、发布间隙、完成游标和 shutdown drain |
| `disruptor_broadcast.pml` | 多 consumer 广播、慢 consumer gate、每条消息观察/释放一次、排空关闭 |
| `ring_spsc.pml` | SPSC byte ring、acquire/release、partial read、wrap marker、FIFO 和容量边界 |
| `buffer_ownership.pml` | `mem_buffer_t` retain/release、`mem_slice_t` 保活、external free exactly once、pool quiescence |
| `priority_queue.pml` | MPMC 四档优先级、满队列重试、blocking pop wakeup、严格优先级和 per-bucket FIFO |
| `byte_buffer.pml` | 单 owner 有界字节缓冲、追加/压缩/抽象扩容、FIFO、错误路径原子性、borrowed view、reset/destroy |
| `disruptor_wait_shutdown.pml` | worker blocking claim 的 should-run 停止唤醒、发布唤醒、唯一 claim/release |
| `disruptor_topology.pml` | broadcast chain/fan-in 依赖、下游 gate、有效拓扑提交和环依赖拒绝 |
| `priority_queue_timeout.pml` | MPMC 空队列 try_pop、timeout false、输出保持和后续 push/pop 排空 |
| `byte_buffer_errors.pml` | 非法 init/append/view、consumed-region alias 拒绝、`append(NULL, 0)` no-op、destroy |

关键事实来源包括：

- `include/disruptor.h` 与 `src/disruptor.c`：Disruptor 的 worker/broadcast/拓扑 API；
- `../concurrency/include/salts/spsc_ring.h` 与
  `../concurrency/src/spsc_ring.c`：SPSC 角色和原子游标的事实源；
- `include/ring_buffer_spsc.h` 与 `src/ring_buffer_spsc.c`：旧 Core API 的兼容包装；
- `include/cmeta_buffer.h` 与 `src/cmeta_buffer.c`：buffer/slice 引用和 pool 生命周期；
- `src/bucket_priority_queue_mpmc.c`：优先级队列对 Disruptor 和条件变量的包装。
- `include/cmeta_byte_buffer.h`、`src/cmeta_byte_buffer.c` 与 `tests/test_cmeta_byte_buffer.c`：单 owner 字节缓冲的游标、容量、view 和错误语义。
- `tests/test_disruptor.c`：worker wait/wake、consumer dependency 和 topology cycle 测试；
- `include/bucket_priority_queue_mpmc.h`、`src/bucket_priority_queue_mpmc.c` 与 `tests/test_bucket_priority_queue_mpmc.c`：MPMC try/blocking/timeout 语义。

## Windows 验证

Spin 6.5.1 在当前环境使用 Clang 预处理器。建议在临时目录运行，避免把 `pan.c`、`pan.exe` 和 trail 写入源码目录：

```powershell
$spin = 'C:\tools\cpp-dev\bin\spin.exe'
$utils = 'C:\projects\cpp\turbonet\salts\utils'
$work = Join-Path ([IO.Path]::GetTempPath()) ('salts-spin-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $work | Out-Null
Push-Location $work
try {
& $spin '-Pclang -E -x c' -a "$utils\verification\disruptor_worker.pml"
clang -DWIN32 -Dwrite=_write -include io.h -DNFAIR=16 `
  -Wno-deprecated-declarations -Wno-pointer-to-int-cast -Wno-format `
  -O2 -o pan_worker.exe pan.c
.\pan_worker.exe -a -f -N no_double_worker_processing
.\pan_worker.exe -a -f -N worker_only_claims_published
.\pan_worker.exe -a -f -N completed_cursor_never_exceeds_capacity
.\pan_worker.exe -a -f -N published_entries_eventually_processed
.\pan_worker.exe -a -f -N shutdown_eventually_quiesces
} finally {
  Pop-Location
}
```

其余模型使用同样的生成和编译命令，只需替换源文件和验证器：

```powershell
& $spin '-Pclang -E -x c' -a verification\disruptor_broadcast.pml
& $spin '-Pclang -E -x c' -a verification\ring_spsc.pml
& $spin '-Pclang -E -x c' -a verification\buffer_ownership.pml
& $spin '-Pclang -E -x c' -a verification\priority_queue.pml
& $spin '-Pclang -E -x c' -a verification\byte_buffer.pml
& $spin '-Pclang -E -x c' -a verification\disruptor_wait_shutdown.pml
& $spin '-Pclang -E -x c' -a verification\disruptor_topology.pml
& $spin '-Pclang -E -x c' -a verification\priority_queue_timeout.pml
& $spin '-Pclang -E -x c' -a verification\byte_buffer_errors.pml
```

每个 claim 的成功标准都是输出 `errors: 0`。完整 claim 列表如下：

当前共 10 个模型、50 个 claims。

```text
disruptor_broadcast:
  every_consumer_sees_every_entry
  broadcast_releases_after_observe
  slowest_consumer_gates_reuse
  broadcast_eventually_drains

ring_spsc:
  ring_never_overruns
  ring_never_reads_unpublished_bytes
  every_write_claim_is_released
  fifo_payload_is_preserved
  ring_eventually_drains

buffer_ownership:
  no_free_while_referenced
  slice_keeps_source_alive
  external_callback_once
  handoffs_eventually_release
  pool_destroy_is_quiescent

priority_queue:
  capacity_is_never_exceeded
  all_pushed_jobs_eventually_pop
  full_try_push_is_observable
  blocking_pop_wakes_after_publish
  priority_queue_eventually_quiesces

byte_buffer:
  byte_buffer_capacity_is_bounded
  byte_buffer_cursors_remain_valid
  failed_append_is_atomic
  failed_consume_is_atomic
  borrowed_view_matches_current_state
  byte_buffer_eventually_destroyed

disruptor_wait_shutdown:
  shutdown_waiter_returns_without_claim
  shutdown_wake_eventually_returns
  worker_claims_only_published_entry
  published_entry_eventually_releases
  worker_release_follows_claim

disruptor_topology:
  acyclic_topology_commits
  cyclic_topology_is_rejected
  middle_stages_follow_parse
  sink_waits_for_fan_in
  topology_pipeline_eventually_drains

priority_queue_timeout:
  empty_try_pop_preserves_output
  timeout_pop_returns_false
  queue_capacity_is_bounded
  timeout_wait_eventually_finishes
  successful_transfer_drains

byte_buffer_errors:
  invalid_append_preserves_state
  consumed_alias_is_rejected
  invalid_view_preserves_output
  null_zero_append_is_noop
  invalid_paths_eventually_destroy
```

当前未把 Spin 验证器加入默认 CMake 测试目标：`tests/CMakeLists.txt` 会自动构建 `test_*.c`，而 Spin/Clang 是外部、可选工具链，直接接入会让普通构建依赖额外工具并生成临时验证器。模型通过本 README 的 PowerShell 命令单独运行；如需 CI 门禁，建议在 CI job 中安装 Spin/Clang 后执行同一套命令。

`-a` 启用 acceptance-cycle 检查，`-f` 启用 weak fairness；正常验证保留 partial-order reduction。只有诊断或交叉检查时才使用 `-DNOREDUCE` 重新生成验证器。

## 发布版分配器 TSan 门禁（#1170）

[Salts 2.3.0-rc.15 源码 / 发布二进制并发验收对照](global_buffer_release_tsan_qualification.md)
记录了原始发布标签源码完整 TSan、发布包双活租约与跨 Owner 同一
buffer 复用测试，以及混合插桩产生的独立 TSan 诊断。

**重要：** 发布版 `libsalts.so` 未插桩时，外部程序的 TSan 结果不能等同于
整个分配器的完整源码 TSan；现有混合插桩报告保留为未通过的诊断，
不使用 suppressions，也不据此宣称所有生产并发路径均已证明安全。

## 验证边界

Promela 验证的是有限容量和有限消息数下的协议交错，不能单独证明：

- C11 原子内存序在真实编译器/CPU 上的实现正确性；
- 指针算术、越界、未初始化读、ABI 或实际分配器行为；
- 未抽象进模型的 OOM、系统调用和平台条件变量细节。

`byte_buffer.pml` 采用小容量来强制走压缩和扩容分支；这验证容量与状态协议，不等价于实现中的具体增长因子或实际分配器行为。

因此仍需配合现有 TinyTest、压力测试、TSAN、ASan/UBSan 和泄漏检查。模型发现的 counterexample 应先映射回对应 API 的 claim/publish/release 或 retain/release 路径，再判断是否需要生产代码修复。
