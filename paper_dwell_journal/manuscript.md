# Time-domain noise as an evaluation axis for raw-signal nanopore mapping

Khushal Singh\*, Ritu Garg

Department of Computer Engineering, National Institute of Technology Kurukshetra, Kurukshetra,
Haryana 136119, India

\*To whom correspondence should be addressed: khushal6002@gmail.com

Article type: Original Paper — Sequence Analysis

---

## Abstract

**Motivation:** Raw-signal nanopore mappers segment the current trace into events before matching,
so their accuracy depends on how event boundaries respond to variation in the time the motor protein
holds each k-mer. Mappers are evaluated on real datasets, in which this dwell-time dispersion is
fixed by the run, so their sensitivity to it is not measured. Measuring it requires a variable that
can be moved, which a signal simulator provides.

**Results:** We sweep the dwell-time coefficient of variation (CV) from 0 to 0.60 in Squigulator,
with a composition-matched control and three seeds. In one quantised-event pipeline, fixed-width
segmentation leads event detection at CV 0, 49.7% against 18.4% true-positive rate at zero false
positives, and trails it 10.8-fold at the simulator's default CV of 0.44, 1.3% against 14.1%; the
replicate ranges do not overlap at any dispersion tested. Evaluation at constant dwell therefore
selects the worse configuration. RawHash2 loses 2.9 points at its default setting, so the reversal
belongs to the segmentation choice, not to signal-space mapping. RawHash2's minimizer sketching does
carry a dwell-dependent cost: paired within seed, the accuracy penalty of window 10 against window 0
rises from 11.7 points at CV 0 to 19.5 at CV 0.60, and is larger at CV 0.60 in every seed.

<!-- The archive DOI is written as a URL on purpose: scripts/verify_dwell_journal_numbers.py
     strips URLs, but would read a bare "doi:10.5281/..." as an unregistered numeral. -->
**Availability and implementation:** Code, result files, a patch adding dwell variation to the
Icarust simulator, and a script that recomputes every number in this article:
https://github.com/khushal6001-maker/microsecond-readuntil, archived at
https://doi.org/10.5281/zenodo.23604395 (release v1.0.0).

**Supplementary information:** Supplementary data are available at *Bioinformatics* online.

**Contact:** khushal6002@gmail.com

---

## 1 Introduction

Signal-space mappers align a nanopore current trace to a reference without basecalling. UNCALLED
(Kovaka et al., 2021) matches events against an FM-index of the reference; Sigmap (Zhang et al., 2021)
indexes event-value seeds in a k-d tree and chains them; RawHash (Firtina et al., 2023) and
RawHash2 (Firtina et al., 2024) hash quantised event k-mers and chain the resulting anchors; and
Sigmoni (Shivakumar et al., 2024) classifies reads with an r-index over a small current alphabet.
Their principal use is where a decision must be made while the molecule is still in the pore, as
in adaptive sampling (Kovaka et al., 2021; Payne et al., 2021).

Every pipeline in this family begins by dividing the trace into events, each intended to
correspond to one k-mer. The order of stages matters. A key is built from consecutive events, so
a query boundary that does not correspond to one implied by the reference changes the key, and no
later stage can restore it (Figure 1A, B). Event duration is set by the motor protein, whose stepping is
stochastic: the number of samples per k-mer varies from one k-mer to the next, and its
distribution is right-skewed. On real signal the variation is large enough that signal aligners
model it explicitly: Uncalled4 attributes frequent stays, consecutive events that represent the
same k-mer, to variable sequencing speed (Kovaka et al., 2025), and SegPore builds the motor
protein's dynamics into segmentation itself (Cheng et al., 2025). The accuracy of a
segmentation-based mapper is therefore a function of dwell-time dispersion. What is not known, for any published pipeline or parameter choice, is
how strong that dependence is.

![Figure 1](figures/fig1-dwell-schematic.png)

**Figure 1.** How dwell dispersion reaches the keys, drawn as a schematic: the current levels,
durations and key ranks are illustrative, not data. (A) At constant dwell every fixed-width window
covers one k-mer, so its mean sits on the current level, as the detected events do. (B) With
durations drawn lognormal at CV 0.44, fixed-width windows straddle level changes (shaded) and their
means fall between levels, while detected events still follow the level changes. (C) The candidate
mechanism of Section 3.3, simplified to one minimizer window of w = 4 keys: a displaced boundary
changes key K2 in the query, so the query keeps K4 as its minimizer; K4 is identical on both sides
but was not the reference's choice, and the two share no seed.

