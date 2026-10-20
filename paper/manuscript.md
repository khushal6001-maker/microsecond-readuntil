# Microseconds Are Not the Problem: A Lock-Free Soft-Real-Time Data Plane for Nanopore Adaptive Sampling, and the Signal-to-Seed Bottleneck That Bounds It

**Abstract**

Oxford Nanopore adaptive sampling ("Read Until") requires a client to decide, within a
physical deadline, whether to eject a molecule still transiting the pore. We make two
contributions. First, we show the decision budget is not the obstacle: a single-process C++20
data plane with single-copy ingestion, zero-copy propagation, lock-free single-producer
single-consumer rings and a reference-counting arena protocol reaches a median per-chunk
decision latency of 124.8 µs [115.6–132.3] against a derived service bound of 1.56 ms, at
3.70 GiB peak resident set over a 64 Mb reference index of 63.9 M minimizers, sustaining
604–645 chunk/s with zero drops across nine 40 s runs. For calibration, the bare orchestration
layer of a basecalling client (readfish with a no-operation caller) costs 124.3 µs per
read-chunk on the same rig — indistinguishable from our *complete* signal-space decision.
Second, we show where the real limit lies. We frame Read Until as a soft-real-time **loss
system**, in which a mean comfortably inside budget is consistent with an arbitrary
deadline-miss rate, and we report a distribution against a derived budget rather than a mean.
Our pipeline resolves 17.6% [16.6–18.8] of reads at a zero-false-positive threshold where
RawHash2 resolves 91.8% [91.2–92.4] on identical input. We account for this gap without appeal
to the decision rule. We proposed a seed-survival law `P(key) = a^E` in the per-event bucket
agreement `a` and key length `E`, argued it was a lower bound, and then **refuted our own
bound**: once event retention is data-dependent the governing form is `P(key) ≈ (1−g)^E · a^E`,
where `g` is the rate at which the two event sequences fail to correspond at all. Measured
under one estimator on identical input at realistic motor-dwell dispersion, RawHash2 achieves
`a` = 0.911, `g` = 0.092 at `E` = 8, giving `P(key)` = 0.254 and 467 anchors per read, against
our `a` = 0.783, `g` = 0.196 at `E` = 12, giving 0.0055 and 14.9 anchors. The 46-fold
key-match difference decomposes into 9.3× from encoding and segmentation and 4.9× from key
length; anchor supply differs 31-fold. We identify three root causes — asymmetric event
retention between reference and query encoders, a quantiser that spends too little alphabet on
agreement, and recoverable loss inside the chainer — and argue that `a` and `g` should be
measured and reported before any decision rule is proposed.

**Keywords:** nanopore sequencing, adaptive sampling, soft real-time systems, lock-free data
structures, raw signal mapping, quantisation

---

## 1 Introduction

Nanopore sequencing reads a molecule as it translocates a pore, emitting an ionic current
trace in real time. Because the molecule is still in the pore while data arrives, the
instrument exposes an interface — Read Until — that lets a client request ejection of a
molecule it does not want, freeing the channel for another. The economic case is direct:
on-target yield rises without changing library preparation.

The interface imposes a deadline that is physical rather than conventional. Signal arrives in
chunks at a fixed cadence, and a chunk describes bases that have *already* passed through the
pore. A decision delivered after the molecule has finished translocating has no value at all;
it does not merely arrive late. With `C` active channels delivering a chunk every `T_c`
seconds, and a tolerance of `W` chunks before the decision is worthless, the admissible mean
service time per decision is

&nbsp;&nbsp;&nbsp;&nbsp;`S < W · T_c / C`

which at `W` = 2, `C` = 512 and `T_c` = 0.4 s gives **1.5625 ms**, hereafter 1.56 ms.

Two framings follow, and the paper is organised around them.

**Soft real time.** Utility falls to zero at the deadline rather than degrading smoothly. This
makes the *distribution* of decision latency, not its mean, the quantity of interest.

**A loss system, not a delay system.** Under saturation a client must choose between queueing
work and discarding it. Queueing converts decisions that would have been useful into decisions
that arrive too late, so dropping is correct rather than merely pragmatic. The consequence for
evaluation is sharp and, we argue, under-appreciated: **a mean comfortably inside the budget is
consistent with an arbitrary deadline-miss rate.** A system reporting "mean 124 µs against a
1.56 ms budget" has said nothing about whether it meets its deadline.

Existing Read Until work does report latency, in two forms: signal consumed before a decision
(mean bases or chunks per read), and mean processing time per read. What we did not find in
the systems we surveyed is a deadline-miss rate against a stated service bound, a latency
distribution with tail percentiles, or the fraction of decisions arriving too late to act on.
That gap — reporting a distribution against a derived budget, with the instrument and
environment characterised well enough for the tail to carry meaning — motivates the first half
of this paper.

**Contributions.**

1. A lock-free soft-real-time data plane for signal-space adaptive sampling, with a
   reference-counting arena protocol that makes reset-under-dispatch unrepresentable by
   construction rather than by a timing assumption (§3), evaluated as a distribution against a
   derived budget over nine runs (§4).
2. A negative result, explained rather than excused: our pipeline is 5.2× less accurate than a
   published signal-space mapper on identical input, and we locate the cause before seeding
   (§5).
