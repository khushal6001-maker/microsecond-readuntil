# Thesis outline: a real-time systems architecture for nanopore adaptive sampling

Target: M.Tech, computer engineering. ~90-110 pages.

**Thesis statement.** Adaptive sampling is a soft-real-time systems problem. This work builds
a data plane that makes eject/keep decisions in tens of microseconds with bounded per-channel
state, characterises its latency honestly, and establishes a quantitative ceiling on the
accuracy of quantised-event signal matching that explains why the matching stage -- not the
data plane -- is the binding constraint.

**What this thesis does NOT claim.** Write these on a card and keep it visible:

- Not a novel matching method. RawHash (Bioinformatics 2023) and RawHash2 (2024) already do
  quantised-event hashing with minimizers and chaining.
- Not higher accuracy than existing tools. Ours is 18.8% where RawHash is 92.4% on identical
  data. State this in the abstract, not in a footnote.
- Not a proof that fixed-width segmentation is "invalid". It is a measurement that our
  fixed-width configuration collapses while a detection-based tool does not.
- Not a claim that simulators in general omit dwell. Squigulator models it (`--dwell-std`,
  default CV ~0.44). Icarust specifically does not.

---

## Chapter 1 — Introduction (8-10 pp)

**1.1** Nanopore sequencing and the read-until opportunity. A pore can eject a molecule
mid-read; deciding which to eject is a per-molecule computation under a deadline.

**1.2 The deadline is the whole problem.** Frame adaptive sampling as soft real time: the
decision has value only while the molecule is still in the pore. Introduce deadline-miss rate
as the metric the literature does not report (it reports throughput and accuracy). This
framing is the thesis's organising idea and belongs here, not in Chapter 5.

**1.3** Why basecalling in the loop is the wrong architecture for the deadline: GPU latency,
queueing, Python orchestration.

**1.4 Contributions**, stated at the level they can be defended:

- **C1** A zero-copy, lock-free data plane with bounded per-channel state (Ch 3).
- **C2** Streaming decision structures: bounded-memory diagonal voting and incremental
  gap-tolerant chaining, with their measured trade-offs (Ch 4).
- **C3** Honest latency characterisation, including a measurement methodology that caught
  three of our own invalid results (Ch 5).
- **C4** A dwell-sensitivity evaluation axis no existing framework sweeps, and the finding
  that the standard read-until simulator cannot exercise it (Ch 6).
- **C5** A quantitative accuracy ceiling for quantised-event matching, `a^E` in per-event
  bucket agreement, which predicts observed behaviour and explains five failed interventions
  (Ch 7).

**1.5** Roadmap.

---

## Chapter 2 — Background and related work (12-15 pp)

**2.1** Pore physics only as far as needed: translocation, dwell time, the motor protein, why
dwell is variable and right-skewed.

**2.2** The MinKNOW Read Until API: chunk delivery granularity, `get_live_reads`, the action
channel, classification codes.

**2.3** Basecalling-based adaptive sampling: readfish, its 400 ms poll, Dorado/Guppy in the
loop.

**2.4** Signal-space mapping in chronological order, with what each contributed: UNCALLED
(FM-index over event sequences), Sigmap (k-d tree, seed chaining), RawHash (quantised-event
hashing), RawHash2 (minimizers, adaptive quantisation, frequency filters), RawAlign (DTW atop
seeding), Sigmoni (r-index).

**2.5** Benchmarking and simulation: RawBench (modular pore model / segmentation / matching
over real datasets), Squigulator (`--dwell-std`, `--amp-noise`), DeepSimulator, Icarust.

**2.6 The gap this thesis occupies.** Two sentences, precisely scoped: existing evaluations
hold signal-acquisition characteristics fixed and report throughput and accuracy; none sweeps
time-domain noise, and none reports decision latency against a deadline. Cite RawBench's own
statement that it benchmarks methods "rather than exploring parameter sensitivity across
signal acquisition characteristics".

---

## Chapter 3 — System architecture (15-18 pp) · C1

The strongest chapter. Write it first; it is finished work.

**3.1** Design constraints: per-channel state across 512-2675 channels, no allocation on the
decision path, no basecaller, a hard per-chunk budget.

**3.2** Ingestion and zero copy. `ChunkRef` as a 56-byte borrowed view; nothing is copied
after the arena. Show the struct and its cacheline budget.

