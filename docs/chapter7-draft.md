# Chapter 7 — The Accuracy Ceiling of Quantised-Event Matching

> **Draft status.** Structural first draft. `[VERIFY]` marks figures to be re-derived from a
> recorded run; `[TABLE 7.x]` / `[FIG 7.x]` mark artefacts still to be produced.

---

## 7.1 Locating the deficiency

Chapters 3 and 5 establish that the data plane is not the limiting component of the system.
Per-chunk decision latency corresponds to a utilisation of approximately 0.1 against the
budget the instrument imposes, no chunks are dropped at the sustained arrival rate, and the
concurrency primitives are verified under both thread and address sanitisers. Chapter 6
establishes that the system nevertheless resolves 18.8% of on-target reads where a published
signal-space mapper resolves 92.4% on identical simulated input `[VERIFY]`.

A deficiency of that magnitude admits two broad explanations. Either the decision stage —
how evidence accumulated from matched seeds is converted into an accept or reject — is
inadequate, or the evidence itself is absent. Six interventions on the decision stage were
implemented and measured in the course of this work, spanning gap-tolerant chaining,
best-predecessor chaining with bounded look-back, occurrence-frequency filtering, key
geometry, and difference-based key encoding. None produced more than a 32% relative
improvement `[TABLE 7.3]`. The consistency of that outcome across structurally unrelated
interventions is itself evidence: it suggests a quantity upstream of all of them is binding.

This chapter identifies that quantity, gives a model for how it propagates, measures it,
verifies the model against observed behaviour, and shows that it accounts for each of the six
negative results. The contribution is not the algebra, which is elementary, but the
identification of the quantity as the one a designer must measure first, and the
demonstration that it predicts the behaviour of a real pipeline.

---

## 7.2 A model for key match probability

### 7.2.1 Definitions

The matching pipeline of Chapter 4 reduces both reference and query to sequences of
quantised events. Let

- `B` — bits per quantised event, so each event takes one of `2^B` bucket values;
- `E` — events concatenated into one key, so a key occupies `B·E` bits;
- `N` — events available from a read at the point of decision;
- `w` — minimizer window, with `w = 1` selecting every key as a seed;
- `a` — the **per-event bucket agreement**: the probability that, for an event pair
  corresponding to the same locus on reference and query, the two sides assign the same
  bucket.

`a` is a property of the composition of the quantiser, the normalisation estimator and the
segmentation rule. It is not a property of the matching algorithm, the index, or the decision
structure, none of which can influence it.

### 7.2.2 Key match probability

A key is a concatenation of `E` event buckets, and the hash of that concatenation equals the
reference hash only if every constituent bucket agrees. Writing `A_i` for the event that
event `i` of a key agrees,

&nbsp;&nbsp;&nbsp;&nbsp;P(key matches) = P(A_1 ∩ A_2 ∩ … ∩ A_E)

If the `A_i` were mutually independent this is `a^E`. The expected number of true-locus
seeds a read contributes is then, for `w = 1`,

&nbsp;&nbsp;&nbsp;&nbsp;**M ≈ (N − E + 1) · a^E**

The relationship is exponential in `E` with base `a < 1`. This is the central structural fact
of the chapter: key length is not a tuning parameter that trades accuracy smoothly, it is an
exponent on the loss term.

### 7.2.3 The independence assumption, and the direction of its error

Independence does not hold here, and the direction in which it fails must be established
before the model is used.

Two mechanisms induce positive correlation between the `A_i`. First, consecutive events
derive from overlapping k-mers — adjacent 9-mers share eight bases — so their levels, and
therefore their quantisation errors, are not independent. Second, every event of a read is
normalised by a single shift-and-scale estimate derived from that read, so an error in that
estimate displaces all of the read's z-values together, producing runs of agreement and runs
of disagreement rather than independent trials.

