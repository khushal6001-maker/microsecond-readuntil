# Chapter 1 — Introduction

> **Draft status.** Structural first draft. Every number is recomputed from a committed
> file in `results/` by `scripts/verify_thesis_numbers.py`; `[UNVERIFIED]` marks the
> exceptions, which Appendix F.3 lists and explains. `scripts/audit_manuscript.py` checks
> that no marker, cross-reference or figure in this chapter dangles. Literature claims in this chapter are deliberately
> scoped to the systems surveyed in Chapter 2; §1.7 states the convention and its reason.

---

## 1.1 Selective sequencing as a soft-real-time loss system

Nanopore sequencing differs from other sequencing technologies in one respect that is
architecturally decisive: the instrument can be instructed, during sequencing, to eject the
molecule currently occupying a pore. A client that identifies uninteresting molecules early
and ejects them frees the pore for another molecule, raising the proportion of the run's
capacity spent on the target. This is *selective sequencing*, or adaptive sampling.

The control action is bounded in time by physics. A molecule translocates through the pore at
a few hundred bases per second, and an ejection request has value only while the molecule
remains in the pore. After it has passed, the request is not merely late: the capacity it was
intended to reclaim has already been consumed, and the outcome is identical to having issued no
request at all. Utility is therefore a decreasing function of decision latency with an effective
cut-off, rather than a continuous function of throughput.

Two standard characterisations follow, and the thesis is organised around them.

First, the client is a **soft-real-time** system: a decision delivered after its deadline has
no utility, though the consequence is lost opportunity rather than failure of the instrument.
The appropriate performance metrics are therefore the distribution of decision latency and the
proportion of decisions delivered within the budget, not the aggregate rate at which decisions
are produced.

Second, the client is a **loss system** rather than a delay system. When arriving work exceeds
the capacity to serve it, queueing that work does not defer the cost — it converts decisions
that would have been useful into decisions that arrive after their deadlines. Discarding
arrivals is therefore the correct policy under saturation, and a client that blocks is not
merely slower but incorrect. Chapter 3 §3.5 develops this argument and Chapter 3 §3.1.3 derives
the admissible service time it implies: with `C` active channels delivering a chunk every `T_c`
and `W` workers, mean per-chunk service must satisfy `S < W·T_c/C`, which for 512 channels, a
0.4 s cadence and two workers is 1.56 ms.

These two framings are conventional in real-time systems and are, among the systems surveyed in
Chapter 2, not the framings under which read-until clients are usually characterised; that
literature reports throughput and mapping accuracy. The present work adopts them because they
are what the physical constraint implies, and because they determine which measurements are
worth making.

---

## 1.2 The basecalling-based architecture and its cost

The established approach converts signal to sequence before deciding. Each arriving chunk is
basecalled by a neural network, the resulting sequence is aligned to a reference with a
conventional aligner, and the decision follows from the alignment. This design is well
supported, is the basis of the reference implementation surveyed in Chapter 2 §2.3, and has the
substantial advantage that its accuracy is the accuracy of mature, independently validated
components.

Its cost is architectural rather than algorithmic. The basecaller is typically a separate,
accelerator-resident process, so the decision path spans a process boundary and a queue.
Measured here, the orchestration of such a client — with basecalling and mapping removed, so
that only its own loop remains — costs 124 µs per chunk, which is already comparable
to a complete signal-space decision. Its polling interval additionally imposes a latency floor
independent of how quickly any computation completes.

This motivates asking whether an adequate decision can be reached without basecalling, in a
single process, with the signal left where the transport deposited it.

---

## 1.3 Signal-space matching and the ordered pipeline

The alternative is to transform the reference into signal space under a pore model and compare
raw signal against it directly. The systems surveyed in Chapter 2 §2.4 differ in their index
structures and chaining strategies but instantiate a common five-stage pipeline:

1. **Reference encoding** — reference sequence to expected signal under a pore model.
2. **Segmentation** — the raw sample stream divided into events.
3. **Quantisation** — event levels discretised into buckets.
4. **Seeding** — keys formed from consecutive quantised events and matched against an index.
5. **Chaining** — matched seeds accumulated into evidence for a locus.

Two properties of this decomposition organise the whole thesis.

**The stages are ordered, and earlier stages bound later ones.** A key is a window of
consecutive quantised events, so if segmentation places event boundaries differently on query
and reference, the key bits themselves differ; and if quantisation assigns different buckets,
the key does not match. No seeding or chaining strategy can recover evidence that stages 2 and
3 destroyed. Chapter 7 makes this quantitative.