3. A seed-survival law, including the refutation of our own earlier formulation of it, and an
   estimator-symmetric measurement of its two parameters for both systems (§6).
4. Three measured root causes and the design prescriptions that follow (§7).

**What this paper does not claim.** We do not claim competitive mapping accuracy; we report
17.6% against 91.8% and explain it. We do not claim a novel matching method; quantised-event
hashing is established. We do not claim trustworthy tail latency: every measurement here was
taken under a hypervisor, and §9 scopes what that permits. All signal is simulated.

---

## 2 Background and related work

**The interface.** MinKNOW exposes a bidirectional streaming RPC delivering raw signal chunks
per channel and accepting actions (eject, or stop receiving further data for a read). Read
identity is carried by a string; a numeric read index was removed from the API in version 6.0,
which constrains client-side data structures.

**Basecalling clients.** readfish and similar clients basecall each chunk and map the resulting
sequence. This is accurate and reuses mature tooling, but places a neural network and a
sequence aligner inside the decision loop.

**Signal-space mappers** avoid basecalling by encoding the reference into expected signal under
a pore model and matching quantised query events directly. UNCALLED uses an FM-index over
events; Sigmap and RawHash use seed-and-chain over quantised event k-mers; RawHash2 adds an
adaptive quantiser; RawAlign adds alignment to seeding; Sigmoni classifies with an r-index over
a small picoamp alphabet. A recent benchmarking framework, RawBench, decomposes raw signal
analysis into reference encoding, segmentation, and matching, and evaluates three segmentation
methods — t-test changepoint detection, basecaller move tables, and a learned segmenter. We
adopt that decomposition, splitting matching into quantisation, seeding and chaining, because
our argument needs that granularity.

**The pipeline is ordered, and earlier stages bound later ones.** Reference encoding →
segmentation → quantisation → seeding → chaining. No chaining strategy can recover evidence
that quantisation destroyed. This observation is the spine of §5–§7.

**Streaming decision structures.** Misra–Gries and Space-Saving maintain approximate frequent
items in bounded space. We use a set-associative variant (§3.5) whose update cost is O(ways)
rather than O(counters), which matters on a decision path.

**Simulation.** Squigulator generates signal with configurable dwell-time dispersion
(`--dwell-std`); its default is `--dwell-mean 9.0 --dwell-std 4.0`, a coefficient of variation
of approximately 0.44. Icarust serves a live Read Until API but emits a fixed sample count per
k-mer, so it produces the CV = 0 case exactly; we patched it to draw lognormal dwell.

---

## 3 The data plane

### 3.1 Design constraints

Four constraints shape the design. The decision path must not allocate, because an allocator
excursion is unbounded. It must not take a lock, because a blocked worker holds a deadline it
cannot meet. State must be O(channels) with compile-time per-channel constants, because read
length is set by library preparation and is not observable in advance. And backpressure must
discard rather than queue, for the loss-system reason of §1.

### 3.2 Single-copy ingestion, zero-copy propagation

Signal is copied exactly once, by the protobuf runtime, into arena-backed storage at the
transport boundary. Nothing downstream copies signal again. Workers receive a 56-byte borrowed
descriptor, `ChunkRef`, holding an absolute sample index, borrowed pointers into arena-owned
read-identifier and payload storage, the owning arena slot, a receive timestamp, and a narrow
tail of lengths and type codes. Declared in natural reading order the structure is 72 bytes;
field-ordered for size it is 56, and a compile-time assertion that it does not straddle a
cacheline is what caught the difference.

Two narrowings are guarded at ingest. Chunk length drops from 64 to 32 bits because a chunk is
sub-second and 2³² samples is about twelve days at 4 kHz. The read-identifier length drops to
16 bits because identifiers are 36-character UUIDs. The absolute sample index is deliberately
*not* narrowed: a 72-hour run at 4 kHz already reaches ≈10⁹, uncomfortably close to 2³².

### 3.3 The arena reference-counting protocol

The hazard is reset-under-dispatch: an arena slot recycled while a descriptor referring to it
is still in a ring. Because the memory remains mapped and arena-owned, the failure is silent
payload corruption rather than a crash.

The protocol removes it structurally. `acquire()` returns a slot at refcount 1, and that
initial reference is held by the **reader**, not by a consumer. The reader pushes descriptors
to `n` shard rings, raising the count to `1 + n`, and only then releases its own. The count can
therefore reach zero only after every worker reference has been taken and dropped, and the slot
cannot be reset while any dispatch is outstanding. No bound on how long a worker takes is
required. Figure 3.2 gives the state machine: free → acquired (count 1, reader) → dispatched
(count 1 + n) → draining (count n, reader has released) → reset → free.

### 3.4 Lock-free handoff and sharding

Handoff uses single-producer single-consumer rings with free-running 64-bit counters and
power-of-two capacity, so the index is a mask rather than a modulus and the full capacity is
usable rather than capacity − 1. Producer and consumer each cache the other's index, removing
cross-core loads from the unsaturated steady state; head, tail and both cached copies are
separately cacheline-aligned. Shard rings hold 2048 `ChunkRef` (114,688 B); action rings hold
1024 `Decision` (73,728 B). At four shards the rings total under 0.75 MB, negligible beside the
index.

Chunks are sharded by `channel & mask`, so all per-channel state is reached by exactly one
thread **by construction**. This is what makes the lock-free claim cheap: there is no shared
mutable per-channel state to protect.

