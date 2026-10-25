# CNet

## Default progress strategy for an ordinary client

`cnet_client_poll_strategy` provides an opt-in balanced policy on the existing
fixed owner. `NULL` selects `CNET_PROGRESS_STRATEGY_INIT`: up to 32 nonblocking
progress passes and a maximum 10 ms idle wait. A pass with no callbacks stops
draining. Waiting is permitted only if the whole call delivered no callbacks;
the caller's timeout further caps it. Timeout zero means bounded nonblocking
draining. This does not change `cnet_client_poll(..., 0, ...)`, which still makes
one nonblocking pass, or any existing config layout. The timeout caps the
requested OS wait, not actual return latency: timer granularity and scheduling
can overshoot it. Use `CNET_PROGRESS_STRATEGY_SPIN_INIT` (32 passes, no blocking)
when timer latency matters more than idle CPU, and measure on the target OS.

```c
cnet_progress_strategy strategy = CNET_PROGRESS_STRATEGY_INIT;
size_t events = 0;
/* application_ready and until_next_timer_ms belong to the application. */
int status = cnet_client_poll_strategy(
    &client, &strategy, application_ready ? 0u : until_next_timer_ms, &events);
/* Propagate status; inspect application state even when events == 0. */
```

The strategy is copied on entry, holds no resources, and allocates no queues or
threads. `max_poll_passes` accepts 1–1024; `idle_wait_ms=0` explicitly selects
continuous nonblocking progress. These are progress-pass and wait budgets, not
message batch size, send aggregation, a delay before sending, or a hard callback
time limit. Callbacks stay ordered and inline and must not block. Stop/cancel
still requires real terminal completion and drain before storage is released.

The existing wake primitive interrupts an idle wait. Callback-issued wakes end
draining and remain pending for the next call. Zero callbacks is not proof of
quiescence. Recursive/concurrent progress is rejected throughout the strategy
call, and external clients return `SALTS_ENOTSUP`: their host owns backend
observation and must choose its own policy.

### Decision and compatibility

FlowMQ's direct rc.5 CNet experiment showed that batching owner progress can
reduce queue residence without reducing total polls per message. That supports
an explicit drain budget, but does not prove that 32 passes is optimal for every
connection count or handler. Unlike that caller experiment's 32-record receive
target, this library cannot infer protocol message boundaries; one pass may
deliver many callbacks. Applications must bound their admission and callback
work separately. Send-buffer/SG/packing selection remains explicit because CNet
does not own the consumer's record framing or permission to copy.

Alternatives considered were changing legacy poll semantics, adding I/O worker
handoff, and keeping a caller-only loop. The selected API shares the existing
poll guard and wake protocol, avoiding a lost application wake between a public
nonblocking poll and a separate blocking call. Its additional state is local to
one call; work is bounded by the configured passes plus the capped idle wait.
It adds no client fields or CFlow dependency. Adoption is one call-site change;
rollback is to `cnet_client_poll`. max_poll_passes=1/idle_wait_ms=0 reproduces its
nonblocking progression shape, not its positive-timeout waiting behavior.

