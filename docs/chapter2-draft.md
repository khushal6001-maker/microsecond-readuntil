# Chapter 2 — Background and Related Work

> **Draft status.** Structural first draft.
>
> **PROVENANCE WARNING — READ BEFORE EDITING.** This chapter is the only one in the thesis
> whose content is not derived from measurements taken in this work. Claims below carry one of
> three markers, and **every `[2°]` claim must be checked against the primary source before
> submission.**
>
> - `[1°]` — verified firsthand in this work: source code read, tool executed, or CLI and
>   defaults inspected directly.
> - `[2°]` — taken from secondary summaries (abstracts, search results, documentation pages)
>   and **not** verified against the paper. Treat as a hypothesis about the literature.
> - `[CHECK]` — a specific venue, year, or numeric claim requiring confirmation.
>
> §2.10 tabulates the status of every system discussed. That table is a drafting aid and
> should be deleted before submission, once its contents have been discharged.

---

## 2.1 Nanopore sequencing and the origin of dwell-time variability

A nanopore sequencer measures ionic current through a protein pore embedded in a membrane. A
motor protein ratchets a single DNA strand through the pore, and the instantaneous current
depends on the group of bases occupying the constriction — approximately nine bases for the
R10 chemistry used throughout this work `[1°: the pore model tables used here are 4^9 entries,
confirming a 9-mer context]`. The resulting current trace, sampled at a fixed rate, is the raw
signal.

Two properties of this process determine everything in Chapters 6 and 7.

**The mapping from sequence to current is many-to-one and noisy.** Distinct k-mers can produce
indistinguishable mean levels, and each k-mer's level is itself a distribution rather than a
value: the model tables used here carry a per-k-mer standard deviation alongside the mean, and
for the k-mer `AAAAAAAAA` the shipped values are a mean of 54.316 pA with a standard deviation
of 1.195 pA `[1°: read from the simulator's model table]`. Any quantisation of the level into
discrete buckets therefore has a non-zero per-event error rate, which Chapter 7 measures and
shows to be the binding constraint on the whole pipeline.

**The translocation rate is not constant.** The motor protein's stepping is stochastic, so the
number of samples for which a given k-mer occupies the constriction — its *dwell* — is a random
variable, positive and right-skewed. A segmentation rule that divides the sample stream into
fixed-width windows is exactly correct only if dwell is constant. The accepted signal simulator
models dwell as a distribution with a tunable standard deviation and documents a default of 4.0
samples against a mean of 9.0 `[1°: read from the tool's own `--help`]`, i.e. a coefficient of
variation of approximately 0.44, which establishes that the variability is both substantial and
treated as a first-class parameter by the community's tooling.

Basecalling is the inverse problem — recovering sequence from signal — and is performed by
neural networks whose accuracy is high and whose latency is substantial. The architectural
question that motivates signal-space methods is whether a decision can be made without
solving it.

`[CHECK]` Standard references for pore physics, the R10 chemistry, and motor-protein kinetics
are required here. None is cited from firsthand reading in this draft.

---

## 2.2 The Read Until API and the constraints it imposes

The instrument's control software exposes a bidirectional streaming interface over gRPC
through which a client receives accumulated raw signal per channel and may request that the
molecule in a given channel be ejected `[1°: the client in this work implements
`get_live_reads` against this interface and the transport is TLS-secured gRPC]`.

Four properties of the interface shape any client.

**Chunked delivery.** Signal arrives in accumulating chunks at a cadence on the order of
0.4 s, so a client sees a prefix of a read and must decide on partial evidence or wait for
more `[1°: the chunk cadence is the figure this work's chunking uses throughout; `[CHECK]` the
authoritative value and its configurability in the API documentation]`.

**Channel parallelism fixed by hardware.** A MinION flow cell presents 512 channels and a
PromethION 2675 `[CHECK]`, so the client's concurrency is imposed rather than chosen
(Chapter 3 §3.1.2).

**Ejection is the only control action, and it is irreversible.** A molecule ejected in error is
lost, so the cost of a false accept and a false reject are not symmetric, and the operating
point is an experimental design choice rather than a tuning parameter. This is why Chapters 6
and 7 report true-positive rates at a constrained false-positive rate rather than a single
accuracy figure.

**Classification metadata is version-coupled.** The API reports a read classification whose
permitted values changed between major versions; a client that hardcodes the set fails against
a server that does not provide it `[1°: observed directly — a current client refused to start
against the read-until simulator because the simulator's classification map predates the
client's expected set, which required patching the simulator]`.

---

## 2.3 Basecalling-based adaptive sampling

