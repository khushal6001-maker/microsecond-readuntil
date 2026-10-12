# Chapter 6 — Dwell-Time Variability as an Evaluation Axis

> **Draft status.** Structural first draft. `[VERIFY]` marks figures to be re-derived from a
> recorded run; `[TABLE 6.x]` / `[FIG 6.x]` mark artefacts still to be produced. Raw data for
> §6.6 is in `results/rawhash-dwell-sweep.csv`.
>
> **Scoping discipline.** This chapter makes three claims that are easily conflated and must
> be kept separate throughout. (i) *This system's* fixed-width configuration collapses as
> dwell variability rises. (ii) A published signal-space mapper does *not* collapse, so the
> sensitivity is a property of fixed-width segmentation and not of signal-space matching.
> (iii) The standard read-until simulator cannot exercise the axis at all. None of the three
> is a claim that fixed-width segmentation is invalid in general, that published tools are
> mis-evaluated, or that signal simulators generally omit dwell — and none of those three
> stronger statements is true.

---

## 6.1 Motivation: a signal property that evaluation holds fixed

The matching pipeline of Chapter 4 reduces a raw current trace to a sequence of discrete
events before any comparison occurs. Segmentation is therefore prior to everything else in the
pipeline: a key is a window of consecutive events, so if the event boundaries on the query do
not correspond to those implied by the reference, the key bits themselves differ and no
subsequent stage can recover the correspondence.

The simplest segmentation rule divides the sample stream into fixed-width windows. It is
exactly correct under one condition — that the pore translocates at a constant rate, so that a
fixed number of samples always corresponds to one k-mer — and the condition is not satisfied
by any real pore. The motor protein's step rate is variable and the resulting dwell-time
distribution is positive and right-skewed. Published signal-space mappers accordingly segment
by detecting level changes rather than by time, and do so without remarking on the alternative.

The question this chapter asks is not whether detection is preferable, which is settled
practice, but a methodological one: **how sensitive is a given pipeline to the magnitude of
time-domain noise, and does any existing evaluation measure that sensitivity?** The answer to
the second part is no, and the consequences of that omission are the chapter's subject.

An established signal simulator exposes time-domain noise directly as the standard deviation
of the number of samples per base, and its documented default is a mean of 9.0 samples with a
standard deviation of 4.0 `[VERIFY]` — a coefficient of variation of approximately 0.44. The
axis is therefore neither hypothetical nor obscure; the default setting of the accepted tool
sits in the middle of the range this chapter sweeps. What is absent from the literature is any
evaluation that varies it.

---

## 6.2 Experimental method

**Signal generation.** Reads are simulated with Squigulator under its R10 promethion profile,
with the dwell mean fixed at 10.0 samples per base and the dwell standard deviation swept over
{0, 1, 2, 3, 4.4, 6}, giving coefficients of variation {0, 0.10, 0.20, 0.30, 0.44, 0.60}. The
mean is fixed so that the only varying quantity is the dispersion; a varying mean would also
change the number of bases a chunk contains and confound the axis with read length. Three
independent generator seeds are used throughout, and all reported figures are over three
replicates.

**Why not this project's own generator.** Earlier measurements in this work used a signal
generator written for the purpose. A claim about evaluation methodology cannot rest on signal
produced by the party making the claim, and §6.7 shows that the detector calibration derived
from that generator does not transfer. All results in this chapter use the external simulator.

**Reference and index.** A 20 Mb prefix of human chromosome 20, indexed on both strands
(40 Mb of encoded sequence). Reference encoding uses the simulator's own pore model, extracted
from its source, for reasons §6.3 gives.

**Negative control.** The off-target set is generated from a composition-matched shuffle of the
same reference, simulated at the same dwell standard deviation as the on-target set in each
cell. Holding composition and dwell fixed between the two sets means the only difference is
whether the sequence is present in the index, which is the difference the measurement is
intended to isolate.

**Decision point.** Ten chunks of 2000 samples each, corresponding to 0.4 s of signal at the
profile's 5 kHz sampling rate, with scaling estimated once per read from the first chunk — the
same procedure the daemon follows.

