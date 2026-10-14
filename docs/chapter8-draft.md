# Chapter 8 — Conclusions

> Numbers in this chapter are carried forward from Chapters 3–7 and are marked `[VERIFY]` at
> first use only; each is to be re-derived from a recorded run before submission, and the
> chapter of origin is cited so that a single corrected measurement propagates to one place.

---

## 8.1 What was built, and what it costs

This thesis set out to determine whether the decision stage of nanopore adaptive sampling can
be made to meet its deadline in software, and what the limiting factor is once it does. The
answer to the first question is affirmative and quantified; the answer to the second is the
substance of Chapter 7 and is not the answer the work expected.

**The artefact.** A single-process C++ daemon that terminates the instrument's bidirectional
streaming API, admits signal chunks with one copy performed by the protobuf runtime into
arena-backed storage, propagates 56-byte borrowed descriptors through per-shard
single-producer single-consumer lock-free rings, maintains per-channel decision state of
3.0 KB under voting or 14.0 KB under chaining `[VERIFY]`, and dispatches eject actions on the
same connection. Per-channel state is reached by exactly one thread, by construction, because
sharding is by channel identifier; the decision path performs no allocation and takes no lock.
Lifetime is managed by a reference count in which the reader holds the initial reference, so
that arena reset cannot race a dispatch in flight — a correctness property established by the
protocol rather than by a bound on dispatch duration.

**The cost, stated against the budget.** The deadline is physical: a chunk describes signal
that has already translocated, and the service bound derived in §1.1 and §3.1 is
`S < W·T_c/C`, which at a two-chunk tolerance, 512 channels and a 0.4 s chunk cadence is
1.56 ms per decision `[VERIFY]`. Measured per-chunk decision latency against a 64 Mb reference
in the frozen configuration is a median of 134–174 µs with a 99th percentile of 630–705 µs at
a resident set of 3.9–4.4 GB, sustaining 975 chunk/s with no drops `[VERIFY]`. The median
therefore sits roughly an order of magnitude inside the budget and the contaminated tail sits
inside it by a factor of two. Against a 37 kb reference the median is 16.3 µs with a 90th
percentile of 24.7 µs `[VERIFY]`, which bounds the fixed cost of the data plane itself at
roughly a tenth of a microsecond per channel-decision and locates the remainder in index
probing.

The honest reading of these figures is narrow, and Chapter 5 §5.9 states it: the median and
the 90th percentile are properties of the implementation, reproducible across runs and seeds;
the 99th percentile and the maximum are upper bounds contaminated by a hypervisor that can
deschedule a vCPU mid-decision, and the thesis does not claim them as measurements of the
software. What can be claimed without reference to the tail is a comparison of configurations
under identical contamination: thread count dominates placement, and processor pinning does
not move the median but reduces the run-to-run spread of the 99th percentile from 133% to 8%
`[VERIFY]` — a result about the variance of the instrument's own environment, which is why it
survives the platform caveat.

**The cost that is not latency.** Two costs are paid for the above and are reported rather
than omitted. Workers spin before yielding, so the system consumes 182–202% of one core
against readfish's 15.6–25.3% for its orchestration alone `[VERIFY]`, an eight- to twelvefold
ratio in processor time that buys the low median. And the index for a 64 Mb reference occupies
most of a 4 GB resident set, which does not scale to a 3.1 Gb human reference in the present
in-memory form; §8.4 returns to this.

---

## 8.2 What was disproved, including this work's own results

Twelve results or positions taken during this work were subsequently withdrawn, and they are
tabulated rather than quietly dropped. The justification is given in §1.6 and bears repeating
in its conclusive form: a thesis that reports only the results that survived offers no
evidence about the reliability of the method that produced them. The mechanism by which each
of these failed is transferable to a subsequent worker; the surviving results, taken alone,
are not.

`[TABLE 8.1]` **Retraction log.** Appendix D gives the full entry for each row, with the
measurement that produced the withdrawn claim and the measurement that replaced it.

