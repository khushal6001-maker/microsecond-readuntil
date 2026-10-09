# Paper direction, with the literature actually checked

Written 2026-10-05, after verifying prior art rather than assuming it. Two earlier framings
in this repo were wrong and are corrected here.

## What is already taken

**The matching method is not ours.** RawHash (Bioinformatics 2023) and RawHash2 (2024) do
what `src/index/` does: quantise event values, concatenate consecutive quantised events,
hash them, match through a table, with minimizers and chaining in RawHash2. Any claim to
novelty in quantised-event hashing is unsupportable.

**Event detection is not ours either.** UNCALLED, Sigmap, RawHash and RawHash2 all segment
by detected events. The t-test detector in `src/index/event_detect.hpp` is adopted standard
technique.

**"Signal simulators omit dwell variability" is FALSE and must not be claimed.**
Squigulator exposes `--dwell-std`, which sets the standard deviation of the number of
signal samples per k-mer. Time-domain noise is a known, modelled, tunable quantity in the
accepted community simulator. What is true is far narrower: *Icarust*, the only read-until
simulator, emits a fixed sample count per k-mer, so the live read-until loop cannot
exercise dwell variation without the patch in `third_party_patches/`.

**A comprehensive benchmarking framework already exists.** RawBench (CMU-SAFARI, 2025) is
modular across pore models (ONT, uncalled4), segmentation (t-test, move tables), and
matching (hash-based, FM-index, r-index, DTW, vector distances), over real E. coli,
D. melanogaster, H. sapiens and Zymo datasets at R9.4.1 and R10.4.1.

## What is NOT taken, and is where our results sit

Checked against RawBench's own description of its scope, which says it benchmarks existing
methods "rather than exploring parameter sensitivity across signal acquisition
characteristics":

1. **Nobody sweeps time-domain (dwell) noise as an evaluation axis.** RawBench uses real
   data exclusively, so every number in it is at one unmeasured, fixed level of dwell
   variability. Squigulator can vary it and nobody has used that to characterise how
   raw-signal mappers degrade. Published accuracy figures therefore carry an unknown
   sensitivity to the single signal characteristic that determines whether
   segmentation-based matching works at all.

   Our measurement: fixed-width segmentation goes from 43.5% to 0.5% live accept rate when
   dwell CV moves from 0 to 0.35, an 87x fall, with no change to the engine. Offline, TPR
   at zero false positives goes 64.7% -> 1.7% between CV 0.00 and 0.30.

2. **Nobody evaluates minimizer sketching under segmentation uncertainty**, although
   RawHash2 adopted minimizers. A minimizer is chosen by RANK within a window, so a shifted
   event boundary makes reference and query select DIFFERENT keys even where both keys are
   individually correct. This cannot happen when dwell is constant, because boundaries never
   move.

   Our measurement, 64 Mb chr20, CV 0.35, detection on, TPR at zero FPR, 10-chunk reads:
   window 10 gives 3.0% and window 1 gives 18.2% at 3x14; 0.0% against 11.3% at 3x12. The
   sketch discards precisely the seeds detection exists to recover. This is the most
   interesting result in the project and the one most likely to be novel.

3. **Read-until as a soft-real-time system.** The literature reports throughput and
   accuracy, not deadline-miss rate -- the fraction of decisions arriving after the molecule
   has translocated past the point where ejecting it helps. The daemon already counts
   TOO LATE. Supporting contribution, not a headline.

## What this means for the method

Our own accuracy is weak and that is not hidden: with all four parameters re-frozen against
dwell (4x12, window 1, detection on, accept_votes 8) three replicates give 3.2-3.6%
on-target against 0.76-1.08% on a composition-matched shuffled control -- about 3.5x
enrichment at 3.4% yield. RawHash reports far better on real data. The gap is in the
matching, not the engine.

That is survivable for a sensitivity-analysis paper, where the contribution is the axis and
the finding rather than winning the benchmark, PROVIDED the sweep is run on an established
baseline too. A sensitivity curve for our engine alone shows only that our engine is
fragile. The same curve for RawHash is the result.

## The headline result, on squigulator