Evaluation practice does not measure it. Mappers are evaluated chiefly on real datasets, and RawBench
(Eris et al., 2025), which decomposes raw-signal analysis into reference encoding, signal encoding
and representation matching and evaluates combinations of them, does so on real datasets as well.
In a real dataset dispersion is a property of the run. The experimenter neither sets it nor, in
general, reports it, and two datasets differ in more than dispersion. Each published accuracy
figure is therefore one point at an unknown position on an axis whose slope has not been measured.
CRANE (Ambrozak et al., 2026) corrects errors in raw signal before analysis and reports gains for
existing raw-signal tools, which shows directly that signal-level error propagates to mapping
accuracy; it does not isolate time-domain noise as a variable.

**Why simulation is the instrument.** A sensitivity is a derivative, and measuring one requires
moving the variable while everything else is held. A fixed biological dataset supplies a single
value of dwell dispersion, which cannot be changed without also changing the sample, the pore, the
chemistry or the run. Squigulator (Gamaarachchi et al., 2024) exposes dispersion directly, as the
standard deviation of the number of samples per base, with a documented default of mean 9.0 and
standard deviation 4.0 samples, a CV of approximately 0.44. For this question simulation is not a
substitute for real data but the only instrument with the required control. What simulation cannot
establish, the correspondence between a simulator setting and the dispersion of a particular device
or run, is stated as a limitation in Section 5 rather than assumed.

The question has a practical edge. Icarust (Munro et al., 2024), the simulator that serves the
MinKNOW read-until interface and so allows a client to be tested end to end, emits a fixed number
of samples for every k-mer, which is the CV = 0 case exactly. The one setting in which a read-until
client can be exercised end to end is therefore the one setting in which a dwell-sensitive failure
cannot appear.

This article makes five contributions.

1. A protocol for sweeping dwell dispersion as an evaluation variable, with a composition-matched
   negative control simulated at matching dwell, three generator seeds, and three compatibility
   conditions, each of which, when violated, produces a plausible curve that measures something
   else (Section 2).
2. A ranking reversal: two segmentation configurations of one pipeline exchange order between
   CV 0 and CV 0.10, so that an evaluation at constant dwell selects the configuration that is
   10.8-fold worse at the simulator's default (Section 3.1).
3. A bounding result: RawHash2 does not show the reversal, which locates it in the segmentation
   choice rather than in the method class (Section 3.2).
4. A dwell-dependent accuracy cost of minimizer sketching in RawHash2, which we have not found
   measured elsewhere (Section 3.3).
5. A patch that adds lognormal dwell variation to Icarust, so that live read-until testing can
   exercise the axis (Section 3.4).

**What this article does not claim.** It does not claim that published mappers segment by fixed
time windows; none of those surveyed does. The fixed-width arm is a configuration of the authors'
own pipeline, included because it is the configuration a constant-dwell evaluation favours. It does
not claim that signal simulators omit dwell variation: Squigulator models it, and the gap identified
here is specific to Icarust. It does not claim that the authors' pipeline is competitive. At CV 0.44
its better configuration resolves 14.1% of reads at zero false positives, where RawHash2 maps 91.8%
correctly at its default operating point and 80.5% at zero false positives (Section 3.2), and the
comparisons drawn below concern the shape of a system's response to dispersion, not its level.

## 2 Methods

### 2.1 Signal generation

Reads were simulated with Squigulator under its R10.4 PromethION DNA profile (dna-r10-prom), which
samples at 5 kHz. The dwell mean was fixed at 10.0 samples per base and the dwell standard deviation
swept over {0, 1, 2, 3, 4.4, 6} samples, giving CV ∈ {0.00, 0.10, 0.20, 0.30, 0.44, 0.60}. The mean
is held fixed so that only dispersion varies; a varying mean would also change the number of bases
a chunk of fixed duration contains, and so confound dispersion with the quantity of evidence at the
decision point. Each cell was simulated with three generator seeds (42, 43 and 44) and contains 500
on-target reads and 500 control reads.

### 2.2 Reference, pore model and control

The reference is a 20 Mb prefix of human chromosome 20 (GRCh38), indexed together with its reverse
complement, 40 Mb of encoded sequence in all. The pore model is Squigulator's own R10.4 9-mer table,
extracted from its source, and is used both to generate signal and to encode the reference in both
systems under test. The negative control is a composition-matched shuffle of the same reference,
simulated at the same dwell setting as the on-target set in each cell. Composition and dwell are
therefore identical between the two sets, and the only difference between them is whether the
sequence is present in the index.

