# Chapter 7 — The Accuracy Ceiling of Quantised-Event Matching

> **Draft status.** Structural first draft. Every number is recomputed from a committed
> file in `results/` by `scripts/verify_thesis_numbers.py`; `[UNVERIFIED]` marks the
> exceptions, which Appendix F.3 lists and explains. `scripts/audit_manuscript.py` checks
> that no marker, cross-reference or figure in this chapter dangles.

---

## 7.1 Locating the deficiency

Chapters 3 and 5 establish that the data plane is not the limiting component of the system.
Per-chunk decision latency corresponds to a utilisation of approximately 0.1 against the
budget the instrument imposes, no chunks are dropped at the sustained arrival rate, and the
concurrency primitives are verified under both thread and address sanitisers. Chapter 6
establishes that the system nevertheless resolves 17.6% of on-target reads [16.6–18.8 across
three seeds] where a published signal-space mapper resolves 91.8% [91.2–92.4] on identical
simulated input.

A deficiency of that magnitude admits two broad explanations. Either the decision stage —
how evidence accumulated from matched seeds is converted into an accept or reject — is
inadequate, or the evidence itself is absent. Six interventions on the decision stage were
implemented and measured in the course of this work, spanning gap-tolerant chaining,
best-predecessor chaining with bounded look-back, occurrence-frequency filtering, key
geometry, and difference-based key encoding. None produced more than a 35% relative
improvement (Table 7.3). The consistency of that outcome across structurally unrelated
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

**conditional on an exact correspondence existing between the two event sequences** — that
is, conditional on reference event `d + i` being the counterpart of query event `i` for every
`i` in the window. Subject to that condition `a^E` is a lower bound on key match probability
and `M` a lower bound on expected anchor yield, which is the useful direction: a bound saying
a configuration *cannot* work is stronger evidence than an estimate of how poorly it does.

**The condition is not innocuous, and §7.4.4 shows it fails for most configurations of
interest.** A key requires `E` consecutive events, so a window containing a single inserted
or deleted event cannot match however well the surrounding events agree. Writing `g` for the
probability that an alignment column is an insertion or deletion rather than a matched pair,
the quantity that governs yield is

&nbsp;&nbsp;&nbsp;&nbsp;P(key matches) ≈ (1 − g)^E · a^E

and the first factor is absent from the simple form. Where segmentation is fixed-width and
dwell is constant, `g` is small, the first factor is near unity, and `a^E` behaves as the
lower bound derived above — measured at 1.8× and 5.1× below the direct count in the two such
cells of §7.4.4. Where segmentation is data-dependent, or dwell varies, `g` rises to 0.09–0.21
and `a^E` becomes an **over-estimate**, by between 1.9× and 14× across the remaining cells.

The consequence for how this chapter should be read is specific rather than general. The
model's qualitative content — that yield falls geometrically in `E` and that the base is a
measurable property of the quantiser — is unaffected. Its use as a one-sided bound is
restricted to the exact-correspondence case.

`[FIG 7.3]` Measured key match probability against what `a^E` predicts, with marker area
scaled by the correspondence-failure rate, which is the clearest statement of where the simple
form is a bound and where it is not.

Section 7.4.4 therefore reports the key match
probability **measured directly** alongside every modelled value, and §7.5 is stated against
the measurement rather than against the model.

### 7.2.4 Sensitivity

Differentiating `M` with respect to `E` gives `∂M/∂E ∝ a^E · ln a`, so the proportional loss
per added event is `ln(1/a)`, independent of `E`. At `a = 0.559` each additional event in a
key removes 44% of the expected anchor yield. Design discussions of key length
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

Measured on the 20 Mb chr20 reference used throughout Chapters 6 and 7, encoded with the
same squigulator-derived model used on both sides there:

| geometry | nominal key width | distinct keys realised | realised entropy | shortfall |
|---|---|---|---|---|
| 3 × 8 | 24 bits | 92,482 | 16.5 bits | 7.5 |
| 4 × 8 | 32 bits | 1,519,844 | 20.5 bits | 11.5 |
| 3 × 13 | 39 bits | 5,096,781 | 22.3 bits | 16.7 |
| 3 × 14 | 42 bits | 7,985,629 | 22.9 bits | 19.1 |
| 4 × 12 | 48 bits | 14,908,778 | 23.8 bits | 24.2 |

`[TABLE 7.1]`

