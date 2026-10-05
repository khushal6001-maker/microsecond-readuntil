# microsecond-readuntil

A zero-copy C++20 daemon for real-time nanopore adaptive sampling: it connects
directly to the MinKNOW gRPC API, ingests raw signal chunks into per-shard
lock-free rings, performs cache-conscious signal-space matching, and dispatches
unblock actions without a Python process or a basecaller in the critical path.

**Status: end to end against a simulator, with a known scaling failure.** The
accept path produces zero accepts against a 64 Mb reference — see Measured
results. Treat the accuracy claims as unvalidated at realistic scale.

**Previously:** Transport, index, policy and daemon
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
accept/unblock/defer rule. Four repetitions per cell, 20 s each, ~17k decisions
per run, microseconds, HdrHistogram at 3 significant figures. Median across the
four reps, with the observed range in brackets.

The matrix crosses shard count against pinning, because those two were previously
confounded. 2 shards means 4 data-plane threads (reader, writer, 2 workers) and
fits this host's 4 physical cores exactly; 4 shards means 6 threads and does not.

| config | p50 | p90 | p99 | p99.9 | max |
|---|---|---|---|---|---|
| **pinned-2** | **16.3** [15.2–16.5] | **24.7** [23.1–25.7] | **49.2** [47.0–50.9] | **158** [139–246] | 719 [387–1238] |
| unpinned-2 | 15.8 [15.6–16.6] | 25.1 [24.2–26.4] | 50.5 [44.9–104.5] | 166 [117–240] | 736 [294–844] |
| pinned-4 | 18.7 [18.0–19.7] | 31.1 [27.9–35.4] | 70.4 [65.7–142.7] | 313 [234–614] | 1376 [1248–4933] |
| unpinned-4 | 18.4 [17.9–18.8] | 31.6 [31.3–33.4] | 83.9 [72.1–350.5] | 387 [352–2519] | 3440 [1458–12484] |

Three things fall out, and only the third is the one that was expected.

**Thread count dominates, not pinning.** Both 2-shard cells beat both 4-shard
cells at every percentile. Six data-plane threads on four physical cores is
oversubscription, and no placement policy fixes that — unpinned-4 produced a
12.5 ms maximum and a 2.5 ms p99.9 in one run. On this host the daemon should run
2 shards; throughput is not the constraint, since Icarust saturates first.

**Pinning does not make the median faster. It makes the tail reproducible.** At a
fixed 2 shards, pinned and unpinned p50 and p90 overlap and are not separable.
What separates them is the *spread* of p99 across repetitions:

| | p99 across 4 reps | spread |
|---|---|---|
| pinned-2 | 46.99, 48.32, 49.97, 50.88 | **3.9 us (8%)** |
| unpinned-2 | 44.86, 45.52, 55.45, 104.45 | 59.6 us (133%) |

That is the honest claim for pinning: not a faster number, a *repeatable* one.
Unpinned runs are sometimes just as quick and sometimes 2.3x worse, because the
scheduler is free to migrate a worker mid-run.

**SMT siblings were being treated as separate cores, and that was a real bug.**
`CorePlan` assigned roles to consecutive CPU ids. On this machine siblings are
adjacent (`0-1, 2-3, 4-5, 6-7`), so reader→cpu0 and writer→cpu1 put the two
hottest threads on one physical core's execution units while a fourth core sat
idle. The fix reads `thread_siblings_list` rather than assuming a stride, because
the enumeration is not portable: VMs commonly pair siblings adjacently while
bare-metal Intel boxes often list all physical cores first, so `cpu0`/`cpu4` are
siblings there. No fixed stride is correct on both. The daemon now prints its
layout and warns when roles have to share a core.

#### The previous table in this README was measured on a broken machine

An earlier revision reported p50 ≈ 27 us, p99 108–319 us and maxima of 4–9 ms.
**Those numbers are withdrawn, not merely superseded.** They were taken while the
host's system drive had *zero* bytes free, which is a pathological state for a VM
whose disk is a dynamically-growing file. The evidence is the unpinned-4 cell,
which is the same configuration measured both times and changed only because the
machine did: p50 went from 27.0–27.3 us to 17.9–18.8 us, and the maximum from
9.2 ms to 1.5 ms, with no code change affecting that path (the SMT fix touches
`--pin` only). Roughly a third of the previously reported median was the
environment.

The lesson is the same one the HdrHistogram change taught, one level further out:
before trusting a latency number, check the instrument *and* the machine. The
provenance block in `matrix.sh` now records disk headroom, hypervisor, governor,
SMT state and kernel command line beside every run for exactly this reason.

#### These are still not bare-metal numbers