Measured 2026-10-05 with `bench/squig_sweep.cpp`: 20 Mb of chr20 indexed on both strands,
squigulator's own R10.4 9-mer model on both sides, 518 reads per cell, 10 chunks of 2000
samples (0.4 s at 5 kHz), negative control = composition-matched shuffle of the same
reference simulated at matching dwell. TPR at the lowest zero-false-positive threshold:

| dwell CV | fixed-width (3x14, w=10) | event-detected (4x12, w=1) | ratio |
|---|---|---|---|
| 0.00 | **49.2%** | 18.4% | 0.37x |
| 0.10 | 6.6% | **23.0%** | 3.5x |
| 0.20 | 1.8% | **19.0%** | 10.6x |
| 0.30 | 1.4% | **18.8%** | 13.4x |
| 0.44 (squigulator default) | 1.4% | **14.2%** | 10.1x |
| 0.60 | 1.2% | **8.0%** | 6.7x |

Fixed-width falls 41x across the range. Detection is nearly flat, and its events-per-read
stays at 1507-1625 regardless of CV, which is the segmentation doing its job.

**The finding is the crossover, not either curve.** Fixed-width wins only at CV 0 -- the
one condition no pore satisfies, and exactly the condition Icarust ships. Evaluating at
CV 0 does not merely inflate a number, it INVERTS THE RANKING of two methods. A developer
who tuned against stock Icarust would ship the configuration that is 10x worse at the
accepted simulator's own default dwell, and every measurement they took would agree with
them.

That is the claim the paper can make, it is measured on a community-standard instrument
rather than on our own generator, and it does not depend on our engine being good.

## Second-order finding: calibration target

Tuning the detector by matching events-per-read to the ideal of one per base is wrong. At
CV 0.44: threshold 2.0 gives 2017 events/read (ideal 2000) and 2.0% TPR, while threshold
3.0 gives 1562 and 14.2%. Deliberately under-segmenting by ~20% is seven times better.

Mechanistic, not empirical luck: a key packs a fixed number of CONSECUTIVE events, so a
spurious event shifts every later key in the read, whereas a missed boundary merges two
events and leaves the rest of the frame intact. Missing boundaries is cheap, inventing
them is not. Any paper reporting detection results needs to state how the detector was
calibrated and on what data, because our generator-derived calibration scored 0.6% where
the squigulator-derived one scores 18.4%.

## Required to make this publishable

1. **Use Squigulator, not our synthetic generator.** `bench/` generates its own signal,
   which a reviewer will not accept as a basis for a claim about evaluation methodology.
   Squigulator with `--dwell-std` swept is the credible instrument, and it is what makes the
   work reproducible by others.
2. **Run the sweep on RawHash as well as on this engine.** Without it there is no finding,
   only a limitation of our code. RawHash is open source and RawBench provides harnesses.
3. **Keep the Icarust patch** for the live read-until loop, which Squigulator cannot
   provide. It is an artifact contribution, not a paper.
4. **Re-verify finding 2 against RawHash2's own evaluation** before claiming it is novel.
   RawBench does not test it; confirm RawHash2 itself does not either.

## Framings that must not be used

- "We present a novel signal-space matching method." Taken by RawHash.
- "Existing simulators omit dwell variability." False; Squigulator models it.
- "No benchmarking framework exists for raw signal analysis." False; RawBench.
- "Our engine is more accurate than existing tools." It is not.

## Replicated, three independent squigulator seeds

Seeds 42/43/44, 500 reads per cell, otherwise identical to the table above. TPR at the
lowest zero-false-positive threshold, mean [range]:

| dwell CV | fixed-width | event-detected |
|---|---|---|
| 0.00 | **49.7** [49.2-50.2] | 18.4 [17.0-19.8] |
| 0.10 | 5.9 [5.4-6.6] | **19.3** [15.2-23.0] |
| 0.20 | 1.7 [1.2-2.0] | **17.9** [17.2-19.0] |
| 0.30 | 2.3 [1.4-4.2] | **15.5** [12.6-18.8] |
| 0.44 | 1.3 [1.0-1.6] | **14.1** [13.0-15.0] |
| 0.60 | 1.7 [0.4-3.4] | **7.5** [7.2-8.0] |