`[FIG 7.6]` Realised entropy against nominal width, and cap survival, at both reference sizes.

A 39-bit key therefore realises 22.3 bits of entropy on this reference, a shortfall of 16.7
bits, and the frozen 48-bit key realises 23.8, a shortfall of 24.2. **The shortfall grows
faster than the nominal width**: doubling the nominal key space from 24 to 48 bits buys 7.3
bits of realised entropy. Specificity must be reasoned about in realised entropy, not nominal
width, and at the frozen geometry the two differ by a factor of 1.6 × 10⁷ in key count.

**Realised entropy depends on reference size, and the dependence is the point.** On the 64 Mb
reference of regime R3 the same geometries realise 25.0 bits at 4 × 12 and 23.7 at 3 × 14
(`results/key-entropy.csv`) — more distinct keys, as a larger reference must produce, but a
*smaller* shortfall reduction than the fourfold size increase would suggest. An earlier version
of this table quoted counts whose reference size was never recorded and which reproduce at
neither size; both are now measured and the size is a column.

### 7.3.2 Occurrence capping as an observable of saturation

The index retains at most a fixed number of positions per key. The proportion of seeds whose
occurrence list is truncated is therefore a direct observable of how far the realised key
distribution falls short of uniformity. Measured saturation rises steeply as `E` falls. At 3 × 8 only 2.8% of minimizer occurrences
survive the cap on the 20 Mb reference — 97.2% truncated, into 92,482 distinct keys — against
93.5% surviving at the frozen 4 × 12. At that point a seed carries essentially no positional
information and the structure has degenerated.

The scale dependence is sharp and worth stating beside it: on the 64 Mb reference the same
3 × 8 geometry keeps 0.7% of occurrences, truncating 99.3%, while 4 × 12 keeps 83.7%
(`results/key-entropy.csv`). Short keys degrade faster than the reference grows, which is why
a geometry chosen on a small reference does not transfer to a large one.

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
| detected | 0.44 | 2 | 57.5% | 0.130% | `[TABLE 7.2]`

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

The sections that follow remove this limitation rather than carrying it: §7.4.4 implements the
correspondence-aware instrument and §7.4.5 applies it to the comparison system.

### 7.4.4 A correspondence-aware measurement

**Instrument.** Both systems are made to emit their per-event quantised symbols — reference
side and query side — in one format, and one program scores them. Estimator symmetry is the
point: an agreement figure computed two different ways for two systems is not a comparison.
For this pipeline the symbols come from `bench/symbol_dump.cpp`, which calls the production
`PoreModel::reference_to_events` and `quantise_signal_detected` paths. For the comparison
system they come from its own `--out-quantize`, which is a facility it already ships, plus a
four-line change exposing the same dump on the reference side, which it does not
(`third_party_patches/`, Appendix B). Scoring is `bench/symbol_agreement.cpp`.

The scorer aligns the two symbol streams with a semi-global dynamic program — the query
aligned in full, free gaps at both ends of the reference window, match +1, mismatch −1, gap
−1 — and reports three quantities rather than one:

- `a_dp`, matched pairs over aligned pairs under the maximising alignment. An **upper bound**:
  the alignment is chosen to maximise matches, so a genuinely ambiguous correspondence is
  flattered.
- `a_affine`, the same ratio under the single best constant offset. This is the §7.4.1
  estimator, and a **lower bound** once correspondence drifts.
- the indel fraction, without which the gap between the two is uninterpretable.

It then counts, directly from the alignment path, the fraction of positions beginning a
gap-free all-matching run of `E` columns. That is the key match probability itself, measured
rather than modelled, and it is reported for `E` = 12 (this pipeline) and `E` = 8.

**Validation.** Under the conditions of §7.4.2 — fixed-width segmentation, zero dwell
dispersion, normalisation from the first 2000-sample chunk as the production path performs it
— the independent instrument returns `a_affine` = 0.556, against the 0.559 of §7.4.2. The two measurements are made by different code on the same input and agree to
0.3 percentage points, which is the basis for trusting the figures below.

**Results**, 200 reads per cell, 20 Mb chr20, both strands indexed, the same
squigulator-derived model on both sides.