The established approach basecalls each chunk, aligns the resulting sequence against a
reference with a conventional sequence aligner, and decides from the alignment.

**readfish** is the reference implementation of this design `[1°: version 2024.3.0 was built
and executed against a simulator in this work]`. Its operation is a polling loop: it requests
accumulated chunks at a configurable interval, defaulting to 0.4 s `[1°: the `--throttle`
default]`, basecalls them through an external server, maps with a `minimap2` binding, and
applies a per-region policy expressed in a configuration file with separate actions for
single-locus and multi-locus hits on and off target `[1°: the configuration schema was written
and validated in this work]`. It supports a no-operation caller, which bypasses basecalling and
mapping and is intended for measuring orchestration cost `[1°]`.

Two architectural consequences follow and are measured in Chapter 5. The basecaller is a
separate process, usually GPU-resident, so the decision path includes an inter-process
round trip and a queue. And the polling interval sets a floor on decision latency independent
of how quickly the computation completes.

`[CHECK]` readfish's own published latency figures, and the venue and year of its description,
must be cited from the paper rather than from this work's measurements of it.

---

## 2.4 Signal-space mapping

The alternative is to compare raw signal against a reference transformed into signal space,
avoiding basecalling entirely. The published systems form a lineage with a common pipeline
shape, and identifying that shape precisely is what positions the present work.

**2.4.1 UNCALLED** `[2°]` builds an FM-index over the reference, segments raw signal into
events, converts each event into the set of k-mers consistent with it under the pore model, and
tracks the surviving candidate positions as evidence accumulates. `[CHECK]` venue and year;
`[CHECK]` whether the characterisation of its index and candidate-tracking scheme is accurate.

**2.4.2 Sigmap** `[2°]` indexes the reference in signal space using a k-d tree over
multi-dimensional event vectors, with a seed-selection strategy and a chaining step adapted to
signal characteristics, and reports improved accuracy and speed relative to UNCALLED.
`[CHECK]` venue, year, and the comparison's reported magnitude.

**2.4.3 RawHash** `[2°, with CLI-level corroboration]` quantises event levels so that
near-identical events share a value, concatenates the quantised values of several consecutive
events, and hashes the concatenation to form a seed, which is then matched through a hash table
and chained. `[1°: the tool's own interface exposes exactly these parameters — bits of
quantisation, events concatenated per hash, and a signal-difference threshold — which
corroborates this description of the mechanism.]` `[CHECK]` venue, year, and reported accuracy
and throughput relative to UNCALLED and Sigmap.

**This is the same construction as the index described in Chapter 4 of this thesis, and that
must be stated plainly in the introduction and here.** The present work claims no novelty in
quantised-event hashing.

**2.4.4 RawHash2** `[2°, with CLI-level corroboration]` extends it with minimizer sketching,
more sensitive quantisation, weighted mapping decisions, frequency filters for ambiguous
seeds, and support for current chemistry and file formats. `[1°: the implementation's defaults
are 4 bits of quantisation, 8 events per hash, and minimizer sketching **disabled**, with its
own help text stating that enabling sketching "may reduce accuracy but improves the performance
and memory space efficiency".]` That the sketching trade-off is declared but unquantified
against time-domain noise is the gap Chapter 6 §6.6 occupies.

**2.4.5 Further developments.** RawAlign `[2°]` combines seeding with dynamic time warping;
Sigmoni `[2°]` uses an r-index. `[CHECK]` both, including whether either is better positioned
as the nearest prior art than RawHash2.

**2.4.6 The common pipeline shape.** All of the above instantiate the same five stages:
reference encoding into signal space under a pore model; segmentation of the query into events;
quantisation or discretisation of event levels; seeding by exact or approximate match; and
chaining or candidate tracking to accumulate evidence.

This decomposition is not original to this thesis and should not be presented as though it
were. RawBench organises raw signal analysis into three modular stages — reference encoding,
segmentation, and matching — and the five stages here are that decomposition with matching
split into quantisation, seeding and chaining, which is the granularity Chapter 7's argument
requires `[1°, from the paper's full text]`. Two observations about the shape matter for the
present work.

First, **every published system segments data-dependently rather than by fixed time windows**
`[1° for the enumeration; 2° for individual systems]`. This is now verified at the level that
matters: RawBench evaluates exactly three segmentation methods — t-test changepoint detection,
basecaller move tables, and a learned CNN segmenter — and all three are data-dependent; none
is fixed-width. UNCALLED's documentation describes segmenting current into events before k-mer
conversion, and Sigmoni segments before quantising into its alphabet.