**Reported statistic.** True positive rate at the lowest threshold producing zero false
positives on the control set. A secondary figure at a 1% false-positive budget is also
reported, because a zero-false-positive threshold is determined by the single largest
off-target score and is therefore a high-variance statistic; non-monotonic behaviour in earlier
sweeps was traced to exactly this.

---

## 6.3 Three compatibility conditions, each of which silently invalidates the measurement

Comparing two systems on simulated signal requires agreements that are easy to assume and
expensive to get wrong. Each of the following was violated in a first attempt, and each
produces a smooth, plausible curve that measures something other than the intended quantity.
They are reported because they generalise to any comparison of this kind.

**6.3.1 The pore model must be identical on both sides and in the generator.** The simulator's
built-in R10 9-mer table and the table shipped with the read-until simulator disagree: for the
k-mer `AAAAAAAAA` they give 54.316 pA and 57.203 pA respectively `[VERIFY]`, a difference of
approximately 2.9 pA against a per-k-mer level standard deviation of approximately 1.2 pA.
Encoding the reference with one table while generating signal from the other therefore measures
model mismatch, and would do so monotonically in nothing at all. The model used here is
extracted from the generator's own source, and the extraction verifies that the table is in
lexicographic nucleotide order rather than assuming it, because the loader relies on line order
to establish the 2-bit packing.

**6.3.2 Both strands must be indexed.** The simulator emits reads from both strands; reference
encoding is defined on the forward sequence. Indexing only the forward strand therefore renders
approximately half of all reads unmatchable by construction, halving every true-positive rate
for a reason unrelated to the axis under study. The reverse complement is appended before
encoding.

**6.3.3 Chunk size must follow the profile's sampling rate.** A chunk is defined by time, not
by sample count: 0.4 s at this profile's 5 kHz is 2000 samples, where an earlier bench used
1800 for a 4 kHz assumption. Using the wrong constant misaligns chunk boundaries with the
decision procedure and changes the quantity of evidence available at the decision point.

The general lesson is that a sensitivity curve is only as meaningful as the invariance of
everything other than the swept parameter, and that the properties most likely to be violated
are the ones no error message reports.

---

## 6.4 Result: this system's fixed-width configuration collapses

True positive rate at the lowest zero-false-positive threshold, three seeds, mean with observed
range `[TABLE 6.1]` `[VERIFY]`:

| dwell CV | fixed-width segmentation | detected-event segmentation |
|---|---|---|
| 0.00 | **49.7** [49.2–50.2] | 18.4 [17.0–19.8] |
| 0.10 | 5.9 [5.4–6.6] | **19.3** [15.2–23.0] |
| 0.20 | 1.7 [1.2–2.0] | **17.9** [17.2–19.0] |
| 0.30 | 2.3 [1.4–4.2] | **15.5** [12.6–18.8] |
| 0.44 | 1.3 [1.0–1.6] | **14.1** [13.0–15.0] |
| 0.60 | 1.7 [0.4–3.4] | **7.5** [7.2–8.0] |

The fixed-width configuration falls by a factor of approximately 38 across the range and is
already reduced sevenfold at CV 0.10, the smallest non-zero dispersion tested. The
detected-event configuration is comparatively flat, declining from 18.4% to 7.5%, and its
event-per-read count is stable at 1507–1625 across the whole sweep `[VERIFY]`, which is the
segmentation behaving as intended: the number of events a read yields is determined by the
number of level changes it contains and not by how long each level is held.

The two configurations cross between CV 0 and CV 0.10, in all three seeds. The replicate
spread is tight where the comparison turns — 49.2–50.2 for fixed-width at CV 0 — and widest in
the fixed-width tail cells, which is expected: once the true positive rate is near 1%, the
zero-false-positive threshold is being set by one or two outlier control reads, and the 1%
false-positive column is the more stable figure there.