**3.3** The arena pool and its refcount protocol. This is where the AddressSanitizer argument
belongs: a broken refcount surfaces as use-after-free rather than silent corruption.

**3.4** Lock-free SPSC rings sharded by `channel & mask`, with cached head/tail. The
drop-never-block backpressure policy and why `offered == pushed + dropped` is the invariant
that matters.

**3.5** Thread topology and the SMT finding. **Include the bug:** `CorePlan` assigned roles to
consecutive CPU ids, which on an SMT host put the reader and writer on one physical core while
a core sat idle. Show that sibling enumeration is not portable (VMs pair adjacently,
bare-metal Intel splits), so no fixed stride is correct and `thread_siblings_list` must be
read. Table: four cells crossing shard count against pinning.

**3.6** Correctness methodology: the five-part stress test, ThreadSanitizer as the run that
matters, ASan for the arena protocol, `-Werror` on two compilers, the CI matrix.

*Figures:* F3.1 data-plane block diagram · F3.2 arena refcount state machine · F3.3 SMT
sibling map, broken and fixed assignments side by side.

---

## Chapter 4 — Streaming decision structures (12-15 pp) · C2

**4.1** The problem: find the mode of a stream of diagonals under fixed memory, per channel,
decided incrementally as chunks arrive.

**4.2** Bounded diagonal voting with Misra-Gries replacement. 64 sets x 4 ways, ~3 KB/channel,
~1.5 MB across 512. Why a running maximum is wrong (a decayed entry's historical peak invents
accepts) and why `best()` scans.

**4.3 The lockout bug, as a case study in bounded-structure design.** The old rule evicted
only when the weakest slot held one vote; at ~95 candidates/chunk every slot passes two votes
in the first chunk and the condition is never true again, so any diagonal not among the first
16 observed is permanently locked out. Measured consequence: 0 accepts of 763. Worth a full
section.

**4.4** Per-chunk run-length pre-aggregation: within a chunk the true diagonal repeats and
noise does not, so the run length is the vote weight.

**4.5** The read-relative diagonal. `seed_offset` is chunk-relative, so a read's true diagonal
shifted by one chunk length per chunk and votes could never accumulate. **The diagnostic is
the lesson:** the tell was an inverted metric — correct-diagonal counts *fell* with read
length (49/300 at 2 chunks to 7/300 at 20), the signature of accumulating the wrong quantity
rather than of a weak signal.

**4.6** Incremental gap-tolerant chaining. The extension rule, why exact diagonal voting is
the `band=0` special case, and why a batch DP chainer is unaffordable here. Table: votes
14.2%, greedy chains 17.4%, anchor-history chains 18.8% at CV 0.44, with `band=0` as the
control (9.6%) proving the tolerance rather than the restructuring does the work.

**4.7** Cost model: O(1) per candidate for voting, O(lookback) per anchor for chaining, paid
once and never re-paid on a later chunk.

---

## Chapter 5 — Latency characterisation and measurement methodology (15-18 pp) · C3

The methodological backbone. Its value is that it shows how to measure honestly, with three
of our own retracted results as evidence.

