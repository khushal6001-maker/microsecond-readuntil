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