| configuration | dwell CV | `a_dp` | `a_affine` | indel | P(key), `E`=12 | anchors/read |
|---|---|---|---|---|---|---|
| fixed-width, chunk normalisation | 0.00 | 0.686 | **0.556** | 0.129 | 0.0549 | 143 |
| fixed-width, read normalisation | 0.00 | 0.793 | 0.726 | 0.073 | 0.1113 | 285 |
| fixed-width, read normalisation | 0.44 | 0.648 | 0.187 | 0.208 | 0.00038 | 1.1 |
| detected (frozen), read normalisation | 0.00 | 0.797 | 0.182 | 0.170 | 0.0124 | 33.8 |
| detected (frozen), read normalisation | 0.44 | 0.783 | 0.188 | 0.196 | 0.00554 | 14.9 |

`[TABLE 7.4]`

`[FIG 7.4]` The per-read distributions behind these pooled figures, so that the separation is
visible as a distribution rather than asserted as a mean.

Four readings, in descending order of how much they change the chapter.

**The affine estimator was materially, not cosmetically, wrong.** For the frozen
detected configuration it reports 0.188 where the correspondence-aware figure is 0.783 — an
understatement by a factor of four. §7.4.3 predicted the direction; the magnitude means the
detected rows of §7.4.2 cannot be used at all, and the claim that detection roughly halves
agreement is withdrawn. It does not: detection and fixed-width reach 0.783 and 0.648
respectively at CV 0.44, and detection is the **better** of the two there.

**Per-event agreement is not the binding constraint for the detected configuration; the
indel rate is.** Agreement barely moves with dwell (0.797 → 0.783) while the measured key
match probability falls by 2.2× (0.0124 → 0.00554), because the indel fraction rises from
0.170 to 0.196 and a key spanning an indel fails outright. This is the mechanism §7.2.3 now
carries and the simple `a^E` form does not.

**Normalisation is worth 2× in yield, which quantifies a recommendation §7.8 could previously
only motivate.** Estimating the shift and scale from the whole read rather than from one
2000-sample chunk raises `a_affine` from 0.556 to 0.726 and the measured key match probability
from 0.0549 to 0.1113. It remains the case that a read has only its own samples; the finding
is that *which* of its samples are used is worth a factor of two.

**Fixed-width's collapse under dwell is a correspondence failure, not a quantisation
failure.** Its `a_dp` falls only from 0.793 to 0.648 while its indel fraction trebles from
0.073 to 0.208 and its key match probability falls by 293×. The quantiser still recovers most
of the information; the pipeline cannot use it, because a key of 12 consecutive events has no
tolerance for a boundary that has moved. That is the same conclusion Chapter 6 reached from
accuracy alone, now located in a measured quantity.

### 7.4.5 The comparison system, measured the same way

Chapter 6 §6.10 and §7.9 of this chapter named one experiment as the most valuable not
performed: measuring per-event agreement for the published mapper on the same input. It is
performed here.

The comparison system is run at its own defaults, which the help text gives as `-q 4` (four
bits, sixteen buckets), `-e 8` (eight events per key), `-w 0` (no sketching) and
`--sig-diff 0.35`.

| system | segmentation | bits | `E` | dwell CV | `a_dp` | indel | P(key) at own `E` | anchors/read |
|---|---|---|---|---|---|---|---|---|
| this pipeline | detected (frozen) | 4 | 12 | 0.00 | 0.797 | 0.170 | 0.0124 | 33.8 |
| this pipeline | detected (frozen) | 4 | 12 | 0.44 | 0.783 | 0.196 | 0.00554 | 14.9 |
| comparison | its own | 4 | 8 | 0.00 | **0.913** | 0.055 | **0.350** | 651 |
| comparison | its own | 4 | 8 | 0.44 | **0.911** | 0.092 | **0.254** | 467 |

`[TABLE 7.5]`

`[FIG 7.2]` Agreement, correspondence-failure rate and measured key match probability for both
systems side by side.

**The gap is real and it is large.** At squigulator's default dispersion the comparison
system's per-event agreement is 0.911 against this pipeline's 0.783, and its measured key
match probability is 0.254 against 0.00554 — a factor of **46**, giving 467 anchors per read
against 14.9. Against a measured accuracy of 91.8% for that system and 17.6% for this one on
the same data, the ceiling model is doing the work it claimed: the accuracy difference is
accounted for by anchor supply, not by the decision rule, and the anchor supply difference is
accounted for by what happens before seeding.