| | withdrawn claim or position | class | cause | replacement | § |
|---|---|---|---|---|---|
| 1 | A thread-placement change produced a stable 99th percentile, reproducible to 0.2% | instrument | Power-of-two bucketing; the four reported values were all boundaries of the representable set at the host's TSC frequency | HdrHistogram at three significant figures, 0.1% bounded relative error, backend labelled in every line of output | 5.2 |
| 2 | Unpinned median latency of 27.0–27.3 µs | environment | Host system drive exhausted; the guest's expanding disk image could not extend, stalling incidental writes. About one third of the reported median was host state | Re-measured at 17.9–18.8 µs for the identical configuration; harness now records virtualisation, topology, clocksource, governor, isolation, storage and load per run | 5.3 |
| 3 | Reader and writer threads were placed on distinct physical cores | platform | Logical processor adjacency was assumed to imply distinct cores; on the measurement host CPUs 0 and 1 are SMT siblings | Sibling sets read from `thread_siblings_list`; placement derived from topology rather than from identifier arithmetic | 5.4 |
| 4 | First key-geometry freeze, selected analytically | model | An analytical model of key-space occupancy whose fit did not converge | Replaced by empirical sweep | 6.9 |
| 5 | Second key-geometry freeze, 4 bits × 13 events | algorithm | Selected from live measurements taken while diagonals were computed chunk-relative; with the defect corrected the ordering of candidate geometries reversed | 4 bits × 12 events, sketching disabled, selected on the external simulator with dwell swept | 4.5, 6.9 |
| 6 | The vote table was a frequent-items estimator | algorithm | The admission rule `count ≤ 1` with no decrement is permanently false once every way of a set holds two votes; the structure became an arrival-order cache. 0 accepts of 763 decisions at 64 Mb scale, where the same binary accepted 854 of 1366 at 37 kb | Misra–Gries-style replacement with decrement; stress test now drives candidate density to the production value | 4.4 |
| 7 | Diagonal votes identified a stable locus across chunks | algorithm | Seed offsets restart at each chunk boundary, so the true locus was renamed by one chunk length per chunk. The tell was an *inverted* metric: correct-diagonal fraction fell from 49/300 at two chunks to 7/300 at twenty | Read-relative diagonals via a running consumed-event count; correct-diagonal counts now rise with read length, 118 → 196 of 300 | 4.5 |
| 8 | The work's headline framing as a competitive signal-space mapper | framing | A baseline run of a published mapper on identical input resolved 92.4% of reads against this system's 18.8% | Reframed to the three contributions the measurements support: the data plane, the measurement methodology, and the ceiling model | 5.8, 7 |
| 9 | A two-scale event detector would improve segmentation under dwell | algorithm | Implemented as a standard improvement and measured worse: 3 against 38 correct diagonals at CV 0.50, and it regressed the single-scale path | Reverted; single-scale two-window t-test retained with the frozen calibration | 6.7 |
| 10 | Difference-based key encoding would remove the shared normalisation error | algorithm | Measured worse, 18.8% falling to 2.8–8.4% true-positive rate; one symbol per event was retained and only its meaning changed, so `E` agreements were still required | Rejected; §7.8 distinguishes it from *merging*, which reduces `E` and remains untested | 7.6 |
| 11 | A fixed-width true-positive rate of 8.3% at the swept dwell level | replication | Quoted from a single 60 s run; three replicates placed it at 3.2–3.6% | All dwell results reported as three-seed means with spreads | 6.4 |
| 12 | The published mapper's minimizer sketching would collapse under dwell dispersion | prediction | Predicted from the mechanism and refuted by measurement: 95.0% to 92.0% across CV 0 to 0.60 | The prediction's failure became §6.5, which bounds every claim in Chapter 6, and redirected the novel finding to the *cost* of sketching rather than its collapse | 6.5 |

Four observations about this table are more useful than the individual rows.

**First, three of the twelve are failures of measurement rather than of design** (rows 1–3),
and each had already produced a result that read as publishable. Two of the three were
diagnosable without any additional experiment: comparing a reported percentile against the
instrument's representable set is arithmetic, and reading the sibling list is a file read.
The error common to both is reporting a measurement without reporting the conditions under
which the apparatus is valid — in row 1 the apparatus was the histogram, in row 2 the machine.
Chapter 5 §5.3.4 states the resulting ordering of checks: establish the instrument's
resolution relative to the expected between-run variance, record the environment, and only
then interpret the numbers.

**Second, the two most consequential algorithmic defects (rows 6 and 7) were both scale- or
length-dependent, and both passed their initial tests.** The lockout is invisible at one
candidate per chunk and total at ninety-six; the chunk-relative diagonal is mild over two
chunks and severe over twenty. The transferable lesson is that a bounded structure's
correctness argument must be stated in terms of the arrival density it will see rather than
the density at which it was tested, and that monotonicity in evidence — more chunks must not
produce a worse answer — is a cheap and unusually sensitive invariant, because additional
evidence degrading an estimate cannot be explained by insufficient evidence.