Backpressure is drop-never-block, with the invariant `offered = pushed + dropped` maintained by
counters, so a drop is reported rather than absorbed.

### 3.5 Bounded per-channel state

Per-channel state comprises a scaling estimate and validity flag, a chunk counter, the
consumed-event base that makes seed diagonals read-relative, and a decision structure. The
decision structure dominates. The set-associative vote table is 64 sets × 4 ways × 12 B =
3,072 B per channel (1.57 MB at 512 channels, 8.22 MB at 2675); its update cost is O(4) rather
than the O(256) a classical Misra–Gries table of the same counter count would pay on the
decision path, and its expected retention threshold is m/320 against m/257. The anchor-history
chainer is 1024 anchors × 14 B = 14,336 B per channel. Every figure is a compile-time constant
times the channel count; nothing depends on read length or elapsed run time.

### 3.6 Thread topology

Threads are placed from the kernel's SMT sibling sets, read from
`/sys/devices/system/cpu/cpu*/topology/thread_siblings_list`, rather than from processor-index
arithmetic. This is not fussiness: our measurement host enumerates siblings adjacently
(`0-1 2-3 4-5 6-7`), so an earlier rule that gave each role "the next logical processor" placed
the reader and writer on *the same physical core* while two whole cores sat idle. The
relationship is not portable, and a placement derived from identifiers is correct on some
machines and wrong on others.

At two shards the data plane is four threads (reader, writer, two workers), which fits four
physical cores. At four shards it is six, which does not.

---

## 4 Evaluation of the data plane

### 4.1 Method and instrumentation

Latency is the interval from the transport read returning to the decision being reached. It is
recorded with HdrHistogram at three significant figures, bounding relative error at 0.1% across
the dynamic range. The recorder reports its own backend on every line, so a degraded estimate
cannot be mistaken for a measurement.

This instrumentation is a correction. An earlier power-of-two histogram reported four 99th
percentiles of 111.93, 112.12, 223.66 and 447.59 µs across four runs. At the host's calibrated
invariant TSC frequency of 2418.7 MHz the only values representable in that range are 54.19,
108.38, 216.76 and 433.53 µs. Every reported value was a bucket boundary; two runs "agreeing"
at 112 µs agreed only that the true value lay in [54.19, 108.38]. The apparent reproducibility
was a resolution artefact, and a conclusion had been drawn from it. The general form is worth
stating: *any* instrument whose quantisation is coarse relative to the between-run variance will
report stable values and thereby invite an inference of stability.

The measurement harness records per run the virtualisation status, SMT topology, TSC capability
flags, clocksource, kernel command line including isolation parameters, frequency governor,
turbo state, free storage and load average, and refuses to label a run publishable unless
virtualisation is absent, cores are isolated and the governor is set to performance. Every run
reported here carries the development-only label; §9 states what that permits.

**Host.** Intel Core i5-1135G7, 4 physical cores / 8 logical, under WSL2. **Regimes.** R1: a
37 kb reference under constant dwell. R3: a 64 Mb human chr20 reference under variable dwell
(CV 0.35) at the frozen configuration — 4 bits × 12 events, minimizer window 1, detected-event
segmentation, two shards, pinned.

### 4.2 Placement and thread count

Table 1 gives R1 over four repetitions per cell, medians across runs.

**Table 1.** Regime R1, 37 kb reference, four repetitions per cell. Median across runs, µs.

| configuration | p50 | p90 | p99 | p99.9 |
|---|---|---|---|---|
| pinned, 2 shards | 16.3 | 24.7 | 49.1 | 158.0 |
| unpinned, 2 shards | 15.8 | 25.1 | 50.5 | 165.7 |
| pinned, 4 shards | 18.7 | 31.1 | 70.4 | 312.7 |
| unpinned, 4 shards | 18.4 | 31.6 | 83.8 | 386.7 |

**Thread count dominates placement.** Both two-shard cells beat both four-shard cells at every
percentile. Six data-plane threads do not fit four physical cores, so the four-shard cells
oversubscribe however threads are assigned.

**Pinning does not reduce the median; it makes the tail reproducible.** At fixed shard count the
pinned and unpinned ranges overlap at p50 and p90. The 99th percentile across the four
repetitions is 46.99, 48.32, 49.97, 50.88 µs pinned — a spread of 8% — against 44.86, 45.52,
55.45, 104.45 µs unpinned, a spread of 133%. The defensible claim about processor affinity is
therefore about variance, not location.

### 4.3 Genome scale: regime R3

Nine 40 s runs. The daemon's own startup line confirms the configuration in every log:
63,981,237 reference bases, geometry 4 × 12 = 48-bit keys, window 1, 63,981,218 minimizers,
32,888,127 distinct keys, 512.0 MiB table + 204.3 MiB positions.

**Table 2.** Regime R3, nine runs. Median across runs, with observed range.

| | p50 | p90 | p99 | p99.9 | mean |
|---|---|---|---|---|---|
| µs | 124.8 [115.6–132.3] | 192.8 [183.0–306.0] | 370.2 [346.8–1969.3] | 1224.5 [1044.9–3284.0] | 138.2 [131.2–206.0] |

