# microsecond-readuntil

A zero-copy C++20 daemon for real-time nanopore adaptive sampling: it connects
directly to the MinKNOW gRPC API, ingests raw signal chunks into per-shard
lock-free rings, performs cache-conscious signal-space matching, and dispatches
unblock actions without a Python process or a basecaller in the critical path.

**Status: end to end against a simulator.** Transport, index, policy and daemon
all exist and have been run against [Icarust](https://github.com/LooseLab/Icarust)
in both directions — accepting on-target reads and unblocking off-target ones.
It has **never been run against a real sequencer**, and every latency figure below
comes from WSL2, which is a development platform and not a measurement one.

## What is here

| File | Purpose |
|---|---|
| `src/core/cacheline.hpp` | cacheline constant, padding helper |
| `src/core/spsc_ring.hpp` | bounded SPSC queue with cached head/tail |
| `src/core/arena_pool.hpp` | refcounted arena slot recycling + `BumpArena` for dependency-free tests |
| `src/core/tsc.hpp` | calibrated RDTSCP, invariant-TSC check, coarse histogram |
| `src/core/hdr_latency.hpp` | HdrHistogram percentiles, with the coarse histogram as a labelled fallback |
| `src/core/affinity.hpp` | core pinning, thread naming, SCHED_FIFO, `CorePlan` |
| `src/transport/chunk_ref.hpp` | 56-byte borrowed view of one chunk; nothing is copied after the arena |
| `src/transport/proto_compat.hpp` | field-number assertions against `minknow_api`, so a schema drift fails at compile time |
| `src/transport/auth.hpp` | credential discovery; `describe()` never prints the token |
| `src/transport/live_reads_stream.hpp` | `get_live_reads` bidirectional stream, sharded fan-out, unblock writer |
| `src/index/quantise.hpp` | median/MAD normalisation, quantised events, packed keys, minimizers |
| `src/index/minimizer_index.hpp` | open-addressed index, one entry per distinct key |
| `src/index/batched_probe.hpp` | batched probing with multi-probe over bucket boundaries |
| `src/daemon/policy.hpp` | diagonal voting and the accept / unblock / defer rule |
| `src/daemon/main.cpp` | the daemon |

Steps 1-2 (`src/core`, `test/ring_tsan.cpp`) need no vcpkg, protobuf, gRPC or
nanopore data. The transport layer needs `-DMRU_WITH_TRANSPORT=ON` and the
`minknow_api` submodule.

## Build and run

Submodules first. `minknow_api` is only needed for the transport layer;
`HdrHistogram_c` is what makes the latency percentiles trustworthy, and without it
the recorder silently degrades to power-of-two buckets (it does say so on every
line it prints):

```bash
git submodule update --init --recursive
```

Plain build:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo && cmake --build build -j
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

The daemon needs a pore model TSV and a reference FASTA, and writes its full
latency distribution where you point it:

```bash
./build/mru_daemon --target 127.0.0.1:10001 --model r10.tsv --reference target.fa --latency-out lat.hgrm
```

`--target` defaults to Icarust's position port; a real MinKNOW position differs.
`--pin` pins the data-plane threads to distinct cores and is **off** by default,
because pinning helps only on a host prepared for it and hurts on a shared or
virtualised one. `mru_daemon --help` lists the rest.

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

`core.hdr_latency` is the companion for the measurement path: it checks that a
reported percentile actually tracks the true percentile, rather than that the
histogram merely initialises. See below for why that test exists.

## Measured results

### Decision-path latency

Policy only — signal-space matching for one chunk, diagonal voting, and the
accept/unblock/defer rule. Four runs against Icarust, 512 channels, ~10k
decisions each, microseconds, HdrHistogram with 3 significant figures:

| mode | n | p50 | p90 | p99 | p99.9 | p99.99 | max | mean |
|---|---|---|---|---|---|---|---|---|
| pinned | 10107 | 27.48 | 52.25 | 318.81 | 1194.54 | 4671.66 | 9192.18 | 41.97 |
| pinned | 10115 | 26.41 | 45.34 | 108.19 | 440.30 | 3070.78 | 4121.14 | 31.01 |
| unpinned | 10241 | 27.27 | 51.19 | 170.18 | 826.88 | 2830.78 | 5240.52 | 34.98 |
| unpinned | 10709 | 27.03 | 49.51 | 220.49 | 966.60 | 6994.75 | 8626.64 | 36.87 |

**Quote p50 and p90. Do not quote p99 or beyond from this platform.** p50 holds
at 26.41-27.48 us and p90 at 45.34-52.25 us across all four runs. p99 ranges
108.19-318.81 us pinned and 170.18-220.49 us unpinned: the spread *within* a mode
is larger than the difference *between* modes, so thread pinning and no pinning
are **not separable** here, and no claim about placement should rest on these
tails. WSL2 virtualises the clock and the scheduler, so the tail measures the
hypervisor as much as the daemon.

An earlier commit (`ec1208b`) reported "p99 falls from 224-448 us to a steady
112 us, a 2-4x improvement". **That is withdrawn.** It was an artefact of the
instrument: `tsc.hpp`'s power-of-two buckets meant that at this host's 2418.7 MHz
the only p99 it could ever print was 54.19, 108.38, 216.76 or 433.53 us, and the
four values reported were 111.93, 112.12, 223.66 and 447.59 — every one a bucket
edge. Two runs both reading "112" did not agree to 0.2%; they agreed on an octave,
somewhere in [54, 108] us. The underlying *bug* that commit fixed was real (six
threads were being crowded onto core 0); only the measured gain is retracted.

`test/hdr_latency_test.cpp` now holds that lesson in place. Built against the
fallback it demonstrates the coarse histogram reporting **32767 cycles for the
p50, the p90 and the p99 of one distribution** — three different values collapsed
onto one, with errors of 20% to 60% — and reporting both 1100 and 1900 as 2047.
Against HdrHistogram the same input returns within 0.072% at every percentile.

### Selectivity

Same daemon and the same reads, changing only which sequence the index was built
from:

| index built from | accepted | unblocked | deferred | candidates |
|---|---|---|---|---|
| the sequenced FASTA | 795 | 362 | 15475 | 20833 |
| a different sequence | 0 | 577 | 19600 | 324 |

Zero false accepts off-target, with 64x fewer candidates. The live on-target
accept rate of 68.7% falls between the 66.3% (2.5 chunks) and 74.0% (5 chunks)
predicted offline, which is an independent check that the offline bench and the
live daemon agree.

Offline against human chr20, with a composition-matched (shuffled) control, TPR
at **zero** false positives improves with read length: 66.3% at 2.5 chunks, 74.0%
at 5, 89.3% at 10, 97.0% at 20. Deferral is sound rather than hopeful because the
asymmetry is measured: off-target votes stay flat at 3-4 however long the read
runs, since random diagonal coincidences do not concentrate on one diagonal, while
on-target votes grow 8 -> 23 -> 60 -> 174 over the same lengths. `max_chunks` is
therefore an operator's economic choice, not a constant.

### Frozen index geometry

3 bits per event, 13 events per key, uniform quantisation, minimizer window 10,
10 samples per event, z-clip 3.0. Multi-probe over the two nearest bucket
boundaries is worth about +5 points of seed recall: +14% more seeds at w=10,
against +74% at w=1 — the minimizer window already recovers most of what
multi-probe would otherwise find.

### Negative results, recorded rather than buried

- **Software prefetching does not help, and this kills a novelty claim.** Batched
  prefetch measures 0.76-0.99x against the plain scalar loop across runs — at best
  neutral, usually a small loss. The out-of-order engine already overlaps several
  outstanding misses, and a few hundred instructions of reorder window is a larger
  effective prefetch distance than this code can express. Explicit prefetching
  wins when a dependency chain stops the hardware from running ahead, which is not
  the situation here. **"Batched software prefetch" is therefore not supportable as
  a contribution** — the structure is kept because it is harmless and readable, not
  because it is faster.
- **Adaptive quantisation loses** to uniform bucketing on real data.
- **Throughput is not a differentiator against readfish.** We issued 10089
  unblocks in 20 s (504/s), readfish 3098 in 7.3 s (424/s). Both are limited by
  how fast Icarust produces reads, not by either client; claiming a throughput
  win here would be measuring the simulator.
- **The 7x latency comparison against readfish is not apples-to-apples.** Our
  27.8 us median is per chunk; readfish's ~197 us is per read inside its batch
  loop. readfish was run with a `no_op` caller, so neither figure includes
  basecalling, and a comparison with Dorado in the loop is not possible here.
- **`effective_key_space()` was mathematically invalid** and has been purged. Its
  Poisson fit assumed uniform key probability; the fitted K tracked n instead of
  converging. Every conclusion derived from it, including an earlier 3x15 geometry
  freeze, was void and was redone with a model-free frequency measure.

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

The instrument is no longer the limiting factor — HdrHistogram gives 0.1%
relative error across the range, and `LatencyRecorder::backend()` is printed
beside every figure so a fallback bound can never be mistaken for a measurement.
The platform still is. p50 and p90 are reproducible on WSL2; p99 and beyond need
bare metal.

## Next steps

- **Real hardware.** Nothing here has touched a sequencer. Icarust is a faithful
  enough API peer to verify protocol and logic, not timing under real load.
- **Bare-metal tail measurement** with the kernel configuration above, which is
  the only way the p99 claim becomes quotable.
- **Use MinKNOW's `median` and `median_before`** from `ReadData` instead of
  computing a median per read. It removes a sort from the hot path and costs 8
  bytes in `ChunkRef`, which has exactly that much room before it outgrows a
  cacheline. Recall and the vote threshold must be re-measured afterwards.
- **Position the introduction against ReadBouncer**, not readfish alone; it is
  the nearest prior art and its claims need reading carefully.