**The gap decomposes, and the larger part is not key length.** Within this pipeline, measured
key match probability at `E` = 8 rather than 12 is 0.0274 against 0.00554, a factor of 4.9.
The remaining factor of 9.3 — 0.254 against 0.0274 at the same key length — is attributable to
the encoding itself. Shortening keys is the smaller lever and, as §7.3 shows, it is the one
that pays for itself in lost specificity.

**Its agreement is dwell-invariant where ours is not.** 0.913 → 0.911 across CV 0 to 0.44,
against 0.797 → 0.783 for this pipeline's agreement and a 2.2× fall in its key match
probability over the same range. This is the mechanism behind Chapter 6 §6.5, which observed
the robustness and could not explain it. The explanation is that the comparison system's event
retention rule is applied **symmetrically to both sides**: `--sig-diff` drops any event whose
level differs from the last retained event by less than 0.35, and it is applied to the
reference encoding and to the query alike, retaining 73.4% on each. Its indel rate
is consequently half this pipeline's at the same dispersion, 0.092 against 0.196. The
mechanism is visible in its source and consistent with the measurements; it has not been
isolated by ablation, and is stated as the explanation the evidence supports rather than as a
demonstrated cause.

**It buys agreement by spending alphabet, which is the opposite of this pipeline's design
choice.** Its nominal four bits admit sixteen buckets; the realised symbol distribution over
chr20 occupies six of them, with frequencies 7.7, 21.1, 9.7, 27.6, 24.0 and 8.8 per cent plus
a 0.9 per cent outlier bucket, for a realised entropy of approximately 2.46 bits per event. Its quantiser is deliberately non-uniform — fine resolution inside a configurable
central band and coarse resolution outside it — so events near the distribution's mode are
resolved and tails are collapsed. This pipeline's uniform quantiser over a clipped z-range
spends its alphabet evenly and realises more of it, and achieves lower agreement.

A third system points the same way, which matters because two points could be coincidence.
Sigmoni, which classifies rather than maps and therefore shares none of this pipeline's
decision machinery, quantises signal into an alphabet of **six** symbols by default, chosen on
the stated grounds that it balances robustness to noise against specificity in exact matching
`[2°]` `[CHECK against the paper's methods]`. Six symbols is 2.58 bits, within a tenth of a bit
of the 2.46 measured here for the comparison mapper's realised alphabet, and both are far below
this pipeline's four. Two independent systems arriving at an effective alphabet near 2.5 bits,
one of them stating noise robustness as the reason, is evidence that the operating point is not
accidental.

The design axis this exposes is **per-event entropy against per-event agreement**, and the
two systems sit at opposite ends of it. A key of 8 events at 2.46 bits carries about 19.7 bits
of nominal specificity; one of 12 events at close to 4 realised bits carries about 48, of which
§7.3.1 measures some 18 to be unrealised. The system with the less specific key is the more
accurate one by a factor of five, because specificity it does not need costs it nothing and
agreement it does need costs this pipeline everything. §7.7 is strengthened accordingly: `a`
is not merely the quantity to measure first, it is the quantity to **optimise against**, and
alphabet is the currency available to pay for it.

---

## 7.5 Verification against observed behaviour

**The original prediction, and its standing.** At the decision point used throughout
Chapter 6 — ten chunks of 2000 samples, ten samples per event, `E` = 12, `w` = 1 — a read
supplies `N` = 2000 events and `N − E + 1` = 1989 seeds. With the `a` = 0.559 of §7.4.2 the
simple form predicts `a^E` = 0.0931% and `M` ≈ 1.85 true anchors per read, against a measured
median best-chain score of 4. That remains the correct calculation *for the
configuration it describes* — fixed-width segmentation at zero dwell with chunk normalisation
— and §7.4.4 confirms it independently, measuring the key match probability of that exact cell
at 0.0549 against the modelled 0.0108, the model under-predicting by 5.1× in the direction
§7.2.3 requires when correspondence is exact.

**What the configuration actually shipped predicts.** The frozen configuration is not that
one: it uses detected events, and at squigulator's default dispersion §7.4.4 measures its key
match probability directly at 0.00554, giving 14.9 anchors per read. The agreement with
observation is therefore not 1.85 against 4 but 14.9 against a median best-chain score of 4,
with the measurement now **above** the observation by 3.7×. Both directions are explicable and
the second is the more informative: a chainer that sees 15 anchor-eligible positions and
recovers a best chain of 4 is losing most of them, which is a statement about the decision
stage that the earlier figure could not support, because a decision stage cannot recover more
anchors than exist and 1.85 left no room for loss.