Peak resident set is 3.70 GiB in every run — this is the maximum including the index-build
transient; the steady-state index structures are 716 MiB. Offered chunk arrival is 604–645 per
second, set by the simulator's channel count, sampling rate and read length rather than by the
daemon. **No chunks were dropped and no deadline misses were recorded in any of the nine runs.**

At the median this is a utilisation of **0.080** against the 1.56 ms bound. A 10-chunk read
completes its decisions in **1.25 ms** — a derivation, ten chunks times the measured median
per-chunk latency, not an independent measurement.

**The tail is where the loss-system framing earns its place.** Eight of the nine runs sit
strictly inside the budget through the 99.9th percentile, at 1.04–1.40 ms. The ninth does not:
one run reports p99 = 1969.3 µs and p99.9 = 3284.0 µs, and is also the worst of the nine at p90
and at the mean. Figure 5.4 shows it separated from the other eight.

We do not read this as a deadline miss. Across the nine runs the observed *maximum* spans
2.0 ms to 59.4 ms — two orders of magnitude in a quantity the software should not vary in at
all — and, tellingly, the run with the largest maximum (59.4 ms) is **not** the run with the
worst p99; its p99 is 630 µs. Maximum and p99 are not correlated here, which is the signature of
externally injected stalls rather than of slow decisions. On a hypervisor without core
isolation, this measurement cannot establish that the system meets its deadline, and it cannot
establish that it misses it. That is precisely the claim §1 says the field does not currently
make, and stating it honestly is part of the contribution.

### 4.4 Comparison with existing clients

Comparisons are reported with their boundaries attached, because the three systems do not
divide the work identically.

**Table 3.** Cost comparison on the same host. readfish figures are its own progress line
divided out; processor and memory figures are from `/usr/bin/time -v`.

| system | mode | per read-chunk | processor | peak RSS |
|---|---|---|---|---|
| readfish | unblock-all (no-op caller) | 124.3 µs | 15.6% | 86.0 MiB |
| readfish | targets (caller + mapper) | 66.2 µs | 25.3% | 663.4 MiB |
| this work | frozen, 64 Mb, 2 shards pinned | 124.8 µs | 181.5–202.3% | 3786 MiB |

**The architectural point.** readfish with a no-operation caller performs no basecalling and no
mapping; 124.3 µs is its orchestration alone, a lower bound on its cost. Our *complete*
signal-space decision over a 64 Mb index costs 124.8 µs. The orchestration layer of a
basecalling client is already as expensive as an entire signal-space decision. This does not
establish superiority — a production readfish does work this configuration omits — but it does
establish that the decision budget is not what stands between signal-space matching and
deployment.

The cost is processor time. We consume 181.5–202.3% of one core against readfish's 15.6%, an
eight- to twelvefold ratio, because workers spin before yielding. That is the price of the low
median and we report it alongside.

**Against a signal-space mapper.** RawHash2, single-threaded on identical reads with the same
pore model, requires 10.64 ms of processor time per read including index load, or 10.18 ms
excluding the load a long-running daemon pays once. Our comparable figure is the derived
1.25 ms. The ratio is 8.2×, and it must be read with §5: a latency ratio between a system that
resolves reads and one that largely does not is a bound on data-plane cost, not a performance
result.

---

## 5 The accuracy gap

### 5.1 Head-to-head

Both systems were run on the same 20 Mb chr20 reference, the same Squigulator-derived R10.4
9-mer pore model on both sides, the same SLOW5 files, three seeds, 500 reads per cell, at
Squigulator's default dwell dispersion (CV ≈ 0.44).

Our figures are true-positive rates at the lowest threshold giving zero false positives on a
composition-matched shuffled control. RawHash2's are correctly-mapped fractions at its own
default operating point, which is **not** zero-false-positive: 8.4–10.6% of control reads are
mapped. The comparison is therefore of magnitude, not of level, and we state it as such.

**Table 4.** Accuracy, three seeds, 500 reads per cell, CV ≈ 0.44.

| system | seed 42 | seed 43 | seed 44 | mean [range] |
|---|---|---|---|---|
| this work (anchor-history, L = 48) | 18.8 | 17.4 | 16.6 | **17.6** [16.6–18.8] |
| RawHash2 (defaults) | 92.4 | 91.8 | 91.2 | **91.8** [91.2–92.4] |

A factor of 5.2. The rest of this paper accounts for it.

### 5.2 Replication is not optional at this scale

Table 5 gives six decision rules in triplicate (Figure 4.1).

**Table 5.** Decision rules at the same operating point, three seeds.

| decision rule | s42 | s43 | s44 | mean [range] |
|---|---|---|---|---|
| exact-diagonal votes (256 counters) | 14.2 | 15.0 | 13.0 | 14.1 [13.0–15.0] |
| greedy chains, β = 0 *(control)* | 9.6 | 10.8 | 7.0 | 9.1 [7.0–10.8] |
| greedy chains, β = 12 | 17.4 | 16.6 | 15.6 | 16.5 [15.6–17.4] |
| anchor-history, L = 48 | 18.8 | 17.4 | 16.6 | 17.6 [16.6–18.8] |
| anchor-history, L = 128 | 19.6 | 19.4 | 18.0 | **19.0** [18.0–19.6] |
| anchor-history, L = 48, occurrence filtering | 17.8 | 15.8 | 15.4 | 16.3 [15.4–17.8] |