The consequence for Chapter 6 is favourable and should be stated there rather than left
implicit. Its fixed-width arm is not a straw man of any published system: no surveyed system
uses fixed-width segmentation, so the dwell-sensitivity result concerns a configuration choice
the literature has already made correctly, which is exactly the scoping §6.5 applies. The
original form of this claim — that every system uses *detected events* specifically — was too
narrow, since move tables and learned segmenters are neither fixed-width nor t-test event
detection; the claim that carries the argument is the broader one about data dependence.

Second, the stages are ordered, and the earlier stages bound the later ones. Chapter 7
formalises this: quantisation fidelity caps seeding yield exponentially in the number of events
per key, so no chaining strategy can recover evidence that quantisation destroyed. That the
literature reports chaining improvements without reporting quantisation fidelity is the
observation Chapter 7 §7.7 builds on.

---

## 2.5 Simulation

**2.5.1 Signal simulators.** Squigulator `[1°: built and used extensively in this work]`
generates raw signal from a reference sequence under a pore model, exposing translocation
speed, amplitude noise, read-length distribution, and — centrally for this thesis — the
standard deviation of samples per base as independent parameters. `[CHECK]` venue and year;
Genome Research 2024 with a 2023 preprint is indicated by secondary sources `[2°]`.
DeepSimulator `[2°]` is an earlier alternative reported to be substantially slower.

The relevant property is that **time-domain noise is a first-class, documented, tunable
parameter of the accepted tool, with a non-zero default.** Chapter 6's contribution is not to
introduce the axis but to sweep it, which no published evaluation does.