Under positive correlation, agreement events cluster, and clustering increases the
probability that a run of `E` consecutive events is entirely in agreement relative to the
independent case. Hence

&nbsp;&nbsp;&nbsp;&nbsp;P(key matches) ≥ a^E

so `a^E` is a **lower bound** on key match probability, and `M` is a lower bound on expected
anchor yield. This is the useful direction: a bound that says a configuration *cannot* work
is stronger evidence than an estimate of how poorly it works, and §7.5 shows the observed
yield sitting above the bound as the argument requires.

### 7.2.4 Sensitivity

Differentiating `M` with respect to `E` gives `∂M/∂E ∝ a^E · ln a`, so the proportional loss
per added event is `ln(1/a)`, independent of `E`. At `a = 0.559` each additional event in a
key removes 44% of the expected anchor yield `[VERIFY]`. Design discussions of key length
conducted in units of bits, as is conventional, obscure this: adding one event to a 4-bit
geometry adds four bits of nominal key width and removes nearly half the matches.

`[FIG 7.1]` Expected anchor yield `M` over the `(a, E)` plane, with iso-yield contours and
the measured operating point marked.

---

## 7.3 Specificity: the opposing constraint

Reducing `E` raises match probability and must therefore be bounded by something, or the
design would be trivially resolved. The bound is specificity.

### 7.3.1 Nominal and realised key entropy

A key of `B·E` bits can take `2^(B·E)` values, and if keys were uniformly distributed over
that space a reference of `L` positions would yield an expected `L / 2^(B·E)` chance matches
per query seed. Keys are not uniformly distributed. Quantised event sequences derived from
real sequence are strongly non-uniform, and the realised number of distinct keys is far
smaller than the nominal space:

| geometry | nominal key width | distinct keys realised | realised entropy |
|---|---|---|---|
| 3 × 13 | 39 bits | 1,961,432 | 20.9 bits |
| 3 × 14 | 42 bits | 3,332,137 | 21.7 bits |
| 4 × 12 | 48 bits | 32,840,924 | 25.0 bits |

`[VERIFY]` `[TABLE 7.1]`

A 39-bit key therefore realises approximately 21 bits of entropy on this reference, a
shortfall of some 18 bits, and a 48-bit key realises 25. Specificity must be reasoned about
in realised entropy, not nominal width, and the two differ by more than an order of magnitude
in key count.

### 7.3.2 Occurrence capping as an observable of saturation

The index retains at most a fixed number of positions per key. The proportion of seeds whose
occurrence list is truncated is therefore a direct observable of how far the realised key
distribution falls short of uniformity. Measured saturation rises steeply as `E` falls: at
3 × 8 the index caps 99.3% of its seeds into 65,760 distinct keys `[VERIFY]`, at which point
a seed carries essentially no positional information and the structure has degenerated.

### 7.3.3 The squeeze, as a design inequality

A workable configuration must satisfy two conditions simultaneously. It needs enough true
anchors for the decision rule to act on,

&nbsp;&nbsp;&nbsp;&nbsp;(N − E + 1) · a^E ≥ M_min

and it needs the realised entropy to exceed what the reference demands, approximately

&nbsp;&nbsp;&nbsp;&nbsp;H_realised(B, E) ≳ log₂ L

The first is decreasing in `E` and the second increasing. A feasible region exists only if the
two cross, and whether they cross is determined by `a`. For `a` close to 1 the first
constraint is weak and `E` may be chosen freely to satisfy the second; as `a` falls the
feasible region narrows and may close entirely. The geometry sweeps of Chapter 6, which
reversed their conclusion twice under different decision rules and segmentation settings,
were searching a region that the value of `a` had already made very small — which is why they
produced unstable answers rather than a clear optimum.

---

## 7.4 Measuring per-event bucket agreement

### 7.4.1 Method

`a` is measurable directly and cheaply. The simulator records each read's true locus in its
identifier, so for a read originating at reference base `d` the reference event sequence
corresponding to that read is known. Both sides are quantised with the configuration under
test, and agreement is counted event-wise.

