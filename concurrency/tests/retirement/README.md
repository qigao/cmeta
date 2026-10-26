# Disruptor retired-slot ownership regression

Tracked by [Salts Issue #1319](https://github.com/qigao/salts/issues/1319).

This standalone Linux CMake project compiles the actual production
`concurrency/src/disruptor.c` together with the actual
`platform/src/thread_posix.c`, without vcpkg, mocked atomic operations,
changed public headers, or an installed SDK.

## Why a release store is necessary

A publisher may reuse a ring slot after an acquire load of the cached
`slowest_consumer.sequence` without refreshing the underlying consumer
or worker-completion cursor. The old consumer's non-atomic payload read
must happen-before that publisher's next payload write:

    old payload read
        -> consumer cursor / worker completion release
        -> frontier refresh acquire
        -> cached frontier release
        -> next publisher acquire
        -> payload reuse

Changing only numeric counters is insufficient. The earlier
[exact-source TSan failure](https://github.com/qigao/salts/actions/runs/40232943463)
reported real payload read/write races in the Broadcast and 4x4 MPMC tests.
After changing the three advancement stores to release, the same
[exact-source TSan test](https://github.com/qigao/salts/actions/runs/40233132136)
passed all four Concurrency cases.

## Regression coverage

The FIFO test exercises Broadcast and Worker Pool with 20,000 frames each,
including capacity-full rejection, reverse-order publication/completion,
and immediate slot reuse. The MPMC test has four concurrent publishers,
four workers, a bounded 64-slot ring, and 12,000 uniquely verified
messages; the mutex protects only the testing verdict, never the
Disruptor storage or cursor operations.

The dedicated workflow makes GCC TSan and GCC ASan/UBSan **required**,
and exercises native GCC and Clang C11 baseline tests. No TSan suppression,
feature workaround, or TinyCC backend is used.

### Standalone TSan reproduction

    cmake -S concurrency/tests/retirement -B build/retirement -G Ninja -DCMAKE_C_COMPILER=gcc -DSALTS_RETIREMENT_SANITIZER=tsan
    cmake --build build/retirement --parallel 2
    ctest --test-dir build/retirement --repeat until-fail:5 --output-on-failure

Passing the above is not a claim of installed SDK, Windows/macOS, ARM64
weak-memory, or CFlow/ServiceHost lifecycle qualification. The independent
TCC experiment remains Draft in PR #1316.