**The consequence for configuration selection.** An evaluation conducted only at CV 0 would
select the fixed-width configuration, which is superior by a factor of 2.7 at that operating
point and inferior by a factor of 10 at the simulator's own default dispersion. This is a
statement about selecting between configurations of *this* pipeline under a mis-specified
evaluation. It is not a statement about the field, and §6.5 is the reason why.

`[FIG 6.1]` The two curves with replicate ranges, crossover region marked, and the simulator's
default CV indicated on the axis.

---

## 6.5 Result: a published mapper does not collapse

The same sweep was run against RawHash2, given the same simulator-derived pore model, the same
reference indexed on both strands, and the same signal files. Proportion of reads mapped within
500 bases of the locus recorded in the read identifier, with matching strand
`[TABLE 6.2]` `[VERIFY]`:

| minimizer window | CV 0.00 | 0.10 | 0.20 | 0.30 | 0.44 | 0.60 | change |
|---|---|---|---|---|---|---|---|
| 0 (tool default) | 95.0 | 95.2 | 94.6 | 94.2 | 92.4 | 92.0 | −3.0 pts |
| 5 | 90.0 | 88.8 | 89.4 | 88.0 | 85.2 | 81.6 | −8.4 pts |
| 10 | 82.2 | 82.2 | 82.2 | 79.2 | 77.2 | 71.0 | −11.2 pts |

At its default configuration the comparison system declines by three percentage points across
the entire range. There is no collapse and no crossover.

**This result bounds the chapter's claim and is stated here rather than in a limitations
section.** Dwell sensitivity of the magnitude shown in §6.4 is a property of fixed-width
segmentation, not of quantised-event matching; a developer evaluating this mapper on
constant-dwell signal would reach the same conclusion as on realistic signal. The sensitivity
observed in §6.4 is consequently a defect of this implementation's segmentation choice, and
Chapter 7 identifies the quantity — per-event bucket agreement — through which that defect
propagates, and shows that it also accounts for this system's absolute accuracy deficit
independently of dwell.

A fairness note belongs with the numbers. The comparison system's default operating point is
not zero-false-positive: 8–10% of control reads are mapped at window 0 and 11–15% at window 10
`[VERIFY]`. Its 95% is therefore not level-comparable with the zero-false-positive rates of
§6.4, and the comparison drawn here is of *shape* — the response to the swept axis — not of
level. The comparison system also receives whole reads of approximately 2500 bases where §6.4
uses the first ten chunks, which favours it slightly. Neither consideration affects the
conclusion, since the conclusion is that its curve is flat.

---

## 6.6 Result: the minimizer trade-off is dwell-dependent

The three rows of §6.5 differ only in whether minimizer sketching is enabled and at what window
size. The accuracy cost of sketching, taken as the difference between window 0 and window 10 in
percentage points of correctly-mapped reads, is:

| dwell CV | 0.00 | 0.10 | 0.20 | 0.30 | 0.44 | 0.60 |
|---|---|---|---|---|---|---|
| penalty (pts) | 12.8 | 13.0 | 12.4 | 15.0 | 15.2 | **21.0** |

`[VERIFY]` `[FIG 6.2]`

The penalty grows by approximately 64% across the range. At window 10 and CV 0.60, sketching
costs 21 points of sensitivity and also raises the false-mapping rate from 8.4% to 15.2%
`[VERIFY]`, so both error types worsen together.

**Why this is the chapter's novel finding, and how modest it is.** The tool's own documentation
states that enabling minimizers "may reduce accuracy but improves the performance and memory
space efficiency", and it ships with sketching disabled. That a trade-off exists is therefore
known and declared. What is not documented, and what no evaluation measures, is that the cost
is a function of time-domain noise: it is approximately 13 points under constant dwell and 21
points at realistic dispersion. A practitioner choosing a window size from published figures —
all of which are obtained at one unmeasured level of dwell variability — will therefore
underestimate the cost on real signal.