Three details are necessary for the measurement to be meaningful. Only forward-strand reads
are used, because the reverse complement occupies a separate coordinate system in the index.
A bounded search over offsets is performed and the best agreement retained, which absorbs an
off-by-`k` arising from the convention relating a k-mer's level to a position without
concealing genuine disagreement. And the query is normalised exactly as the daemon normalises
it — once per read, from the first chunk — so that the measurement includes the normalisation
error the production path incurs rather than an oracle value.

### 7.4.2 Results

| segmentation | dwell CV | B | agreement `a` | `a^12` |
|---|---|---|---|---|
| fixed-width | 0.00 | 4 | **55.9%** | 0.093% |
| detected | 0.00 | 4 | 24.4% | ~0 |
| detected | 0.44 | 4 | 23.6% | ~0 |
| detected | 0.44 | 3 | 38.2% | 0.001% |
| detected | 0.44 | 2 | 57.5% | 0.130% |

`[VERIFY]` `[TABLE 7.2]`

Two observations. Agreement is at best 55.9%, attained in the most favourable configuration
available — no dwell variability, fixed-width segmentation, the frozen 4-bit geometry.
Against a chance level of `2^-B` = 6.25% at four bits, the quantiser is clearly carrying
information; it is simply carrying far less than a 12-event key requires. Second, agreement
rises as bucket width rises, from 23.6% at four bits to 57.5% at two, confirming that the
dominant error is quantisation boundary crossing rather than gross mis-segmentation.

### 7.4.3 Limitations of the diagnostic

The measurement assumes that query event `i` corresponds to reference event `d + i` for a
constant offset `d`. This holds for fixed-width segmentation, where events are a fixed number
of samples and the correspondence is affine. It does not hold under event detection, where a
missed or spurious boundary shifts the correspondence for the remainder of the read, so the
offset drifts rather than remaining constant. The detected rows of §7.4.2 therefore
**understate** `a` and should be read as lower bounds, not as evidence that detection halves
agreement. A correspondence-aware measurement — aligning the two event sequences before
comparing, rather than assuming a constant offset — is the correct instrument for the
detected case and is not implemented here.

This limitation does not affect the chapter's argument, which rests on the fixed-width
measurement of 55.9% and on the model's behaviour at that value.

---

## 7.5 Verification against observed behaviour

The model makes a quantitative prediction that can be checked against an independent
measurement.

At the decision point used throughout Chapter 6 — ten chunks of 2000 samples, ten samples per
event, `E` = 12, `w` = 1 — a read supplies `N` = 2000 events and `N − E + 1` = 1989 seeds.
With `a` = 0.559 the model predicts

&nbsp;&nbsp;&nbsp;&nbsp;a^E = 0.0931%,&nbsp;&nbsp; M ≈ 1.85 true anchors per read

The measured median best-chain score on on-target reads is 4 `[VERIFY]`. The observed yield
therefore exceeds the model's lower bound by a factor of 2.2, which is the direction §7.2.3
requires of a bound derived under an independence assumption that fails positively. Order of
magnitude and sign both agree; a model predicting 1.85 against an observation of 4 is doing
useful work at a scale where the alternative hypotheses — that the decision rule is at fault
— predict values two to three orders of magnitude higher.

The model's behaviour in `E` is also consistent with the geometry results:

| `E` | `a^E` | predicted anchors | measured TPR at that geometry |
|---|---|---|---|
| 8 | 0.953% | 19.0 | 3.2% (4 × 8) |
| 10 | 0.298% | 5.9 | 15.8% (4 × 10) |
| 12 | 0.093% | 1.85 | 17.4% (4 × 12) |