### 2.3 Three compatibility conditions

Each of the following was violated in a first attempt during this work, and each violation produced
a smooth, plausible curve. They are stated because they apply to any comparison of this kind.

*The pore model must be the same in the generator and in both encoders.* The R10 9-mer table
shipped with Icarust and the table built into Squigulator differ, so encoding the reference with
one while generating signal from the other measures model mismatch rather than dispersion.

*Both strands must be indexed.* Squigulator samples reads from both strands, while reference
encoding is defined on the forward sequence. Indexing one strand makes about half of all reads
unmappable by construction and halves every true-positive rate for a reason unrelated to the axis.

*Chunk length must be defined in time.* The decision procedure consumes chunks of fixed duration.
At this profile's 5 kHz, 0.4 s is 2000 samples; a sample count carried over from a profile with a
different sampling rate misaligns the decision point and changes the evidence available at it.

A sensitivity curve is meaningful only to the extent that everything other than the swept variable
is invariant, and these are invariances that no error message reports.

### 2.4 Configurations under test

Both arms are configurations of one quantised-event read-until pipeline written by the authors, and
both are available in the repository. The *fixed-width* arm divides the trace into windows of a
constant number of samples and builds keys of 14 events at 3 bits per event, sketched with minimizer
window 10. The *detected* arm segments with a two-window t-test changepoint detector (window 4
samples, threshold 3.0, minimum event length 4 samples) and builds keys of 12 events at 4 bits per
event, with minimizer window 1. Both decide after 10 chunks of 2000 samples, 0.4 s each, with signal
scaling estimated from the first chunk, as the pipeline does in live operation. The detector
threshold was calibrated on Squigulator signal (Supplementary Section S2).

The reported statistic is the true-positive rate (TPR) at the lowest score threshold that admits no
control read, that is, at zero false positives. This threshold is set by the single highest-scoring
control read, so it has high variance when the TPR is low; for that reason we report the range over
seeds alongside the mean. Table 1 sets both arms beside the comparison mapper of Section 2.5.

**Table 1.** The three systems compared. Both arms are configurations of the authors' pipeline;
RawHash2 v2.1 runs at its own defaults except for the minimizer window, w = 0 being its default.
Chunks are 2000 samples, 0.4 s at 5 kHz. The detector uses windows of 4 samples and a minimum event
length of 4 samples; RawHash2 merges events whose levels differ by less than its `--sig-diff`
threshold of 0.35.

| System | Segmentation | Key (bits × events) | Minimizer window w | Reported statistic | Signal used |
|---|---|---|---|---|---|
| Fixed-width arm | constant-width windows | 3 × 14 | 10 | TPR at zero false positives | first 10 chunks |
| Detected arm | two-window t-test, threshold 3.0 | 4 × 12 | 1 | TPR at zero false positives | first 10 chunks |
| RawHash2 | its own detection, with event merging | 4 × 8 | 0, 5 and 10 | correct mapping at its default operating point | whole read |

### 2.5 Comparison mapper

RawHash2 v2.1 was run on the same SLOW5 files, given the same pore model through `-p`, `-k 9` and
`--level_column 1`, and otherwise run at its defaults (`-q 4 -e 8 --sig-diff 0.35`) except for the
minimizer window, w ∈ {0, 5, 10}; w = 0, sketching off, is the default. A read is counted as correctly mapped if it
maps within 500 bases of the locus Squigulator records in the read identifier, on the matching
strand, and the false-mapping rate is the percentage of control reads that receive any mapping.

Two differences from Section 2.4 limit what may be compared. RawHash2's default operating point
admits false mappings, so its correct-mapping rate in Table 3 is not level-comparable with a
zero-false-positive TPR, and the comparison there is of shape; Section 3.2 also places RawHash2
at matched operating points. RawHash2 also receives whole reads rather than the first 10 chunks,
which favours it. The dwell × window grid was run for the same three seeds as Table 2, with 500 reads
per set, by `scripts/rerun_rawhash_grid.sh`; seed 42 reproduces an earlier single-seed run of the
grid exactly, in every cell. At its default and at CV 0.44, RawHash2 maps 91.8% [91.2–92.4] of reads
correctly and 9.1% [8.4–10.6] of control reads.

### 2.6 Per-event agreement