This is a genuine sharpening rather than a reversal, and it narrows the chapter's claim. The
deficiency is still located before seeding — 14.9 anchors per read against the comparison
system's 467 is a 31× supply difference that no decision rule closes. But the decision stage
is no longer exonerated: the gap between 15 available and 4 realised is the one place in this
pipeline where the measurements now show recoverable loss, and §7.8 is amended to say so.

**Order of magnitude and sign.** Across five configurations §7.4.4 measures key match
probabilities spanning 0.00038 to 0.111, a range of 293×, and the accuracy ordering of those
configurations follows the measured key match probability in every case. That is the property
the model was proposed for, and it survives the correction to its functional form.

The model's behaviour in `E` is also consistent with the geometry results:

| `E` | `a^E` | predicted anchors | measured TPR at that geometry |
|---|---|---|---|
| 8 | 0.953% | 19.0 | 3.2% (4 × 8) |
| 10 | 0.298% | 5.9 | 15.8% (4 × 10) |
| 12 | 0.093% | 1.85 | 17.4% (4 × 12) |. Anchor yield rises monotonically as `E` falls while measured accuracy does not,
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
| gap-tolerant chaining, band 12 | drift between anchors | 14.1 → 16.5% | reorganises a fixed supply |
| anchor-history chaining, look-back 48 | optimality of chain selection | → 17.6% | reorganises a fixed supply |
| occurrence filtering | false anchors | 16.3% | removes noise, adds no signal |
| longer look-back (128) | predecessor horizon | 19.0% | the one that does help, and by 1.4 points |
| shorter keys (4 × 8) | raise `a^E` | 3.2% | yield ↑ 10×, entropy collapses |
| difference-based keys | normalisation shift | 8.3% [3.6–11.8] | differencing raises noise by √2 |

`[TABLE 7.3]`

`[FIG 7.5]` Voting against chaining across key geometries, which shows the decision rule
moving the result by tens of percent where the geometry moves it by an order of magnitude.

The pattern is near-uniform: interventions that reorganise existing anchors produce
improvements of tens of percent relative and no more — the best of them, a longer predecessor
horizon, reaches 19.0% against voting's 14.1%, or 35% relative — and the one intervention that
raised anchor yield by an order of magnitude lost more to specificity than it gained.

One qualification, from replicating the table in triplicate. Longer look-back was previously
reported as not helping, on the strength of a single run; it does help, by 1.4 points and in
every seed. That does not disturb the argument, because 35% relative on a supply of 14.9
anchors is still an order of magnitude short of the 467 the comparison system starts from. It
does show that the margins between adjacent rows of this table are at the edge of what one
seed resolves, and §4.7.3 reports the spreads that make that concrete. The difference-based encoding is
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

For `M_min` = 20, `N` = 2000 and `E` = 12 this requires `a` ≥ 0.80; at `E` = 8, `a` ≥ 0.72. These are the agreement levels a designer should treat as entry conditions, and
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

**Merging, applied symmetrically to both sides.** The published mapper against which this
system is compared exposes a parameter that merges consecutive events whose signal difference
falls below a threshold into a single hashed symbol. This raises `a` by construction: two
events straddling a bucket boundary, which would independently disagree with probability near
1/2 each, become one symbol whose value is determined by their aggregate. It is distinct from
the difference-based encoding rejected in §7.6, which retained one symbol per event and merely
changed what the symbol encoded.

Section 7.4.5 upgrades this from a proposal to a measured property of a working system: the
threshold is 0.35 in normalised units, retention is 73.4% on both sides, agreement is 0.911
and the indel rate is 0.092 — half this pipeline's. **The operative word is symmetrically.**
The rule is applied to the reference encoding and to the query by the same code, so the two
sides agree on how many events a given span contains. This pipeline's reference encoder emits
one event per k-mer unconditionally while its detector emits roughly 0.78, and §7.4.4 measures
the resulting indel rate at 0.170 even at zero dwell dispersion, where there is no timing
noise to blame. An indel rate that high at zero noise is a structural mismatch between the two
encoders rather than a property of the signal, and it is the first thing to fix. It remains the
single most promising change and it is now better specified: merge on both sides with one rule,
not merely on the query.