**5.1** What to measure: per-chunk decision latency, percentiles not means, and deadline-miss
rate (the daemon's TOO LATE counter).

**5.2 Instrument before concluding.** The power-of-two histogram could only print 54.19,
108.38, 216.76 or 433.53 µs at this host's clock; the four values reported were 111.93,
112.12, 223.66, 447.59 — every one a bucket edge. Two runs reading "112" agreed on an octave,
not to 0.2%. Replaced with HdrHistogram at 0.1% relative error, with the backend printed
beside every figure so a fallback bound cannot pass as a measurement. Include the unit test
showing the old histogram reporting 32767 cycles for the p50, p90 and p99 of one distribution.

**5.3 Environment provenance.** A full host disk inflated the measured median by about a third
(p50 27.0-27.3 µs against 17.9-18.8 µs, same configuration, no code change). The harness now
records hypervisor, SMT layout, TSC flags, clocksource, isolcpus/nohz_full, governor, turbo,
disk headroom and loadavg beside every run, and refuses to stamp a run publishable unless
`systemd-detect-virt` reports none, isolcpus is set and the governor is performance.

**5.4** Results, scoped to the platform. Table 5.1: the four-cell pinning matrix on the toy
reference (pinned-2 p50 16.3 µs, p90 24.7, p99 49.2 [47.0-50.9]). Table 5.2: the frozen
configuration at genome scale (p50 134-174 µs, p99 630-705 µs, 3.9-4.4 GB RSS, 0 chunks
dropped at 975 chunks/s).

**5.5** The pinning result, stated correctly: pinning does not lower the median, it makes the
tail reproducible (p99 spread 8% pinned against 133% unpinned). Thread count dominates
placement.

**5.6** Comparison against readfish: 124 µs per chunk for orchestration alone — no basecalling,
no mapping — at 15.6-25.3% of one core and 86-663 MB. We are faster per chunk and 8-12x worse
on CPU because the workers spin. **Report the trade as a trade.**

**5.7** Comparison against RawHash, with the caveat in the same breath: 9.66 ms per read
single-threaded against our ~1.34 ms, at 18.8% accuracy against 92.4%. A latency ratio between
a working matcher and a non-working one bounds the data plane's cost; it is not a performance
win. If you want the quadratic re-chaining argument, instrument RawHash in read-until mode
first — it is currently a hypothesis.

**5.8** Threats to validity: WSL2 virtualises the clock and scheduler; a 4-core laptop
thermally throttles; p50/p90 are reproducible here and p99 is not.

---

## Chapter 6 — Dwell sensitivity as an evaluation axis (12-15 pp) · C4

**6.1** Why time-domain noise is the axis that matters for segmentation-based matching, and
why holding it fixed hides a failure mode.

**6.2** Method: Squigulator at `--dwell-mean 10`, `--dwell-std` swept 0 to 6 (CV 0 to 0.60),
20 Mb chr20 indexed on both strands, Squigulator's own pore model on both sides, 0.4 s chunks,
composition-matched shuffled control simulated at matching dwell, three seeds.

**6.3 The three compatibility traps**, each of which would have produced a plausible curve
measuring the wrong thing: mismatched pore models (54.316 vs 57.203 pA for `AAAAAAAAA` against
a ~1.2 pA level stdev), forward-strand-only indexing (halves every TPR), and wrong chunk size
at 5 kHz. A methods contribution in itself.

**6.4** Result 6.1: our fixed-width configuration falls 49.7% to 1.3% across three seeds; our
detection-based configuration is nearly flat, 18.4% to 14.1%. Crossover between CV 0 and 0.10
in every seed.

**6.5** Result 6.2: **RawHash does not collapse** — 95.0% to 92.0% at `w=0`. State this plainly
and early. It means the sensitivity is a property of fixed-width segmentation, not of
signal-space mapping, and that published tools already avoid it.

**6.6** Result 6.3: the minimizer trade-off is dwell-dependent. The `w=0` minus `w=10` penalty
grows 12.8, 13.0, 12.4, 15.0, 15.2, 21.0 points as CV goes 0 to 0.60, and at `w=10`/CV 0.60
minimizers also nearly double the false-mapping rate. RawHash2 documents that `-w` "may reduce
accuracy"; that it worsens with time-domain noise is undocumented and unmeasured. This is the
chapter's novel, defensible, modest finding, and it is directly actionable for choosing `-w`.

**6.7** Result 6.4: Icarust emits a fixed sample count per k-mer, so its signal is the CV 0
case. With stock Icarust our fixed-width configuration measures 43.5% accept and the detection
configuration 9.6%; with dwell enabled they are 0.5% and 9.6%. **The stock simulator reports
the worse configuration as 4.5x better.** Present the patch as an artifact.

**6.8** Scope the claim: Squigulator models dwell and defaults to CV ~0.44. The gap is that no
evaluation *sweeps* it, and that the read-until simulator cannot.

*Figures:* F6.1 the two curves with error bars and the crossover marked · F6.2 RawHash's three
`w` curves · F6.3 minimizer penalty against CV.

---

## Chapter 7 — The accuracy ceiling of quantised-event matching (12-15 pp) · C5

The intellectual core. Argue the model, verify it, then show it predicts failures.

**7.1 The model.** A key is E consecutive quantised events; it matches only if all E buckets
agree. With per-event agreement `a`, the key match rate is `a^E`, and the expected number of
true anchors in a read of N events is roughly `N·a^E`. Derive the squeeze: raising E buys
specificity and costs `a^E` exponentially; lowering E buys match rate and costs specificity,
which on a large reference appears as occurrence-cap saturation.

**7.2 Measuring `a`.** Compare reference and query buckets at the truth locus from the read id,
forward strand, with a bounded offset search. Results: 55.9% at CV 0 fixed-width 4-bit; 38.2%
at 3 bits; 57.5% at 2 bits. State the diagnostic's own limitation: the detection rows
understate `a`, because the query-to-reference correspondence drifts rather than holding a
constant offset.

**7.3 Verification.** `a = 0.559`, `E = 12` predicts 0.09% key match, so ~1.5 true anchors in a
~1600-event read. The measured median on-target chain score is 4. The model predicts the
observed order of magnitude.

**7.4 The model explains five independent failures**, which is the strongest evidence for it:
gap-tolerant chaining (14.2 to 17.4%), anchor-history chaining (18.8%), occurrence filtering
(17.8%), shorter keys (4x8: 3.2%), difference-based keys (2.8-8.4%, worse because differencing
multiplies noise by √2 and crowds buckets near zero). All act downstream of a 0.09% match rate.
It also explains why the geometry freeze reversed twice.

**7.5 Implication for designers:** report `a` before reporting a decision rule. A pipeline with
`a < 0.9` and `E > 8` cannot work regardless of its chaining, and `a` is cheap to measure.

**7.6 What would raise `a`**, as future work with the reasoning: merging consecutive events
below a signal-difference threshold (RawHash's `--sig-diff`) raises `a` by construction,
because two events straddling a bucket boundary become one symbol; better normalisation
(oracle normalisation recovers 100% of seeds against 50.5% per-read at zero noise);
non-uniform bucket boundaries fitted to the level distribution.

---

## Chapter 8 — Conclusions (6-8 pp)

**8.1** What was built and what it costs.

**8.2** What was disproved, including our own results. **Keep the retraction table** — a
power-of-two histogram that fabricated reproducibility, a full disk that moved the median by a
third, a geometry frozen three times, a vote table that locked out, a chunk-relative diagonal,
and a headline framing withdrawn after the RawHash baseline. A thesis that shows its own
corrections is more credible, not less.

**8.3** The honest position: the data plane is solved, the matching stage is not, and Chapter 7
says why.

**8.4** Future work: raise `a`; deadline-miss rate on bare metal with isolcpus; real hardware.

---

## Appendices

- **A** Reproduction: build, submodules, the Squigulator and RawHash invocations, the HPC probe.
- **B** The Icarust dwell patch and its rationale (`third_party_patches/`).
- **C** Frozen parameters with the measurement behind each, lifted from the source comments.
- **D** Retraction log: every withdrawn result, why, and what replaced it.
- **E** CI configuration and the sanitizer matrix.

---

## Writing order

Ch 3 (finished work, builds momentum) → Ch 5 → Ch 4 → Ch 7 (hardest; write when warm) → Ch 6
→ Ch 2 → Ch 1 → Ch 8 → appendices.

## Figures and tables to produce

| | |
|---|---|
| F3.1 | data-plane block diagram |
| F3.2 | arena refcount state machine |
| F3.3 | SMT map, before and after |
| F5.1 | latency CDF from the HdrHistogram dumps |
| F5.2 | bucket-edge artefact illustration |
| F6.1 | dwell curves with error bars, crossover marked |
| F6.2 | RawHash `w` curves |
| F6.3 | minimizer penalty against CV |
| F7.1 | `a^E` surface over (a, E), measured point marked |
| T3.1 | pinning matrix |
| T5.1 | latency tables |
| T5.2 | readfish and RawHash comparison |
| T6.1 | dwell sweep, three seeds |
| T6.2 | Icarust stock against patched |
| T7.1 | agreement measurements |
| T8.1 | retraction log |

## Defence questions to prepare for

1. **"Your accuracy is 18.8% against 92.4%. Why is this a thesis?"** Because the contribution
   is the data plane, the measurement methodology and the ceiling model — and because Ch 7
   explains the 18.8% rather than excusing it.
2. **"Isn't event detection already standard?"** Yes, and 6.5 says so. The finding is the
   sensitivity axis and the simulator gap, not the detector.
3. **"Did you beat RawHash?"** No. 5.7 states the latency ratio and the accuracy gap together.
4. **"Is the `a^E` model novel?"** The arithmetic is elementary; the contribution is measuring
   `a` in a real pipeline and showing it predicts five intervention failures.
5. **"Why trust numbers from WSL2?"** You should not trust the tail; 5.8 scopes it, and 5.3
   shows what instrumenting the environment caught.