To test whether a flat accuracy curve reflects a flat encoding, we measured agreement between
reference and query symbol sequences for both systems with one estimator. Each system's quantised
symbols were dumped for the read and for the reference span at its true locus. For RawHash2 the
read side comes from its own `--out-quantize` option; the reference side comes from
`--out-quantize-ref`, which a patch supplied in the repository adds, since the stock reference path
emits no symbols. The two sequences were aligned, agreement was taken as matches over aligned pairs under the maximising
alignment, and the indel fraction as gap columns over all alignment columns. Both of this pipeline's
segmentations were measured with keys of 12 events at 4 bits per event, so the fixed-width row
isolates the segmentation change from the 3-bit key of the fixed-width arm in Table 2, and scaling
was estimated from the whole read. The measurement covers 200 reads per cell, at CV 0 and CV 0.44
only, with one seed.

### 2.7 Reproducibility

Every number in this article is recomputed from the committed result files by
`scripts/verify_dwell_journal_numbers.py`, which uses decimal arithmetic with round-half-up, checks
every numeral in this text, in its LaTeX source and in the Supplementary Information against the
recomputed set, and exits non-zero
on any disagreement. Three observations made during development, on live accept rates against
Icarust, on the calibration target of the event detector and on the transfer of a detector
calibration between signal generators, rest in part on runs whose output was not captured; they are
reported, marked, in the Supplementary Information and are used in no claim here.
Figures are drawn by `scripts/generate_thesis_figures.py` from the same files and from nothing else.

## 3 Results

### 3.1 Constant dwell reverses the ranking of two segmentation configurations

Table 2 and Figure 2 give the TPR of both arms across the sweep. The fixed-width arm falls from 49.7%
at CV 0 to 1.3% at CV 0.44, a 38.2-fold reduction, and is already reduced 8.4-fold at CV 0.10, the
smallest non-zero dispersion tested. The detected arm remains between 14.1% and 19.3% up to CV 0.44
and declines to 7.5% at CV 0.60.

**Table 2.** True-positive rate (%) at zero false positives, mean [range] over three seeds, 500
on-target and 500 control reads per cell.

| Dwell CV | Fixed-width (3 × 14, w = 10) | Detected (4 × 12, w = 1) |
|---|---|---|
| 0.00 | 49.7 [49.2–50.2] | 18.4 [17.0–19.8] |
| 0.10 | 5.9 [5.4–6.6] | 19.3 [15.2–23.0] |
| 0.20 | 1.7 [1.2–2.0] | 17.9 [17.2–19.0] |
| 0.30 | 2.3 [1.4–4.2] | 15.5 [12.6–18.8] |
| 0.44 | 1.3 [1.0–1.6] | 14.1 [13.0–15.0] |
| 0.60 | 1.7 [0.4–3.4] | 7.5 [7.2–8.0] |

![Figure 2](figures/fig6-1-dwell-crossover.png)

**Figure 2.** True-positive rate at zero false positives against dwell-time CV for the two
segmentation configurations. Bars span the observed range over three seeds. The shaded band marks
the interval in which the two curves cross; the dotted line marks Squigulator's default dispersion.

The two arms exchange order between CV 0 and CV 0.10. The replicate ranges do not overlap at any
dispersion tested: at CV 0 the lowest fixed-width replicate, 49.2, exceeds the highest detected
replicate, 19.8, and at every non-zero dispersion the highest fixed-width replicate falls below the
lowest detected one. The order therefore holds in every seed and not only in the means.

The consequence is for configuration selection. An evaluation conducted at CV 0 selects the
fixed-width arm, which leads by 2.7-fold there and trails by 10.8-fold at CV 0.44. This is not an
error of the kind that more data would correct, because every measurement available at CV 0
supports it. The fixed-width arm is not a published method, and the finding concerns the choice
between two configurations of one pipeline under a mis-specified evaluation. Section 3.2 shows that
it does not extend to the method class.

### 3.2 A published mapper does not reverse

At its default, w = 0, RawHash2 loses 2.9 points [2.8–3.0] of correct mapping between CV 0 and
CV 0.60, with no crossover and no collapse (Table 3, Figure 3). The sensitivity found in Section 3.1
is therefore a property of fixed-width segmentation and not of quantised-event matching, and a
developer evaluating RawHash2 at constant dwell would reach the same conclusion as one evaluating it
at realistic dispersion. With sketching enabled the decline is steeper, 7.0 points [5.2–8.4] at
w = 5 and 10.7 [9.4–11.6] at w = 10, which is the subject of Section 3.3.

