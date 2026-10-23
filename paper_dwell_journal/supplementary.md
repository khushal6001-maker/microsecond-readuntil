# Supplementary Information

**Time-domain noise as an evaluation axis for raw-signal nanopore mapping**

Khushal Singh\*, Ritu Garg

Department of Computer Engineering, National Institute of Technology Kurukshetra, Kurukshetra,
Haryana 136119, India

\*To whom correspondence should be addressed: khushal6002@gmail.com

---

Three observations made during this work are reported here rather than in the main text, because
each rests in part on a measurement for which no run output was captured. Those values are marked
with a dagger (†). None of them is used in any claim in the main text, and
`scripts/verify_dwell_journal_numbers.py` lists each as outstanding, together with the run that
would settle it. Section and table numbers without an S refer to the main text.

## S1 Live accept rates against Icarust

With the same client, reference and run length, the fixed-width arm accepted 43.5%† of reads and
the detected arm 9.6%† against stock Icarust, and 0.5%† and 9.6%† against the patched simulator at
CV 0.35. Taken at face value, stock Icarust reports the fixed-width arm as 4.5-fold† better, and the
patched simulator reverses the order, which is the reversal of Section 3.1 observed on the live
interface. These are accept rates on the live loop and are not comparable with the offline
true-positive rates (TPR) of Table 1. Re-running stock and patched Icarust against the client would
settle them.

## S2 Calibration target for event detection

The calibrated detector, threshold 3.0, emits 1562 events per on-target read at CV 0.44, against an
ideal of approximately 2000, one event per base, which is 21.9% under-segmentation; seed 42 gives
14.2% TPR in the captured log. In a development run that was not captured, threshold 2.0 gave 2017†
events per read, close to the ideal, and 2.0%† TPR, 7.1-fold† lower. An argument offered during
development attributed the difference to frame displacement: a spurious boundary shifts every later
key in the read. That argument does not distinguish the two error types, because a missed boundary
removes an event and shifts every later event index by one exactly as a spurious boundary adds one.
A sufficient explanation is simpler. The event count measures net error, insertions minus
deletions, and is blind to errors that cancel, so a detector that emits the ideal number of events
may still place many boundaries wrongly. The recommendation that follows does not depend on the
uncaptured values: calibrate a detector against agreement or indel fraction (Section 2.6), not
against its event count.

## S3 Calibration transfer between generators

A detector threshold calibrated against signal from a generator written for this project, applied
unchanged to Squigulator signal, gave 0.6%† TPR at CV 0, where the Squigulator-calibrated detector
gives 18.4% [17.0–19.8] (Table 1), a 30.7-fold† difference. The two generators differ in their
amplitude-noise models, and the detector threshold is a t-statistic whose operating point depends
on that model. Whatever the magnitude, a result obtained with event detection is interpretable only
together with a statement of how the detector was calibrated and on what signal.