**The decomposition is also an implementation decomposition.** Stages 1 and 4 determine memory
footprint and cache behaviour; stage 5 determines what state must be retained per channel and
for how long; and the whole pipeline must execute within the service budget of §1.1. The
systems engineering and the matching method are therefore not separable concerns, which is the
premise of this work.

---

## 1.4 Scope: what this thesis claims and does not claim

Stated before the contributions, because the scope determines how they should be read.

**This thesis does not present a signal-space matching method, and claims no novelty in
quantised-event hashing.** The index construction of Chapter 4 — quantising event levels,
concatenating consecutive quantised values, hashing the concatenation, matching through a table
— is the construction already published by the systems surveyed in Chapter 2 §2.4.3.

**This thesis does not claim a superior end-to-end classifier.** At realistic dwell-time
variability the system resolves 17.6% of on-target reads at a zero-false-positive threshold
[16.6–18.8 across three seeds], where a current event-detecting mapper correctly maps 91.8%
[91.2–92.4] of the same reads. The
two figures are taken at different operating points and are compared on shape rather than level
in Chapter 6 §6.5, but no reading of them favours the present system's accuracy, and none is
offered.

**This thesis does not claim that fixed-width segmentation is invalid in general**, nor that
simulators generally omit dwell-time variability. Chapter 6 §6.5 reports that a published mapper
is substantially insensitive to the axis this work sweeps, and Chapter 2 §2.5.1 records that the
accepted signal simulator models dwell as a documented tunable parameter with a non-zero
default.

**What the thesis delivers** is a characterised real-time data plane — single-copy admission,
zero-copy propagation, lock-free handoff, state bounded in the number of channels, and
per-chunk decision latency under 200 µs at genome scale — together with a
quantitative account of why this class of pipeline fails when its segmentation assumes constant
translocation, and of why the evaluation practice represented by the simulators and benchmarks
surveyed in Chapter 2 does not expose that failure.

The negative result is a contribution and is presented as one. A system that does not work, for
a reason that is measured, modelled, and shown to account for six independent attempts to fix
it, is more useful to a subsequent designer than a system that works for reasons its authors
cannot identify.

---

## 1.5 Contributions

The five contributions map onto the pipeline of §1.3, and are stated at the level the evidence
supports.

**C1 — A real-time data plane for stages 1 and 4 (Chapter 3).** Ingestion incurs one copy,
performed by the protobuf runtime into arena-backed storage, and no further copy of signal
occurs during sharding, decision, or action dispatch. Descriptors are 56-byte borrowed views
asserted at compile time not to straddle a cacheline. Handoff is by single-producer,
single-consumer lock-free rings, one per shard, with producer- and consumer-private cached
indices that remove cross-core loads from the unsaturated steady state. Lifetime is managed by
a reference-counting protocol in which the reader's own reference eliminates a
reset-under-dispatch race by construction rather than by a timing assumption. Total state is
O(channels) with compile-time per-channel constants and no allocation on the decision path.

**C2 — Streaming decision structures for stage 5 (Chapter 4).** The decision stage is
formulated as a frequent-items problem under four simultaneous constraints — bounded space
independent of stream length, a decision available at every chunk boundary, bounded
per-observation cost, and no allocation — which jointly exclude both batch accumulation and
classical Misra–Gries at adequate resolution. Two structures are presented: a set-associative
counter table with Misra–Gries-style replacement, 3.0 KB per channel, whose update cost is
O(ways) rather than O(counters); and an incremental gap-tolerant chainer that bounds drift per
link rather than in total, with cost linear rather than quadratic in chunk count over a read.
**This contribution is an engineering instantiation of known streaming techniques under these
constraints, not a new theoretical guarantee.** Chapter 4 §4.3.3 states the expected-case and
worst-case behaviour of the set-associative variant explicitly, and Chapter 2 §2.7 records that
the literature on hash-partitioned frequent-items sketches has not been surveyed; if a published
analysis covers this structure, the contribution reduces to its application here.