**Table 3.** RawHash2 across the dwell sweep: percentage of on-target reads correctly mapped and
percentage of control reads mapped at all, by minimizer window w. Mean over three seeds, 500 reads
per set; Figure 3 shows the range across seeds, and `results/rawhash-grid-seeds.csv` every seed.

| Rate | w | 0.00 | 0.10 | 0.20 | 0.30 | 0.44 | 0.60 |
|---|---|---|---|---|---|---|---|
| Correct | 0 | 93.5 | 93.8 | 93.3 | 92.7 | 91.8 | 90.5 |
| Correct | 5 | 88.8 | 88.4 | 88.7 | 87.4 | 85.4 | 81.8 |
| Correct | 10 | 81.8 | 82.1 | 82.1 | 80.5 | 76.4 | 71.1 |
| False-mapped | 0 | 10.1 | 11.1 | 8.1 | 10.4 | 9.1 | 10.5 |
| False-mapped | 5 | 9.8 | 10.7 | 8.5 | 9.5 | 9.9 | 10.7 |
| False-mapped | 10 | 14.4 | 12.9 | 13.9 | 13.7 | 14.3 | 15.8 |

![Figure 3](figures/figj-2-rawhash-dwell-3seeds.png)

**Figure 3.** RawHash2 under the dwell sweep. Solid, dashed and dash-dotted lines: percentage of
on-target reads correctly mapped, by minimizer window. Dotted lines: percentage of control reads
mapped. Points are means over three seeds and bars span the observed range. The default operating
point is not zero-false-positive, so these rates are not level-comparable with Figure 2.

The response to dispersion is visible in the encoding before any seeding or chaining (Table 4).
RawHash2's per-event agreement is 0.913 at CV 0 and 0.911 at CV 0.44, and that of the detected arm
0.797 and 0.783. The fixed-width segmentation's agreement falls from 0.793 to 0.648, and its indel
fraction rises 2.8-fold, from 0.073 to 0.208. RawHash2's indel fraction also rises, from 0.055 to
0.092, without a corresponding loss of agreement. Both segmenters that detect level changes thus
have dispersion-invariant agreement, and the one that segments by time does not. These are two
dwell levels, 200 reads and one seed (Section 5).

**Table 4.** Per-event agreement and indel fraction between reference and query symbol sequences,
one estimator applied to both systems, 200 reads per cell, one seed.

| System | Segmentation | Dwell CV | Agreement | Indel fraction |
|---|---|---|---|---|
| This pipeline | fixed-width | 0.00 | 0.793 | 0.073 |
| This pipeline | fixed-width | 0.44 | 0.648 | 0.208 |
| This pipeline | detected | 0.00 | 0.797 | 0.170 |
| This pipeline | detected | 0.44 | 0.783 | 0.196 |
| RawHash2 | own, with merging | 0.00 | 0.913 | 0.055 |
| RawHash2 | own, with merging | 0.44 | 0.911 | 0.092 |

**At matched operating points.** RawHash2 can be put on the statistic of Table 2 without new runs.
A threshold on its own mapping quality is raised until no control read is mapped, and the correct
on-target mappings at or above it are counted; `scripts/rawhash_zero_fp.py` does this from the same
output, after first reproducing every cell of Table 3 from it, and also applies a budget of 1% false
positives. At CV 0.44 and zero false positives RawHash2 resolves 80.5% [76.2–84.6] of reads, 5.7-fold
the detected arm's 14.1% [13.0–15.0]; the two ranges are far apart, so the difference in level is not
an artefact of the operating point. Nor does dispersion collapse RawHash2 at either point. At zero
false positives every replicate at every CV lies between 76.2% and 92.8%, and 4.3 points [0.4–6.6]
are lost between CV 0 and CV 0.60, although the rate is not monotone in CV, since each threshold is
set by the single highest-scoring control read. With the 1% budget, whose threshold is set by the
sixth-highest, 3.1 points [2.6–3.6] are lost, close to the 2.9 at the default (Supplementary
Section S4, Table S1).

### 3.3 The cost of minimizer sketching depends on dwell dispersion