[LF CPU pool PR #1104](https://github.com/qigao/salts/pull/1104) is a separate,
explicitly selected `Salts::Concurrency` backend. Its input/provider lifetimes
and application result return path remain the consumer's responsibility. It
does not transfer this client's I/O ownership, and is not enabled by this
strategy. Short transport callbacks have no measured reason to enter a worker
pool; heavy application handlers need their own bounded handoff measurement.

Formal coverage lives in `cnet_progress_strategy_test`, the ordinary API and
external-progress suites, C++ headers, and the installed C11/C++17 strategy
consumer. `cnet_progress_strategy_benchmark` compares legacy spin, bounded-drain
spin and balanced waiting for idle, scheduled bursts and saturated callback
chains. Results and platform limits are recorded after execution below.

### Windows qualification, 2026-10-10

This change is based on published rc.5 source
`d04b3c909a5f1ac49883f36dcaa7b57de71627cb`, in the isolated
`work/cnet-default-poll-strategy` worktree. It does not alter the released SDK.
The Windows MSVC 19.44 Release full configured build and all **59 CNet CTests**
passed (16.04 s), including the installed C11/C++17 consumer, external progress,
retained sends, TLS, stop and header contracts. The strategy-specific cases
cover invalid bounds, caller timeout, default/nonblocking profiles, budgets
1/32/1024, callback counting, recursive/concurrent rejection, callback wake
preservation, cross-thread wake, and shutdown with an unfinished retained chain.

The [45 raw observations](benchmarks/cnet-progress-strategy-windows-20261010.csv)
are three policies × three loads × five repetitions, with rotated policy order.
Hardware: Ryzen 9 7940HX; one ordinary CNet owner, both loopback TCP endpoints on
that owner, unbound affinity, 64-byte fully checked immutable records. One send
completion admits the next record; native completion capacity is deliberately
one. This isolates progress behavior and is not a maximum-throughput SG/packed
write benchmark. Establishment, warmup and shutdown are outside measurement.

| Load / metric, median of five | Legacy spin | Drain spin | Balanced default |
| --- | ---: | ---: | ---: |
| Idle, CPU equivalents | 1.016 | 1.016 | below CPU timer resolution |
| Idle, public poll calls | 1,108,006 | 1,034,863 | 25 |
| Paced, CPU equivalents | 1.024 | 1.024 | 0.048 |
| Paced, maximum scheduled completion, ms | 0.618 | 0.600 | 18.237 |
| Paced, maximum burst service time, ms | 0.617 | 0.600 | 0.731 |
| Saturated, 16,384 records elapsed, ms | 100.897 | 111.424 | 103.511 |
| Saturated, public poll calls | 32,768 | 1,024 | 1,024 |

Idle runs request 400 ms with receive demand outstanding. Paced runs release
32-record bursts every 5 ms on a fixed schedule, 64 bursts total; delayed releases
catch up without loss. The two latency columns are medians of **per-run maxima**,
not P99: completion relative to the planned release includes timer overshoot,
whereas burst service time begins at actual admission. They do not measure
remote network-arrival wake latency. Public poll-call reduction does not imply
the same reduction in native observations/syscalls. CPU is whole-process CPU
time divided by wall time; Windows accounting is quantized, hence zeros and
values slightly above one. No zero-CPU or universal throughput gain is claimed.

The balanced profile is suitable when idle CPU matters and OS timer granularity
is acceptable. The 5 ms timer workload shows why it must remain explicit:
blocking saved CPU but substantially delayed planned releases on this machine.
Use the spin initializer for tight timer workloads, or timeout zero while the
application has runnable work. Neither profile enables LF/Token or automatically
changes FlowMQ's external-owner path. That host needs separate policy adoption.

With the repository's SDK/vcpkg environment and Re2c configured:

```powershell
cmake --preset win-release-ci -DBUILD_EXAMPLES=OFF -DBUILD_BENCHMARKS=OFF -DCNET_BUILD_BENCHMARKS=ON
cmake --build --preset win-release-ci -j 12
ctest --preset win-release-ci -R '^cnet_.*test$' --output-on-failure
# Override the normal preset's benchmark-label exclusion for this one test.
ctest --preset win-release-ci -LE '^$' -R '^cnet_progress_strategy_benchmark$' -V
```

Benchmark passed in 12.71 s. Local build/test log:
`build/strategy-final-validation.log`. These local measurements do not qualify
Linux/macOS behavior or sanitizer safety. CI runs the strategy contracts on the
native hosts and ASan/UBSan/TSan profiles, and archives the three Release hosts'
45-row measurements as `cnet-progress-strategy-<profile>`. Use the matching PR
check results for qualification. Mixed heavy/light connections, remote peers
and integrated FlowMQ/LF return paths need separate application measurements.

CNet is the connection-oriented layer above NativeIO. Applications see a
client, generation-checked connections, send/receive operations, explicit
progress polling, and ordered state notifications. NativeIO remains the raw,
threadless operating-system I/O backend; CFlow Actor and Reactive code continue
to depend on NativeIO directly.

The canonical lower-layer contract is [NativeIO execution and endpoint architecture](../native-io/ARCHITECTURE.md). CNet is an optional network/session semantic consumer: raw TCP/UDP/VSOCK/PIPE data paths do not require CNet, and CNet does not own NativeIO execution-style or terminal-completion truth.

CNet is built unconditionally. Its source-tree target is `cnet`; installed
consumers link `Salts::CNet` and include `<cnet/cnet.h>`. The independent
WebSocket session API is declared by `<cnet/websocket.h>`.

The [IPC, WebSocket, and KCP evolution design](#ipc-websocket-and-kcp-evolution-design)
records the consumer contract, implementation boundaries, and release gates.
The implemented additive capabilities are listed below; downstream integration
and release gates in the design remain separate work.
Its [WS and IPC multicore design](#ws-and-ipc-multicore-design) specifies fixed
connection placement, bounded handoff, cross-owner commands, and shutdown.

## Owner-local connection management

The owner-local slice of [#1001](https://github.com/qigao/salts/issues/1001) is available
through `<cnet/manager.h>` in the same `Salts::CNet` shared library.
The architecture proposal is [PR #1002](https://github.com/qigao/salts/pull/1002).
The manager is opt-in at runtime, not a separate shared-library target.
The production CNet DSO keeps incarnation epochs and owner-thread checks
consistent across consumers loaded from different DSOs.

One manager borrows an initialized client on its progress owner. Initialization
allocates a fixed record table; admission and management progress never grow it.
`connection_capacity` bounds RESERVED plus BOUND records; `record_capacity`
also includes RETIRED contexts. These are helper credits, not reservations of
the client's transport slots: unmanaged users can still exhaust that client.

The lifecycle is `reserve -> connect/adopt -> real terminal -> recycle`, or
`reserve -> cancel -> recycle`. Each successful reserve must be consumed or
canceled. Connect/adopt retire a valid reservation even on immediate rejection;
they never fabricate state callbacks. A valid adopt attempt consumes the
detached stream on success or failure. Invalid identities leave it untouched.
TCP and TLS are supported in this stage; other transport URIs return ENOTSUP.

The attachment copies an observer and optionally a once-only cleanup callback.
Its user pointer remains borrowed. Transport terminal returns the helper credit
before forwarding the real state callback. With `hold_context=true`, the host
aggregates business references and calls `release_context` once when they end.
That hold consumes record storage without retaining a transport slot. Cleanup
runs only in a later `advance`, after callbacks and the extra hold end. Retained
identity values neither retain storage nor authorize raw CNet operations.

For example, after initializing `client` and preparing `observer`:

```c
cnet_manager manager = {0};
const cnet_manager_config config = {
    sizeof(config), CNET_MANAGER_VERSION, &client, 64u, 32u};
int status = cnet_manager_init(&manager, &config);
if (status != SALTS_OK) return status;
const cnet_manager_attachment attachment = {.observer = observer};
cnet_managed_connection managed = {0};
cnet_connection connection = {0};
status = cnet_manager_reserve(&manager, &attachment, &managed);
if (status == SALTS_OK) {
  const cnet_connect_options options = {.uri = "tcp://127.0.0.1:8080"};
  status = cnet_manager_connect(&manager, managed, &options, &connection);
}
/* Preserve status. The owner continues its normal CNet poll and bounded
 * manager advance, including recycling an immediately rejected attempt. */
```

Send, receive demand, TLS policy and protocol state remain on existing CNet and
host APIs. Install owned receive handlers through the manager adapter so its
callback guard also covers transferred slices. `seal` blocks admission without
closing. `request_close` seals and schedules closes only for this manager's
subset. `advance(budget)` visits at most that many records round-robin, without
polling or waiting; rejected closes retain their obligations for retry. The
snapshot reports runnable work separately from drained obligations. Destroy is
owner-only and returns EBUSY until drained; it never stops the borrowed client.
Keep that client initialized through manager destruction.

CHTTP maps one manager and one optional admission inbox to each existing owner
lane. HTTP/1 deferred responses and HTTP/2 deferred
streams delay context release. FlowMQ maps one manager to each socket client;
queued parts delay peer context release, and external owners drive explicit
close progress. These integrations retain protocol state in their consumers.

This additive stage changes no wire format or base CNet ABI and adds no external
dependency. It trades one bounded record per managed context for explicit
lifetime accounting; no performance gain is claimed. Rollback disables the
consumer's explicitly configured manager while retaining raw CNet calls.
Placement and retention policies remain later stages. The owner-local manager
creates no worker, timer or cross-thread queue; handoff is a separate opt-in helper.

Server/Client strategy composition and SG Owner progress integration are tracked
under [CNet strategy umbrella #1050](https://github.com/qigao/salts/issues/1050).
The CNet x Actor integration path is discontinued; CNet handles network
lifecycle without any CFlow Actor, Mailbox or Scheduler dependency.

The formal `cnet_manager_test` covers full capacities, immediate admission
failure, real TCP terminal/owned receive callbacks, detached ownership, stale
identities, owner affinity, callback reentrancy, explicit holds and subset close.
Downstream suites cover HTTP deferred work and TCP/TLS messaging integration.

### Optional final-owner handoff

`<cnet/handoff.h>` in `Salts::CNet` supplies a bounded
MPSC admission inbox. It extracts CHTTP's detached-stream queue and final-owner
credits without moving the listener, owner threads, backend, protocol state or
service stop machinery into CNet. FlowMQ's caller-driven single-owner path does
not need this queue. A host chooses a destination before reserving its credit;
no established connection moves between owners.

Each inbox preallocates `connection_capacity` generation-checked records and
`queue_capacity + 1` index entries (the extra ring entry distinguishes full from
empty). Queue capacity must be positive and no greater than connection capacity;
all size arithmetic is checked. Credits cover RESERVED + QUEUED + TAKEN, while
the queue limit covers only QUEUED. They are independent of the manager's
owner-local record/context budget and the raw client's transport capacity.

The lifecycle is `reserve -> publish -> take -> release`, or `reserve -> release`.
Publication copies the peer endpoint and moves the descriptor only on success.
Every rejected publication leaves both descriptor and reservation with the
producer. Taking an item returns queue space but keeps its credit until the
owner releases the ticket; in CHTTP that is transport terminal. A taken stream
must be adopted or closed exactly once. Adoption through `cnet_manager_adopt`
consumes the detached stream on any valid reserved attempt, including immediate
TCP/TLS failure. The host then returns handoff credit on failure and continues
manager context retirement. The final owner retains its immutable TLS policy;
no borrowed TLS/configuration pointer is stored in an inbox entry.

For example, on a producer with an active `accepted` stream and initialized inbox:

```c
cnet_handoff_ticket ticket = {0};
int status = cnet_handoff_reserve(&inbox, &ticket);
if (status == SALTS_OK) {
  status = cnet_handoff_publish(&inbox, ticket, &accepted);
  if (status != SALTS_OK) {
    int release_status = cnet_handoff_release(&inbox, ticket);
    if (release_status != SALTS_OK) return release_status;
  }
}
if (status != SALTS_OK) {
  /* accepted is still owned here: close it or retry per host policy. */
  return status;
}
/* accepted is empty. Wake the final owner using the host's existing mechanism.
 * Even if that wake fails, do not republish, close or release this ticket.
 * Arrange bounded owner progress or service shutdown to drain the inbox. */
```

The owner uses `cnet_handoff_take` to obtain a ticket and detached stream, then
its existing TCP/TLS adoption and real terminal path. `seal` atomically rejects
new reserves/publications but permits take/release so all old obligations can
finish. It neither revokes outstanding producer reservations nor closes sockets.
The snapshot reports the three credit states coherently in O(1). All operations
are allocation-free after init, with one mutex protecting the lifecycle table
and the existing Core byte ring; no ring view escapes that critical section.
This deliberately favors a small admission-only critical section over an extra
lock-free publication protocol. No throughput or latency improvement is claimed.
The helper does not participate in steady send/receive data paths.

**Shutdown requires explicit producer quiescence.** Seal admission, arrange
completion/join of all producer calls **including their publish-to-wake tails**,
drain/cancel queued entries, and finish taken connections and credits before
destroying the inbox or any wake target. `snapshot.drained` proves only that
credits are returned, not that a producer cannot still issue a wake. Tickets
are values, not retained references: no ticket/raw pointer may be used after
inbox destruction. CHTTP enforces this with its existing `listener_done` barrier
before owner backend teardown, then joins all threads before storage destruction.

The formal `cnet_handoff_test` covers capacity one/full/overflow, foreign and
stale tickets, FIFO wrapping, rejected publication ownership, each sealed state,
MPSC publication, seal races, failed wake and delayed wake-tail shutdown.
`cnet_manager_test` also verifies adoption and invalid TLS adoption after handoff.
CHTTP's existing topology, stalled-owner, TLS, HTTP/2 and deferred suites exercise
the real consumer. This additive helper changes no wire format or raw CNet ABI.
Rollback drains active work, restores the consumer's old admission adapter and
removes its optional helper use; live inbox/backend replacement is unsupported.

## Base API

Include `<cnet/cnet.h>`, initialize one bounded `cnet_client_config`, then use:

- `cnet_connect` with `tcp://host:port`, `tls://host:port`, `udp://host:port`,
  `pipe://name`, `ipc://name` (Windows), `ipc:///absolute/path` (POSIX),
  or Linux `vsock://CID:PORT`;
- `cnet_send_buffer` for retained contiguous payload ownership;
- `cnet_send_slice` / `cnet_send_slicev` for retained subrange and scatter/gather ownership;
- `cnet_send_buffer_and_close` / `cnet_send_slicev_and_close` for retained final writes;
- `observer.on_send` to observe completion before admitting the next ordered
  write on that connection;
- `cnet_receive` to add explicit receive demand;
- `cnet_set_receive_slice_handler` to opt one connection into explicit
  application-owned receive slices without changing the legacy observer layout;
- `cnet_close` for one connection;
- `cnet_client_poll` to advance I/O and invoke callbacks on the caller;
- `cnet_client_stop` followed by `cnet_client_destroy` for shutdown.

## Additive transport capabilities (#999 / #1000)

These capabilities are present in the source tree, not a claim about a published
2.1 SDK. Existing config layouts, `pipe://` semantics and owned APIs are preserved.

| Header capability | Implemented surface |
| --- | --- |
| `CNET_DATAGRAM_EXTERNAL_PROGRESS_VERSION == 1` | `cnet_datagram_init_external`, `advance_external`, `route_external_completion`, `stop_external` in `<cnet/cnet.h>` |
| `CNET_IPC_VERSION == 1` | Versioned listener, typed detached child, wait snapshot, stop/drain, `cnet_client_adopt_ipc` in `<cnet/ipc.h>`; `cnet_connect` accepts the local IPC URI |
| `CNET_WEBSOCKET_TAGGED_SEND_VERSION == 1` | Versioned `cnet_websocket_init_tagged` policy, copied `send_tagged`, bounded `advance`, one logical-message terminal in `<cnet/websocket.h>` |

### Borrowed UDP progress

One fixed owner initializes the backend and each datagram, advances deferred
rearms, observes one batch, then routes **every** completion to its consumer.
Routing first matches the retained request `(slot,generation)`, then validates
the local tag and endpoint. A matched malformed terminal is consumed and returns
an error; finish routing the rest of the batch. Never route across backends.
The bounded scan costs O(send_capacity + 1) per instance and completion.

Budget one endpoint and at most `send_capacity + 1` active requests per UDP
instance, plus the other consumers' budgets. Borrowed mode allocates no private
completion batch. Full admission returns `ENOBUFS`; a deferred receive rearm can
be retried with `advance_external` after host capacity recovers. Receive views
remain borrowed only through their callback, including reentrant demand updates.

`stop_external` cancels only that datagram's requests and never observes or
closes the host backend. An already-observed, not-yet-routed terminal still owns
its storage and callback obligation: `ENOENT` from cancel in this race is not
permission to free it. Continue routing until `out_stopped` is true; actual
errors are reported independently of that flag. Old `poll`/blocking `stop`
return `ENOTSUP` in borrowed mode. See the executable
[external UDP tests](tests/cnet_datagram_external_test.c).

### Local IPC lifecycle

The [public IPC header](include/cnet/ipc.h) defines every parameter and ownership
result. A typical sequence, with error handling at each call, is:

```c
cnet_ipc_listener listener = {0};
cnet_ipc_accepted child = {0};
cnet_ipc_listener_config cfg = {
    sizeof(cfg), CNET_IPC_VERSION, backend_kind, local_uri,
    8, 16, 4096, 4096
};
int status = cnet_ipc_listener_init(&listener, &cfg);
/* Each host turn: advance, detach available children, then snapshot wait sources.
 * Transfer child ownership only after successful bounded queue publication. */
status = cnet_ipc_listener_advance(&listener, 8, &events);
status = cnet_ipc_listener_accept_detached(&listener, &child);
/* On the destination client owner, after a successful detach: */
status = cnet_client_adopt_ipc(client, &child, &observer, &connection);
/* child is empty after every valid consuming attempt, including failure. */
```

The [IPC tests](tests/cnet_ipc_test.c) provide complete runnable connect/adopt,
duplex send/receive, bounded wait, failure cleanup and two-owner examples.
Windows accepts retain their OVERLAPPED/event in NativeIPC until completion;
only the destination client first associates the child with IOCP. POSIX children
are nonblocking Unix streams. Listener-owned pending plus ready children cannot
exceed `child_capacity`; detached children require a separate host lane budget.
Pipe buffer sizes are Windows OS hints; POSIX data-buffer budgets belong to the
destination client. Nondefault TCP socket policy rejects IPC connect with `ENOTSUP`.

`wait_sources` exposes borrowed pending Windows events or the POSIX readable
listener fd, plus an immediate-work flag. Refresh after each mutation; unregister
before events/fds can close. The host supplies its own command/stop wake and
honors native wait limits. This API does not merge Windows events into IOCP or
provide a hidden thread. Stop cancels pending accepts; advance drains them, and
`out_stopped` determines when destroy is safe. Detached children survive stop.
POSIX pathnames are never unlinked by the library, even on bind/listen failure;
the caller must manage only paths it owns. IPC adds no authentication identity.

### Tagged WebSocket output

Opt in only with a writer whose `SALTS_OK` means complete local transport success.
An asynchronous writer retains the frame and returns `WRITE_PENDING`, then calls
`write_complete` exactly once at its authoritative terminal. Legacy adapters that
return success after copying into a queue must keep the ordinary WS init path.

`send_tagged` copies one complete validated message and reserves one terminal;
it performs no write or callback. `advance(max_frames, &events)` drives bounded
frame attempts, including control responses, and dispatches at most one logical
terminal after admission has returned. One message buffer, one output frame and
one terminal record stay bounded; busy output is retried, asynchronous output is
never resubmitted. Final success requires all data fragments, not control writes.
Close/error still settles the accepted tag once. Destroy remains busy until the
tag is dispatched and native output borrows have drained. Graceful `close` returns
`EBUSY` while a tag is active: keep advancing and retry under the adapter's close
deadline. `transport_closed`
must only be called after the adapter has established that drain.

CHTTP's H1/H2 authoritative-terminal adapters remain a downstream integration.
In particular, H2 stream backpressure must retain bounded stream DATA/credit while
continuing connection reads and control-frame processing; do not pause the whole
connection while waiting for a blocked WS write. HTTP policy does not enter CNet.

Validation on Windows IOCP: Debug/ASan and Release each passed all 40 selected
CNet/NativeIPC regression targets. In an x64 VS developer shell,
the focused capability checks can be repeated with:

```powershell
cmake --build --preset win-dev-user --target cnet_datagram_external_test cnet_ipc_test cnet_websocket_tagged_test native_ipc_test
ctest --preset win-dev-user -R '^(cnet_(datagram_external|ipc|websocket_tagged)|native_ipc)_test$' --output-on-failure
```

Linux epoll and explicit io_uring variants, macOS kqueue (GCC and AppleClang),
Windows Release, and the six-platform SDK package passed
[the cross-platform acceptance run](https://github.com/qigao/salts/actions/runs/37628231430).
Android and iOS evidence is cross-compilation/packaging, not device execution.
That preliminary package used 2.1.2; the additive release candidate is 2.2.0 and
must be rebuilt from its own exact commit before publication. Existing CHTTP
Windows Debug/ASan regression passed 93/93 tests against the installed SDK;
this proves compatibility, not completion of the downstream tagged adapter.

`cnet_transport_owner_benchmark` measures four independent duplex pairs on
1/2/4 owners with a fixed total of 4096 measured 1024-byte messages per run.
Each owner owns its backend and both ends of its assigned pairs. IPC rendezvous
is performed before warmup, then the listener is stopped before payload timing;
this measures established connections, not a central acceptor's saturation or
handoff throughput. A separate dual-owner IPC test covers detached handoff.
The benchmark uses 32 warmup exchanges and three independent lifecycle samples.
Steady wall time spans the earliest owner start through the latest owner finish;
CPU sums owner-thread usage. Windows also reports thread cycles because CPU time
has coarse resolution. P95/P99 measure complete bidirectional pair exchanges,
including byte validation. Setup and drain durations are reported separately.
Zero loss is reported only after all expected receive bytes and send terminals
are verified; timeout, corruption or rejected admission fails the measurement.
Configured payload budgets exclude allocator/native metadata; `rss_at_ready` is
a process RSS sample, not a peak or a hard memory limit. Owner count increases
per-owner metadata and command capacity even though traffic is fixed.

Run from the matching developer environment (the preset normally excludes the
benchmark label, hence the explicit empty exclusion):

```powershell
cmake --build --preset win-release-user --target cnet_transport_owner_benchmark
ctest --preset win-release-user -LE '^$' -R '^cnet_transport_owner_benchmark$' -V
```

Release preparation runs this same CTest workload on IOCP, epoll, io_uring and
kqueue and retains its raw output in `io-benchmark-*` artifacts. No multicore
speedup or production capacity is inferred from this loopback workload.

## Execution ownership and multicore composition

One `cnet_client` is one connection/progress owner. The ordinary
`cnet_client_init()` + `cnet_client_poll()` mode remains caller-driven:
callbacks execute inline on that one poll owner and CNet creates no hidden
worker pool.

Multicore execution is **host-composed**, not an implicit multi-shard mode
inside one client. A runtime that already owns NativeIO progress lanes creates
one backend/client pair per owner lane with `cnet_client_init_external()`:

```text
owner lane 0
  NativeIO backend 0
      |
  cnet_client_init_external(client 0)
      |
  fixed connections + callbacks on lane 0

owner lane 1
  NativeIO backend 1
      |
  cnet_client_init_external(client 1)
      |
  fixed connections + callbacks on lane 1
```

The host selects the client/owner when a connection is admitted. That
connection never migrates. The existing data-plane calls stay unchanged:

```c
cnet_connect(client, ...);
cnet_send_buffer(client, connection, ...);
cnet_receive(client, connection, ...);
cnet_close(client, connection);
```

There is deliberately no public shard id, `shard_count`, CNet worker pool,
work stealing, or connection migration. CNet also does not need
`native_io_sharded` merely to obtain multicore scaling; independent fixed CNet
owners over direct NativeIO backends are sufficient.

For external-progress clients, the embedding owner performs the canonical host
loop:

```text
cnet_client_advance_external()
cnet_client_external_timeout()
native_io_backend_observe()
cnet_client_route_external_completion() for the observed batch
return to the host loop
```

Do not add a second `advance_external()` immediately after routing a successful
batch; the next loop iteration already performs the next control/session/deadline
pass.

The retained-TCP topology evidence behind this policy is:

- #680: two independent ordinary CNet owners scale real TCP work; 64 KiB
  distinct-core throughput reached about 2x the one-owner serial baseline;
- #684: one private shared multi-owner engine retained that owner-local scaling,
  proving the engine itself is not the bottleneck;
- #690: a hidden central-callback compatibility facade was rejected. Requiring
  each logical operation to cross a command hop and then return through central
  event hops retained only about 0.63-0.73x of the owner-affine distinct-core
  throughput and increased tail/CPU cost;
- #695/#713: host-driven external progress preserves the owner-affine class when
  the host loop is shaped correctly. Repeated epoll 64 KiB external/ordinary
  throughput was about 0.99x on one CPU/SMT and 1.04x on distinct cores;
  io_uring remained similarly near parity.

Therefore multicore CNet is a **throughput/concurrency execution policy**, not a
universal low-latency default. Small-message latency and CPU efficiency must
still be measured for the workload.

An io_uring-backed owner must create its NativeIO backend on the final owner
thread. NativeIO uses `IORING_SETUP_SINGLE_ISSUER`, and backend initialization
arms the wake poll; constructing the ring on one thread and later driving it
from another violates the kernel single-issuer contract.

`command_capacity` bounds the generic control/deferred command mailbox. `command_buffer_bytes`
bounds copied payloads that still belong to that control plane; stream data-plane sends do not
copy payload bytes into this mailbox. `max_send_bytes` remains the per-logical-send bound, while
retained stream writes are bounded by their fixed owner-local write slots. Capacity exhaustion
returns `SALTS_ENOBUFS`.

`event_capacity` likewise bounds event descriptors while `event_buffer_bytes`
bounds their aggregate copied payload. Event payloads and per-connection receive
buffers are allocated only while in use; configuring a large per-message bound
therefore no longer reserves its product with every event or connection slot.

TCP, VSOCK, and Pipe deliver byte chunks. Connected UDP delivers one datagram per
receive callback. The ordinary `observer.on_receive` view is borrowed only until
its callback returns. Applications that need receive bytes to outlive the callback
can install `cnet_set_receive_slice_handler()` for that connection. While
installed, owned delivery takes precedence over the borrowed callback; changing
the handler while receive demand is outstanding returns `SALTS_EBUSY`, so one
admitted demand cannot change lifetime contracts in flight.

For each connection CNet now keeps the receive scratch itself as a canonical
`mem_buffer_t` from `mem_global()`. Plaintext NativeIO reads and TLS plaintext
reads write directly into `mem_buffer_data()`; a RECEIVE event carries that same
backing through the dispatcher. The borrowed callback still sees only a
callback-scoped byte view. The owned handler creates a `mem_slice_t` over the
same backing, so there is no additional CNet payload memcpy between the producer
receive scratch and the public owned callback.

After successful publication, CNet reuses the same receive backing when nobody
retained it. If an owned callback or fallback event queue retains the backing,
the session moves that reference into one lazy spare slot and rearms on another
buffer, so future network progress cannot overwrite application-owned bytes.
For the common case where at most one slice remains live across callback return,
the two buffers then ping-pong: once the application releases the previous slice,
its spare refcount returns to one and CNet promotes it as the next active backing
without allocation. If multiple slices remain live concurrently, CNet drops only
its old spare reference and allocates another active buffer; user-owned slices
stay valid. Thus CNet itself caches at most one spare per session.

The fallback event queue retains canonical DATA backing rather than copying it
into its private payload pool, while non-DATA payloads such as ALPN keep the
existing copied path.

The application releases owned slices with `mem_slice_release()`. Because the
canonical backing belongs to the process-global Salts pool rather than the
client, an already delivered slice remains valid across later poll calls,
connection close, `cnet_client_stop()`, and `cnet_client_destroy()`. An empty
UDP datagram is represented by an empty slice with no backing reference.

This is a **CNet producer-to-callback zero-copy ownership path, not kernel
zero-copy**. The operating system still writes into user memory, and TLS still
performs the required ciphertext-to-plaintext transform before the canonical
plaintext backing is published.

TLS delivers verified encrypted byte streams through the same send/receive
contract. The same header also exposes bound UDP, the KCP session engine, and
their unified packet endpoint; WebSocket remains in `<cnet/websocket.h>`. CNet parses
TCP, TLS, and UDP URIs through Salts UriParser and then applies
transport-specific constraints: network URIs require an explicit port and reject
userinfo, path, query, and fragment components instead of accepting truncated or
ambiguous input. Bracketed network hosts must be valid IPv6 literals;
bracketed names, IPv4 addresses, and malformed IPv6 return `SALTS_EINVAL`,
while unsupported IPvFuture literals return `SALTS_ENOTSUP`. These failures
occur before connection admission or DNS lookup. VSOCK uses a separate strict
decimal `uint32` CID/port parser and never enters DNS. Pipe is a scheme-specific
IPC endpoint rather than a network authority, so its bounded name after
`pipe://` is preserved byte-for-byte.

## Socket tuning

`cnet_stream_socket_options` is the public, versioned TCP policy shared by
outgoing TCP/TLS connections and detached listener sockets consumed by that
final client owner. Set it on a stopped `cnet_client` with
`cnet_client_set_stream_socket_options()`; listener owners use
`cnet_listener_init_ex()` with versioned `cnet_listener_options`. The policy
exposes OS receive/send buffers, explicit `TCP_NODELAY` via `nodelay`,
keepalive enable plus idle/interval/probe count, and linger. Generic native
socket adoption preserves an externally configured live policy; only
`cnet_accepted_stream` handoff applies the final client's configured policy.
Direct `cnet_listener_accept*()` calls, including TLS accept, preserve the
listener's TCP policy without applying the receiving client's future-connection
policy.
`cnet_datagram_config.reuse_port` exposes the same listener-port
sharing decision for UDP and the unified UDP/KCP packet endpoint.

Zero-valued buffer and timing fields preserve platform defaults. `nodelay=0`
keeps the platform's Nagle policy; `nodelay=1` requests `TCP_NODELAY` and is
appropriate for latency-sensitive protocols that may emit one logical frame
through multiple native write windows. Keepalive detail without `keepalive`
is invalid; enabled linger with zero milliseconds is an abortive close. CNet
copies every policy into its owner command, so no caller
pointer is retained. Unsupported platform options return `SALTS_ENOTSUP` before
the socket is published, and invalid sizes or combinations fail without a
silent fallback.


## Bounded write ownership

Ordinary stream-send payload lifetime lives in the dedicated owner-local write
queue. The queue has a fixed global slot bound plus per-connection FIFO chains.
Every public stream data-plane write retains caller-provided `mem_buffer_t` ownership
directly or retains the unique backing buffers referenced by canonical `mem_slice_t`
ranges. CNet does not maintain a copied stream-payload queue or copied-byte budget.

`cnet_send_slicev()` preserves scatter/gather ownership without flattening.
`cnet_send_slicev_and_close()` uses the same retained ownership for the final
logical write and closes only after that vector settles. CNet validates
1..`CNET_RETAINED_VECTOR_MAX` canonical `mem_slice_t` ranges (currently 32),
retains each unique backing buffer once, copies only fixed range descriptors
into the owner-local write slot, and preserves those ranges until the single
logical terminal completion.

For plaintext stream transports, the retained ranges flow into NativeIO
scatter/gather. One NativeIO submission exposes at most
`NATIVE_IO_VECTOR_MAX` spans (currently 16); a larger CNet logical vector
advances through successive native span windows without flattening or
publishing an intermediate CNet send terminal.

TLS keeps the same retained-vector ownership but necessarily transforms
plaintext before NativeIO sees it. CNet feeds retained spans to the TLS engine
in order, advances only accepted spans, then sends the generated ciphertext
through the normal TLS/NativeIO path. It does not flatten plaintext into copied
CNet storage. TLS record and receive-callback boundaries are intentionally not
preserved as vector boundaries. Small, highly fragmented TLS vectors may cost
more TLS write/record operations; callers should avoid unnecessary
fragmentation when throughput matters. UDP retained slicev remains unsupported;
there is no hidden flatten fallback.

The larger logical range bound remains fixed-memory. On a 64-bit build, raising
the retained range/owner arrays from 16 to 32 adds approximately 512 bytes per
owner-local write slot. Write slots are preallocated at
`write_capacity_per_shard` (derived from the client command capacity), so the
incremental metadata is approximately
`512 * command_capacity * shard_count` bytes and does not grow with payload
size or runtime duration. For a 16-slot shard this is about 8 KiB.

## Canonical session-state authority

The generation-checked `cnet_session_table` is the sole CNet connection
lifecycle authority. The client record does not mirror CONNECTED/OPEN as an
independent boolean. Steady-state send/receive admission delegates lifecycle
validation to `cnet_shards`, which checks the canonical session state.

The remaining client booleans are deliberately narrower:

- `active` owns the public-handle/observer slot mapping, not connection state;
- `write_pending` is the temporary one-write admission reservation tracked
  for #479;
- receive demand is a bounded delivery counter, not lifecycle state;
- close/TLS command-pending flags only reserve same-owner command admission
  until #478 removes that mailbox hop.

TLS inspection and upgrade readiness query the canonical session state instead
of a duplicated client CONNECTED flag. Terminal NativeIO/session state remains
authoritative throughout close, failure and TLS-handshake transitions.

## Linux VSOCK

CNet supports plaintext Linux `AF_VSOCK` `SOCK_STREAM` over the epoll and
io_uring NativeIO backends. An outbound URI has exactly the form
`vsock://CID:PORT`; both values are unsigned decimal 32-bit integers in host
byte order. Signs, whitespace, userinfo, paths, queries, fragments, overflow,
`CNET_VSOCK_CID_ANY`, and `CNET_VSOCK_PORT_ANY` are rejected for outbound
connections. Non-Linux platforms return `SALTS_ENOTSUP` before reserving a
connection or publishing a callback, with no TCP or Pipe fallback.

Servers use a versioned `cnet_vsock_listener_config` with
`cnet_listener_init_vsock()`. The ANY constants are valid for listener bind;
`cnet_listener_vsock_local()` queries the current full-width CID and port with
`getsockname()` on every call. `cnet_listener_accept_vsock_peer()` returns a
copied `cnet_vsock_peer`; the shorter `cnet_listener_accept_vsock()` omits that
metadata. The common wait/close/destroy lifecycle is unchanged. TCP/TLS accept
functions reject a VSOCK listener, and VSOCK accept functions reject a TCP
listener.

`cnet_client_adopt_vsock()` consumes a connected native VSOCK stream. Accepted
and directly adopted sockets transfer exactly once: failed admission closes the
native socket and leaves the public connection zero. CNet applies the same
bounded byte send/receive demand, one-write-at-a-time, timeout, cancellation,
callback ordering, and stop/drain rules as TCP. The current stream tuning API
expresses TCP policy, so any requested receive/send buffer, keepalive, or linger
setting is rejected for VSOCK with `SALTS_ENOTSUP` instead of being ignored.

Phase 1 does not provide TLS-over-VSOCK. Passing TLS policy with a `vsock://`
URI is invalid because a CID is not a certificate identity. VSOCK is only a
transport and does not itself authenticate or encrypt application data. Live
migration disconnects an active stream and may change the local CID; CNet
reports the normal terminal stream failure and does not reconnect implicitly.
The Linux-only `cnet_vsock_integration_test` exercises ANY-CID bind, local-CID
connect, accept, ownership, and bidirectional bytes, and is explicitly skipped
when the kernel or sandbox does not expose AF_VSOCK.

## Unified UDP/KCP packet endpoint

`cnet_packet_endpoint` is the application-facing interface for bound UDP and
KCP. Select `CNET_PACKET_UDP` or `CNET_PACKET_KCP`; both use the same explicit
session open, copied send, poll, session close, stop, and destroy operations.
Sessions are generation-checked values indexed by a fixed-capacity `(peer,
conversation)` table. UDP requires conversation zero. Plain KCP requires a
non-zero conversation and retains message boundaries while adding ordering,
ACKs, retransmission, and fragmentation. Authenticated KCP is selected
explicitly with `CNET_KCP_SECURITY_PSK_V1`; callers open it with conversation
zero, observe `CONNECTING`, and receive `OPEN` only after the PSK handshake
derives the read-only conversation id.

An unknown inbound key is admitted only when `observer.on_admit` returns
`SALTS_OK`. This decision is synchronous on the poll owner; a missing callback
rejects unknown peers. Capacity exhaustion reports `SALTS_ENOBUFS` rather than
growing or evicting live sessions. `cnet_packet_send()` remains admission-only:
it means that CNet copied and admitted the message and does not claim transport
completion or remote delivery.

Consumers that need authoritative per-message settlement use
`cnet_packet_endpoint_init_ex()` with `cnet_packet_terminal_config`, then call
`cnet_packet_send_tagged()`. The terminal configuration is size/versioned and
reserves a fixed logical-operation capacity before the endpoint is published.
Pool exhaustion returns `SALTS_ENOBUFS`; a failed admission never produces a
callback. The opaque caller tag is copied and returned exactly once. UDP settles
from its NativeIO datagram terminal. KCP, authenticated KCP, and FEC settle only
after KCP's cumulative acknowledgement point passes the logical message's last
segment; individual emitted or retransmitted UDP packet completions never settle
the logical send. Tagged KCP rejects stream mode because it cannot retain these
message boundaries. Explicit close/stop cancels unacknowledged KCP sends with
`SALTS_ECANCELED`, drains UDP native terminals, and dispatches logical terminals
before the corresponding CLOSED state. Asynchronous socket failures and KCP
output backpressure also remain visible through the generation-checked
`on_error` callback.

The lower-level `cnet_datagram` API remains available for protocols that need
raw peer-addressed UDP. Each successful send retains its caller tag and reports
that tag exactly once in the terminal send callback. `cnet_kcp` remains
available as a socket-independent engine for applications with an existing
datagram transport. Its `output` callback is borrowed, `input` consumes one
borrowed wire packet synchronously, and `update/check` make timer ownership
explicit.

Each `cnet_kcp_send()` call may add at most 127 new segments after any
stream-mode tail merge. Exceeding this protocol limit returns `SALTS_EMSGSIZE`
before retaining input or changing an already queued stream tail; the separate
conservative `send_segment_capacity` admission bound remains unchanged.

Plain `cnet_kcp` provides reliability, not confidentiality or peer
authentication. `cnet_secure_kcp` and the packet endpoint's explicit PSK v1
mode add the CoroNet-compatible authenticated handshake, XChaCha20-Poly1305
records, replay rejection, and Reed-Solomon FEC. There is no plaintext fallback
or wire sniffing. Unknown peers reach `on_admit` only after their client hello
passes a stateless PSK MAC check.

FEC `max_payload_bytes` must not exceed 65533: parity shards include the
two-byte data length and must fit the 16-bit wire length field. Packet endpoint
datagram send/receive limits must cover `max_payload_bytes + 48` bytes (30-byte
header, two-byte parity length metadata, and 16-byte MAC); undersized limits
are rejected during initialization.

## WebSocket session engine

`<cnet/websocket.h>` provides a transport-independent RFC 6455 session after a
successful HTTP Upgrade. It handles text/binary messages, fragmentation,
ping/pong, close handshakes, client masking, strict UTF-8 validation, and role
masking rules. Input chunks may split or coalesce frames; event payloads are
borrowed only until their callback returns.

Initialization allocates fixed-capacity input, reassembled-message, and
single-frame output storage. `max_frame_bytes`, `max_message_bytes`, and
`max_buffered_input_bytes` are mandatory. A write callback returning
`SALTS_EBUSY` retains exactly one complete frame; the owner calls
`cnet_websocket_flush()` before feeding or sending more data. Other write errors
move the session to `CNET_WEBSOCKET_FAILED` and are available through
`cnet_websocket_last_error()`. A peer Close commits `CLOSING` before its event
callback and becomes `CLOSED` only after any retained echo Close is transferred.

The engine owns protocol state but performs no socket I/O and creates no thread.
It is single-owner and can therefore be driven by a CNet callback, Executor,
Actor mailbox, or another ordered byte-stream adapter. CHTTP remains responsible
for HTTP/1.1 Upgrade routing/header validation and HTTP/2 extended CONNECT.
`cnet_connect()` does not accept `ws://` or `wss://`; applications use CHTTP's
WebSocket routes and clients, which adapt those sessions onto this engine.

The following complete adapter example sends one server-side text frame into a
bounded transport sink:

```c
#include <cnet/websocket.h>

#include <string.h>

typedef struct frame_sink {
  unsigned char bytes[270];
  size_t size;
} frame_sink;

static int write_frame(void *user, const uint8_t *data, size_t size) {
  frame_sink *sink = (frame_sink *)user;
  if (size > sizeof(sink->bytes)) return SALTS_EMSGSIZE;
  memcpy(sink->bytes, data, size);
  sink->size = size;
  return SALTS_OK;
}

static void on_event(void *user, cnet_websocket *websocket,
                     const cnet_websocket_event *event) {
  (void)user;
  (void)websocket;
  (void)event;
}

int main(void) {
  cnet_websocket websocket = {0};
  frame_sink sink = {0};
  cnet_websocket_config config = {
      .size = sizeof(config),
      .role = CNET_WEBSOCKET_SERVER,
      .max_frame_bytes = 256,
      .max_message_bytes = 512,
      .max_buffered_input_bytes = 1024,
      .write = write_frame,
      .on_event = on_event,
      .user = &sink,
  };
  int status = cnet_websocket_init(&websocket, &config);
  if (status == SALTS_OK) status = cnet_websocket_send_text(&websocket, "hello", 5);
  (void)cnet_websocket_destroy(&websocket);
  return status == SALTS_OK && sink.size != 0 ? 0 : 1;
}
```

## TLS transport

TLS is an opt-in bounded transport implemented by CNet over the same NativeIO
TCP endpoints. Set both `cnet_client_config.tls_io_buffer_bytes` (at least
`CNET_TLS_MIN_IO_BUFFER_BYTES`) and `tls_handshake_timeout_ms` to admit TLS
connections. Leaving both zero preserves a TLS-free client and makes a
`tls://` connect fail with `SALTS_ENOTSUP`.

The repository manifest selects the canonical GmSSL overlay. CMake consumes
`GmSSL::GmSSL` only as a private implementation dependency; Salts never
exports that target in CNet's public CMake link interface. The overlay packages
GmSSL statically, so the Windows native SDK ships no private TLS-provider DLL
beside `cnet.dll`.

The pinned provider includes bounded encrypted TLS 1.3 handshake reassembly and
variable-width ECDSA DER scalar decoding fixes (GmSSL 3.2.0 port revision 4).
Rebuild the SDK with matching provider headers and libraries; the connection
layout changed internally. `cnet_tls_handshake_framing_test` and
`cnet_tls_signature_test` cover record boundaries, transcript progression,
malformed records, and valid short-scalar CertificateVerify signatures.

`cnet_connect()` accepts either a one-shot `cnet_tls_client_config` or a reusable
`cnet_tls_client`; the two fields are mutually exclusive. NULL uses the platform
trust store and the URI host as the verified identity. An explicit configuration
can select CA file/path, client certificate/key, SNI/identity, and an ordered
ALPN offer. `cnet_tls_client_init()` builds immutable GmSSL provider contexts and
consumes all input synchronously. A successful connect retains that context, so
the public wrapper may be destroyed after admission while the connection remains
valid. Certificate-chain and hostname/IP verification are mandatory; CNet
exposes no insecure mode and never retries `tls://` as plaintext.

After CONNECTED, `cnet_tls_negotiated_alpn()` copies the selected protocol. It
can be called from the CONNECTED callback because CNet records ALPN before
invoking user code. No overlap returns `SALTS_ENOENT`; protocol layers such as
HTTP/2 must treat that result as a policy decision rather than assume `h2`.

The TLS protocol version and cipher suite remain automatically negotiated.
`cnet_tls_negotiated_version()` and `cnet_tls_negotiated_cipher()` expose the
effective session metadata after CONNECTED without exposing the underlying TLS
implementation. The policy remains TLS 1.2 minimum with no maximum cap, so a
TLS 1.3-capable peer is negotiated automatically while TLS 1.2 peers remain
supported.
`cnet_tls_peer_certificate_sha256()` copies the verified peer leaf certificate
fingerprint while the TLS connection remains open; a server session whose peer
did not present a client certificate returns `SALTS_ENOENT`.
`cnet_tls_server_end_point_binding()` exposes the distinct RFC 5929
`tls-server-end-point` semantic value for SASL-style channel binding. It hashes
the exact verified peer leaf DER certificate with the certificate signature
digest, upgrading MD5/SHA-1 signatures to SHA-256 as required by RFC 5929.
This API is intentionally separate from `cnet_tls_export_channel_binding()`,
which remains the RFC 9266-style `EXPORTER-Channel-Binding` contract; callers
must not substitute one binding type for the other.

Servers that need transport identity can use `cnet_listener_accept_peer()` or
`cnet_listener_accept_tls_peer()`. They preserve the existing accept lifecycle
while also returning an owning `cnet_stream_peer` value containing the remote
IPv4/IPv6 address, scope id and host-order port. The original accept helpers
remain source-compatible wrappers when endpoint metadata is not needed.

Servers initialize one reusable `cnet_tls_server`, accept sockets with
`cnet_listener_accept_tls()`, and destroy the public context after closing
admission. Accepted sessions retain their context, but accept and destroy on
the same wrapper must not overlap. Optional client authentication requires an
explicit CA source and validates the client certificate during the handshake.

Protocols such as SMTP, IMAP, and POP3 can upgrade an already connected
plaintext TCP stream without reconnecting. After the application has completed
its plaintext negotiation and observed the final send/receive completion, call
`cnet_start_tls()` on the client side and `cnet_start_tls_server()` on the server
side. The connection handle remains unchanged. CNet publishes
`CNET_CONNECTION_TLS_HANDSHAKING`, rejects new send/receive work during the
transition, then publishes CONNECTED again only after certificate and hostname
verification succeeds. A handshake error is terminal and never restores the
plaintext stream.

TLS upgrade admission is intentionally strict: the stream must be a quiescent,
connected `tcp://` session with no pending send, receive, close, or earlier
upgrade. `cnet_start_tls_options` is consumed synchronously. A one-shot config
is converted to an immutable context before admission; a reusable client/server
context is retained by the bounded command and may be destroyed by the caller
after success. Queue exhaustion is reported immediately, and the existing
client TLS buffer and handshake-timeout bounds apply unchanged.

Each TLS session owns bounded ciphertext input/output rings plus fixed-capacity
transport receive/send and plaintext scratch buffers. The transport receive
buffer remains reserved until its NativeIO completion is consumed; ordinary
TLS reads, no-demand probes, and partially consumed plaintext use separate
storage. The plaintext buffer adds `tls_io_buffer_bytes` per TLS session inside
the existing engine allocation, with no allocation in the receive hot path.
GmSSL consumes and emits ciphertext only through CNet's
external-I/O callbacks; NativeIO remains the sole transport/socket authority.
Handshake, encrypted reads/writes, ALPN, cancellation, and `close_notify`
stay on the CNet progress owner; TLS creates no worker thread. Handshake timeout is reported with stage `handshake`, malformed or
truncated TLS never falls back to plaintext, and user close during a handshake
cancels the in-flight transport without publishing CONNECTED.

The adapter uses GmSSL's bounded `TLS_IO` callback contract and performs DNS
hostname or IP-address SAN verification without transferring socket ownership
to the provider. ALPN wire behavior follows
[RFC 7301](https://www.rfc-editor.org/rfc/rfc7301).

The stacked, **unreleased** RFC 8998 ShangMi TLS 1.3 profile is explicitly
opt-in. It requires SM2 identity/curve, SM3, SM4-GCM, a separately verified
trust store, and fail-closed version/cipher policy. It does **not** replace
standard TLS or permit an opportunistic downgrade to standard TLS, TLCP or
plaintext. For its C11 API, mTLS examples, native Windows/Linux/macOS
qualification and outstanding independent-interoperability release gates,
see [RFC 8998 ShangMi profile](RFC8998_SHANGMI_PROFILE.md).

TLCP 1.1 is a separate, explicitly selected profile with ordered signing and
encryption identities and no TLS/plaintext fallback. See the
[TLCP 1.1 profile](TLCP11_PROFILE.md) for its C API contract and qualification
scope.

The versioned [CNet X.509 compatibility matrix](X509_COMPATIBILITY_MATRIX.md)
separates verified, unsupported and not-yet-verified certificate/profile
behavior without generalizing CNet evidence to downstream products.

## Ownership and progress

`cnet_managed_dial_seal` seals recovery immediately and closes only its own
physical connection. If bounded close admission fails, retain the dial, advance
the client and Manager, and retry `seal`; sealing recovery does not discharge
that pending close. Once close is accepted, repeated `seal` is idempotent.
`cnet_managed_dial_advance` returns `ESHUTDOWN` after seal. Continue transport
progress and Manager recycle before destroying the dial or its borrowed owners.

One client owns one session engine and one NativeIO backend. CNet creates no
I/O worker thread. The application repeatedly calls `cnet_client_poll`; that
call drains bounded commands, observes NativeIO direct completions, routes those
completions through the CNet owner, and invokes callbacks before returning.
Calls to poll must not overlap. Callbacks for the client are FIFO and
non-concurrent, and no internal lock is held while user code runs.

The core client is single-thread-owned: connect/send/receive/close and poll are
issued by that owner or by its inline callback. Cross-thread producers use an
external bounded mailbox and wake policy; CNet does not silently create that
thread or queue topology.

A blocking poll continues through internal-only send completions until it
delivers a public callback or reaches its timeout. A zero timeout performs one
nonblocking progress pass. This keeps completion batching internal instead of
forcing the application to call poll once per backend completion.

CNet's stream owner uses NativeIO direct completion ownership internally for
TCP, VSOCK, and Pipe stream requests. A successful first submission stores the
generation-checked NativeIO request in the bounded CNet request record; observed
completions are routed back to that exact live record before CNet publishes the
logical event. Owner-issued receive demand bypasses the client/shard admission
locks and command queue: it is recorded directly in the canonical owner and
placed on the bounded owner-local rearm queue for the next poll. Callback-issued
receive remains deferred through the command queue because the owner may still
be routing later completions in the current NativeIO batch; this prevents a
callback from reusing a CNet request slot still named by that batch. A
quiescent owner-issued close likewise bypasses the generic command queue: it
commits DRAINING immediately and schedules bounded owner-local close work, but
CLOSING/CLOSED callbacks are still emitted only from the next poll. A close
with earlier send/receive/TLS/connect work remains on the FIFO command path so
ordering is preserved. No path requires an operating-system wake or owner-thread
handoff.

Cancellation is a request, not terminal evidence. CNet retains the request and
any owned payload until the matching terminal NativeIO completion is observed;
an `SALTS_EALREADY` cancellation result therefore does not recycle ownership.
Partial stream writes remain one logical CNet send and may require multiple
NativeIO submissions before the single terminal send event is published.
NativeIO coroutine APIs remain available to other consumers and to the
standalone NativeIO coroutine benchmark; CNet simply no longer creates one
coroutine per stream I/O.

The URI and observer configuration are copied before their admitting call returns
success. Stream send payload bytes are not copied into CNet: successful
`cnet_send_buffer()`, `cnet_send_slice()`, and `cnet_send_slicev()` admission
retains the required backing ownership until the logical send terminal. A callback
may admit another retained send, call `cnet_receive`, or call `cnet_close` for
its client. Calling `cnet_client_poll`, `cnet_client_stop`, or
`cnet_client_destroy` recursively from that callback returns `SALTS_EBUSY`.
Plain non-TLS connections admit multiple logical writes into a fixed-capacity
owner-local FIFO. Admission is bounded by write slots; capacity exhaustion returns
`SALTS_ENOBUFS`. NativeIO progresses one stream write head at a time where the
transport requires serialization, and `on_send` callbacks remain FIFO.
Retained final-send APIs close further public send/receive admission immediately
after successful admission and begin transport/TLS close only after the final
logical write settles.

Hostname resolution uses c-ares' external-event-loop integration. The same
poll owner checks its bounded DNS socket set without blocking and advances
c-ares timers; no resolver thread or synchronous DNS fallback is created.
While at least one hostname query is active, a NativeIO wait is capped to a
1 ms fairness quantum so DNS and transport completions both make bounded
progress. Numeric TCP/UDP addresses, VSOCK endpoints, and Pipe endpoints do not
enter this path.

### Client data-path ownership

Connection records and ordinary client admission are owned by the same CNet
progress thread. Receive/send callbacks, connection mapping, connect admission,
TLS admission/inspection and callback-issued send/receive/close therefore do
not acquire the client control mutex. That mutex is reserved for
poll/profile/wake/stop/destroy overlap handling, where `cnet_client_wake()`
is the only operation permitted from a non-owner thread.

This does not make command publication multi-threaded: cross-thread application
admission still requires an external mailbox, and callback-issued operations
retain their explicit deferred paths when completion-batch safety requires it.

This owner-local layout is also the CNet side of the NativeIO coarse-handoff
contract. Ordinary send/receive/close progress does not cross a NativeIO owner
boundary and does not need an owner-to-owner acknowledgement. A future sharded
CNet facade may route an owned command to a connection's fixed owner, but it
must keep subsequent data-plane progress there instead of bouncing each
completion back through another NativeIO shard.

### Dispatcher ownership

The client dispatcher is a same-owner callback bridge, not a synchronization
boundary. Registration, direct event preparation, inline observer invocation,
terminal recycle, drain and profiling all run on the CNet progress owner, so
the dispatcher owns no mutex. Its fallback lane retains bounded atomic
driving/pending guards, and first-error publication remains atomic; those
mechanisms do not make the dispatcher a cross-thread execution runtime.

## Shutdown and errors

`cnet_client_stop(client, timeout_ms)` closes admission and drives the same
caller-owned loop until connections, NativeIO requests, and terminal callbacks
settle. `SALTS_ETIMEDOUT` is retryable and preserves the
client. Destroying a client before successful stop returns `SALTS_EBUSY`.
If progress has already recorded a fatal error, stop keeps that first error as
its return value while still driving close/recycle to quiescence. The caller
must still attempt `cnet_client_destroy`; it succeeds when cleanup completed,
even though stop reported the terminal diagnostic. Listener bind and accept
failures use portable Salts status codes such as `SALTS_EADDRINUSE`.

Immediate connect validation failure clears the output handle and emits no
callback. Asynchronous failures emit exactly one `CNET_CONNECTION_FAILED` with
a stable stage string. Salts status codes use `cnet_error.status`; a raw
platform status is normalized to `SALTS_EIO` and retained in
`cnet_error.native_status`.

The executable contracts are in `tests/cnet_api_test.c` and
`tests/cnet_tls_test.c`; they cover caller-owned callback execution, TCP,
Linux VSOCK validation/integration, connected UDP, platform Pipe, callback
reentrancy, stale handles, live drain,
receive demand across request-slot reuse, verified TLS, ALPN, mTLS, partial
records, handshake timeout/cancel, accepted sockets, and clean close.

## Benchmark

设置 `CNET_IO_BENCHMARK_RECEIVE_COMPARE=1` 会运行独立的 receive ownership
comparison，使用同一 CNet TCP echo harness 比较 `borrowed callback + consumer
memcpy` 与 `producer-owned slice retain + bounded backing rotation`，覆盖 64 B、64 KiB、
1 MiB。owned benchmark 会让 slice 跨 callback return 存活，直到下一 receive callback
或本次 poll 返回后才 release，因此实际覆盖 retained owner 触发的 receive-buffer rotation，
而不是只测 callback 内立即释放。该实验用于量化 producer-owned receive 的成本/收益；
这里的 zero-copy 只指
CNet receive backing → public owned callback 之间不再 payload memcpy，不代表
kernel/TLS transform zero-copy，也不混入主 libuv/NativeIO 排名表。

`cnet_io_benchmark` 比较 libuv、NativeIO direct、NativeIO coroutine 和 CNet
public byte API。每个客户端使用独立 blocking loopback echo peer；每个 payload
运行 5 轮，每轮各有 32 次预热和 512 次串行 RTT。RT/s 是往返次数，**不是并发饱和吞吐**。
不测每次 I/O 的 deadline；超时语义由 contract tests 验证。

### 成绩、噪声与诊断分离

每轮以未插桩 A/B 四驱动组夹住诊断组，四驱动顺序轮换，A/B 的前后顺序逐轮交换。
所有平台、所有驱动都收集 A/A 对照；主成绩只使用 A 组，以逐轮配对的 median/MAD
报告相对 **libuv** 的 p50、p95、RT/s 差值。MAD 不是置信区间，不自动把大于 5% 的
单次结果判为回归。A/A 与插桩扰动和差距相当时，应在受控宿主复测。

PR 的 TCP/UDP 对照图保留 A 组实线，同时展示 B 控制组虚线。每组都与同组 libuv
逐轮配对；不能将 A 的候选除以 B 的参照。报告 CSV/JSON 另保留 B/A 控制漂移及
A/B 相对差值是否反号。缺少 B 时留空，不能据此声称测量稳定；组内 MAD 较小也
不能排除组间整体漂移。该图属于诊断证据，不改变已有性能门禁。

诊断报告替代旧的重复 inclusive 宽表和逐轮闭合明细：

- 线程 CPU、wall-CPU 估计、A/A 波动、插桩组相对周围 A/B 的均值偏移。
  CPU 来自已有 libuv 的 [`uv_getrusage_thread`](https://docs.libuv.org/en/v1.x/misc.html#c.uv_getrusage_thread)，
  在整个测量 batch 两端读取，不包含 echo 线程。Windows CPU 时间记账较粗，摘要改用
  [`QueryThreadCycleTime`](https://learn.microsoft.com/en-us/windows/win32/api/realtimeapiset/nf-realtimeapiset-querythreadcycletime)
  的线程 cycles/RT，不把 cycles 换算成时间；粗粒度 CPU ns 仍保留在 CSV，wall-CPU 标为
  unresolved，不把量化误差解释成等待。调用不支持或失败时报错。
- 四驱动采用同一外层边界：start API、drive API、payload 校验、剩余 harness 时间。
  CNet 的校验嵌套于 poll，从 drive 中扣除；其他驱动的校验在 drive 之后。
  CNet start 是发送入队，NativeIO start 是提交，libuv start 是接收启用和发送，
  coroutine start 是任务启动；**这些边界职责不同，不是同语义内部阶段**。
  CNet 的实际提交在 drive 内，drive 仍混合等待和分发，不能直接归因为 CPU 成本。
- 阶段差值是逐轮配对后的**算术均值差**，可加和到同批诊断均值差。
  不拿诊断均值解释另一批未插桩 p50/p95；剩余时间保留，不用代数闭合宣称根因已解释。
- CNet 内部只保留一张紧凑证据表：固定控制、发送复制、NativeIO 提交/重提交、observe、
  请求/完成数。这是自身成本，不是相对 libuv 的因果结论。

计时区间没有文件写入或逐事件日志。固定大小的样本数组在 benchmark 中保留全部数据，
空间为 O(payload 数 × 4 驱动 × 3 组 × 5 轮 × 512 样本)，实际字节数在报告开头打印。
不修改生产 CNet/NativeIO ABI、队列、生命周期或调度；仍使用既有私有 profiling target。

### 原始证据与复算

设置 `CNET_IO_BENCHMARK_OUTPUT` 为输出路径前缀（父目录必须已存在），全部计时结束后写：

- `<prefix>.runs.csv`：backend/protocol/payload/driver/pass/repeat 标识、RT 数、batch wall、
  客户线程 CPU、Windows 线程 cycles、p50/p95、上下文切换和外层 API 调用次数。
- `<prefix>.samples.csv`：相同标识加原始 sample 编号，每次 RTT 的 wall 和诊断阶段数据。
  未插桩阶段、Windows/macOS 不支持的上下文切换字段留空，不能视为零。

前缀指定的两个文件会覆盖；写入/关闭失败使测试失败。未设置前缀时仅输出摘要。
CI 保存 CSV，不把所有样本灌入 step summary，并运行
`pwsh -File cnet/benchmarks/verify_io_benchmark.ps1 -Prefix <prefix>`，核对 540 轮、
276480 个样本、逐样本非重叠阶段、计数以及从样本重算的 p50/p95。
慢样本的阶段只能与**同一诊断轮次、同一 sample 编号**关联。

### retained/owned 基准

默认四驱动基线中的 CNet 使用 `cnet_send_buffer`；CI 只把 retained/owned
路径与 NativeIO direct 比较和设门禁。CNet copied-send benchmark mode 已随
copied stream API 一起移除。NativeIO 自身仍可保留 flatten-vs-SG 诊断，用于隔离
payload memcpy 与 vectored submit 成本，但这不代表 CNet 存在 copied data plane。

Linux `cnet_scaling_benchmark` 每个场景保留 5 次配对测量。
`verify_scaling_benchmark.ps1` 对 16 连接、32/64 KiB 场景的 owner residual、
observer framework 和 client poll wrapper，分别以 5 次中位数检查
`2/1/1 us/op` 上限，并报告 MAD 和最大值。单次超限保留警告，持续超限仍失败；
该门禁衡量典型开销，不保证每次运行都低于上限。墙钟计时可能包含调度停顿，
仅凭单个尖峰不能归因于框架回归。非有限值、负耗时和嵌套区间错误仍逐行拒绝，
不参与中位数聚合。NativeIO 配对吞吐/延迟的 MAD 稳定性门禁独立保留。

### Linux 系统调用证据

`CNET_IO_BENCHMARK_TRACE=<driver>:<protocol>:<bytes>` 单独运行一个未插桩 workload，
只输出 trace 身份与测量区间标记，**不输出性能成绩**。driver 为
`libuv|native|coroutine|cnet`，protocol 为 `tcp|udp`，bytes 为 TCP 1–65536 或
UDP 1–8192。非法配置报错，不降级到默认 workload。

CI 在成绩完成后使用 [`strace`](https://strace.io/) 收集 TCP 1/32/64 KiB、UDP 8 KiB
各驱动独立 trace，按线程分文件。`summarize_io_trace.ps1` 只统计客户端
`IO_BENCH_MEASURE_BEGIN/END` 之间的系统调用，排除 setup、warmup、cleanup 和 echo 线程；
生成 `syscalls.csv`、`summary.csv`，分别输出每 RTT 的数据调用、epoll poll、io_uring_enter、直接 epoll_ctl、
EAGAIN 和空 epoll 返回次数；另列 `poll+ppoll/RT` 和空 `poll+ppoll/RT`。
io_uring 后端对 ring/wake fd 的 `poll` 等待必须采集，不能只数 `io_uring_enter`。
超时的 `0 (Timeout)` 与裸 `0` 都计作空返回。解析失败、缺少区间、缺少客户端文件均报错。

用它检查“是否多做了注册/提交/空等待”，不要把 ptrace 下的调用耗时当作未插桩成本。
`io_uring_enter` 次数也不是 SQE 数。libuv 可能通过 io_uring 提交 epoll 控制操作，
不能把“直接 epoll_ctl 为零”解释为“没有注册成本”（参见
[libuv 1.51 Linux 实现](https://github.com/libuv/libuv/blob/v1.51.0/src/unix/linux.c)）。
trace 是独立复跑，不能关联先前未插桩的单个慢样本。
CPU 函数栈以及 blocked/runnable 等待分离仍需具备权限的 profiling host 上采集
[`perf record`](https://man7.org/linux/man-pages/man1/perf-record.1.html) /
[`perf sched`](https://man7.org/linux/man-pages/man1/perf-sched.1.html)；普通 CI 明确不宣称已采集。
例如在已用 `linux-release-user` 构建且已配置运行库路径的宿主执行：

```sh
CNET_IO_BENCHMARK_BACKEND=epoll CNET_IO_BENCHMARK_TRACE=native:tcp:32768 \
  strace -ff -ttt -T -s 128 -o native-tcp-32768.trace \
  build/linux-gcc-release/bin/cnet_io_benchmark --no-color
```

更换驱动重复同一 workload。只有 syscall 次数、CPU/调度证据与未插桩复测相互印证后，
才把候选原因升级为根因；否则报告保留“未解释”，不凭阶段表直接优化生产路径。

### macOS kqueue 注册与等待证据

macOS 构建另提供 `cnet_io_benchmark_kqueue_trace`，通过独立 Mach-O 动态库
对 libuv 与 NativeIO 的 `kevent` 入口做同一套插桩。原有 `cnet_io_benchmark`
不加载这份库，A/B 测量不带该探针。追踪窗口使用线程局部状态，仅覆盖客户端
512 次往返，排除 setup、32 次预热、cleanup 与 echo 线程。

CI 在主测量之后复跑 TCP 1/32/64 KiB、UDP 8 KiB，每种驱动各五次，保留
`kqueue-trace/summary.csv` 和每个 workload 的 CTest 日志。输出注册调用数、
等待调用数、请求的 ADD/DELETE 项数、返回事件数、错误数，以及各类调用的
累计/最大 wall 耗时；wait 可同时携带批量注册，不能仅比较注册调用数。
错误计数只统计 `kevent` 返回失败，ADD/DELETE 包含重试请求。

在已用 `mac-arm64-release-user` 构建的宿主执行：

```sh
ctest --preset mac-arm64-release-user --no-tests=error -R '^cnet_kqueue_trace_test$'
CNET_IO_BENCHMARK_BACKEND=kqueue CNET_IO_BENCHMARK_TRACE=native:tcp:32768 \
  ctest --preset mac-arm64-release-user --no-tests=error -V -R '^cnet_io_benchmark_kqueue_trace$'
```

缺少 trace 配置时该程序失败，不生成普通比较成绩。插桩自测验证实际符号替换、
errno、线程隔离和窗口生命周期。耗时包含内核等待和调度，不等于 CPU 成本；
这些独立复跑也不能解释之前 A/B 中某一个慢样本。CPU 栈和 runnable/blocked
分离需要进一步在 profiling host 采集。本机制依据
[Apple dyld interposition ABI](https://github.com/apple-oss-distributions/dyld/blob/main/include/mach-o/dyld-interposing.h)，
libuv 的批量注册/等待入口参见其
[kqueue 实现](https://github.com/libuv/libuv/blob/v1.51.0/src/unix/kqueue.c)。

### TLS receive cadence attribution

Linux `cnet_tls_public_owner_benchmark` 的私有 profiling 构建可对一次精确 echo
记录有界事件时间线。trace 模式要求 `echo`、`owners=1`、`fresh` 和一次 operation，
避免把多个逻辑操作混入同一 session/request 时间线：

```sh
CNET_TLS_PUBLIC_TRACE=1 \
CNET_TLS_PUBLIC_OWNER_OPS=1 \
CNET_TLS_PUBLIC_NODELAY=1 \
CNET_TLS_PUBLIC_RECEIVE_DEMAND=single \
  build/linux-gcc-release/bin/cnet_tls_public_owner_benchmark \
  echo 32768 1 fresh
```

stderr 的 JSON lines 按统一单调时钟合并 client/server 事件，包含 TLS ciphertext
write submit、socket write completion、NativeIO read arm/completion、TLS record decrypt、
plaintext publish、completion bytes 以及 session/request/endpoint identity。已经在 trace
启用前 arm 的 TLS read 使用 request record 保存的原始 submit 时间，不把 trace 启用时间
误当成 read-arm 时间。固定 trace buffer 满额会令 benchmark 失败，不静默丢事件。
正常 benchmark 未设置 `CNET_TLS_PUBLIC_TRACE` 时不记录逐事件时间线；安装的
`Salts::CNet` target 也不包含该私有 profiling 路径。


### TLS logical write ownership

Steady-state TLS payload ownership uses the same bounded write FIFO as plain
streams. A plaintext write slot remains the logical owner while GmSSL may
generate one or more ciphertext NativeIO writes; ciphertext requests borrow
only the TLS scratch buffer and never own application plaintext. The logical
slot settles only after all ciphertext generated for that plaintext has reached
terminal success. The next TLS logical head starts on a later owner drive.

A final TLS write carries the same close-after-send marker. Further public
admission closes immediately, while `close_notify` starts only after the final
plaintext has been accepted and all of its ciphertext has flushed.


## Coroutine ownership boundary

CNet remains a caller-driven, single-owner session/request state machine above NativeIO. It does
not create a private coroutine executor, I/O thread pool, or second await/result registry.

The dependency boundary is intentional:

```text
CNet session state
      |
      v
NativeIO request/completion authority
      |
      +-> Direct / Coroutine / Sharded execution style
```

NativeIO coroutine frames are an optional execution-position mechanism owned by NativeIO. They do
not replace CNet's session lifecycle, write FIFO, TLS state, or terminal callback contract.
Application code that wants coroutine orchestration may use the generic Coroutine Executor above
CNet, but the executor must return to the connection owner before mutating CNet session state.

Current owner tests require that ordinary CNet NativeIO requests do not retain NativeIO coroutine
frames. This is deliberate: adding a coroutine frame around the existing CNet owner state machine
would duplicate execution state without removing an I/O hop. A future CNet coroutine-facing adapter
requires a concrete consumer and paired evidence before it can change this boundary.

Cross-owner work remains explicit and bounded. CNet never hides live connection migration,
work-stealing, or an implicit worker-pool hop behind its public send/receive APIs.

Plaintext TCP connections retain the historical close-on-peer-EOF behavior by
default. A consumer with directional stream semantics can call
`cnet_connection_preserve_send_on_eof(client, connection)` after the connected
callback and before submitting receive demand, outside callbacks. The owner
then emits one empty byte receive (or an empty retained-slice callback), rejects
further receive demand with `SALTS_ESHUTDOWN`, and preserves send admission.
The client marks receive closed before invoking the EOF callback, so reentrant
receive cannot admit work on that direction. Close/failure still produces the
ordinary terminal callback and settles retained writes. This additive opt-in
does not resize public configuration or observer structures, does not apply to
TLS, and is qualified by `cnet_external_progress_test` with a real loopback
half-close and a send after peer EOF.

## IPC, WebSocket, and KCP evolution design

Status: joint design, recorded 2026-10-07. The additive UDP, IPC and tagged WS
engine capabilities are implemented as described above; the remaining adapter,
platform, benchmark and SDK release gates below are not declared complete.
`cnet_connect()` supports `ipc://`, but does not add `ws://` or `kcp://`.
Current headers and executable tests remain the implementation authority.

For UDP/WS destination, Owner placement, admission, recovery and pool boundaries,
see the [#1095 strategy applicability review](SG_POLICY_HOST_CONTRACT.md#udp-and-websocket-policy-review-1095),
including the source-based acceptance inventory and remaining composition gates.
The [implemented composition boundaries](SG_POLICY_HOST_CONTRACT.md#implemented-composition-boundaries)
describe opt-in mixed SG routing, the dedicated TCP/TLS
[WS write bridge](include/cnet/websocket_transport.h), and admitted ManagedDial
initialization. These preserve existing config layouts and keep HTTP/WS
handshake policy in the host.

Prerequisite and governing progress contract:
[Salts #999: UDP multi-owner composition and datagram external progress](https://github.com/qigao/salts/issues/999).
Its datagram external APIs are present in the current headers and implementation.
This section extends their owner/completion rules to IPC/WS/KCP; it does not
claim that all platform, performance and release gates of #999 have passed.

### Problem and current evidence

FlowMQ currently admits only TCP/TLS and directly owns a CNet client/listener.
Before it can add IPC, WS, and KCP, CNet must provide complete connection,
ownership, backpressure, terminal, and shutdown contracts for each transport.
Adding URI cases in FlowMQ alone would leave these contracts incomplete.

| Path | Existing implementation | Remaining work for this design |
| --- | --- | --- |
| TCP/TLS | `cnet_client`, `cnet_listener`, retained sends, demand, owner-local callbacks | Preserve behavior and use as the byte-stream reference |
| Pipe / IPC | Legacy `pipe://` remains unchanged; `ipc.h` adds typed detached listener/adoption and a distinct Unix-domain-socket path | Validate remaining POSIX platform gates and downstream consumption |
| NativeIPC | `native_ipc.h`: bounded Windows accepts, host event snapshots and move-owned endpoints; POSIX FIFO open | Reuse rendezvous and terminal drain, not a new payload engine |
| WS | `websocket.h`: framing/session engine, retained output and opt-in tagged logical-message terminals | Integrate authoritative terminals into HTTP adapters and validate H1/H2 backpressure |
| KCP | `cnet_packet_endpoint`: bounded sessions, explicit admission, tagged terminals, timers, optional PSK/FEC | First compose #999 external progress; then add protocol deadlines and consumption/backpressure policy suitable for bounded upper-layer queues |

Evidence locations: [client admission](src/cnet_client.c),
[listener](src/cnet_listener.c), [URI parser](src/cnet_uri.c),
[Pipe transport](src/cnet_transport_pipe.c),
[NativeIPC contract](../native-io/include/salts/native_ipc.h),
[POSIX IPC implementation](../native-io/src/native_ipc_posix.c),
[WebSocket engine](src/cnet_websocket.c), and
[packet endpoint](src/cnet_packet_endpoint.c).

The current `cnet_listener` accepts TCP/VSOCK streams; it is not a named-pipe
acceptor. The current POSIX NativeIPC server returns `SALTS_ENOTSUP`. Neither
fact may be hidden behind a new URI alias. Existing behavior is covered by
[API tests](tests/cnet_api_test.c), [URI tests](tests/cnet_uri_test.c),
[WebSocket tests](tests/cnet_websocket_test.c), and
[packet endpoint tests](tests/cnet_packet_endpoint_test.c).

### Layering and alternatives

Keep three existing composition boundaries:

```text
FlowMQ protocol / patterns / application acknowledgements
  byte-stream adapter       binary-message adapter       message adapter
  CNet TCP/TLS/IPC           CHTTP WS route/client        CNet packet endpoint
                            CNet WebSocket engine        CNet KCP / security / FEC
  NativeIO STREAM/PIPE      NativeIO STREAM via CNet     NativeIO DATAGRAM
```

NativeIPC owns named-pipe rendezvous; NativeIO owns payload I/O and native
terminals; CNet owns session policy. CHTTP owns HTTP Upgrade, routing, header
validation, subprotocol negotiation, and HTTP/2 Extended CONNECT. CNet must
not depend on CHTTP. Adding a WS convenience facade in the future must preserve
this dependency direction; it cannot make CNet parse HTTP or access CHTTP's
private owner state.

The target host-composed topology follows #999. This is not a claim that the
current CHTTP public server exposes a borrowed-backend constructor:

```text
owner lane 0                        owner lane 1
  NativeIO backend 0                  NativeIO backend 1
    TCP/TLS client + WS sessions         TCP/TLS client + WS sessions
    IPC established connections         IPC established connections
    UDP datagrams + KCP endpoints        UDP datagrams + KCP endpoints
  one host observe/route loop          one host observe/route loop
```

Each live instance stays on its final owner. A packet endpoint owns its datagram
composition and sessions, but borrows the lane backend in external mode. Separate
owners use separate backends. Do not create a worker pool, public shard-count
knob, same-instance cross-thread poll, or transparent live migration. Existing
owned-mode APIs remain available; a live object cannot switch modes in place.

The chosen common contract is lifecycle, bounded ownership, ordered delivery,
and explicit completion meaning. Keep byte streams and messages distinguishable.
Use the existing CNet client, WS engine, and packet endpoint as state owners.
A consumer may have a small fixed adapter table; it must not create a parallel
connection table in CNet or pretend all existing handles are `cnet_connection`.

Alternatives considered:

- One universal `cnet_connect(uri)` plus a new transport runtime: convenient
  spelling, but duplicates packet/WS owners and pulls HTTP policy into the wrong
  layer. Do not introduce it for this work.
- FlowMQ implementing pipes, WS framing, or KCP directly: duplicates lower-layer
  lifecycle and failure handling. Complete reusable CNet contracts first.
- Reinterpret POSIX `pipe://` as Unix sockets: breaks the established paired-FIFO
  wire/rendezvous contract. Add IPC independently.
- Shared-memory IPC: requires a separate synchronization, crash-recovery, and
  mapping-lifetime design. It is outside this socket/pipe IPC change.

### Common consumer contract

| Concern | Required behavior |
| --- | --- |
| Owner | One non-overlapping progress owner per client/endpoint/session composition; no hidden worker, owner migration, or concurrent callback invocation |
| Identity | Retain existing generation checks; asynchronous tags include enough session identity to reject stale completions after slot reuse |
| Admission | Failure retains no new logical operation and produces no logical terminal; success commits exactly one later success/failure/cancellation terminal in the new terminal-capable adapters |
| Input ownership | URI/configuration copied during successful admission; borrowed callback payload valid only until callback return; retain or copy before deferral |
| Output ownership | Retained CNet slices follow their existing lease contract; WS owns its frozen frame until authoritative completion; KCP copies into bounded session storage |
| Ordering | Per-session ordered byte/message delivery; no ordering promise across sessions |
| Capacity | Separate hard limits for connections, pending accepts, logical sends, input/output bytes, and maximum message size; checked arithmetic before allocation |
| Backpressure | Report explicit full/busy status without accepting a second copy; resume only when the owning resource becomes available |
| Errors | Keep first concrete error and stage; distinguish invalid input, unsupported mode, size limit, capacity, timeout, cancellation, and peer close |
| Shutdown | Stop admission, request close/cancel, drain native and logical terminals, publish terminal state, then release storage; timeout leaves the owner alive for continued progress |

Do not retroactively change the existing CNet callback ABI to implement this
contract. Add size/versioned options or new entry points where existing APIs do
not provide a logical-send terminal. A close notification alone is not proof
that retained buffers can be reclaimed.

Successful completion must carry or document its strength:

| Path | Successful send terminal proves | It does not prove |
| --- | --- | --- |
| TCP/TLS/IPC | The complete logical write reached its local transport terminal | Peer application consumption |
| WS | Every data fragment of the logical message reached the underlying write terminal | Peer WS/application consumption |
| KCP tagged send | Peer KCP acknowledged all segments of the logical message | Peer application consumption or persistence |

Application acknowledgement, persistence, deduplication, and exactly-once
processing remain above CNet. There is no automatic retry of application sends
on a newly established session.

### IPC design

The additive `ipc://` byte-stream transport uses this platform mapping:
Windows uses local byte-mode overlapped named pipes; POSIX uses pathname
`AF_UNIX/SOCK_STREAM`. Preserve `pipe://` exactly. Abstract-namespace Unix
sockets, descriptor passing, shared memory, and peer-credential policy are not
part of the initial IPC contract.

Use `ipc://name` on Windows and `ipc:///absolute/path` on POSIX. The endpoint
name/path is copied and never sent to DNS. Validate the platform-specific
length before native calls; reject truncation, query/fragment syntax, invalid
names, and unsupported backend combinations. Windows mapping produces a local
named-pipe name and does not accept a remote machine/UNC endpoint. POSIX requires
an absolute pathname; there is no percent decoding or implicit directory creation.

`cnet_connect()` now composes the IPC client path with a separate opaque
`cnet_ipc_listener` and size/versioned configuration, preserving
`cnet_listener_config`. The public `<cnet/ipc.h>` provides init, bounded advance,
detached accept/adopt, host wait snapshots, stop and destroy. Executable
client/server and two-owner examples are in `tests/cnet_ipc_test.c`.

The listener owns a bounded number of pending accepts and accepted-but-not-yet-
transferred children. A detached accept moves one child into a descriptor with
no active I/O. Queue publication moves that descriptor to the destination owner;
failed publication leaves it with the producer. Adoption consumes the descriptor
on every valid consuming attempt, including capacity failure, matching the
existing TCP detached-adoption contract. Failure closes the child exactly once;
it is not retried using the emptied descriptor. Applications compose detached
accept and client adoption explicitly. Transferred connections may outlive the
listener. See the multicore contract below for credits and shutdown races.

Windows composes the existing NativeIPC accept service and NativeIO PIPE
attachment. Its accepted handles need a pipe-specific CNet adoption path and
pipe-specific cleanup in every rejected command and terminal path; socket
`closesocket` cleanup cannot be reused for these handles. POSIX uses NativeIO
STREAM operations and the existing family-neutral CNet connect/adopt helpers.
Socket options that have no IPC meaning must fail explicitly instead of being
silently applied or ignored.

The IPC listener remains caller-driven and creates no thread. Windows accept
progress is observed through NativeIPC; POSIX accepts are nonblocking. The first
release exposes nonblocking listener progress with explicit work counts and
typed host wait snapshots. The existing external TCP accept API does not drive
Windows pipe accepts; the host integrates their explicit event sources.

Established IPC children use the selected CNet client's owned or borrowed
backend; they never create a per-child backend. For a #999-style host, POSIX
listener readiness must participate in the host wait and Windows NativeIPC
accept readiness uses the explicit event snapshot and host wake source. A host
must not add a second blocking wait to its data lane; it can keep rendezvous in
a separate, explicit acceptor/control lane. Stopping an IPC listener or child never
closes the lane backend or neighboring TCP/UDP endpoints.

POSIX bind must not unlink an existing path, including a stale socket. Existing
paths fail explicitly and remain untouched. The path lifecycle is caller-owned:
close releases the socket but does not unlink its pathname; examples clean up
their exclusively owned temporary directory after all endpoints close. This
avoids deleting a replacement pathname during shutdown. Windows preserves the
NativeIPC local-only accept policy and OS access checks; POSIX preserves directory
permissions and the caller's umask. IPC is not automatically an authenticated
FlowMQ identity.

Listener stop cancels pending accepts and drains each authoritative terminal;
destroy before quiescence returns busy. Configure a hard pending-accept bound
and explicit pipe buffer limits. Listener capacity and active CNet connection
capacity are separate budgets. No silent TCP fallback is permitted.

### WS/WSS design

Reuse `cnet_websocket`, including its role checks, masking, fragmentation,
control frames, bounded reassembly, and close state. Do not add HTTP ownership
to that engine. CHTTP's current client/server WS implementations already consume
it; audit those public APIs for terminal visibility before adding another wrapper.
The checked sibling checkout is `http-services`, with public headers under
`http_client/include/http_client/` and `http_server/include/http_server/`.

The WS engine now provides an additive, versioned
tagged-send policy: reserve one bounded operation record before
admitting its first fragment, record the message tag, and settle it exactly once
after the final fragment's transport terminal. Initial tagged sends allow one
logical message in flight and serialize fragments through the existing one-frame
output slot; control writes never settle or reuse a data-message tag. Mixing
legacy untagged data sends with a live tagged message is rejected explicitly.

An asynchronous write callback returns `CNET_WEBSOCKET_WRITE_PENDING` only with
configured retained output. The embedding transport reports the authoritative
frame terminal through `cnet_websocket_write_complete()`. `SALTS_OK` from
`cnet_websocket_send_binary()` alone remains admission, including a retained
frame waiting behind backpressure; it cannot be reported as logical completion.
Synchronous write completion must defer the new logical callback until the
admitting operation has returned, so admission and terminal ownership do not
race through reentrancy.

Provide an explicit owner-local, bounded dispatch/advance operation for that
deferred terminal; a later application send must not be required to release the
previous operation. Adapters must report real asynchronous retention as pending:
a CHTTP adapter that copies into an asynchronous transport buffer and returns
`SALTS_OK` to the old engine cannot be used as evidence of a new tagged message's
transport terminal. Update and test that handoff before enabling tagged WS sends.

Retain a bounded copy or lease for the complete tagged input until all required
fragments are encoded. Incoming events remain borrowed. When feed returns busy
without consuming input, an H1 adapter retains that chunk and pauses that
connection's new reads under its bounded receive contract. An H2 adapter retains
bounded stream DATA/credit while continuing connection reads and control frames;
it must not pause the whole connection for one blocked stream. Upgrade response bytes
and following WS bytes must be handed off in order without loss or duplicate
delivery. This is a CHTTP integration gate, not a new HTTP parser in CNet.

Graceful close drains admitted data under an explicit deadline, sends/observes
the WS close handshake, and then closes the transport. Abort or deadline expiry
fails/cancels pending logical sends and drains native writes before releasing
output storage. WSS continues to use the existing verified TLS policy. HTTP/2
WS progress belongs to the HTTP stream owner; no adapter polls or closes the
whole connection independently of sibling streams.

In external mode, the host routes NativeIO completions to the owning CNet/CHTTP
transport; that owner then feeds the WS engine or reports its retained write
terminal. The WS engine does not claim native requests, observe a backend, or
create its own completion registry. Close/heartbeat deadlines are advanced on
the same lane and included in the host's minimum wait deadline where configured.

### KCP design

Use `cnet_packet_endpoint_init_ex()` with `CNET_PACKET_KCP` and a bounded terminal
policy, then `cnet_packet_send_tagged()`. Preserve packet-session handles, peer
admission, optional PSK/FEC, and owned-mode `cnet_packet_poll()` timer ownership. A new KCP
thread, second peer table, or KCP-as-TCP handle is unnecessary. FlowMQ-facing
URI parsing belongs to its eventual adapter; security keys and tuning parameters
are structured configuration, never URI text.

For external mode, complete #999's datagram constructor, bounded advance,
completion routing, and stop/drain first. Then add versioned/additive packet
endpoint operations for external initialization, advance, completion routing,
next protocol deadline, and nonblocking stop with an independent quiescence
result. Exact names/signatures remain design proposals. Both modes share packet,
KCP, and shutdown state; external mode must reject the old blocking poll/stop
entry points rather than observing the borrowed backend.

Raw UDP has no protocol retransmission deadline and must not gain an empty
timeout API. KCP does have retransmission, probing, and optional secure-handshake
deadlines: expose their minimum from existing `cnet_kcp_check()` and
`cnet_secure_kcp_check()` state, and advance due work without blocking. Specify
clock units and wrap-safe conversion between the host clock and KCP's 32-bit
millisecond clock. The host waits for the minimum across TCP/TLS, KCP, and other
services, with a bounded amount of work per instance each turn.

Only the composed datagram matches a native request. Follow #999 by matching
active `(slot, generation)` in the correct backend before interpreting local
`user_data`; foreign completions return unconsumed with zero events. Once matched,
malformed completion contents remain consumed with an error. One error must not
discard other completions already observed in the host batch. Protocol sessions
are selected only after the correct datagram accepts the completion.

Require message mode for the tagged contract. Validate maximum logical message
size against configured message/segment limits before admission; preserve the
existing per-send 127-new-segment limit and reject oversize messages. Do not
silently split an application message into independently completed KCP messages.
The upper protocol must choose an explicit fragmentation envelope if larger
messages are required.

Use fixed session, logical-send, segment, datagram, and receive-message budgets.
Local queue pressure does not settle a message successfully or discard retained
segments. Continue ACK/retransmission/handshake timers while application output
is blocked. Packet receive callbacks currently deliver borrowed views without
an application-demand return value; do not advertise pull backpressure that
 does not exist. Before bounded FlowMQ integration, add explicit per-session
receive credit/pause in CNet, retain complete messages within configured bounds,
and verify that window/protocol progress continues without admitting unbounded
application data. Capacity exhaustion must have a tested terminal/rejection
policy, not silent message loss.

The current KCP engine already has receive-window and message-size bounds; the
gap is application delivery control, not a claim that existing KCP storage is
unbounded. Define credit in complete messages, with the existing maximum message
bytes remaining a separate hard bound. Zero credit retains messages inside
bounded protocol storage and suppresses application delivery while continuing
protocol control traffic. Apply the same rule through authenticated/FEC paths.

Plain KCP and authenticated KCP remain explicit choices. Peer creation stays
behind `on_admit`; authenticated mode must preserve its handshake checks and
must not fall back to plaintext. ACK completion does not authorize reuse of a
different session's generation or tag. Close/stop cancels unacknowledged logical
sends, drains native UDP terminals, and delivers those logical terminals before
the session CLOSED notification, as the existing endpoint contract requires.

External stop follows OPEN -> STOPPING -> QUIESCENT -> DESTROYED. It suppresses
new application receive delivery, cancels pending logical sends, and keeps
advance/route callable while the host drains requests. Report the first concrete
cancel/release error independently from `out_stopped`; neither timeout nor error
authorizes freeing active requests. Destroy requires drained requests and callback
borrows. The host alone closes/destroys the shared backend after all users drain.

Follow #999's bounded request scan initially; do not add a second routing index
without measurement. For D datagrams and send capacities S_i, reserve at least
D UDP endpoints and sum(S_i + 1) UDP requests, plus the actual TCP/IPC and other
service endpoint/request budgets. Check all sums/products for overflow. A bounded
completion batch need not hold all in-flight requests.

Multi-owner UDP does not imply same-port session routing. Preserve #999's
explicit `reuse_port` platform policy; multiple ephemeral binds do not form a
same-port group. KCP `(peer, conversation)` state must stay on its chosen owner;
NAT rebinding and live reuseport-group changes are not transparent migration.
If a consumer explicitly chooses an ingress owner, its cross-owner queues must
be bounded and own their packets, and replies remain on the source socket owner.
This is a separate topology, never an automatic fallback or UDP detached accept.

### WS and IPC multicore design

Status: IPC primitives are implemented; host/CHTTP composition and release gates
remain as described below. The CHTTP server's fixed WS owner lanes already exist.
Multicore here means parallel progress of distinct
connections on distinct owner threads. One WS/IPC connection has one transport,
protocol, and callback owner for its entire admitted lifetime. CPU pinning is an
optional host scheduling policy, not a requirement or a CNet worker API.

#### Existing capabilities and remaining boundaries

| Area | Current evidence | Design action |
| --- | --- | --- |
| CNet stream placement | `cnet_listener_accept_detached()`, `cnet_client_adopt_accepted()`, and `cnet_client_adopt_accepted_tls()` in [cnet.h](include/cnet/cnet.h); TLS state starts on the final owner | Reuse unchanged for WS/WSS connection admission |
| CHTTP WS server | Versioned `chttp_server_execution_options.owner_count`, fixed connection ranges, bounded owner admission queues, and copied WS commands | Reuse its configured lanes and thread-safe captured-session entry points; no second WS worker pool |
| CHTTP WS client | `chttp_websocket_client` and H2 pool are single-owner, internally driven facades | Independent clients/pools can live on independent threads; sharing one host loop additionally needs a nonblocking CHTTP adapter |
| CNet IPC | Typed listener, detached IPC descriptor, adoption and wait snapshots exist in `ipc.h` | Complete remaining platform and consumer gates before publishing an SDK |
| External progress | CNet TCP and datagram external APIs exist | Apply the same fixed-owner rules to IPC; KCP timer/credit integration remains separate |

CHTTP evidence in the sibling checkout:
[server public contracts](../../http-services/http_server/include/http_server/http.h),
[owner admission](../../http-services/http_server/src/chttp_server.c),
[WS command routing](../../http-services/http_server/src/chttp_websocket_server.c),
[owner topology tests](../../http-services/http_server/tests/chttp_server_owner_topology_test.c),
and [WS generation tests](../../http-services/http_common/tests/integration/chttp_websocket_test.c).
These tests are source evidence here, not a claim that they were run for this
documentation change.

There are two explicit hosting modes. Existing CHTTP owns its configured server
threads and pollers; callers use its public server/session APIs. A future
application-hosted CHTTP adapter borrows the application's lane backend and
exposes nonblocking advance/route/deadline/stop operations. It must be additive
and complete, not an assumption that passing `cnet_client_init_external()` to
today's public CHTTP server changes its execution model. Do not block a shared
lane inside the current WS client/pool facade.

#### Topology and placement

```text
WS TCP listener / IPC rendezvous owner (explicit host control lane)
       | accept terminal; no child payload I/O or protocol state yet
       | select READY lane with available connection credit
       +-- move-owned admission queue --> owner 0 thread
       |                                  NativeIO backend 0
       |                                  CNet client 0
       |                                  TLS/HTTP/WS or IPC sessions
       +-- move-owned admission queue --> owner 1 thread
                                          NativeIO backend 1
                                          CNet client 1
                                          TLS/HTTP/WS or IPC sessions

application producers -- bounded copied commands + wake --> owning lane
```

The host chooses N and per-lane capacities before startup. Each thread creates
its backend and owner-local state on that final thread, publishes READY only
after successful initialization, and stays alive through its drain. If one lane
fails startup, stop admission and drain already-created lanes; do not silently
reduce N. N=1 preserves the single-owner topology. CNet creates no threads; CHTTP
may create threads only under its existing explicit execution policy.

Use the existing CHTTP round-robin-with-capacity-leases policy for WS. Use the
same policy in the host IPC accept dispatcher: scan at most N READY lanes and
reserve one destination connection credit, then publish to its bounded queue.
Queued, adopting, active, and closing connections all consume that credit until
failure cleanup or terminal drain releases it. Queue fullness or a destination
that has stopped admission closes/rejects the still-detached child and releases
the reservation; it never silently grows a backlog. A different application
placement policy can be selected before admission, without migrating live peers.

One listener owner per address is the initial portable IPC topology. POSIX does
not bind N listeners to the same pathname; Windows uses bounded pipe instances
under one explicit rendezvous owner. The listener may share a data lane only
when its readiness participates in that lane's wait. Otherwise the host explicitly
provides a control lane; this is not a hidden accept thread or a fallback. The
acceptor's capacity/CPU is a separate measurable limit; established payloads
never pass back through it.

Outbound WS/IPC connections choose their lane before connect. WSS creates TLS
state there before the HTTP handshake. For WS servers, detached TCP adoption
precedes TLS, HTTP Upgrade, and WS engine creation; no live handshake or parser
state crosses owners. With H2, the TCP/TLS connection, H2 flow-control and header
state, and all WS streams remain on the same owner. To use more I/O cores, create
multiple physical connections; a single large stream is not split across owners.

#### Detached IPC transfer and native completion ownership

Use the IPC-specific move-owned `cnet_ipc_accepted` descriptor, separate from the TCP-only
`cnet_accepted_stream`. It identifies a Windows pipe or POSIX Unix stream,
owns exactly one native resource, and contains no outstanding request, callback
borrow, attached data endpoint, or protocol state. CNet API roles:

| Operation | Required result and ownership |
| --- | --- |
| IPC listener detached accept | Success moves one completed child into an empty descriptor; no-ready reports an explicit non-success without publishing a child |
| IPC descriptor close | Closes only a still-detached child and empties the descriptor; repeated close has a documented result |
| IPC client adopt | Runs only on the destination client's owner; consumes a valid descriptor on every consuming attempt, closing on failure and emptying the source |
| IPC listener advance/stop | Nonblocking, bounded; retains pending native accept state until its terminal; stop completion is independent of the first concrete error |

These operations are declared in `<cnet/ipc.h>`. Keep the descriptor's
platform identity private, validate its kind, and use the matching close path.
Never cast a pipe descriptor to `cnet_accepted_stream` or use socket cleanup for
a pipe. Keep existing unversioned public structs unchanged.

The ownership transitions are:

```text
ACCEPT_PENDING -> DETACHED -> QUEUED -> ADOPTING -> ACTIVE -> CLOSING -> CLOSED
      |              |          |          |
      +-- cancel     +-- reject +-- stop   +-- fail
          then terminal       each ends in exactly one native close
```

ACCEPT_PENDING belongs to the rendezvous owner; no transfer is legal yet.
DETACHED belongs to the producer; successful queue publication empties its local
descriptor and commits QUEUED ownership. Failed publication leaves the descriptor
untouched with the producer. The consumer moves it out and empties the slot before
adoption. Once publication succeeds, an acceptor must not close its old copy even
if wake fails. Destination shutdown drains queued children without adopting them.
The connection reservation follows the same ownership transfer and is released
exactly once, including a stop/adopt race.

On Windows, the existing NativeIPC accept service uses event-backed overlapped
accepts. Its [terminal dispatcher](../native-io/src/native_ipc_windows.c) releases
the completed accept slot before passing a move-owned pipe endpoint to its callback.
Build detached transfer at that terminal boundary. Do not associate a pending
child with the acceptor's IOCP and later try to attach it to another lane: a handle
remains associated with its completion port until closed
([Microsoft CreateIoCompletionPort contract](https://learn.microsoft.com/en-us/windows/win32/api/ioapiset/nf-ioapiset-createiocompletionport)).
The destination performs the first data-backend attachment after accept completion.
The accept `OVERLAPPED` and event stay with NativeIPC until the native terminal;
an early connected notification never authorizes their reuse
([Microsoft overlapped pipe example](https://learn.microsoft.com/en-us/windows/win32/ipc/named-pipe-server-using-overlapped-i-o)).

NativeIPC now exposes `cmeta_ipc_pipe_server_wait_sources`; CNet exposes the typed
`cnet_ipc_listener_wait_sources` snapshot. Hosts integrate these borrowed events
with their own control wake and native wait limits. Preserve event-based detached accepts for central placement;
any future accept-on-IOCP variant must select the final owner before association
and keep that handle there. It is a distinct, explicit admission topology.
This does not introduce helper threads, arbitrary timer polling or an event-to-IOCP bridge.

On POSIX, accept a nonblocking Unix stream, finish listener-owned accept work,
and transfer it before attaching it to the destination NativeIO backend. The
listener pathname remains under the previously defined caller-owned lifecycle.
For every platform, endpoints and `(slot, generation)` request identities are
backend-local: the host routes completions only within that backend, matches
active identity before interpreting local tags, and continues the remainder of
an observed batch after a consumed error, following #999.

#### Cross-owner sends, application work, and backpressure

Reuse CHTTP's `chttp_server_websocket_session_capture()` and
`chttp_server_websocket_send_*()` for server WS cross-thread submission. A captured
session does not retain its server. Direct `chttp_websocket*` and CNet/WS engine
calls remain owner-only. The existing implementation may admit a stale copied
command and reject it when the owner checks its generation; its integration test
explicitly exercises that behavior. Preserve that admission contract.

For IPC, a host routing token carries runtime lifetime/epoch, owner identity,
and the owner-local connection slot/generation. It is not a live object pointer
or permission to invoke `cnet_send_*()` from another thread. An H2 WS token also
identifies its stream within the generation-checked physical connection. Reuse
the existing server-global WS session identity instead of wrapping it in another
mutable routing table. Producers must finish before the owning runtime is freed;
an epoch cannot make a freed server pointer safe.

Use bounded MPSC command admission and one consumer per lane. Reuse the existing
CHTTP copied-command queues and Salts queue/synchronization primitives as appropriate;
do not require a new lock-free queue. Reserve entry and byte credits before copy
or retain, release them on rejected publication, and transfer them on success.
Copied input is independent after admission. A retained immutable payload must
hold a real lease whose allocator/pool survives every final release. Callback
views and mutable WS output buffers cannot be enqueued as borrowed pointers.

Queue admission success means ownership transfer only. For the proposed tagged
adapter, a command invalidated after queue admission still receives exactly one
failure terminal on the target owner; reserve terminal storage with the command.
Queue admission failure receives no terminal. Underlying CNet/WS transport
admission is a later step: busy leaves one bounded queued operation for retry,
with no duplicate send. Terminal bytes/strength follow the earlier common
contract. Keep this additive; do not change legacy CHTTP send callbacks implicitly.

Publication precedes wake. Use the owning backend/client's existing concurrent
wake capability and ensure the consumer checks published work before sleeping.
If wake fails after publication, the command remains accepted; report the
infrastructure error without returning a retryable admission failure or freeing
queue-owned data. The host must restore progress or execute coordinated drain
before destroying the lane. Keep queue and backend lifetimes protected against
concurrent publishers. Test the publish/wait transition for lost wakeups.

Serialize commands by destination queue publication order; racing producers
have no predetermined relative order. Close is an ordered command that ends
later send admission. Reserve bounded control capacity or an explicit stop flag
plus wake so full data queues cannot block shutdown. Bound work per connection
and per loop turn; a slow WS/IPC connection must not block unrelated connections
behind an indefinitely retried head entry. Preserve per-connection FIFO while
scheduling other runnable connections.

Broadcast is per-recipient admission with a bounded result for each recipient,
not an implicit atomic all-or-nothing send. Share only immutable retained data;
account for every recipient's queue/terminal entry and total retained bytes.
Do not let one slow recipient pin unbounded producer storage. Define application
policy for individual backpressure rather than dropping messages silently.

CPU-heavy application work may be sent to a host-supplied bounded executor.
Jobs contain copied/retained input and a generation-checked reply token; workers
return owned results through the lane queue and never mutate WS/TLS/IPC state.
If replies require input order, include a sequence and bound the reorder window;
otherwise complete in the application's documented order. Disconnect invalidates
reply delivery, not the worker's obligation to release its input/result leases.

#### Capacity, stop, and validation gates

For lane i configure connection budget C_i, detached queue H_i, command entries
Q_i, and queued/retained byte budget B_i. Let R be the rendezvous accept bound.
Maintain `reserved_i + queued_i + adopting_i + active_i + closing_i <= C_i`
with credits, counting a selected but not-yet-published child as reserved, and
`queued_i <= H_i`; total pending native accepts never exceeds R. Application job
and terminal budgets are additional explicit bounds. Charge transient producer
copies to admission credit; do not multiply an unaccounted max-sized allocation
by an unbounded number of producers. All budget arithmetic is checked.

Total connection capacity is sum(C_i), not N copies of a global limit. Preserve
CHTTP's existing partitioned connection ranges; report separately which existing
command/buffer budgets are per lane. Backend endpoint/request budgets include
the actual TCP/IPC/UDP users on that lane and handshake/close work. Closing a
connection does not release its credit before native and logical terminals drain.
Placement costs at most O(N) per new connection; steady-state owner routing is
fixed and does not rescan N owners for every frame or write.

Stop is coordinated by the host: close new connection and command admission;
quiesce producers; cancel pending accepts and collect their terminals; move/close
queued descriptors on their current owners; cancel or deadline-drain active
protocol sessions; continue native completion routing and terminal delivery;
release leases; then destroy clients, backends, queues, and join owned threads.
Keep the rendezvous and destination lanes alive until all in-flight handoffs
settle. A timeout or concrete error leaves needed state available for continued
drain; never force-free it. Stop one lane without stopping peers on other lanes.
Application jobs that finish after command admission closes release rejected
results on their documented allocator owner; shutdown must not wait for a result
that can only enter a closed queue. Already-admitted operation terminals use their
reserved completion path through drain, independent of new-send admission.

Owner-count changes apply only to a stopped runtime in the initial design.
Rolling deployment uses a new runtime and explicitly drains the old one. There
is no owner-index remapping or replay of in-flight sends during resize/restart.

| Gate | Required evidence before advertising support |
| --- | --- |
| Real WS multicore | Two physical H1 WS/WSS connections execute on distinct owners; each connection's TLS/HTTP/WS callbacks stay on one owner; H2 streams stay together while two H2 connections can use two owners |
| Real IPC multicore | Windows named-pipe and POSIX Unix-stream duplex sessions run on two final owners; no data I/O occurs on the acceptor after transfer; IOCP association happens only on the final owner |
| Handoff | Empty/full queues, failed startup, adoption failure, stop after native completion but before queue publication, stop after publication, pending accept cancellation, and exactly one native close/credit release |
| Commands | Multiple producers, per-connection ordering, stale slots/runtime lifetimes/H2 streams, full byte and entry limits, disconnect after admission, tagged one-terminal behavior, publish/wake races, and a slow peer alongside a runnable peer |
| Drain | Stop one lane while others continue; partial send and WS close/control interleaving; outstanding application jobs and retained buffers; observed-but-unrouted completions; retryable timeout with no early destroy |
| Platforms | Formal Windows IOCP, Linux epoll/io_uring, and macOS kqueue tests; unsupported capabilities fail explicitly; C/C++ header and existing TCP/TLS/Pipe/CHTTP regression suites |

Extend the existing CHTTP topology/WS integration tests and CNet/NativeIPC tests,
not source-marker checks. Add sanitizer coverage appropriate to memory and
cross-thread lifetime behavior. For performance, extend the existing
`chttp_server_ws_owner_benchmark` and add a formal IPC benchmark: compare 1/2/4
owners with equal total connections, messages, byte budgets, and payload sizes;
report per-owner CPU, throughput, P95/P99, queue rejection/high-water marks, memory,
and drain time. Measure connection/handshake churn separately from established
payload traffic. Record machine/core placement and run variance; no linear-scaling
claim follows merely from adding threads.

**HIGH / design risks:** transferring a pending/already-associated pipe handle,
double ownership after handoff, stale command delivery, premature backend/pool
destruction, and confusing queue admission with transport completion.
**MED / design risks:** centralized accept saturation, mailbox contention,
unfair retry scheduling, and a single physical H2 connection limiting I/O parallelism.
The chosen fixed-owner design bounds these responsibilities; it does not remove
the need for the executable gates above.

### Implementation sequence, validation, and compatibility

1. Complete #999's datagram external APIs and formal mixed UDP/TCP ownership,
   completion routing, and shutdown tests. Publish an explicit SDK capability;
   existing Salts 2.1 APIs do not establish that capability.
   This is a dependency for mixed UDP/KCP hosting; WS and IPC work can reuse the
   existing TCP external-progress contract without waiting for UDP implementation.
2. Complete IPC client/listener/adoption as one usable feature, including Windows
   and POSIX paths, ownership cleanup, examples, public C/C++ headers, and tests.
   Track nonblocking rendezvous and full host-wait integration separately.
3. Add WS logical-message terminals and their lifecycle tests, then verify CHTTP
   client/server terminal propagation, upgrade leftovers, and WSS/H2 ownership.
4. Compose external KCP progress/deadlines on #999 and add bounded receive-credit
   control, preserving existing tagged send and security/FEC behavior. Verify
   slow consumers, host fairness, and timer progress together.
5. Only after these gates, add FlowMQ adapters using its existing protocol and
   pattern state. Define binary-message framing and maximum-size policy there;
   keep the current TCP/TLS path intact.

| Gate | Required executable coverage |
| --- | --- |
| IPC | Real client/server duplex bytes; multiple clients; missing/busy endpoint; full listener/client; invalid/long names; pending-accept stop; adoption failure; partial writes; stale handle; ordinary and replaced pathname preservation; Windows IOCP and POSIX supported backends |
| WS | Tagged complete/fragmented messages; synchronous/asynchronous transport; busy retry without duplicate admission; control/data interleaving; partial/failing transport writes; close/abort with retained output; exactly one terminal; real CHTTP H1/WSS/H2 handoff |
| KCP | Tagged ACK completion; bounded slow receiver; loss/reordering/retransmission; full session/send/receive budgets; maximum message and one over; timer progress under pressure; close/cancel; stale generations; explicit PSK/FEC regressions |
| Shared | First-error propagation; no terminal after admission failure; no resource release before drain; callback reentrancy rejection; C++ header compilation; existing TCP/TLS/Pipe tests unchanged |
| #999 composition | Multiple UDP instances plus TCP/IPC on one backend; local user_data collisions; foreign/duplicate/stale requests; matched malformed completion followed by valid terminals in the same batch; observed-but-not-routed stop; cancellation races; neighbor survival after stop; actual dual-owner callback affinity |
| External protocol progress | KCP deadlines expire with no UDP input; zero receive credit still permits bounded ACK/probe progress; WS/control and TCP work are not starved; borrowed mode never observes or destroys the host backend |

Use the current user presets and CTest registrations, starting with affected
formal suites and then neighboring transport/lifecycle suites. Windows-only
results cannot certify POSIX IPC; unexecuted platform and integration gates must
be reported as such. Performance improvements require separate measurements.

These changes are additive. Do not resize unversioned public structs or change
existing URI, error, send, or stop semantics. Implement each complete feature
with tests before exporting its entry points; no placeholder public APIs.
The consumer migration is opt-in. Rollback removes new adapter selection while
leaving existing TCP/TLS/Pipe/WS/KCP APIs intact; drain active new sessions before
changing SDK/runtime versions. There is no data-format migration in this CNet
design and no new external dependency is required by the selected approach.

Risk classification: **HIGH** for wrong native-handle cleanup, borrowed payload
retention, incorrect logical terminals, or premature destruction; **HIGH** for
an unbounded KCP receive path under a stalled consumer; **MED** for the additional
listener progress surface and cross-platform integration burden. The ownership
and validation gates above are required before FlowMQ can advertise support.

## Portable datagram controls and name lookup

`cnet_datagram_open_external` adds unbound UDP creation on a borrowed NativeIO
backend. Bind before sending/receiving; endpoint queries preserve IPv6 flow/scope.
`cnet_datagram_send_endpoint` copies even zero-byte messages, while the existing
`cnet_datagram_send` admission remains unchanged. Pause cancels demand without
releasing an active request; route its actual terminal and check quiescence
before changing peer association. Options support hop limit and receive/send
buffers before binding. The backend remains owned and observed by the host.

`<cnet/name_lookup.h>` exposes bounded ordered address streams using existing
c-ares progress and strict ASCII LDH hostname validation. Already-encoded
Punycode A-labels pass through unchanged without IDNA validation; raw non-ASCII
names fail closed. Applications accepting Unicode domains explicitly call
[`Salts::IDNA`](../idna/README.md) before submission, then use the same validated
ASCII identity for DNS, TLS hostname verification and SNI. CNet itself has no
IDNA dependency or implicit conversion. Its existing trailing-dot and numeric
address behavior is unchanged; the IDNA strict profile rejects trailing dots.
Numeric literals issue no DNS
request. Every query is scoped to its owner and generation; result overflow is
an error instead of truncated success. Pending is `SALTS_ETIMEDOUT`, exhaustion
is `SALTS_EOF`, and a query deadline is `SALTS_EAI_AGAIN`.

```c
cnet_name_lookup lookup = {0};
cnet_name_query query = {0};
cnet_name_lookup_config config;
cnet_name_lookup_config_init(&config);
int status = cnet_name_lookup_init(&lookup, &config);
if (status == SALTS_OK)
    status = cnet_name_lookup_submit(&lookup, "127.0.0.1", 9, &query);
if (status == SALTS_OK) {
    cnet_ip_address address;
    status = cnet_name_lookup_next(&lookup, query, &address);
    /* Use address only when status == SALTS_OK. */
}
if (query.owner) cnet_name_lookup_query_drop(&lookup, &query);
if (lookup.impl) {
    cnet_name_lookup_close(&lookup);
    cnet_name_lookup_destroy(&lookup);
}
```

For DNS names, call nonblocking `advance` on the serialized owner and integrate
`next_timeout` with the host timer. Close cancels the c-ares channel through its
real callbacks; destroy succeeds only after every query is dropped. A dropped
active query continues occupying bounded callback storage until that terminal.
Optional copied numeric `servers_csv` permits local/private resolver fixtures.