Three observations.

**The β = 0 control is the informative row.** With gap tolerance removed, chaining is *worse*
than the vote table — 9.1% against 14.1% — as expected when it tracks 24 candidates where the
table tracks 256. The improvement is therefore attributable to the tolerance, not to the change
of data structure, which a comparison without the control could not establish.

**The best decision rule is +35% relative over voting**, 19.0% against 14.1%. No rule we tried
exceeded that.

**Single rules vary by up to 3.8 points across seeds** — the control spans 7.0 to 10.8 — which
is comparable to the gaps *between* rules, and the ranges of adjacent rules overlap. Any
comparison between configurations differing by less than roughly four points requires
replication to mean anything. We learned this the expensive way: an earlier single-run table
reported that increasing look-back from 48 to 128 did not help. It does, by 1.4 points of mean
and positively in every seed; the single run that supported the old claim was a different seed
from the other five rows of the same table.

Even at its best, the decision rule moves accuracy by tens of percent where the gap to be
closed is a factor of five. The deficiency is not here.

---

## 6 A seed-survival law

### 6.1 The model, and the refutation of our own bound

Let `a` be the probability that reference and query assign the same quantiser bucket to
corresponding events. A key packs `E` consecutive events, so a key matches only if all `E`
agree, and expected anchor yield falls as `a^E`.

We originally argued this was a **lower** bound. Two mechanisms induce positive correlation
between per-event agreements: consecutive events derive from overlapping k-mers (adjacent
9-mers share eight bases), and every event of a read is normalised by a single shift-and-scale
estimate, so an error in that estimate displaces all of a read's z-values together. Under
positive correlation, agreements cluster, and clustering raises the probability that a run of
`E` is entirely in agreement relative to the independent case.

**That argument is conditional, and the condition fails for most configurations of interest.**
It holds only given an exact correspondence between the two event sequences — that reference
event `d + i` is the counterpart of query event `i` for every `i` in the window. A key window
containing a single inserted or deleted event cannot match however well its neighbours agree.
Writing `g` for the probability that an alignment column is an indel rather than a matched
pair, the governing quantity is

&nbsp;&nbsp;&nbsp;&nbsp;**`P(key) ≈ (1 − g)^E · a^E`**

and the first factor is absent from the simple form. Where segmentation is fixed-width and dwell
constant, `g` is small and `a^E` behaves as the lower bound derived above — measured at 1.8× and
5.1× below the direct count in the two such configurations. Where segmentation is data-dependent
or dwell varies, `g` rises to 0.09–0.21 and `a^E` becomes an **over-estimate**, by between 1.9×
and 14× across the remaining cells (Figure 7.3). Figure 7.1 shows the `a^E` surface over the
`(a, E)` plane with both systems' measured operating points.

The qualitative content survives — yield falls geometrically in `E`, with a base that is a
measurable property of the quantiser — but the use of `a^E` as a one-sided bound is restricted
to the exact-correspondence case.

### 6.2 An estimator-symmetric instrument

An agreement figure computed two different ways for two systems is not a comparison. We
therefore emit per-event quantised symbols from both systems in one format and score them with
one program (`bench/symbol_agreement.cpp`), which does not know which system produced its
input.

Our symbols come from the production reference-encoding and quantisation paths. RawHash2 ships
`--out-quantize`, which dumps query-side symbols; its reference path hardcodes the dump off, so
we added a four-line `--out-quantize-ref` exposing the same facility on the reference side, and
fixed a record-separator defect that makes the stock multi-read output unparseable.

The scorer aligns the two symbol streams semi-globally — query aligned in full, free end gaps on
the reference, match +1, mismatch −1, gap −1 — and reports `a` under the maximising alignment,
`a` under the single best constant offset, the indel fraction, and, directly from the alignment
path, the fraction of positions beginning a gap-free all-matching run of `E` columns. That last
quantity is `P(key)` **measured rather than modelled**, which removes the independence
assumption entirely.

**Validation.** Under the conditions of our earlier per-event diagnostic — fixed-width
segmentation, zero dispersion, normalisation from the first 2000-sample chunk — the independent
instrument returns a constant-offset agreement of 0.556 against the 0.559 previously published:
different code, same input, agreeing to 0.3 percentage points.

**A note on estimator choice.** The constant-offset estimator, which our earlier work used,
understates agreement by a factor of four once retention is data-dependent: it reports 0.188 for
our frozen configuration where the correspondence-aware figure is 0.783. Results obtained with
it for detected-event configurations are withdrawn, including the claim that detection roughly
halves agreement. It does not; at CV 0.44 detection is the better of our two segmentations.

### 6.3 Results

**Table 6.** Agreement, correspondence failure, and measured key-match probability. 200 reads
per cell, identical input, one estimator (Figure 7.2).

| system | segmentation | `a` | `g` | `E` | `P(key)` | anchors/read |
|---|---|---|---|---|---|---|
| this work | detected (frozen), CV 0.00 | 0.797 | 0.170 | 12 | 0.0124 | 33.8 |
| this work | detected (frozen), CV 0.44 | 0.783 | 0.196 | 12 | 0.0055 | 14.9 |
| RawHash2 | its own, CV 0.00 | 0.913 | 0.055 | 8 | 0.350 | 651 |
| RawHash2 | its own, CV 0.44 | 0.911 | 0.092 | 8 | 0.254 | 467 |