Table 5 and Figure 4 give the accuracy cost of sketching: the correct-mapping rate at w = 0 minus
that at a larger window, in percentage points. Every window of one seed maps the same reads, so the
cost is taken within each seed and then summarised over the three. At w = 10 it is 11.7 points
[10.4–12.8] at CV 0 and 19.5 [18.2–21.0] at CV 0.60. The two ranges do not overlap, and the cost is
larger at CV 0.60 than at CV 0 in every seed, by 7.8 points on average [6.4–8.8]; Supplementary
Table S3 gives each seed. At w = 5 it goes from 4.7 [4.0–5.0] to 8.7 [7.2–10.4], again larger in
every seed. Sketching at w = 10 also maps more
control reads than w = 0 at every dispersion and in every seed (Table 3), so the sensitivity it gives
up is not returned as specificity.

**Table 5.** Accuracy cost of minimizer sketching in RawHash2: correct-mapping rate at w = 0 minus
that at the stated window, in percentage points, taken within each seed; mean [range] over three
seeds.

| Dwell CV | w = 5 | w = 10 |
|---|---|---|
| 0.00 | 4.7 [4.0–5.0] | 11.7 [10.4–12.8] |
| 0.10 | 5.4 [4.6–6.4] | 11.7 [10.6–13.0] |
| 0.20 | 4.7 [3.6–5.2] | 11.2 [10.4–12.4] |
| 0.30 | 5.3 [4.0–6.2] | 12.3 [10.2–15.0] |
| 0.44 | 6.4 [5.4–7.2] | 15.4 [14.6–16.4] |
| 0.60 | 8.7 [7.2–10.4] | 19.5 [18.2–21.0] |

![Figure 4](figures/figj-3-minimizer-penalty-3seeds.png)

**Figure 4.** Accuracy cost of minimizer sketching in RawHash2 against dwell-time CV, for windows 5
and 10 relative to window 0, taken within each seed. Thin lines are the individual seeds; thick
lines with markers are their mean. The dotted line marks Squigulator's default dispersion.

**The shape, and the limits of the claim.** Up to CV 0.30 the mean w = 10 cost stays between 11.2
and 12.3 points, and each of those cells' ranges overlaps the CV 0 range. At CV 0.44, the
simulator's default, it is 15.4 [14.6–16.4], a range already clear of the CV 0 range, and it rises
again at CV 0.60. The claim made is that the cost is larger at realistic and high dispersion than at
constant dwell, in every seed; it is not that the cost rises at every step. Three seeds give an
observed range rather than a confidence interval, which is why each claim in this section is one
that holds in every seed separately.

**At matched operating points.** The claim of this section survives a change of operating point
(Supplementary Table S2). With the 1% budget of Section 3.2 the w = 10 cost is 13.4 points
[12.8–14.6] at CV 0 and 24.5 [21.4–27.0] at CV 0.60, and the w = 5 cost 3.8 [3.4–4.2] and
9.6 [8.6–10.6]; at both windows the ranges are disjoint and the cost is larger at CV 0.60 in every
seed. At zero false positives the w = 10 cost is 15.7 points [13.4–19.6] at CV 0 and 29.5
[16.6–38.6] at CV 0.60, larger in every seed by 2.4 to 20.0 points, but the ranges overlap, and at
w = 5 one seed shows no increase. A zero-false-positive threshold rests on one control read, and the
cost at it varies more between seeds than at the default or with the 1% budget.

**What is known and what is added.** RawHash2 ships with sketching disabled to obtain its highest
accuracy, and reports that its minimizer mode maps with accuracy similar to the default except on the
human genome, at higher throughput (Firtina et al., 2024). That a trade-off exists is therefore known
and documented. What the sweep adds is that its accuracy side is a function of dispersion. A window
chosen from a comparison at one dispersion carries a different cost at another, and a comparison on
constant-dwell signal understates it.

**A candidate mechanism, not isolated.** A minimizer is the key of lowest rank within a window of
consecutive keys (Roberts et al., 2004). A displaced event boundary changes the quantised values of
the keys that overlap it and can therefore change which key in a window attains the minimum, so that
reference and query select different keys even where both would match individually (Figure 1C). This requires
boundaries to move. At constant dwell the cost reflects only the seeds that sketching discards; the
increment above it is consistent with the mechanism, which would be demonstrated by recording which
keys each side selects, and that was not done.

### 3.4 Icarust emits constant dwell, and a patch that removes the restriction

Icarust's signal loop emits `samples_per_base` samples for every k-mer, and a source comment notes
that the count could instead be derived from the profile's dwell mean. Its output is therefore the
CV = 0 case exactly, and no configuration of the stock simulator produces time-domain noise.