**C3 — Latency characterisation and a measurement methodology (Chapter 5).** Per-chunk decision
latency is reported as a *distribution* against a *derived service bound*, with percentiles
from a bounded-error instrument and environment provenance captured per run. The systems
surveyed in Chapter 2 do report latency, as mean bases to decision or mean processing time per
read; what they do not report is a tail, a deadline-miss rate, or the fraction of decisions
arriving too late to act on, and under a deadline a mean inside the budget is consistent with
any miss rate (§2.8). Three measurement errors are documented, each of which had already
produced a result that was subsequently withdrawn: a histogram whose power-of-two resolution
manufactured apparent reproducibility, an exhausted host filesystem that displaced the measured
median by approximately one third, and a thread-placement rule assuming a portable relationship
between logical processor identifiers and physical cores. The methodological finding is that at
this timescale the instrument and the environment require the same scrutiny as the system under
test. The substantive findings are that data-plane thread count dominates placement, and that
processor pinning does not reduce the median but reduces the run-to-run spread of the 99th
percentile from 133% to 8%.

**C4 — Time-domain noise as an evaluation axis (Chapter 6).** Dwell-time dispersion is swept
over a coefficient of variation from 0 to 0.60 on an external simulator, with a
composition-matched control and three replicates. This system's fixed-width configuration falls
from 49.7% to 1.3% true-positive rate, crossing below its own detected-event configuration
between CV 0 and 0.10, so an evaluation conducted at zero dispersion would select the
configuration that is tenfold worse at the simulator's own default. A published mapper, by
contrast, declines by three percentage points across the same range — which bounds the claim and
is stated in that chapter before any interpretation. The novel finding is narrower: the accuracy
cost of minimizer sketching in that published mapper is dwell-dependent, rising from
approximately 13 to 21 percentage points across the range with the false-mapping rate rising in
parallel, a dependence that the evaluation practice represented by the benchmark
surveyed in Chapter 2 §2.6 does not measure. Separately, the simulator used to drive live
read-until clients over the instrument's own API emits a fixed sample count per k-mer and
therefore cannot exercise the axis: against it, the configuration that is worse on realistic
signal measures 4.5 times better.

**C5 — An information-loss account of stages 2 and 3, and a comparative measurement
(Chapter 7).** Two quantities govern how many true anchors a read can supply: `a`, the
probability that reference and query assign the same bucket to corresponding events, and `g`,
the rate at which the two event sequences fail to correspond at all. A key of `E` consecutive
events requires `E` events that both agree and correspond, so key match probability goes as
`(1 − g)^E · a^E` and anchor yield falls geometrically in `E`. The contribution is in three
parts. First, an instrument that measures both quantities from paired reference and query
symbol streams, applied identically to this pipeline and to a published mapper, because an
agreement figure computed two different ways for two systems is not a comparison. Second, the
resulting measurement: this pipeline's frozen configuration achieves `a` = 0.783 at `g` = 0.196
and `E` = 12, giving 14.9 anchor-eligible positions per read, against the published mapper's
`a` = 0.911 at `g` = 0.092 and `E` = 8, giving 467. A 31-fold supply difference
accounts for 17.6% accuracy against 91.8% on the same input without appeal to the decision
rule, and decomposes into a factor of 4.9 from key length and 9.3 from the encoding. Third,
the design axis this exposes: the published mapper realises about 2.46 bits per event of a
nominal 4 and applies one event-merging rule to both reference and query, trading alphabet and
resolution for agreement and correspondence, where this pipeline maximises alphabet and
achieves neither. The prescription is that `a` and `g` should be measured and reported before
a decision rule is proposed, since below a threshold no decision rule can succeed, and both
are cheap to measure.

---

## 1.6 Methodological stance

Three practices are applied throughout and are stated here because they affect how the results
should be read.

**Results that did not survive scrutiny are retained in the text.** Twelve results or positions
taken in this work were withdrawn after further measurement, including the thesis's original
framing. Chapter 8 §8.2 tabulates them and Appendix D is the retraction log. The practice is
adopted because the mechanism by which each failed is transferable — a coarse instrument
manufacturing reproducibility, an uncontrolled host variable, a bounded structure degrading
into an arrival-order cache, a coordinate system that renamed the item being counted — and
because a thesis that reports only surviving results provides no evidence about the reliability
of its own method.

**Claims are scoped to the evidence available.** Literature claims are scoped to the systems
surveyed in Chapter 2 rather than asserted universally, for the reason given in §1.7. Claims
about this system are scoped to the measurement regime that produced them, and Chapter 5
maintains an explicit regime convention because the median differs by an order of magnitude
between the small-reference and genome-scale configurations for entirely explicable reasons.