**The gap is large and it is before seeding.** At CV 0.44, measured key-match probability
differs by a factor of **46** (0.254 against 0.0055) and anchor supply by a factor of **31**
(467 against 14.9 per read, the two ratios differing because the systems retain different
numbers of events per read). A 31-fold difference in anchor supply accounts for 91.8% against
17.6% without any appeal to the decision rule.

**The key-match difference decomposes, and key length is the smaller lever.** Within our
pipeline, measured `P(key)` at `E` = 8 rather than 12 is 0.0274 against 0.0055, a factor of
**4.9**. The remaining factor of **9.3** — 0.254 against 0.0274 at the same key length — is
attributable to the encoding and segmentation themselves. (4.9 × 9.3 = 45.8, recovering the
46-fold total.) Shortening keys is both the smaller lever and, as §7.2 shows, the one that pays
for itself in lost specificity.

**RawHash2's agreement is dwell-invariant where ours is not.** 0.913 → 0.911 across CV 0 to
0.44, against 0.797 → 0.783 for ours, while our key-match probability falls 2.2× over the same
range because `g` rises from 0.170 to 0.196. Agreement is not what moves; correspondence is.

---

## 7 Three root causes

### 7.1 Asymmetric event retention

RawHash2 applies its event-merging rule — drop any event whose level differs from the last
retained event by less than `--sig-diff` = 0.35 — **symmetrically to the reference encoding and
to the query**, retaining 73.4% on each side. Both sides therefore agree on how many events a
given span contains, by construction.

Our pipeline does not. The reference encoder emits one event per k-mer unconditionally; the
query detector emits approximately 0.78. The consequence is measurable and damning: our indel
rate is **0.170 at zero dwell dispersion**, where there is no timing noise to blame. An indel
rate that high with no noise present is a structural mismatch between two encoders, not a
property of the signal. Because correspondence failure enters `P(key)` with the same exponent as
disagreement, it costs as much.

This is the first thing to fix, and it is a change to the indexing path rather than a research
question.

### 7.2 Quantisation: a ≈2.5-bit sweet spot, and scale-dependent truncation

RawHash2's nominal four bits admit sixteen buckets. Its realised symbol distribution over chr20
occupies six of them — 7.7, 21.1, 9.7, 27.6, 24.0 and 8.8 per cent, plus a 0.9 per cent outlier
bucket — for a realised entropy of approximately **2.46 bits per event**. Its quantiser is
deliberately non-uniform: fine resolution inside a central band, coarse outside, so events near
the distribution's mode are resolved and the tails collapsed.

A third system points the same way, which matters because two points could be coincidence.
Sigmoni, which classifies rather than maps and shares none of our decision machinery, quantises
into an alphabet of **six** symbols by default — 2.58 bits — on the stated grounds that it
balances robustness to noise against specificity in exact matching. Two independent systems
landing within a tenth of a bit of each other, one naming noise robustness as the reason,
suggests the operating point is not accidental.

Our uniform quantiser over a clipped z-range spends its alphabet evenly, realises more of it,
and agrees less. **The design axis is per-event entropy against per-event agreement, and the two
systems sit at opposite ends of it.** A key of 8 events at 2.46 bits carries about 19.7 bits of
nominal specificity; one of 12 events at nearly 4 realised bits carries about 48. The system
with the *less* specific key is five times more accurate, because specificity it does not need
costs it nothing and agreement it does need costs us everything.

**Differencing is not the answer, and the reason is instructive.** Encoding successive level
*differences* rather than absolute levels cancels the shared per-read normalisation shift that
§6.1 identifies as a source of correlated disagreement. Measured in triplicate it is much
worse: 8.3% [3.6–11.8] against 17.6% for the absolute encoding, and worse in every seed. It
retained one symbol per event and changed only what the symbol encoded, so `E` agreements were
still required while each symbol became a function of two noisy levels rather than one,
multiplying the residual variance. It attacked the right quantity by the wrong mechanism. What
RawHash2's merging does instead is reduce `E` *and* raise `a`, which the `(1−g)^E · a^E` form
rewards twice.

**Specificity is also scale-dependent, which complicates short keys further.** Table 7 gives
realised entropy and occurrence-cap survival by geometry (Figure 7.6).

**Table 7.** Realised key entropy and cap survival, 20 Mb reference.

| geometry | nominal | distinct keys | realised entropy | shortfall | cap survival |
|---|---|---|---|---|---|
| 3 × 8 | 24 bits | 92,482 | 16.5 bits | 7.5 | 2.8% |
| 4 × 8 | 32 bits | 1,519,844 | 20.5 bits | 11.5 | 31.2% |
| 3 × 13 | 39 bits | 5,096,781 | 22.3 bits | 16.7 | 63.7% |
| 3 × 14 | 42 bits | 7,985,629 | 22.9 bits | 19.1 | 78.8% |
| 4 × 12 | 48 bits | 14,908,778 | 23.8 bits | 24.2 | 93.5% |

A 39-bit key realises 22.3 bits, a shortfall of 16.7; the frozen 48-bit key realises 23.8, a
shortfall of 24.2. **The shortfall grows faster than the nominal width**: doubling nominal key
space from 24 to 48 bits buys 7.3 bits of realised entropy.

