# microsecond-readuntil

A zero-copy C++20 daemon for real-time nanopore adaptive sampling: it connects
directly to the MinKNOW gRPC API, ingests raw signal chunks into per-shard
lock-free rings, performs cache-conscious signal-space matching, and dispatches
unblock actions without a Python process or a basecaller in the critical path.

**Status: step 1-2 of the build plan.** Only the core data plane exists. There is
no transport, no index, and no daemon yet — by design, so the concurrency
primitives can be proven correct before anything depends on them.

## What is here

| File | Purpose |
|---|---|
| `src/core/cacheline.hpp` | cacheline constant, padding helper |
| `src/core/spsc_ring.hpp` | bounded SPSC queue with cached head/tail |
| `src/core/arena_pool.hpp` | refcounted arena slot recycling + `BumpArena` for dependency-free tests |
| `src/core/tsc.hpp` | calibrated RDTSCP, invariant-TSC check, coarse histogram |
| `src/core/affinity.hpp` | core pinning, thread naming, SCHED_FIFO, `CorePlan` |
| `test/ring_tsan.cpp` | five-part stress test, framework-free |

Nothing in step 1-2 needs vcpkg, protobuf, gRPC, or any nanopore data. The
`transport` feature in `vcpkg.json` is there for step 3.

## Build and run

Plain build:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
```

```bash
cmake --build build -j
```

```bash
./build/mru_ring_stress
```

ThreadSanitizer — **this is the run that matters**, and it must be done on
Linux or WSL2 because MSVC has no TSan:

```bash
cmake -S . -B build-tsan -DMRU_TSAN=ON -DCMAKE_CXX_COMPILER=clang++
```

```bash
cmake --build build-tsan -j && ./build-tsan/mru_ring_stress
```

AddressSanitizer, which is what catches a broken arena refcount as a
use-after-free rather than as silent corruption:

```bash
cmake -S . -B build-asan -DMRU_ASAN=ON && cmake --build build-asan -j
```

Long soak (200M ops, 8 shards) once the short run is clean:

```bash
ctest --test-dir build -L soak --output-on-failure
```

Arguments are `mru_ring_stress [ops] [shards] [arena_iters]`; `shards` must be a
power of two.

## What the stress test proves

1. **`spsc_sequence`** — nothing lost, duplicated or reordered across 20M items.
2. **`sharded_fanout`** — the real topology: one producer, N rings keyed by
   `channel & mask`, N pinned consumers, per-shard ordering preserved.
3. **`drop_conservation`** — with the production backpressure policy (drop, never
   block), `offered == pushed + dropped` and every pushed item is consumed.
4. **`arena_pool_refcount`** — the important one. The reader writes a magic value
   into arena memory and hands out spans; workers verify it. If the refcount
   protocol let a worker reset the arena while the reader was still dispatching,
   the magic value would be clobbered. The pool is deliberately starved (8 slots,
   4 workers) so the handoff runs hot. Also asserts no slot leak and exactly one
   `Reset()` per acquisition.
5. **`tsc_and_affinity`** — calibration is sane, invariant TSC is *checked* rather
   than assumed, pinning reports honestly.

Pinning failures are informational, not fatal: restricted cpusets legitimately
refuse them and that must not break a build.

## Before any number goes in the paper

Measurements from Windows, WSL2 or any VM are for development only — the clock
is virtualised and the scheduler is not yours. Publish only from bare-metal
Linux configured as:

```bash
sudo cpupower frequency-set -g performance
```

plus `isolcpus=`, `nohz_full=` and `rcu_nocbs=` on the kernel command line for
the measured cores, IRQ affinity moved off them, and turbo either pinned or
disabled. Record every one of these settings in the methods section; "we pinned
threads" without the kernel configuration is not reproducible.

The histogram in `tsc.hpp` uses power-of-two buckets, so its percentiles are
upper bounds with ~2x resolution. Fine for "is the data plane sane"; replace it
with HdrHistogram before producing figures.

## Next steps

- **Step 0 (do before more code):** confirm Icarust still tracks your MinKNOW API
  version — if it does, you do not need to write a mock server. Also read
  ReadBouncer's claims carefully; it is the nearest prior art and the
  introduction has to be positioned against it, not against readfish alone.
- **Step 3:** `third_party/minknow_api` submodule, `-DMRU_WITH_TRANSPORT=ON`,
  manager connect, `get_live_reads` round trip, unblock echo verified.
- **Step 4:** quantised-event index with batched prefetch.
- **Step 5:** instrumentation, saturation sweep, readfish baseline.