**Mechanism.** A minimizer is selected by rank within a window. A displaced event boundary
changes the buckets of the keys overlapping it, and therefore can change which key attains the
minimum rank within its window, so reference and query may select *different* seeds even where
both individual keys would have matched. The failure mode requires boundaries to move and so
cannot occur under constant dwell, which is why a constant-dwell evaluation cannot reveal it.
This mechanism is consistent with the observed monotone growth of the penalty but was not
isolated experimentally — doing so would require instrumenting which seeds each side selects,
and that is not done here.

An independent and much larger version of the same effect appears in this system, where
sketching at window 10 under detected segmentation reduced the true positive rate to
near zero against 18.2% at window 1 `[VERIFY]`. That magnitude is specific to a pipeline whose
per-event agreement is low (Chapter 7) and should not be generalised.

---

## 6.7 Two methodological results on segmentation calibration

The detected-event configuration of §6.4 required calibration, and two properties of that
process are reportable results rather than implementation detail.

**6.7.1 The calibration target is not the obvious one.** A detector is naturally tuned so that
the number of events it emits matches the number of bases the read contains, on the reasoning
that one event per k-mer is the intended output. That target is wrong. At CV 0.44, a threshold
producing 2017 events per read against an ideal of approximately 2000 yields a true positive
rate of 2.0%, whereas a threshold producing 1562 events — deliberately under-segmenting by
about 22% — yields 14.2% `[VERIFY]`. A sevenfold improvement is obtained by emitting
materially fewer events than the nominally correct number.

The asymmetry has a mechanism. A key is a window of `E` consecutive events. A spurious extra
event displaces the frame for every subsequent key in the read, whereas a missed boundary merges
two events and leaves the remainder of the frame intact. Missing boundaries is therefore cheap
and inventing them is expensive, and a detector should be biased toward the former. This is a
general property of frame-based keying and not specific to this implementation.

**6.7.2 A detector calibration does not transfer between signal generators.** The calibration
derived against this project's own generator, applied unchanged to the external simulator,
produced a true positive rate of 0.6% at CV 0 where the simulator-derived calibration produces
18.4% — a factor of approximately 30 `[VERIFY]`. The two generators differ in their amplitude
noise model, and the detector's threshold is a t-statistic over sample windows, so its operating
point depends on that model.

The consequence for reporting is direct: a detection-based result is not interpretable without
a statement of how the detector was calibrated and on what signal. This is not a minor
methodological nicety. The first version of the sweep in §6.4 showed the detected configuration
failing at every dwell level including zero, a result that would have supported the opposite
conclusion to the one reported, and the cause was calibration transfer rather than any property
of detection.

---

## 6.8 Result: the standard read-until simulator cannot exercise the axis

The simulator used for live end-to-end testing of read-until clients generates signal by
emitting a fixed number of samples for every k-mer. Its own source comments note that the
quantity could be derived from a dwell-time distribution, and does not do so. Its output is
therefore the CV = 0 case exactly, and no configuration of it produces time-domain noise.

Measured consequence, with the same daemon, reference and duration, differing only in whether
the simulator models dwell `[TABLE 6.3]` `[VERIFY]`:

| simulator | fixed-width accept | detected accept |
|---|---|---|
| stock (constant dwell) | **43.5%** | 9.6% |
| patched, CV 0.35 | 0.5% | 9.6% |

Two observations. The detected configuration is unchanged between the two rows at 9.6%, which
is dwell invariance demonstrated end to end rather than offline. And **the stock simulator
reports the configuration that is worse on realistic signal as 4.5 times better.** A client
developed and tuned against it would select fixed-width segmentation, and every measurement
available to the developer would endorse that choice.

A patch adding a lognormal dwell distribution, selectable by environment variable and reducing
to the original behaviour when unset, is supplied as an artefact
(`third_party_patches/icarust-dwell-variability.patch`). It is offered as a tooling
contribution rather than a result.

**The scope of the claim.** This is a property of one simulator, not of simulation. The
signal simulator used in §6.2 models dwell variability as a first-class tunable parameter and
defaults to a dispersion within the range swept here. The gap identified is narrower and
specific: the only simulator capable of driving a live read-until client over the instrument's
own API has no dwell model, so the one configuration in which a client's end-to-end behaviour
can be tested is also the one configuration in which this failure mode is invisible.