Short keys saturate the occurrence cap, and worse on larger references. A 3 × 8 geometry keeps
only 2.8% of minimizer occurrences on the 20 Mb reference — 97.2% truncated — and 0.7% on the
64 Mb one, 99.3% truncated. Short keys degrade faster than the reference grows, so a geometry
chosen on a small reference does not transfer to a large one. This is why raising anchor yield
by shortening keys fails: realised entropy collapses before the yield becomes useful.

### 7.3 Recoverable loss inside the chainer

The correspondence-aware measurement revises our frozen configuration's anchor supply from a
modelled 1.85 per read — derived from the simple `a^E` form for a *fixed-width* configuration we
do not ship — to a measured **14.9** for the configuration we do. The observed median best-chain
score is **4**.

So roughly 11 of 15 anchor-eligible positions per read are not reaching the best chain. This
loss was invisible while the estimate sat *below* the observation: a decision stage cannot
recover more anchors than exist, and 1.85 against 4 left no room for loss. The candidates are
the occurrence cap discarding anchors before chaining, the 24-slot greedy chainer thrashing, and
a chaining band of 12 against a measured indel rate of 0.196. We have not isolated which.

This qualifies, but does not overturn, the location of the deficiency. Closing a 31-fold supply
gap dominates recovering a factor of under four. But the decision stage is no longer blameless,
and because the anchors are known to exist, this is a diagnostic rather than a search — the
cheapest remaining experiment in the pipeline.

---

## 8 Discussion

**Measure `a` and `g` before proposing a decision rule.** Both are a single pass over paired
reference and query events, with no training and no search, and together they establish whether
*any* decision rule can succeed before one is designed. A reported decision rule unaccompanied
by reported agreement and correspondence figures cannot be assessed by a reader. Had we measured
them at the start rather than near the end, four of the corrections in this work would not have
been needed.

**Optimise key-match probability, not agreement alone.** Correspondence failure enters with the
same exponent as disagreement, and our indel rate is 0.170 even with no timing noise present.
The resource that can buy both is alphabet size. RawHash2 and Sigmoni trade it away; we do not.

**Report distributions against derived budgets.** Under a deadline, a mean that falls inside
the budget is consistent with any miss rate. We report nine runs with full ranges and name the one that
crosses, which is a weaker-sounding but more useful statement than a single comfortable mean.

**Run the external baseline first, not last.** Every intervention we made on the decision stage
was measured against our own earlier numbers, which rose and fell credibly and gave no
indication that the ceiling lay elsewhere. One run of a published tool on identical input would
have established within a day that the gap was a factor of five and not a tuning problem. In a
pipeline whose stages are ordered, an internal improvement series measures progress against the
wrong denominator until an external reference fixes the scale.

---

## 9 Limitations and threats to validity

**Every latency figure was taken under a hypervisor.** WSL2 can deschedule a vCPU mid-decision,
and such an event is indistinguishable in the data from a slow decision. We treat p50 and p90 as
properties of the implementation and p99 and above as upper bounds contaminated by the platform.
The 2.0–59.4 ms spread of observed maxima, uncorrelated with p99, is the direct evidence. A
deadline-miss rate against the 1.56 ms bound requires bare metal with isolated cores, tickless
operation, interrupt affinity moved off the measured cores and a fixed governor; we did not have
it, and no claim here depends on the tail.

**All signal is simulated**, from two generators whose detector calibrations do not transfer to
each other by a factor of thirty. There is therefore no basis for assuming either transfers to a
physical device.

**Agreement is measured at two dwell levels, one seed, 200 reads per cell**, where accuracy is
swept at six levels in triplicate. The claim that RawHash2's agreement is dwell-invariant rests
on two points.

**The alignment-based agreement figures are upper bounds**, computed under the correspondence
that maximises matches. Indel fractions of 0.055–0.208 mean the alignments are near-diagonal, so
the estimator is not manufacturing matches through heavy gapping, but the bound is one-sided and
the comparison is only as fair as the two systems' indel fractions are similar. They are not
identical, and the asymmetry favours RawHash2.

**The symmetric-retention explanation is not isolated.** It is visible in RawHash2's source and
consistent with every measurement, but the decisive ablation — applying our detector to our own
reference encoding and re-measuring — is a change to the indexing path and was not made.

**Single reference, single organism.** All figures derive from human chr20 at 20 Mb or 64 Mb.

**Related-work provenance.** RawHash2, Squigulator, readfish and Icarust were built and run
here; claims about UNCALLED, Sigmap, RawAlign and Sigmoni rest on their publications and
repositories rather than on execution, and are marked as such in the text where load-bearing.

---

## 10 Conclusion

We set out to determine whether the adaptive-sampling decision can be made in microseconds. It
can: a median of 124.8 µs at genome scale against a 1.56 ms physical budget, with bounded
per-channel state, no allocation on the decision path, and no drops at the offered rate — while
the orchestration layer alone of a standard basecalling client costs 124.3 µs per read-chunk
before any decision is made. The decision budget is not the obstacle to signal-space adaptive
sampling.

The question we answer better than the one we asked is why that is insufficient.
Quantised-event matching is bounded not by the rate at which decisions can be made but by the
fidelity with which signal survives segmentation and quantisation, captured by two measurable
quantities whose `E`-fold product governs everything downstream. At the agreement and
correspondence our pipeline achieves, no decision rule placed after quantisation can recover
what segmentation and quantisation discard — which is why six attempts to place one moved
accuracy by at most 35% relative against a gap of a factor of five.