`systemd-detect-virt` reports `wsl`, so `matrix.sh` stamps every run
**DEVELOPMENT ONLY** and will only print `PUBLISHABLE` on a host that is not
virtualised, has `isolcpus` on the kernel command line, and is running the
performance governor. None of those hold here, and this is additionally a 4-core
laptop whose thermal management is outside our control. p50, p90 and the
pinned-2 p99 are reproducible enough to develop against; nothing here should go
in a paper as a tail measurement.

Reproduce with:

```bash
REPS=4 SECS=20 bash matrix.sh > results.csv
```

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

### readfish baseline, same host and same simulator

Full setup, TOML, launch commands and the four undocumented fixes needed to make
readfish 2024.3.0 talk to Icarust are in [docs/readfish-baseline.md](docs/readfish-baseline.md).
"per chunk" is the honest unit for both sides: readfish's batch line counts one
entry per read *chunk*, so its `T/N` is per-chunk cost.

| | ours, chr20 | ours, 37 kb toy | readfish unblock-all | readfish targets |
|---|---|---|---|---|
| reference | 64.4 Mb | 37 kb | none | 64.4 Mb |
| per chunk (p50) | 25.6 us | 16.7 us | 124 us | 66 us |
| CPU | 182% | 202% | 15.6% | 25.3% |
| peak RSS | 1.49 GB | 152 MB | 86 MB | 663 MB |
| accepted | **0** | 854 / 1366 | n/a | n/a |

We are **4.9x faster per chunk** at chr20 scale against readfish with no
basecalling and no mapping, so the gap to a production readfish is larger. That is
the one claim that survives this comparison.

We are **8-12x worse on CPU** (182-202% against 15.6-25.3%) and **2.25x worse on
memory** on the same reference. The CPU figure is the design, not a defect: our
workers spin before yielding to keep wakeups off the critical path, while readfish
polls every 400 ms and sleeps. We buy latency with CPU, which is the right trade on
a dedicated host and should be reported as a trade. "Efficient" is the wrong word
for what this daemon provides.

### The accept path does not survive a realistic reference

**0 accepts of 763 decisions against chr20**, where 13 of the 29 sequenced
transcripts map to chr20 and several hundred were expected. The same build on the
37 kb toy reference accepts 854 of 1366 (62.5%).

The mechanism is one number: **candidates per chunk rise from 0.99 to 95.1**, a 96x
flood. `ChannelVotes` keeps 16 diagonal slots and evicts the weakest only when it
holds a single vote, so at ninety-five candidates per chunk a true diagonal is
evicted by noise before reaching `accept_votes = 5`. The slot count and the
threshold were both tuned against a reference 1700x too small.

Every live run before this one used that 37 kb reference, which also fits entirely
in cache — so the cache-conscious lookup was never actually under cache pressure.
The offline bench reports 89.3% TPR at 10 chunks *on chr20*, so offline and live
disagree and at least one is not measuring what it claims. **Resolving that comes
before any further writing.**

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

Two of the three things that were wrong with the earlier measurements are now
fixed. The **instrument** is no longer the limit: HdrHistogram holds 0.1%
relative error across the range, and `LatencyRecorder::backend()` is printed
beside every figure so a fallback bound can never pass as a measurement. The
**machine state** is no longer silently wrong either: `matrix.sh` records disk
headroom, hypervisor, SMT layout, governor and kernel command line next to every
run, because a full disk once cost a third of the reported median without
announcing itself.

The **platform** is still the limit, and no amount of care inside the VM removes
it. `matrix.sh` will not stamp a run `PUBLISHABLE` until
`systemd-detect-virt` reports `none`, `isolcpus` is on the kernel command line
and the governor is `performance`. Run it unchanged on such a host and the
numbers are quotable; run it here and they are not.

## Next steps

- **Real hardware.** Nothing here has touched a sequencer. Icarust is a faithful
  enough API peer to verify protocol and logic, not timing under real load.
- **Bare-metal tail measurement.** This is the one remaining blocker on a
  quotable p99, and it needs hardware rather than code: `matrix.sh` already runs
  unchanged and will stamp its own output `PUBLISHABLE` once the host qualifies.
  A 4-core laptop is a poor choice even bare metal — thermal throttling lands in
  the tail — so prefer a machine with enough cores to leave `isolcpus` a couple
  to spare.
- **Use MinKNOW's `median` and `median_before`** from `ReadData` instead of
  computing a median per read. It removes a sort from the hot path and costs 8
  bytes in `ChunkRef`, which has exactly that much room before it outgrows a
  cacheline. Recall and the vote threshold must be re-measured afterwards.
- **Position the introduction against ReadBouncer**, not readfish alone; it is
  the nearest prior art and its claims need reading carefully.