`[VERIFY]`. Anchor yield rises monotonically as `E` falls while measured accuracy does not,
which is precisely the squeeze of §7.3.3: at `E` = 8 the yield is an order of magnitude higher
and the accuracy is worse, because realised entropy has collapsed and 88% of seeds are
occurrence-capped. The two constraints are visible acting against each other in the same
table.

---

## 7.6 Explanatory power: six interventions

The strongest evidence for a model of this kind is that it accounts for results obtained
before it was formulated. Each intervention below was implemented, measured, and found
wanting; each operates downstream of key formation and therefore cannot alter `a^E`.

| intervention | mechanism addressed | result | model's account |
|---|---|---|---|
| gap-tolerant chaining, band 12 | drift between anchors | 14.2 → 17.4% | reorganises 1.85 anchors |
| anchor-history chaining, look-back 48 | optimality of chain selection | → 18.8% | reorganises 1.85 anchors |
| occurrence filtering | false anchors | 17.8% | removes noise, adds no signal |
| longer look-back (128) | predecessor horizon | 18.0% | no additional anchors exist |
| shorter keys (4 × 8) | raise `a^E` | 3.2% | yield ↑ 10×, entropy collapses |
| difference-based keys | normalisation shift | 2.8–8.4% | differencing raises noise by √2 |

`[VERIFY]` `[TABLE 7.3]`

The pattern is uniform: interventions that reorganise existing anchors produce improvements
of tens of percent relative, and the one intervention that raised anchor yield by an order of
magnitude lost more to specificity than it gained. The difference-based encoding is
instructive as the only intervention that attacked `a` itself and still failed: differencing
two noisy quantities cancels a common shift but multiplies the variance of the residual by
two, and concentrates the resulting distribution near zero where bucket boundaries are most
densely crossed. It addressed the right quantity by the wrong mechanism.

It also explains an anomaly recorded in Chapter 6: the geometry freeze reversed twice under
different decision rules. Within a feasible region as narrow as `a = 0.559` permits, the
ordering of adjacent geometries is determined by second-order effects and is not stable under
changes elsewhere in the pipeline. The instability was a symptom of the ceiling, not an
inconsistency in the measurements.

---

## 7.7 Design implication

The practical consequence is a reordering of the design and reporting sequence for systems
of this class.

**Report `a` before reporting a decision rule.** A pipeline with `a` = 0.56 and `E` = 12 is
infeasible regardless of its chaining algorithm, index structure, or vote accumulation
scheme, and the infeasibility is established by a measurement requiring one pass over a few
hundred reads with known loci. Presenting an improved decision rule for such a pipeline is
not wrong so much as unfalsifiable: the rule's contribution cannot be distinguished from
noise at an anchor yield below two per read.

**Feasibility conditions.** Rearranging §7.3.3, a target anchor yield `M_min` at a given
reference scale requires

&nbsp;&nbsp;&nbsp;&nbsp;a ≥ (M_min / N)^(1/E)&nbsp;&nbsp; with&nbsp;&nbsp; H_realised(B, E) ≳ log₂ L

For `M_min` = 20, `N` = 2000 and `E` = 12 this requires `a` ≥ 0.80; at `E` = 8, `a` ≥ 0.72
`[VERIFY]`. These are the agreement levels a designer should treat as entry conditions, and
they are far above what this pipeline achieves.

**Where effort belongs.** Given a fixed budget, effort spent on the quantiser, the
normalisation estimator and the segmentation rule has leverage `E·ln(1/a)` on anchor yield,
whereas effort spent on the decision rule has leverage bounded by the factor between a greedy
and an optimal chain over the anchors that exist. In this work that factor was measured at
1.3; the exponent was 12.

---

## 7.8 What would raise `a`

Three mechanisms are identified, with the reasoning for each, as the principal avenue for
further work.