That is a negative result about this system and a positive one about where to look. We state it
without apology: a system that fails for a reason that is measured, modelled, and shown to
account for the failure of six attempts to fix it is of more use to the next designer than one
that works for reasons its author cannot name.

---

## Reproducibility

All code, data and scripts are in the accompanying repository at commit `eb0ea28`.

**Verification.** `scripts/verify_thesis_numbers.py` recomputes every number in this paper from
files under `results/`, or, for figures describing code that no longer exists, checks them
against the commit message that recorded them; it exits non-zero on any disagreement. At the
cited commit it reports 143 claims agreeing and none disagreeing.
`scripts/audit_manuscript.py` checks structural consistency and reports zero problems.

**Data.** `results/r3-latency.csv` (nine R3 runs, with per-run HdrHistogram dumps in
`results/hgrm/`), `results/decision-rules.csv` (six rules × three seeds, raw logs in
`results/decision-rules-raw/`), `results/rawhash-seeds.csv`, `results/agreement-summary.csv`
with per-read detail in `results/agreement-*.csv`, `results/key-entropy.csv`,
`results/comparison-costs.csv`, `results/delta-keys.csv`,
`results/latency-matrix-wsl2.csv`.

**Figures.** `scripts/generate_thesis_figures.py` regenerates every data figure from `results/`
and nothing else; output is byte-stable, so regeneration on an unchanged tree leaves it clean.

**Patches.** `third_party_patches/rawhash2-reference-quantise-dump.patch` exposes RawHash2's
reference-side quantised symbols and fixes a record-separator defect in its existing
`--out-quantize`. `third_party_patches/icarust-dwell-variability.patch` adds lognormal
dwell-time variability to Icarust, reducing to stock behaviour when unset.

**Known gaps.** Five claims are not settled by any committed file and are documented as scope
boundaries: a bare-metal deadline-miss rate, real instrument data, agreement across the full
dwell sweep and across seeds, the Icarust accept rates behind the simulator comparison, and the
index resident set at minimizer window 10.

---

## References

1. Oxford Nanopore Technologies. *MinKNOW API*. https://github.com/nanoporetech/minknow_api
2. A. Payne, N. Holmes, T. Clarke, R. Munro, B. J. Debebe, M. Loose. Readfish enables
   targeted nanopore sequencing of gigabase-sized genomes. *Nature Biotechnology*, 2021.
3. S. Kovaka, Y. Fan, B. Ni, W. Timp, M. C. Schatz. Targeted nanopore sequencing by real-time
   mapping of raw electrical signal with UNCALLED. *Nature Biotechnology*, 2021.
4. H. Zhang, H. Li, C. Jain, H. Cheng, K. F. Au, H. Li, S. Aluru. Real-time mapping of nanopore
   raw signals. *Bioinformatics*, 37(Suppl. 1), 2021. (Sigmap)
5. C. Firtina, N. Mansouri Ghiasi, J. Lindegger, G. Singh, M. B. Cavlak, H. Mao, O. Mutlu.
   RawHash: enabling fast and accurate real-time analysis of raw nanopore signals for large
   genomes. *Bioinformatics*, 39(Suppl. 1), 2023.
6. C. Firtina, M. Soysal, J. Lindegger, O. Mutlu. RawHash2: mapping raw nanopore signals using
   hash-based seeding and adaptive quantization. arXiv:2309.05771.
7. J. Lindegger, C. Firtina, N. Mansouri Ghiasi, M. Sadrosadati, M. Alser, O. Mutlu. RawAlign:
   accurate, fast, and scalable raw nanopore signal mapping via combining seeding and
   alignment. arXiv:2310.05037.
8. V. Shivakumar, O. Ahmed, S. Kovaka, M. Zakeri, B. Langmead. Sigmoni: classification of
   nanopore signal with a compressed pangenome index. *Bioinformatics*, 2024.
9. M. Eris, C. Firtina, O. Mutlu et al. RawBench: a comprehensive benchmarking framework for
   raw nanopore signal analysis techniques. *ACM-BCB*, 2025. arXiv:2510.03629.
10. H. Gamaarachchi et al. Squigulator: simulation of nanopore sequencing signal data.
    *Genome Research*, 2024.
11. R. Munro, A. Payne, M. Loose. Icarust: a real-time simulator for nanopore adaptive
    sampling. https://github.com/LooseLab/Icarust
12. J. Misra, D. Gries. Finding repeated elements. *Science of Computer Programming*, 2(2),
    1982.
13. A. Metwally, D. Agrawal, A. El Abbadi. Efficient computation of frequent and top-k elements
    in data streams. *ICDT*, 2005.
14. H. Li. Minimap2: pairwise alignment for nucleotide sequences. *Bioinformatics*, 34(18),
    2018.
15. G. Tene. HdrHistogram: a high dynamic range histogram.
    https://github.com/HdrHistogram/HdrHistogram_c
16. M. Roberts, W. Hayes, B. R. Hunt, S. M. Mount, J. A. Yorke. Reducing storage requirements
    for biological sequence comparison. *Bioinformatics*, 20(18), 2004. (minimizers)
