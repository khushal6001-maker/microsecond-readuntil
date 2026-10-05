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
| `src/daemon/diagonal_votes.hpp` | bounded diagonal voting, shared by the daemon and the benches |
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

**3 bits per event x 14 events = 42-bit keys, minimizer window 10, 10 samples per
event, z-clip 3.0, uniform quantisation, 4 probes per seed, accept at 5 votes.**

Re-frozen 2026-10-05 by `bench/geometry_sweep.cpp` on the full 64 Mb chr20 against a
composition-matched (shuffled) control, using the daemon's own voting code, then
confirmed live against Icarust. TPR at the lowest threshold with **zero** false
positives:

| geometry | key bits | distinct keys | cap saturation | TPR @2 | @5 | @10 | @20 chunks |
|---|---|---|---|---|---|---|---|
| 3 x 13 | 39 | 1.96M (16.7%) | 51.1% | 26.3% | 51.3% | 53.7% | 65.7% |
| **3 x 14** | **42** | **3.33M (28.5%)** | **34.2%** | **35.7%** | **57.3%** | **59.0%** | **69.7%** |
| 4 x 12 | 48 | 6.78M (58.1%) | 14.6% | 31.7% | 39.3% | 42.7% | 49.7% |
| 4 x 13 | 52 | 8.33M (71.5%) | 12.2% | 30.7% | 38.0% | 41.0% | 48.3% |
| 5 x 13 | 65 | rejected: a key must pack into 64 bits | | | | | |

Off-target votes never exceed 4 in any cell, which is what sets `accept_votes = 5`.

Live on a 64 Mb index, 22 s, 2 shards, pinned:

| geometry | accept rate | p50 | p90 | p99 |
|---|---|---|---|---|
| 3 x 13 | 27.7% | 25.60 | 44.82 | 143.40 |
| **3 x 14 (frozen)** | **44.1%** | **23.89** | **41.06** | **119.69** |
| 4 x 13 | 36.5% | 23.88 | 39.37 | 114.01 |

Offline predicts 59.0% at 10 chunks against 44.1% live, which is the agreement to
expect once reads shorter than `max_chunks` and real simulator noise are in play.
The same two numbers were 89.3% and 0% before the offline bench and the daemon were
made to share one implementation.

**4 bits buys specificity and loses the argument.** It is far more selective -- 12.2%
cap saturation against 34.2%, 22 median candidates per read against 463 -- but it
discards too many true seeds to reach the threshold. Candidate counts are not the
objective; TPR at zero FPR is.

#### This parameter has been frozen three times and the first two were wrong

Worth reading before touching it a fourth time.

1. **3 x 15, void.** Rested on `effective_key_space()`, whose Poisson fit assumed
   uniform key probability; the fitted K tracked n instead of converging. Purged.
2. **"4 x 13 wins", void.** Came from live runs taken while the cross-chunk diagonal
   was computed chunk-relative (below), so votes could not accumulate across chunks.
   Under that bug, suppressing candidates looked like the priority. With it fixed the
   4-bit cells are the worst of the four.
3. **3 x 14**, the above.

The lesson that generalises: a 37 kb reference will cheerfully endorse the wrong
geometry, and so will a bench that does not run the code the daemon runs.

### The cross-chunk diagonal was chunk-relative

`SeedMatches::seed_offset` is an offset into the query, and the query is one chunk,
so offsets restart at zero every chunk. A read starting at reference position `d`
therefore produced a true diagonal of `d` for its first chunk, `d + 180` for its
second, `d + 360` for its third. Votes for the correct location could never add up
across chunks; only within one chunk.

The tell was not a low score but an inverted one. The fraction of reads whose best
diagonal was actually *correct* **fell** as reads got longer -- 49/300 at 2 chunks
down to 7/300 at 20 for 3 x 13 -- so more evidence produced a worse answer, which is
the signature of accumulating the wrong quantity rather than of a weak signal.
Accepts still happened, on whatever diagonal collected coincidences inside a single
chunk, which is why this hid behind a plausible-looking TPR.

`ChannelVotes::events_consumed` now carries the read's running event base, so every
chunk of a read agrees on one diagonal. With that fixed, correct-diagonal counts
rise with read length as they must: 118 -> 160 -> 170 -> 196 of 300.

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