The crossover between CV 0 and 0.10 reproduces in every seed, and the fixed-width arm's
collapse is tight at CV 0 (49.2-50.2) where it matters for the comparison. Spread is
widest in the fixed-width tail cells, which is expected: once TPR is near 1% the zero-FP
threshold is being set by one or two outlier off-target reads.

## RAWHASH BASELINE: the main hypothesis is REFUTED

Run 2026-10-09. RawHash2 v2.1 built with `NOHDF5=1 NOPOD5=1` against slow5lib compiled
`slow5_mt=1`, given THE SAME squigulator-derived pore model as the signal generator and our
engine (`-p squig_r10_9mer.tsv -k 9 --level_column 1`), on the same 20 Mb chr20 reference
and the same SLOW5 files. Correctly-mapped % (mapped within 500 bases of the locus
squigulator records in the read id, matching strand):

| minimizer w | CV 0.00 | 0.10 | 0.20 | 0.30 | 0.44 | 0.60 | drop |
|---|---|---|---|---|---|---|---|
| 0 (RawHash default) | 95.0 | 95.2 | 94.6 | 94.2 | 92.4 | 92.0 | 3.0 pts |
| 5 | 90.0 | 88.8 | 89.4 | 88.0 | 85.2 | 81.6 | 8.4 pts |
| 10 | 82.2 | 82.2 | 82.2 | 79.2 | 77.2 | 71.0 | 11.2 pts |

False-mapped on the shuffled control: ~8-10% at w=0 and w=5, 11-15% at w=10. RawHash's
default operating point is therefore NOT zero-FP, so its 95% is not directly comparable
with our zero-FP TPR; the comparison below is about SHAPE, not level.

**RawHash does not collapse under dwell variation.** 95.0% to 92.0% across the full range.
There is no crossover and no cliff. The claim that "evaluating at CV 0 inverts the ranking"
is therefore a statement about OUR TWO CONFIGURATIONS and not about the method class: a
developer evaluating RawHash on stock Icarust would reach the same conclusion they would
reach on realistic signal.

**So the cliff is a defect in our engine, not a property of raw-signal mapping.** The
mechanism is almost certainly what RawHash has and we do not: CHAINING. RawHash chains
anchors with gap tolerance (`--bw 500`, `--max-target-gap 2500`, DP or RMQ), so a slipped
event boundary costs one anchor. Our Misra-Gries vote demands EXACT diagonal equality, so
the same slip moves every later seed onto a different diagonal and the read is lost. Exact
diagonal voting is the fragility.

### What survives, and it is much smaller than hoped

The minimizer penalty IS dwell-dependent, and that is new. w=0 minus w=10, in points of
correct mapping: 12.8, 13.0, 12.4, 15.0, 15.2, 21.0 as CV goes 0 to 0.60 -- a 64% increase
in the penalty. At w=10 and CV 0.60 minimizers cost 21 points of sensitivity AND nearly
double the false-mapping rate (15.2% against 8.4%).

RawHash2's own help says `-w` "may reduce accuracy but improves the performance and memory
space efficiency". That is true and documented; what is not documented or measured anywhere
is that the cost grows with time-domain noise, so the trade-off is worse on real signal than
on any constant-dwell evaluation would suggest. That is a real, quantified, modest finding
that is directly actionable for anyone choosing `-w`.

It is a short paper, not the paper described above.

### Two honest options

1. **Publish the small finding.** The dwell-dependence of the `-w` trade-off, with the
   sweep harness and the Icarust patch as artifacts. Defensible, modest, and already
   essentially measured.

2. **Fix the engine with chaining and change the contribution to systems.** Replace exact
   diagonal voting with gap-tolerant chaining, which is what makes RawHash robust. If that
   brings our accuracy near RawHash's, the microsecond-latency and bounded-per-channel-state
   work becomes a credible real-time engineering contribution with trustworthy accuracy
   behind it. This is re-implementing known technique, so the novelty is the engine and its
   latency characterisation, not the method.