**Comparisons state their boundaries with the number.** The two external comparisons in this
work measure differently scoped quantities — one against a client with its basecaller and mapper
removed, one against a mapper operating at a different false-positive rate on whole reads rather
than chunk prefixes. Both are reported with those boundaries attached, and the latency ratio
against a mapper that resolves reads where this system largely does not is identified as a bound
on data-plane cost rather than a performance result.

---

## 1.7 A note on the scoping of literature claims

Four of the systems discussed in Chapter 2 were not executed in the course of this work and are
characterised from secondary sources; Chapter 2 §2.10 tabulates which. Claims about what the
literature does or does not do are therefore scoped in this thesis to *the canonical systems
surveyed in Chapter 2* and to *standard evaluation practice as represented by the benchmark and
simulators discussed there*, rather than asserted of the field in general.

This is not a hedge of convenience. Three of the thesis's claims about the literature would
require revision if a surveyed system turned out to behave differently, and Chapter 2 §2.10
identifies them in priority order. Scoping the claims to the surveyed set means that such a
discovery would narrow a claim rather than falsify it, and the contributions resting on this
work's own measurements — C1, C2, C3 and C5 — are unaffected either way.

---

## 1.8 Organisation of the thesis

**Chapter 2** surveys the pore physics relevant to dwell-time variability, the Read Until
interface, basecalling-based and signal-space clients, the simulation and benchmarking tooling,
and the streaming-algorithms and real-time systems literature the thesis draws on. It assigns a
provenance marker to every claim and tabulates the verification status of each system discussed.

**Chapter 3** presents the data plane: ingestion and the copy boundary, the arena lifetime
protocol, lock-free handoff, backpressure as a loss system, the per-channel state budget, and
processor affinity. It derives the service-time bound that the remainder of the thesis measures
against.

**Chapter 4** formulates the decision stage as a frequent-items problem and presents the two
structures, with the guarantees of the set-associative variant stated explicitly and two
historical defects retained as failure-mode case studies.

**Chapter 5** reports latency and, at greater length, the methodology required to obtain it:
instrument resolution, host-state provenance, processor topology, the factorial design that
un-confounds placement from thread count, and the separation of intrinsic from
platform-contributed variance.

**Chapter 6** introduces dwell-time dispersion as an evaluation axis, reports the sweep for this
system and for a published mapper, reports the dwell-dependence of the sketching trade-off, and
documents the read-until simulator's inability to exercise the axis.

**Chapter 7** develops the `a^E` model, measures per-event agreement, verifies the model against
observed anchor yield, and shows that it accounts for the six failed interventions.

**Chapter 8** states what was built and what it costs against the budget, tabulates the twelve
withdrawn results with the mechanism of each, separates the solved data plane from the
unsolved matching stage, and orders the further work by expected information per unit effort.
Appendix D expands the retraction log.

---

## 1.9 Summary of principal results

| | result | chapter |
|---|---|---|
| Per-chunk decision latency, genome scale | p50 124.8 µs, p99 370 µs [347–1969], utilisation 0.08 | 5 |
| Per-chunk decision latency, small reference | p50 16.3 µs, p90 24.7 µs | 5 |
| Per-channel decision state | 3.0 KB (voting) / 14.0 KB (chaining) | 3, 4 |
| Effect of pinning | median unchanged; p99 spread 133% → 8% | 5 |
| Fixed-width segmentation under dwell | 49.7% → 1.3% TPR, CV 0 → 0.60 | 6 |
| Detected-event segmentation under dwell | 18.4% → 7.5% TPR over the same range | 6 |
| Published mapper under dwell | 95.0% → 92.0% correctly mapped | 6 |
| Sketching cost, dwell-dependent | 12.8 → 21.0 percentage points | 6 |
| Read-until simulator at constant dwell | reports the worse configuration as 4.5× better | 6 |
| Per-event agreement, this pipeline frozen | `a` = 0.783, `g` = 0.196, 14.9 anchors/read | 7 |
| Per-event agreement, published mapper | `a` = 0.911, `g` = 0.092, 467 anchors/read | 7 |
| Anchor-supply gap accounting for 17.6% vs 91.8% | 31×, as 4.9× key length and 9.3× encoding | 7 |
| Realised quantiser entropy, published mapper | ≈2.46 bits of a nominal 4 | 7 |
| Normalisation from whole read vs first chunk | agreement 0.556 → 0.726, yield 2.0× | 7 |
| Interventions on stages 4–5 | six, none exceeding +35% relative | 4, 7 |

All figures.