**Normalisation.** An earlier measurement in this work recorded that, at zero added noise,
reference-derived (oracle) normalisation recovers 100% of seeds where per-read normalisation
recovers 50.5% (commit `6cb90bb`). Per-read normalisation is unavoidable in principle — a read has
only its own samples — but the estimator need not be the median and MAD of a single 2000-
sample chunk. The instrument reports `median` and `median_before` per read in its own
metadata, and a longer or better-conditioned estimate would reduce the shared shift error
that §7.2.3 identifies as a source of correlated disagreement.

Section 7.4.4 quantifies the available gain rather than leaving it as an argument. Holding
everything else fixed and estimating the shift and scale from the whole read instead of the
first chunk raises agreement from 0.556 to 0.726 and the measured key match probability from
0.0549 to 0.1113, a factor of 2.0 in anchor supply for no change to the index, the geometry or
the decision rule. The whole read is not available at the first chunk, so this exact estimator
is not deployable as stated; the deployable version is an estimate that is **refined as chunks
arrive**, which the measurement now justifies building.

**Non-uniform bucket boundaries.** Uniform bucketing over a clipped z-range places boundaries
without regard to where the level distribution is dense, so a disproportionate share of
events sit near a boundary. Boundaries placed at quantiles of the realised level distribution
would equalise bucket occupancy. An adaptive quantiser was implemented and rejected earlier
in this work on the grounds that it lost more recall than the key space it bought; that
evaluation predates the present model and was conducted without measuring `a`, and should be
repeated with `a` as the dependent variable.

Section 7.4.5 shows the comparison system doing exactly this, and in a direction the earlier
attempt did not try. Its quantiser is not equal-occupancy either: it allocates fine resolution
to a central band and collapses the tails, realising about 2.46 bits of a nominal 4.
Equal-occupancy maximises entropy, which is what the rejected implementation optimised and
what this pipeline's uniform quantiser partly achieves; the working system instead
**minimises boundary crossings where the data is dense**, accepting a smaller alphabet to do
it. Those are different objectives, and the measurements say the second is the one that
governs accuracy.

**Recover the loss inside the decision stage.** Section 7.5 revises the predicted anchor
supply for the frozen configuration from 1.85 to a measured 14.9 per read, against an observed
median best-chain score of 4. Unlike the three mechanisms above this is loss occurring *after*
seeding, and it is newly visible: the earlier figure of 1.85 sat below the observation and so
left no room for it. Roughly 11 of 15 anchor-eligible positions per read are not reaching the
best chain. Whether that is the occurrence cap discarding them before chaining, the 24-slot
greedy chainer thrashing as §4.7.3 describes, or a band of 12 being too narrow for the
measured indel rate of 0.196, is not determined by these measurements — but the anchors are now
known to exist, which makes this the cheapest remaining diagnostic in the pipeline.

---

## 7.9 Limitations

**The alignment-based agreement figures are upper bounds.** `a_dp` is computed under the
correspondence that maximises matches, so a system whose true correspondence is ambiguous is
flattered. The indel fractions are reported beside every figure for this reason: at 0.055 to
0.208 the alignments are near-diagonal, so the estimator is not manufacturing matches through
heavy gapping, but the bound is one-sided and the comparison between systems is only as fair
as their indel fractions are similar. They are not identical — 0.092 against 0.196 at CV 0.44
— and the direction of that asymmetry favours the comparison system. A paired estimator that
constrained both systems to the same indel budget would settle it and is not implemented.

**The symmetric-retention explanation is not isolated.** Section 7.4.5 attributes the
comparison system's low indel rate to applying one merging rule to reference and query alike.
That mechanism is visible in its source and consistent with every measurement taken, but no
ablation was run — the decisive experiment is to apply this pipeline's detector to its own
reference encoding and re-measure, which is a change to the indexing path rather than a
measurement, and was not made.

**Two dwell levels, not a sweep.** Agreement is measured at CV 0 and CV 0.44 only. Chapter 6
sweeps six levels for accuracy; the agreement measurement does not, so the shape of the curve
between those points is unknown and the claim of dwell-invariance for the comparison system
rests on two points.

**Single reference and single simulator.** All agreement figures derive from one 20 Mb
reference under one simulator's noise model. Agreement is a property of the quantiser against
a particular signal-generating process, and both the absolute values and the ordering across
bucket widths could differ on real signal. This is now the chapter's largest unaddressed
threat, since the comparison is otherwise closed.