**Merging rather than differencing.** The published mapper against which this system is
compared exposes a parameter that merges consecutive events whose signal difference falls
below a threshold into a single hashed symbol. This raises `a` by construction: two events
straddling a bucket boundary, which would independently disagree with probability near 1/2
each, become one symbol whose value is determined by their aggregate. It is distinct from the
difference-based encoding rejected in §7.6, which retained one symbol per event and merely
changed what the symbol encoded. This is the single most promising untested change and the
one this chapter recommends first.

**Normalisation.** An earlier measurement in this work recorded that, at zero added noise,
reference-derived (oracle) normalisation recovers 100% of seeds where per-read normalisation
recovers 50.5% `[VERIFY]`. Per-read normalisation is unavoidable in principle — a read has
only its own samples — but the estimator need not be the median and MAD of a single 2000-
sample chunk. The instrument reports `median` and `median_before` per read in its own
metadata, and a longer or better-conditioned estimate would reduce the shared shift error
that §7.2.3 identifies as a source of correlated disagreement.

**Non-uniform bucket boundaries.** Uniform bucketing over a clipped z-range places boundaries
without regard to where the level distribution is dense, so a disproportionate share of
events sit near a boundary. Boundaries placed at quantiles of the realised level distribution
would equalise bucket occupancy. An adaptive quantiser was implemented and rejected earlier
in this work on the grounds that it lost more recall than the key space it bought; that
evaluation predates the present model and was conducted without measuring `a`, and should be
repeated with `a` as the dependent variable.

---

## 7.9 Limitations

**`a` was not measured for the comparison system.** The chapter's argument would be
materially stronger if the agreement achieved by the published mapper were measured on the
same input with the same instrument. Its accuracy of 92.4% implies, through the model, an
agreement substantially above 0.56, and confirming that directly would convert the model from
an account of this system's failure into a comparative explanation of both systems'
behaviour. This is the single most valuable experiment left undone and is stated as such.

**The diagnostic assumes a constant event correspondence.** As §7.4.3 sets out, the detected
rows are lower bounds. The model's use of `a` = 0.559 is drawn from the fixed-width
measurement, where the assumption holds.

**Single reference and single simulator.** All agreement figures derive from one 20 Mb
reference under one simulator's noise model. Agreement is a property of the quantiser against
a particular signal-generating process, and both the absolute values and the ordering across
bucket widths could differ on real signal.

**The independence bound is not tight.** §7.2.3 establishes only a direction, not a magnitude.
A model of the correlation structure — for instance, treating normalisation error as a
per-read random effect shared by all events — would give a tighter prediction than the factor
of 2.2 discrepancy observed in §7.5, and would be the natural refinement.

---

## 7.10 Summary

The accuracy of a quantised-event matching pipeline is bounded by a single measurable
quantity: the probability `a` that reference and query assign the same bucket to
corresponding events. Because a key requires `E` consecutive agreements, expected anchor
yield falls as `a^E`, and this bound holds from below under the positive correlation that
overlapping k-mers and shared per-read normalisation induce.

Measured on this pipeline, `a` reaches 55.9% in its most favourable configuration, giving a
predicted yield of 1.85 true anchors per read against an observed median best-chain score of
4 — agreement in order of magnitude and in sign. Raising anchor yield by shortening keys
fails because realised key entropy, some 18 bits below the nominal key width, collapses
before the yield becomes useful; a 3 × 8 geometry truncates 99.3% of its occurrence lists.
The two constraints define a feasible region whose width is set by `a`, and at `a` = 0.56
that region is narrow enough that the ordering of adjacent geometries is unstable under
changes elsewhere in the pipeline — which is the behaviour Chapter 6 observed and could not
explain.

Six interventions on the decision stage, spanning chaining, filtering, key geometry and key
encoding, are accounted for by the model: those that reorganise anchors yield tens of percent
relative, and the one that multiplied anchor yield lost more to specificity than it gained.
The design conclusion is that `a` is the quantity to measure first and the quantity to
improve, that it is cheap to measure, and that for this class of system a reported decision
rule unaccompanied by a reported agreement figure cannot be assessed.
