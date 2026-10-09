# Chapter 4 — Streaming Decision Structures

> **Draft status.** Structural first draft. `[VERIFY]` marks figures to be re-derived from a
> recorded run; `[TABLE 4.x]` / `[FIG 4.x]` mark artefacts still to be produced.

---

## 4.1 Problem statement

Chapter 3 establishes that sharding by channel makes all per-channel state the property of
exactly one worker thread. The structures in this chapter are therefore single-threaded and
their correctness arguments involve no concurrency. What they must satisfy instead is a
combination of constraints that is unusual, and it is worth stating formally before any
structure is proposed.

For each channel, the matching stage of Chapter 6 emits, per arriving chunk, a set of
candidate positions in the reference. Each candidate is converted to a *diagonal*, the
difference between its reference position and the query offset that produced it. A read
originating at a single locus produces repeated observations of one diagonal; chance matches
produce diagonals distributed over the reference. The decision therefore reduces to
estimating the most frequent diagonal and its multiplicity, which is the classical **frequent
items** (or heavy hitters) problem.

Four constraints apply simultaneously.

**C1 — Bounded space, independent of stream length.** A read may contribute an unbounded
number of chunks, and a run an unbounded number of reads. Space must be O(1) per channel with
a compile-time constant, not O(observations).

**C2 — Incremental decision.** The decision must be available after every chunk, because the
deadline of Chapter 3 §3.1.1 may fall at any chunk boundary. An algorithm that requires the
complete stream before producing an answer is inadmissible regardless of its accuracy.

**C3 — Bounded per-observation cost.** The service-time budget derived in Chapter 3 §3.1.3
is a mean over all work in a chunk, and the candidate count per chunk is itself variable
(measured medians range from under one to approximately ninety-six depending on reference
scale `[VERIFY]`). Per-observation cost must therefore be bounded by a small constant, not
amortised over a long stream.

**C4 — No allocation.** The structure is on the decision path, which Chapter 3 establishes
performs no dynamic allocation after initialisation.

C1 and C2 together exclude the obvious approach of accumulating observations and sorting
them, which is what a batch mapper does. C3 excludes algorithms whose update cost scales with
the number of counters.

---

## 4.2 Classical frequent-items algorithms, and why they do not transfer verbatim

Two classical algorithms address C1 and C2.

**Misra–Gries.** Maintain `k` counters, each holding an item and a count. On an observation:
if the item is present, increment it; if a counter is free, install it with count 1;
otherwise decrement *all* `k` counters and discard any that reach zero. The algorithm
guarantees that the reported count `f̂` of any item satisfies

&nbsp;&nbsp;&nbsp;&nbsp;f − m/(k + 1) ≤ f̂ ≤ f

for a stream of length `m`, so every item with true frequency exceeding `m/(k+1)` is
retained. The guarantee is strong and the space is O(k). Its defect for the present purpose
is the decrement step, which touches all `k` counters and therefore costs O(k) per
observation — a direct conflict with C3 at the counter counts needed for adequate resolution.

**Space-Saving.** Maintain `k` counters; on a miss, replace the minimum-count entry and set
its count to that minimum plus one. Update is O(1) given a structure that exposes the
minimum, but maintaining that structure (classically a heap) reintroduces per-observation
cost and pointer chasing, and the algorithm's guarantee is one-sided in the opposite
direction: counts are overestimates.

The structure adopted here is neither algorithm exactly, and §4.3.3 is explicit that no
published guarantee applies to it verbatim. It is a set-associative hybrid chosen to satisfy
C3, and the cost of that choice is a weaker worst-case bound, stated quantitatively below.

---

## 4.3 The set-associative vote table

### 4.3.1 Structure

The table comprises `S` = 64 sets of `K` = 4 ways, giving 256 counters. Each entry holds a
64-bit diagonal and a 32-bit count, so an entry is 12 bytes and the table is 3.0 KB per
channel — 1.57 MB across 512 channels and 8.22 MB across 2675 `[VERIFY]` `[TABLE 4.1]`.

A diagonal is assigned to a set by a multiplicative hash followed by a shift, rather than by
its low bits. This matters: adjacent diagonals are the common case, because signal jitter
places true seeds on neighbouring diagonals, and low-bit indexing would concentrate them into
a single set and force them to compete with one another for `K` = 4 ways.

### 4.3.2 Update rule

On an observation of diagonal `d` with weight `w` (§4.6 explains why weights are not unit):