The patch in `third_party_patches/icarust-dwell-variability.patch` reads a CV from the environment
variable `ICARUST_DWELL_CV` and draws each k-mer's dwell from a lognormal distribution about
`samples_per_base`, with σ² = ln(1 + CV²) and μ = ln(`samples_per_base`) − σ²/2, so that the mean
before rounding equals `samples_per_base` and the CV equals the requested value. Each draw is rounded
and clamped to at least 1 sample, since a k-mer contributing nothing is a skip, which is a different
phenomenon, and to at most 8 times nominal, beyond which a hold is a pore block rather than a dwell.
Unset or 0, the variable reproduces the stock behaviour exactly, so existing results are unaffected.
An environment variable keeps the change minimal and revertible; a version offered upstream should
read the value from the sample profile.

## 4 Discussion

The sweep makes visible a variable that a point evaluation holds fixed at an unknown value, and in
the two cases measured here the variable decides the outcome. In Section 3.1 it decides which of two
configurations is better; in Section 3.3 it decides how much a documented trade-off costs. Neither
result depends on the authors' pipeline being accurate, since the first concerns the relative order
of two configurations and the second is measured on RawHash2.

We draw six recommendations for evaluating raw-signal methods.

1. **Report dispersion with accuracy.** For simulated signal, report the generator's dwell
   parameters. For real signal, report an estimate of dwell dispersion, which could be obtained from
   basecaller move tables, since these record the samples assigned to each base.
2. **Report a sweep, not a point,** for any method or parameter that depends on segmentation.
3. **Compare configurations at the dispersion of the intended data.** A comparison at CV 0 can
   reverse a ranking (Section 3.1).
4. **Treat the sketching window as dispersion-dependent,** and measure its cost at the dispersion
   of the intended data (Section 3.3).
5. **State the calibration target and calibration signal of any event detector,** and calibrate
   against agreement rather than event count (Supplementary Section S2).
6. **Test read-until clients against a simulator with a dwell model** (Section 3.4).

The sweep is complementary to existing benchmarks. RawBench varies the components of the pipeline on
fixed data; the sweep varies one property of the data with the pipeline fixed, and adding a
simulated-dispersion axis to a modular benchmark would combine the two. It is equally a way to
quantify methods that correct or model the signal before segmentation: the benefit of CRANE's
correction, or of SegPore's model of motor-protein dynamics, could be reported as a function of the
noise source it addresses rather than at one point. Any
pipeline that builds keys from consecutive events, whether by hashing quantised values or by seeding
on event vectors, is exposed to boundary placement in the way Section 3.3 describes, and the sweep is
an inexpensive way to measure that exposure.

## 5 Limitations

*One reference and one profile.* All figures derive from a 20 Mb prefix of one human chromosome under
one R10.4 profile. Reference size affects specificity and hence absolute rates; the claim is the
shape of the response to dispersion, and it has not been tested on a second reference.

*Simulated signal only.* The mapping from Squigulator's dwell parameter to the dispersion of any
particular device or run is not established here. Measuring the empirical dispersion of a public
dataset and marking it on the axis is the most direct extension.

*Replication.* Every accuracy result is over three generator seeds of 500 reads per set. Three seeds
give an observed range, not a confidence interval, so the claims made are those that hold in each
seed separately; they bound variation between simulated read sets, not between sequencing runs.

*Mechanisms.* The minimizer mechanism of Section 3.3 is consistent with the data and not isolated.
The agreement measurement of Section 2.6 covers two dwell levels, 200 reads and one seed.

*Absolute level of the detected arm.* The detected configuration's indel fraction is 0.170 already at
CV 0, where there is no timing noise to explain it, which indicates a structural mismatch between
this pipeline's reference and query encodings. It lowers that arm's absolute TPR, and it is one
reason we draw only comparisons of shape.

*Operating points.* RawHash2's main results are at its default operating point. Section 3.2 places it
at zero false positives by a threshold on its own mapping quality, a score RawHash2 computes and the
authors' pipeline does not, so its level there depends on how well that score separates control
reads, and each such threshold is set by a single control read.

*Scope of the reversal.* The fixed-width arm is a configuration of the authors' pipeline. No
published tool's evaluation is claimed to be wrong.

## 6 Conclusion

Dwell-time dispersion is a signal property that Squigulator exposes as a parameter and that published
evaluations of raw-signal mappers do not vary. Sweeping it, with a composition-matched control and
three replicates, shows that an evaluation at constant dwell can select a segmentation configuration
that is 10.8-fold worse at realistic dispersion, and that the accuracy cost of minimizer sketching in
RawHash2 rises from 11.7 to 19.5 points over the range swept, in every seed. The first result is bounded by
RawHash2's flat response and the second is measured on it. Both follow from holding fixed a variable
that evaluation can move, and both are avoided by reporting accuracy as a function of it.