**2.5.2 Read-until simulators.** Evaluating a client end to end requires a server that speaks
the instrument's control API, which signal simulators do not. Icarust `[1°: built, patched and
executed in this work; source read directly]` provides this: it serves the gRPC interface,
manages simulated channels, and generates signal from a reference.

Its signal generation emits a fixed number of samples per k-mer, with a source comment noting
that the quantity could be derived from a dwell-time distribution and not doing so `[1°: read
from `src/simulation.rs`]`. Its output therefore has zero time-domain noise. Because it is the
only means of exercising a client over the real API, the one configuration in which end-to-end
client behaviour can be tested is also the one in which the failure mode of Chapter 6 is
invisible — the observation §6.8 develops, and the motivation for the dwell patch supplied as
an artefact of this work.

`[CHECK]` Icarust's venue and year, and whether a newer version has since added a dwell model.
The version used here was obtained in October 2026 `[1°]`.

---

## 2.6 Benchmarking frameworks

RawBench `[2°, with repository-level corroboration]` provides a modular framework for
evaluating raw-signal analysis pipelines, decomposing them into reference encoding under
selectable pore models, signal segmentation, and representation matching, and evaluating
combinations across several organisms and both recent chemistries using **real** sequencing
datasets. `[CHECK]` venue, year, authorship, and the dataset list.

Two properties are load-bearing for this thesis and were confirmed from the paper's own
description of its scope and from its repository `[2°, but corroborated from two independent
sources]`:

- It does **not** vary dwell time, translocation rate, or time-domain noise. Its own framing
  describes it as benchmarking existing methods "rather than exploring parameter sensitivity
  across signal acquisition characteristics."
- It does **not** evaluate minimizer sketching or seed subsampling.

These two absences are precisely where Chapters 6 and 7 of this thesis sit, and the thesis's
claim to a methodological contribution depends on them. **They are therefore the second most
important `[2°]` claims to verify**, and the verification should be a direct read of the
paper's experimental section rather than its abstract.

---

## 2.7 Streaming algorithms for frequent items

Chapter 4's decision structure is an instance of a classical problem, and the thesis's
engineering claims rest on the known properties of its solutions.

**Misra–Gries** `[2°, classical result]` maintains `k` counters and, on an unmatched
observation with all counters occupied, decrements every counter. For a stream of length `m` it
guarantees that any item of frequency exceeding `m/(k+1)` is retained, with estimated counts
undercounting by at most `m/(k+1)`. **Space-Saving** `[2°, classical result]` instead replaces
the minimum-count entry, yielding one-sided overestimates and O(1) update given a structure
exposing the minimum.

`[CHECK]` Both require citation to their original papers and a textbook treatment for the
guarantees as stated. The guarantee forms used in Chapter 4 §4.2 should be quoted from a
primary source, not paraphrased.

Chapter 4 §4.3 adopts a set-associative variant that is neither algorithm exactly, restricting
the decrement to one set in order to bound update cost at O(ways) rather than O(counters), and
§4.3.3 states the resulting expected-case and worst-case behaviour. The literature on
set-associative or hash-partitioned frequent-items sketches `[CHECK — this is a gap in the
present draft; such variants very likely exist and must be located, because if a published
analysis covers this structure then Chapter 4 should cite it rather than derive it]`.

---

## 2.8 Real-time systems framing

The thesis's organising claim — that adaptive sampling is a soft-real-time problem — uses
standard terminology that should be grounded rather than asserted.

A **soft real-time** task has a deadline after which the utility of its completion decreases
but does not become negative or catastrophic `[2°, standard definition; CHECK against a
real-time systems text]`. Ejection after translocation has no utility, so the task is soft
real time with a utility function that falls to zero rather than firm or hard.

A **loss system**, in queueing terms, discards arrivals it cannot serve rather than queueing
them indefinitely, in contrast to a delay system `[2°; CHECK against a queueing theory text]`.
Chapter 3 §3.5 argues that a read-until client is correctly modelled as the former, and the
argument is the standard one: where utility decays with latency, admitting work by increasing
delay reduces aggregate utility.

**Deadline-miss rate** as a reported metric, in place of throughput, is conventional in
real-time systems and unconventional in this application domain.

An earlier draft claimed that no read-until or raw-signal paper reports a deadline-related
metric. That was an argument from absence and **it is wrong**; it is corrected here rather
than quietly dropped, and the corrected form is what the rest of the thesis uses `[1°]`.

The surveyed systems do report latency, in two forms. The first is **signal consumed before a
decision**: RawAlign reports mean sequencing latency as the number of bases a read requires to
reach a mapping decision, and a 2025 adaptive-sampling benchmark reports UNCALLED requiring a
3 s chunk, roughly 1.3 kb, where other tools classify from 1 s. The second is **mean
processing time per read**: RawHash times each tool's per-read mapping specifically to
estimate how quickly a Read Until decision can be activated.

What is absent is narrower. All of these are measures of central tendency — how much signal,
or how much time, a decision consumes on average. None of the surveyed work reports a
deadline-miss rate against a stated service bound, a latency distribution with tail
percentiles, or the fraction of decisions that arrive too late to act on. The distinction
matters because the two are not interchangeable under a deadline: a mean comfortably inside
the budget is consistent with any miss rate whatever, and the loss-system framing of §3.5
makes the miss rate, not the mean, the quantity that determines whether the system works.

The gap this thesis addresses is therefore the reporting of a *distribution* against a
*derived budget*, with the instrument and the environment characterised well enough for the
tail to carry meaning — not the reporting of latency as such.

---

## 2.9 The gap addressed by this thesis

Stated as narrowly as the evidence supports.

**Not a gap:** quantised-event hashing as a matching method (RawHash); data-dependent
segmentation as a strategy (all published systems, §2.10); modular benchmarking of raw-signal
pipelines (RawBench); simulation of time-domain noise (Squigulator); the decomposition of a
raw-signal pipeline into reference encoding, segmentation and matching (RawBench uses the same
three stages, so §2.4.6 extends a published decomposition rather than proposing one); reporting
latency for a read-until client (RawAlign, RawHash and the 2025 adaptive-sampling benchmark all
do, §2.8).

**The gaps:**

1. **No published evaluation sweeps time-domain noise.** Existing evaluations, including the
   dedicated benchmarking framework, report accuracy and throughput at one fixed and unmeasured
   level of dwell variability, for the signal property that determines whether
   segmentation-based matching functions at all (Chapter 6). Verified against RawBench's full
   text: its metrics are precision, recall, F1, wall-clock time, CPU time and peak memory, and
   dwell appears only as a dataset selection criterion `[1°]`.

2. **The accuracy cost of minimizer sketching is undocumented as a function of that noise.**
   Its existence is declared in a current tool's documentation; its dependence on dwell
   dispersion is not, and is measured here (Chapter 6 §6.6).

3. **Per-event quantisation fidelity is not reported, although it bounds seeding yield
   exponentially.** Chapter 7 gives the model, measures the quantity, and shows it accounts for
   six independent intervention failures.

4. **The only simulator capable of driving a client over the instrument's API has no dwell
   model**, so end-to-end client evaluation is conducted in the single regime where the above
   failure mode cannot appear (Chapter 6 §6.8).

5. **Decision latency is reported as a central tendency, not as a distribution against a
   deadline.** The surveyed systems report mean bases to decision or mean processing time per
   read (§2.8). None reports a deadline-miss rate against a stated service bound, a tail
   percentile, or the fraction of decisions arriving too late to act on — which is the quantity
   the loss-system framing makes decisive, since a mean inside the budget is consistent with
   any miss rate. Nor are the per-channel state bounds a client must satisfy to scale to a full
   flow cell characterised anywhere surveyed (Chapters 3 and 5).

Gaps 1, 2 and 4 are claims about the literature. Gaps 1 and 2 were verified against RawBench's
full text and Gap 4 against the simulator's own source, so all three now rest on primary
reading `[1°]`. Gap 3 is supported by measurements in this work and stands independently.
Gap 5 is partly a claim about the literature and was **narrowed** after checking: its original
form, that no read-until paper reports a deadline-related metric, was false, and §2.8 records
the correction.

---

## 2.10 Verification status of discussed systems

**Drafting aid — delete before submission once discharged.**

| system | used in this work | claims about it verified how | residual risk |
|---|---|---|---|
| Icarust | built, patched, run | source read directly | low; check for a newer version with dwell |
| Squigulator | built, run extensively | CLI and model table read | low; venue/year unverified |
| readfish 2024.3.0 | built, run | CLI, config schema, behaviour | medium; published figures uncited |
| RawHash2 | built, run, benchmarked, **source read** | CLI defaults, help text, and `rsketch.c`/`rindex.c` read firsthand; patched (Appendix B) | low; paper still unread, mechanism now verified in source |
| RawHash | not run | secondary only | **high** |
| UNCALLED | not run | secondary; event segmentation corroborated from its own documentation | medium |
| Sigmap | not run | secondary only; reported as not integrable for real-time classification | **high** |
| RawAlign | not run | secondary only | **high** |
| Sigmoni | not run | secondary; alphabet size and reference-encoding step corroborated from two sources | medium |
| RawBench | not run | **full text read** (§2.6); evaluation section checked directly | low |
| Misra–Gries, Space-Saving | implemented variant | classical results, paraphrased | medium; quote guarantees from primary |

### Status of the three claims that could change a thesis claim

**1. That all published signal-space mappers segment by detected events (§2.4.6) — upheld,
and restated more precisely.** No surveyed system uses fixed-width segmentation. RawBench
evaluates exactly three segmentation methods — t-test changepoint detection, basecaller move
tables, and a learned CNN segmenter — and all three are data-dependent. UNCALLED's own
documentation describes segmenting current into events, one per k-mer, before k-mer
conversion; Sigmoni segments before quantising into its alphabet. The claim is therefore
rewritten as the stronger and more checkable one that **published systems segment
data-dependently and none uses fixed-width windows**, which is what Chapter 6's scoping
actually needs. The consequence for Chapter 6 is favourable: its fixed-width arm is not a
straw man of any published system, and §6.4 should say so explicitly rather than leaving the
reader to wonder.

**2. That RawBench neither sweeps time-domain noise nor evaluates sketching (§2.6) — upheld,
verified against the full text.** Its reported metrics are true and false positives, negatives,
not-aligned, precision, recall and F1 computed with UNCALLED's `pafstats`, plus elapsed
wall-clock time, CPU time and peak memory, with coverage depth and breadth for the
basecalling study. Dwell time and translocation speed appear only as a dataset selection
criterion — preferring 400 bps over the deprecated 260 bps mode — and not as an experimental
variable; the sole swept axis of that kind is basecaller context length, at 1, 2, 5 and 10
chunks. No window or seed-subsampling parameter is swept in the body text. Gaps 1 and 2 stand.

A second finding from the same reading **supports** the thesis rather than threatening it:
RawBench decomposes raw signal analysis into reference encoding, segmentation, and matching.
That is an independently published decomposition matching the one §2.4.6 proposes, so the
pipeline framing should be cited to it rather than asserted.

**3. That no read-until paper reports a deadline-related metric (§2.8) — NOT upheld as
originally stated; weakened and sharpened.** Published work does report latency, in two
forms: *bases or chunks to decision* (RawAlign reports mean sequencing latency as bases per
read, and a 2025 adaptive-sampling benchmark reports UNCALLED needing a 3 s chunk against 1 s
for other tools), and *mean processing time per read* (RawHash times each tool's per-read
mapping to estimate how quickly a Read Until decision can be activated). The original claim
was an argument from absence and it is wrong.

What is absent is narrower and is what Gap 5 should now claim: these are **central-tendency**
measures of how much signal or time a decision consumes, not measures of whether decisions
meet a deadline. No surveyed work reports a deadline-miss rate against a stated service bound,
a latency distribution with tail percentiles, or the fraction of decisions arriving too late
to act on. Chapter 5's contribution is therefore not that latency is reported here and nowhere
else; it is the reporting of a *distribution* against a *derived budget*, with the instrument
and the environment characterised well enough for the tail to mean anything. That is a
defensible claim and §2.8, §2.9 and §1.5 (C3) are to be reworded to it.