1. Scan the `K` ways of `d`'s set. If `d` is present, add `w` to its count and return.
2. Otherwise let `min` be the smallest count in the set. If `min ≤ w`, replace that entry
   with `(d, w)`. If `min > w`, subtract `w` from it and return without installing `d`.

Step 2 is the Misra–Gries decrement restricted to the set, with the decrement applied to the
minimum rather than to all members. The restriction is what bounds update cost at O(K) = O(4)
independent of the total counter count, where classical Misra–Gries would be O(256).

### 4.3.3 What is and is not guaranteed

Hashing partitions the observation stream into `S` sub-streams. In expectation each set
receives mass `m/S`, and within a set the Misra–Gries argument applies with `K` counters,
giving a retention threshold of

&nbsp;&nbsp;&nbsp;&nbsp;(m/S)/(K + 1) = m/(S(K + 1)) = m/320

against `m/(n+1) = m/257` for classical Misra–Gries with the same 256 counters. The
expected-case threshold is therefore approximately 20% lower, i.e. marginally better, while
the update cost falls by a factor of 64.

This is an expected-case statement and must not be presented as a bound. Hash collisions
place no limit on the mass any individual set may receive; in the adversarial case all heavy
diagonals share a set and compete for four ways, and the effective threshold degrades toward
`m/(K+1)` = `m/5`. The structure therefore trades a worst-case guarantee for a bounded update
cost. That trade is appropriate here because the adversary is signal noise rather than an
opponent, and because C3 is a hard constraint while the worst case is improbable — but the
justification is empirical, resting on the measured behaviour of §4.7, not on a proof.

### 4.3.4 Querying the maximum

The reported decision is the largest count in the table, obtained by scanning all 256
counters once per chunk — 48 cachelines, a cost of order 0.1 µs at the measured clock
`[VERIFY]`.

A running maximum updated on each insertion was implemented first and is incorrect. Because
step 2 of the update rule may *decrement* an entry, a running maximum records the historical
peak of an entry whose count has since been reduced. The error is one-sided in the dangerous
direction: it can only overstate the winner, and an overstated count produces an accept that
the evidence does not support. The scan is therefore retained deliberately, and the
substitution of a cheaper incorrect query for a correct one is recorded here as a design
decision rather than omitted.

---

## 4.4 Case study: degradation of a frequency estimator into an arrival-order cache

The first implementation of step 2 was

```
if (count[weakest] <= 1) { install (d, 1) at weakest; }
```

with no else branch. This admits a new diagonal only into a slot holding at most one vote,
and performs no decrement.

**Formal diagnosis.** Without a decrement step the rule is not a frequent-items algorithm.
Once every way of a set holds a count of two or more, the condition is permanently false and
the set is immutable for the remainder of the read. The structure's contents are then
determined by which diagonals arrived *first*, not by which occur most often — it is a cache
with no eviction policy, and its output is a function of arrival order rather than of
frequency, which is precisely the property it existed to provide.

**Dependence on scale.** The defect is invisible at low candidate density and total at high
density, which is why it survived initial testing. With approximately one candidate per chunk
— the regime of a 37 kb reference — counts accumulate slowly and slots remain weak, so the
condition stays satisfiable. With approximately ninety-six candidates per chunk — a 64 Mb
reference — every way of every set passes two votes within the first chunk, and any diagonal
not among the first few observed can never be installed. The measured consequence was 0
accepts from 763 decisions `[VERIFY]`, where the same binary on the smaller reference
accepted 854 of 1366.

**Lesson.** A bounded structure's correctness argument must be stated in terms of the arrival
density it will actually see, not the density it was tested at. The condition `count ≤ 1` is
a statement about density disguised as a statement about a counter, and the test that would
have caught it is one that drives candidate density to the production value — which is the
form the stress test now takes.

---

## 4.5 Coordinate stability: the chunk-relative diagonal

A frequent-items algorithm estimates the most common *item*. It is therefore a precondition
that repeated observations of the same underlying phenomenon produce the same item identity.
This precondition was violated.

Seed offsets are reported relative to the query passed to the matching stage, and the query
is one chunk. Offsets therefore restart at zero at every chunk boundary. For a read
originating at reference position `r₀`, with `Q` events per chunk, the diagonal observed for a
true seed in chunk `c` is

&nbsp;&nbsp;&nbsp;&nbsp;d_observed = r₀ + cQ − offset_within_chunk = d_true + cQ