**200 reads per cell, no replicate seeds.** The accuracy results of Chapter 6 are reported as
three-seed means after a single-run figure had to be withdrawn (Chapter 8, Table 8.1, row 11).
The agreement figures here are single-seed and inherit that risk; the per-read distributions
are in `results/agreement-*.csv` and are tight, but tightness within a run is not replication
across runs.

**The correlation structure is still unmodelled.** §7.2.3 now gives the functional form
`(1 − g)^E · a^E` and §7.4.4 shows it predicting the two high-indel cells to within 15% and
45% while under-predicting the low-indel cells by 1.8× to 5.1×. The residual is clustering:
indels and disagreements both arrive in runs, which the form treats as independent. Treating
normalisation error as a per-read random effect shared by all events is the natural refinement
and is not attempted.

---

## 7.10 Summary

The accuracy of a quantised-event matching pipeline is bounded by two measurable quantities:
the probability `a` that reference and query assign the same bucket to corresponding events,
and the rate `g` at which the two event sequences fail to correspond at all. A key requires
`E` consecutive events that both agree and correspond, so key match probability goes as
`(1 − g)^E · a^E`. The `a^E` factor alone is a lower bound when correspondence is exact —
measured at 1.8× to 5.1× below the direct count in the two such configurations — and an
over-estimate by up to 14× when it is not, which covers every configuration this pipeline
actually ships.

Measured on this pipeline under a correspondence-aware instrument, `a` is 0.78 in the frozen
configuration and the indel rate is 0.196, giving a directly measured key match probability of
0.55% and 14.9 anchor-eligible positions per read against an observed median best-chain score
of 4. The earlier figure of 1.85 anchors, derived from the simple form at `a` = 0.559, applies
to a fixed-width configuration that is not the one shipped; the independent instrument
reproduces that configuration's agreement to 0.3 percentage points, which is the basis for
trusting both numbers.

Measured the same way on identical input, the published comparison mapper achieves `a` = 0.911
with an indel rate of 0.092 and a key match probability of 0.254 at its own key length of 8 —
467 anchor-eligible positions per read against 14.9, a factor of 31. That accounts for 91.8%
accuracy against 17.6% without appeal to the decision rule, and it decomposes: a factor of
4.9 from key length and a factor of 9.3 from the encoding itself. The encoding difference is
the larger and it is specific. The comparison system applies one event-merging rule to
reference and query alike, so the two sides agree on event counts by construction and its
indel rate is half this pipeline's; and its quantiser spends alphabet to buy agreement,
realising about 2.46 bits of a nominal 4 where this pipeline's uniform quantiser realises
more and agrees less.

Raising anchor yield by shortening keys fails for the reason §7.3 gives: realised key entropy,
16.7 bits below the nominal key width at 3 × 13 and 24.2 below it at the frozen 4 × 12,
collapses before the yield becomes useful, and a 3 × 8 geometry truncates 97.2% of its
occurrence lists on the 20 Mb reference and 99.3% on the 64 Mb one. The two constraints define a feasible
region whose width is set by `a`, and at `a` below 0.8 that region is narrow enough that the
ordering of adjacent geometries is unstable under changes elsewhere in the pipeline — which is
the behaviour Chapter 6 observed and could not explain.

Six interventions on the decision stage, spanning chaining, filtering, key geometry and key
encoding, are accounted for: those that reorganise anchors yield tens of percent relative, and
the one that multiplied anchor yield lost more to specificity than it gained. The revised
anchor supply does, however, withdraw the strongest version of that claim. At 14.9
anchor-eligible positions per read against a realised best chain of 4, the decision stage is
losing roughly 11 of 15, which the earlier estimate of 1.85 could not have revealed. The
deficiency remains located before seeding, because 14.9 against 467 is a supply gap no
decision rule closes; but the decision stage is no longer exonerated, and locating that loss
is the cheapest remaining experiment in the pipeline.

The design conclusion is accordingly sharper than when this chapter was first drafted. `a` is
the quantity to measure first, it is cheap to measure, and for this class of system a reported
decision rule unaccompanied by a reported agreement figure cannot be assessed. To that: the
quantity to **optimise** is key match probability rather than `a` alone, since correspondence
failure enters with the same exponent as disagreement and this pipeline's indel rate is 0.170
even at zero timing noise; and the currency available to buy both is alphabet, which the
comparison system spends and this pipeline hoards.