**Third, rows 9, 10 and 12 are predictions that measurement refuted**, and in two of the three
the mechanism proposed was sound in isolation and wrong in context. Row 12 is the instructive
case: the refutation of the prediction became the section that bounds the whole of Chapter 6,
and the finding that survived — that the accuracy cost of sketching grows with time-domain
noise, from 12.8 to 21.0 percentage points across CV 0 to 0.60 `[VERIFY]` — is narrower than
the prediction and better supported.

**Fourth, row 8 is the one that changed the thesis.** The reframing was not a presentational
decision; it followed from a single baseline measurement that should have been taken earlier
than it was. The methodological point is stated without mitigation at the end of §8.3.

---

## 8.3 The honest position

The data plane is solved for the configuration measured, and the matching stage is not. These
are separate conclusions resting on separate evidence, and the thesis is organised so that
neither is contingent on the other.

**The data plane.** A median decision latency an order of magnitude inside a 1.56 ms budget,
with bounded per-channel state, no allocation on the decision path, and no drops at 975
chunk/s, is sufficient — and the architectural implication is the one Chapter 5 §5.8.1 draws:
the orchestration layer of a basecalling client, measured with its caller removed, costs
124 µs per chunk `[VERIFY]`, which is already comparable to a complete signal-space decision
in this system. The budget is not the obstacle to signal-space adaptive sampling. That is a
result in its own right and it does not depend on the accuracy figures.

**The matching stage.** This system resolves 18.8% of reads where a published mapper resolves
92.4% on the same input `[VERIFY]`. Chapter 7 does not excuse this; it accounts for it. Let
`a` be the probability that reference and query assign the same bucket to corresponding
events. A key of `E` consecutive events matches only if all `E` agree, so expected anchor
yield falls as `a^E`, and this is a lower bound rather than an estimate, because overlapping
k-mers and a shared per-read normalisation estimate make the per-event agreements positively
correlated. Measured on this pipeline, `a` reaches 55.9% in its most favourable
configuration, giving a predicted 1.85 true anchors per read against an observed median
best-chain score of 4 `[VERIFY]` — agreement in order of magnitude and in the direction the
correlation argument requires.

Three consequences follow, and together they are the thesis's principal intellectual claim.

The first is that the deficiency is located in stages 2 and 3 of the five-stage pipeline —
segmentation and quantisation — and not in stages 4 and 5, where most of the engineering
effort went. Six independent interventions on the decision stage, spanning chaining,
occurrence filtering, key geometry and key encoding, are accounted for by the model: those
that reorganise a fixed anchor supply yield tens of percent relative and no more, and the one
that multiplied anchor yield lost more to specificity than it gained. None exceeded a 32%
relative improvement `[VERIFY]`. Because the stages are ordered and earlier stages bound later
ones, this is a statement about the method class at this agreement level, not about this
implementation's decision code.

The second is that the apparent inconsistency in Chapter 6 — a key geometry frozen three
times, with adjacent geometries reordering under changes elsewhere in the pipeline — is a
symptom rather than a measurement defect. Sensitivity requires short keys and specificity
requires long ones; §7.3.3 shows the width of the feasible region between them is set by `a`,
and at `a` = 0.56 it is narrow enough that the ordering of adjacent geometries is governed by
second-order effects. The instability was predicted by the model after the fact, which is the
weaker form of confirmation, and is recorded as such.

The third is a design prescription that costs almost nothing to adopt: measure and report `a`
before proposing a decision rule. It is a single pass over paired reference and query events
with no training and no search, it establishes whether any decision rule can succeed before
one is designed, and a reported decision rule unaccompanied by a reported agreement figure
cannot be assessed by a reader. Had this measurement been made at the start of this work
rather than near its end, four of the twelve rows of Table 8.1 would not exist.

**The methodological failure behind row 8, stated plainly.** The external baseline was run
late. Every intervention on the decision stage was conducted against this system's own earlier
numbers, which rose and fell credibly and gave no indication that the ceiling lay elsewhere.
A single run of a published tool on identical input would have established within a day that
the gap was some seventy percentage points and not a tuning problem. The generalisable form is
that in a pipeline whose stages are ordered, an internal improvement series measures progress
against the wrong denominator until an external reference fixes the scale; the baseline is
therefore the first experiment and not the last.