so the identity under which the correct locus is counted advances by `Q` at every chunk. The
structure was being asked to find a frequent item in a stream in which the item of interest
was renamed on each arrival. Votes for the true locus could accumulate only *within* a chunk,
never across chunks, and the per-chunk evidence is a small fraction of a read's total.

**The diagnostic is the transferable part.** The symptom was not a low score, which would
have been attributed to weak signal and investigated in the matching stage. It was an
*inverted* relationship: the fraction of reads whose best diagonal was the correct one
*decreased* with read length, from 49/300 at two chunks to 7/300 at twenty `[VERIFY]`.
Additional evidence producing a worse answer cannot be explained by insufficient evidence; it
is the signature of accumulating a quantity other than the intended one. Monotonicity in
evidence is therefore a cheap and sensitive invariant to assert, and it is now checked
explicitly in the offline harness.

The correction carries a running count of events consumed by the read and expresses diagonals
read-relative. With it, correct-diagonal counts rise with read length as they must:
118 → 160 → 170 → 196 of 300 `[VERIFY]`.

---

## 4.6 Weighted arrival: per-chunk pre-aggregation

Observations enter the table with weights rather than unit increments, and the weight is
obtained by run-length counting a chunk's diagonals before any insertion: the chunk's
candidate diagonals are sorted, and each distinct diagonal is inserted once with its
multiplicity.

The justification is informational. Within a single chunk a true diagonal is observed
repeatedly, because several seeds from the same locus agree, whereas chance matches are
spread across distinct diagonals. Multiplicity within a chunk is therefore signal, and
inserting candidates individually with unit weight discards it — the structure sees `n`
separate arrivals of a diagonal rather than one arrival of weight `n`, and in the latter form
the item's mass is concentrated into a single event that it need survive only one competition
to retain.

In frequent-items terms this is the weighted variant of the algorithm, and the effect of
weighting on the retention threshold is to reduce the number of decrement opportunities a
heavy item must survive. The same total mass arrives in fewer stream events, so the
probability that a heavy item is evicted between its observations falls.

The sort is over a chunk's candidates only, in caller-owned scratch reserved once, and is
therefore O(c log c) in the per-chunk candidate count with no allocation. The offline
reference implementation performs the same sort-and-count over a whole read, and the two
paths now share one function, for reasons Chapter 6 §6.x gives — a divergence between them
previously allowed an offline accuracy figure of 89.3% to coexist with a live accept rate of
zero.

---

## 4.7 From equivalence classes to chains: tolerating cumulative drift

### 4.7.1 Why a coarser bucket is not sufficient

Exact diagonal equality is an equivalence relation, so counting is well defined: candidates
partition into classes and the table counts class membership. Admitting tolerance by widening
the bucket — assigning diagonals to `⌊d/B⌋` — preserves the equivalence relation and is
therefore a trivial modification, but it tolerates only *bounded absolute* drift. Total drift
across a read must remain within `B` for all of a read's seeds to share a class.

Under event detection the drift is not bounded. A missed or spurious boundary displaces the
correspondence between query and reference events for the remainder of the read, so
displacements accumulate; the total is a sum over boundary errors and grows with read length.
No fixed `B` accommodates it, and a `B` large enough to try would admit chance matches at a
rate that destroys specificity.

### 4.7.2 The chaining relation

Chaining replaces the equivalence relation with a *local* compatibility condition. An anchor
`(q, r)` extends a chain whose last anchor was `(q_prev, r_prev)` when

&nbsp;&nbsp;&nbsp;&nbsp;0 ≤ Δq ≤ G,&nbsp;&nbsp; 0 ≤ Δr ≤ G,&nbsp;&nbsp; |Δq − Δr| ≤ β

for a maximum gap `G` and band `β`. The condition bounds drift *per link*, not in total, so a
chain of `L` links tolerates cumulative drift up to `Lβ`. This is the property banding cannot
provide, and it is the reason a different algorithm is required rather than a different bucket
width.

Exact diagonal voting is the special case `β = 0` with `G` unbounded: `Δq = Δr` for every
link forces every anchor onto one diagonal. The relation is not transitive for `β > 0`, so
equivalence classes do not exist and the structure must build paths explicitly.

### 4.7.3 Two realisations and their measured difference

A **greedy** realisation retains only each chain's final anchor, in 24 chains per channel with
set-associative replacement as in §4.3. Update is O(chains). Its limitation is that an anchor
attached to the wrong chain cannot be reconsidered.