## Funding

[Funding statement to be completed by the authors before submission.]

## Conflict of interest

None declared.

## Data availability

All result files, raw logs, the sweep harness, the figure generator, the Icarust and RawHash2
patches and the verification script are in the repository given under Availability and
implementation, archived at https://doi.org/10.5281/zenodo.23604395 (release v1.0.0).

## References

Ambrozak, S., McConnell, U., Srinivasan, B., Ozkan, B., Zhang, E. and Firtina, C. (2026) CRANE:
correcting errors in raw nanopore signals using hidden Markov models. arXiv:2603.20420.

Cheng, G., Vehtari, A. and Cheng, L. (2025) Raw signal segmentation for estimating RNA modification
from Nanopore direct RNA sequencing data. *eLife*, **14**, RP104618. doi:10.7554/eLife.104618.

Eris, F., McConnell, U., Firtina, C. and Mutlu, O. (2025) RawBench: a comprehensive benchmarking
framework for raw nanopore signal analysis techniques. In *Proceedings of the ACM Conference on
Bioinformatics, Computational Biology, and Health Informatics (ACM-BCB)*. arXiv:2510.03629.

Firtina, C., Mansouri Ghiasi, N., Lindegger, J., Singh, G., Cavlak, M.B., Mao, H. and Mutlu, O.
(2023) RawHash: enabling fast and accurate real-time analysis of raw nanopore signals for large
genomes. *Bioinformatics*, **39**(Suppl. 1), i297–i307. doi:10.1093/bioinformatics/btad272.

Firtina, C., Soysal, M., Lindegger, J. and Mutlu, O. (2024) RawHash2: mapping raw nanopore signals
using hash-based seeding and adaptive quantization. *Bioinformatics*, **40**(8), btae478.
doi:10.1093/bioinformatics/btae478.

Gamaarachchi, H., Ferguson, J.M., Samarakoon, H., Liyanage, K. and Deveson, I.W. (2024) Simulation of
nanopore sequencing signal data with tunable parameters. *Genome Research*, **34**(5), 778–783.
doi:10.1101/gr.278730.123.

Kovaka, S., Fan, Y., Ni, B., Timp, W. and Schatz, M.C. (2021) Targeted nanopore sequencing by
real-time mapping of raw electrical signal with UNCALLED. *Nature Biotechnology*, **39**(4), 431–441.
doi:10.1038/s41587-020-0731-9.

Kovaka, S., Hook, P.W., Jenike, K.M., Shivakumar, V., Morina, L.B., Razaghi, R., Timp, W. and
Schatz, M.C. (2025) Uncalled4 improves nanopore DNA and RNA modification detection via fast and
accurate signal alignment. *Nature Methods*, **22**(4), 681–691. doi:10.1038/s41592-025-02631-4.

Munro, R., Wibowo, S., Payne, A. and Loose, M. (2024) Icarust, a real-time simulator for Oxford
Nanopore adaptive sampling. *Bioinformatics*, **40**(4), btae141. doi:10.1093/bioinformatics/btae141.

Payne, A., Holmes, N., Clarke, T., Munro, R., Debebe, B.J. and Loose, M. (2021) Readfish enables
targeted nanopore sequencing of gigabase-sized genomes. *Nature Biotechnology*, **39**(4), 442–450.
doi:10.1038/s41587-020-00746-x.

Roberts, M., Hayes, W., Hunt, B.R., Mount, S.M. and Yorke, J.A. (2004) Reducing storage requirements
for biological sequence comparison. *Bioinformatics*, **20**(18), 3363–3369.
doi:10.1093/bioinformatics/bth408.

Shivakumar, V.S., Ahmed, O.Y., Kovaka, S., Zakeri, M. and Langmead, B. (2024) Sigmoni: classification
of nanopore signal with a compressed pangenome index. *Bioinformatics*, **40**(Suppl. 1), i287–i296.
doi:10.1093/bioinformatics/btae213.

Zhang, H., Li, H., Jain, C., Cheng, H., Au, K.F., Li, H. and Aluru, S. (2021) Real-time mapping of
nanopore raw signals. *Bioinformatics*, **37**(Suppl. 1), i477–i483. doi:10.1093/bioinformatics/btab264.