---

## 8.4 Further work

The items below are ordered by the ratio of expected information to effort, which is the
ordering this work's own history argues for.

**Raise `a`, and measure it as the dependent variable (Chapter 7 §7.8).** Three mechanisms are
identified. *Merging* consecutive events whose signal difference falls below a threshold into
one hashed symbol raises agreement by construction — two events straddling a bucket boundary,
each disagreeing with probability near one half, become one symbol determined by their
aggregate — and it reduces `E` for the same span, which the `a^E` form rewards twice. It is
distinct from the differencing rejected in row 10 of Table 8.1, and it is the single most
promising untested change. *Normalisation* is the second: at zero added noise,
reference-derived normalisation recovers 100% of seeds where per-read normalisation recovers
50.5% `[VERIFY]`, and while per-read estimation is unavoidable in principle, the estimator
need not be the median and MAD of one 2000-sample chunk. *Non-uniform bucket boundaries*
placed at quantiles of the realised level distribution would equalise occupancy; an adaptive
quantiser was implemented and rejected earlier in this work on recall grounds, but that
evaluation predates the model and did not measure `a`, and should be repeated with `a` as the
dependent variable.

**Measure `a` for the comparison mapper on the same input.** This is named as an outstanding
item in both Chapter 6 §6.10 and Chapter 7 §7.9 and is repeated here because it is the single
most valuable undone experiment in the thesis. It converts §6.5 from an observation that a
published mapper is robust into an explanation of why, and it tests the model's generality
beyond the one pipeline on which it was fitted. If that system's agreement is substantially
higher, the model explains the gap; if it is comparable, the model is incomplete and the
explanation lies further down the pipeline, which would be the more interesting outcome.

**Deadline-miss rate on bare metal.** Every latency figure in this thesis was taken under a
hypervisor, and Chapters 3 §3.8 and 5 §5.9 scope the tail accordingly. The configuration
required is specified rather than approximated: isolated cores, full tickless operation,
interrupt affinity moved off the measured cores, a fixed performance governor, and the
harness's existing publishability gate left enabled so that the resulting figures carry their
own provenance. The quantity to report is not a percentile but the deadline-miss rate against
the 1.56 ms service bound, which is the metric the loss-system framing of §1.1 actually
implies and which Chapter 2 §2.8 records as absent from the read-until papers surveyed. This
is the experiment that would make Gap 5 a demonstrated contribution rather than a stated one.

**Scale the index to a mammalian reference.** The in-memory index consumes most of a 4 GB
resident set at 64 Mb, so a 3.1 Gb reference is roughly fifty times out of reach in the
present form. The data plane's cost is independent of reference size and the probe is not, so
this is an index-representation problem — succinct or sampled occurrence lists, or an
out-of-core structure — and it is orthogonal to the accuracy ceiling. It should not be
attempted before `a` is raised, since an index fifty times larger built on a 0.56-agreement
encoding answers no question that the present one does not.

**Real instrument data.** All signal in this work is simulated, from two generators with
different noise models, and Chapter 6 §6.7 records that a segmentation calibration does not
transfer between generators, by a factor of thirty. There is accordingly no basis for assuming
the frozen calibration transfers to a physical device, and `a` is the natural first
measurement to repeat on real reads, since it is defined without reference to any decision
rule and requires only paired reference and query events.

---

## 8.5 Closing

The question this work asked was whether the adaptive-sampling decision can be made in
microseconds. It can: the median is 134–174 µs at genome scale against a 1.56 ms physical
budget, with bounded state and no drops, and the orchestration layer of the standard
basecalling client already costs a comparable amount before any decision is made. The question
the work answers better than the one it asked is why that is insufficient. Quantised-event
matching is bounded not by the rate at which decisions can be made but by the fidelity with
which signal survives segmentation and quantisation, and that fidelity is captured by one
measurable probability whose `E`-fold product governs everything downstream of it. At the
agreement this pipeline achieves, no decision rule placed after stage 3 can recover what
stages 2 and 3 discard — which is why six attempts to place one did not.

That is a negative result about this system and a positive one about where to look. It is
stated here without apology, for the reason §1.4 gives: a system that fails for a reason that
is measured, modelled, and shown to account for the failure of six attempts to fix it is of
more use to the next designer than one that works for reasons its author cannot name.