An **anchor-history** realisation retains a ring of the most recent 1024 anchors per channel
with the best chain score ending at each, and scores a new anchor against up to `L` = 48
recent predecessors, taking the maximum. This is minimap2-style best-predecessor scoring
restricted to a bounded look-back. Footprint is 14 bytes per anchor, 14.0 KB per channel,
7.34 MB across 512 channels and 38.35 MB across 2675 `[VERIFY]`. Update is O(L) per anchor,
paid once and never re-paid when later chunks arrive.

Measured at the operating point of Chapter 6 `[TABLE 4.2]` `[VERIFY]`:

| decision rule | TPR at zero false positives |
|---|---|
| exact-diagonal votes, 256 counters | 14.2% |
| chains, β = 0, 24 chains *(control)* | 9.6% |
| chains, β = 12, 24 chains | 17.4% |
| anchor-history chains, β = 12, L = 48 | 18.8% |
| anchor-history chains, L = 128 | 18.0% |
| anchor-history chains, with occurrence filtering | 17.8% |

The `β = 0` control is the informative row. It isolates the tolerance from the restructuring:
with tolerance removed, chaining is *worse* than the vote table (9.6% against 14.2%), which is
the expected result given that it tracks 24 candidates where the table tracks 256. The
improvement from 14.2% to 18.8% is therefore attributable to the tolerance and not to the
change of data structure — a distinction that a comparison without the control could not
support.

Increasing look-back from 48 to 128 does not help, and occurrence filtering does not help.
Chapter 7 explains why the whole family saturates near 19%: the structures are reorganising an
expected 1.85 true anchors per read, and no reorganisation of two anchors yields a confident
decision.

---

## 4.8 Cost model

Per chunk, with `c` candidates and `A` anchors:

| stage | cost | re-paid on later chunks |
|---|---|---|
| pre-aggregation sort | O(c log c) | no |
| vote table update | O(K) = O(4) per distinct diagonal | no |
| vote table query | O(S·K) = O(256) once per chunk | no |
| chain update | O(L) = O(48) per anchor | no |
| chain query | O(1), maintained incrementally | no |

The column that matters is the last. A read of `C` chunks accumulating `A` anchors per chunk
incurs total decision cost linear in `C`, because each arrival is processed once against
bounded state and no earlier arrival is revisited. A batch chainer, which sorts and
dynamic-programs over all anchors accumulated so far, incurs cost proportional to the
accumulated count on *every* chunk, giving total work quadratic in `C` over a read.

This asymmetry is the architectural argument for incremental structures in a read-until
setting, and it is the mechanism proposed in Chapter 5 §5.8.2 for part of the measured
latency difference against a batch mapper. The cost model above is derived from this
implementation and is sound; **the corresponding claim about the comparison system's
behaviour in read-until mode was not instrumented in this work and remains a hypothesis**, as
Chapter 5 states. Establishing it requires measuring that system's per-chunk cost as a
function of accumulated chunk count.

---

## 4.9 Summary

The decision stage is a frequent-items problem under four simultaneous constraints —
bounded space independent of stream length, a decision available at every chunk boundary,
bounded per-observation cost, and no allocation — which jointly exclude both batch
accumulation and classical Misra–Gries with adequate resolution. The structure adopted is a
64 × 4 set-associative counter table, 3.0 KB per channel, whose expected-case retention
threshold of `m/320` is marginally better than classical Misra–Gries at the same counter
count while its update cost is lower by a factor of 64; the price is a worst-case bound that
hashing does not preserve, and that price is paid knowingly.

Two defects in earlier versions are retained in the text because each illustrates a failure
mode of bounded streaming structures rather than a coding error. An insertion rule lacking a
decrement step degraded the estimator into an arrival-order cache, invisibly at low candidate
density and totally at high density. And chunk-relative seed offsets renamed the item of
interest on every arrival, violating the precondition that repeated observations of one
phenomenon share an identity; its signature was an inverted relationship between evidence and
accuracy, which is a more sensitive diagnostic than a low score.

Exact diagonal equality tolerates no drift and bucket widening tolerates only bounded drift,
whereas detected event boundaries produce drift that accumulates with read length. Chaining
bounds drift per link instead of in total and is therefore the appropriate generalisation; a
`β = 0` control establishes that the measured improvement follows from the tolerance rather
than from the data structure. All structures here are linear in chunk count over a read,
which is the property a batch chainer cannot offer in this setting.