---

## 6.9 Relation to the geometry parameters

The key geometry was frozen three times in the course of this work and the first two freezes
were invalidated, which is relevant here because both invalidations were caused by the axis
this chapter introduces.

The first rested on an analytical model of key-space occupancy whose fit did not converge, and
was withdrawn on those grounds. The second selected a 4-bit, 13-event geometry on the basis of
live measurements taken while the diagonal was computed chunk-relative (Chapter 4 §4.5); with
that defect corrected the ordering of the candidate geometries reversed. The third, conducted
on the external simulator with dwell swept and with segmentation and sketching varied
simultaneously, selected 4 bits × 12 events with sketching disabled.

That a parameter required three freezes is not merely a project history. Chapter 7 §7.3.3 shows
that the feasible region for the geometry has a width determined by per-event bucket agreement,
and that at the measured agreement of 0.56 the region is narrow enough that the ordering of
adjacent geometries is governed by second-order effects. The instability of the freeze is
therefore a symptom of the ceiling rather than an inconsistency between measurements, and the
practical recommendation is the one Chapter 7 §7.7 gives: establish agreement before tuning
geometry, because below a threshold agreement no geometry is a good choice.

---

## 6.10 Limitations

**One reference, one organism.** All figures derive from a 20 Mb prefix of one human
chromosome. Reference scale enters the specificity constraint of Chapter 7 directly, so the
absolute rates are scale-dependent; the shape of the response to dwell is the claim, and it is
untested on a second reference.

**Simulated signal throughout.** No real reads were used. The simulator's dwell model is
lognormal and its amplitude noise derives from per-k-mer model dispersion; real dwell
distributions and real noise are not identical to either. The axis is therefore swept in a
parameter of a model, and the mapping from that parameter to any particular real dataset is not
established here. Measuring the empirical dwell dispersion of a public dataset and marking it on
the axis would materially strengthen the chapter and is not done.

**The minimizer mechanism is not isolated.** §6.6 gives a mechanism consistent with the
observed monotone penalty but does not demonstrate it; that requires instrumenting seed
selection on both sides.

**Agreement was not measured for the comparison system.** As Chapter 7 §7.9 states, the single
most valuable experiment left undone is measuring per-event agreement for the published mapper
on this same input, which would convert the account of §6.5 from an observation that it is
robust into an explanation of why.

---

## 6.11 Summary

Time-domain noise is a signal property that the accepted simulator exposes as a tunable
parameter with a default dispersion of approximately CV 0.44, and that no published evaluation
of a raw-signal mapper varies. Sweeping it over CV 0 to 0.60 on an external simulator, with a
composition-matched control and three replicates, produces three results.

This system's fixed-width segmentation falls from 49.7% to 1.3% true positive rate, crossing
below its own detected-event configuration between CV 0 and CV 0.10; an evaluation at CV 0
would therefore select the configuration that is tenfold worse at the simulator's default
dispersion. A published mapper, by contrast, declines by three percentage points across the
same range, so the sensitivity is a property of fixed-width segmentation and not of the method
class, and the chapter claims no more than that. The accuracy cost of minimizer sketching in
that published mapper is, however, dwell-dependent — approximately 13 percentage points under
constant dwell and 21 at CV 0.60, with the false-mapping rate rising in parallel — which is
undocumented and is this chapter's novel contribution, modest in size and directly actionable
in choosing a window size.

Separately, the simulator used to drive live read-until clients emits a fixed sample count per
k-mer and so cannot exercise the axis at all: against it, the configuration that is worse on
realistic signal measures 4.5 times better. Two calibration results are reported alongside: a
detector should be biased toward under-segmentation, because a spurious event displaces the key
frame for the remainder of a read while a missed boundary does not; and a detector calibration
does not transfer between signal generators, to the extent of a thirtyfold difference in
outcome, so a detection result is not interpretable without its calibration provenance.
