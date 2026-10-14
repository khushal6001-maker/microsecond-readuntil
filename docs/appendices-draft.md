# Appendices

> These appendices are reproduction material. Unlike Chapters 1–8 they are intended to be
> transcribed from the repository rather than composed, so each entry cites the file it was
> taken from and the figures in them are those recorded in source comments and result files at
> the time of writing. Where a figure also appears in a chapter, the chapter is cited so that
> a correction propagates. Commands are given as executed, including paths, since a path that
> has been tidied for presentation has not been tested.

---

# Appendix A — Reproduction

## A.1 Repository layout

| path | contents |
|---|---|
| `src/core/` | ring buffer, arena, affinity, HdrHistogram recorder |
| `src/transport/` | MinKNOW gRPC client, TLS auth, protobuf compatibility assertions |
| `src/index/` | quantisation, event detection, minimizer index, batched probe |
| `src/daemon/` | sharding, per-channel state, vote table, chainer, policy, `main.cpp` |
| `bench/` | offline sweeps (see A.6) |
| `test/` | stress and unit tests registered with CTest |
| `tools/` | pore-model extraction, PAF scoring |
| `third_party_patches/` | the Icarust dwell patch (Appendix B) |
| `hpc/probe.sh` | cluster environment probe (A.8) |
| `matrix.sh` | the latency matrix of Chapter 5 §5.5, with its provenance block |

## A.2 Submodules

Two, pinned, declared in `.gitmodules`:

```
third_party/minknow_api      https://github.com/nanoporetech/minknow_api
third_party/HdrHistogram_c   https://github.com/HdrHistogram/HdrHistogram_c
```

```bash
git submodule update --init --recursive
```

`minknow_api` is required only for the transport layer and is large. `HdrHistogram_c` is
required for every latency figure in Chapter 5: without it the recorder compiles its
power-of-two fallback and labels every line it prints as an upper bound at approximately 2×
resolution (§5.2.3). A build intended to reproduce latency results must confirm the backend
line in the daemon's output rather than assume it.

## A.3 Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo && cmake --build build -j
```

CMake options, from `CMakeLists.txt`:

| option | default | effect |
|---|---|---|
| `MRU_BUILD_TESTS` | ON | build the stress and unit tests |
| `MRU_WERROR` | OFF | treat warnings as errors |
| `MRU_NATIVE` | OFF | `-march=native` |
| `MRU_TSAN` | OFF | ThreadSanitizer build |
| `MRU_ASAN` | OFF | AddressSanitizer build |
| `MRU_UBSAN` | OFF | UndefinedBehaviorSanitizer build |
| `MRU_WITH_TRANSPORT` | OFF | build the MinKNOW gRPC transport layer |

The transport layer is off by default because it requires a gRPC and protobuf toolchain that
the development host did not have; it is built in continuous integration instead (Appendix E).
`vcpkg.json` declares the dependency as an optional feature, and `CMakeLists.txt` accepts both
the vcpkg CMake-config layout and the Debian pkg-config layout, the latter being the path CI
exercises.

```bash
cmake -S . -B build-transport -DCMAKE_BUILD_TYPE=RelWithDebInfo -DMRU_WITH_TRANSPORT=ON
cmake --build build-transport -j
```

ThreadSanitizer requires Linux or WSL2, since MSVC provides no TSan:

```bash
cmake -S . -B build-tsan -DMRU_TSAN=ON -DCMAKE_CXX_COMPILER=clang++ && cmake --build build-tsan -j
```

## A.4 Tests

```bash
ctest --test-dir build --output-on-failure -LE soak
```

| test | target | asserts |
|---|---|---|
| `core.ring_stress` | `mru_ring_stress` | SPSC ring and arena refcount protocol are race-free |
| `core.ring_soak` | `mru_ring_stress 200000000 8 5000000` | the same over 2×10⁸ operations, label `soak` |
| `core.hdr_latency` | `mru_hdr_latency_test` | reported percentiles track true within guaranteed relative error; demonstrates the fallback defect directly (§5.2.3) |
| `decide.read_state` | `mru_read_state_test` | per-channel state transitions |
| `index.quantise_and_probe` | `mru_index_test` | quantisation and probe round-trip |
| `transport.auth` | `mru_auth_test` | TLS credential construction |
| `transport.proto_compat` | `mru_proto_compat_test` | field-number assertions against the pinned `minknow_api` |
| `transport.live_reads` | `mru_live_reads_test` | stream handling |
| `transport.live_reads_integration` | `mru_live_reads_integration_test` | end-to-end against a local server |

The `soak` label is excluded from the per-push run and executed on a schedule; the rationale
is in Appendix E.

## A.5 The latency matrix (Chapter 5)

```bash
REPS=4 SECS=20 bash matrix.sh > results.csv
```

`matrix.sh` crosses shard count against pinning in four cells — `unpinned-2`, `pinned-2`,
`unpinned-4`, `pinned-4` — which is the design of §5.5: `pinned-2` against `unpinned-2`
isolates pinning at fixed thread count, and `unpinned-2` against `unpinned-4` isolates thread
count at fixed placement. Neither comparison alone attributes a difference to either factor.

Environment variables are the paths to the pore model, reference, CA certificate, daemon build
directory, output directory and gRPC target; defaults in the script are those of the
measurement host. CSV goes to stdout, the provenance block to stderr, so a redirect produces a
clean data file without discarding provenance.

The provenance block records: kernel and machine, CPU model, logical CPU count, SMT sibling
groups read from `thread_siblings_list`, SMT active flag, hypervisor detection, TSC capability
flags (`constant_tsc`, `nonstop_tsc`, `tsc_reliable`, `rdtscp`), current clocksource, the full
kernel command line with `isolcpus`, `nohz_full` and `rcu_nocbs` extracted individually,
frequency governor, turbo state, free space on `/`, and load average. It then emits a verdict
line, which reads `PUBLISHABLE` only when `systemd-detect-virt` returns `none`, `isolcpus` is
present on the command line, and the governor is `performance`; otherwise
`DEVELOPMENT ONLY`. Every latency run in this thesis carries the latter, for the reason given
in §3.8.

## A.6 Offline sweeps

| target | source | purpose |
|---|---|---|
| `mru_squig_sweep` | `bench/squig_sweep.cpp` | the dwell sweep of Chapter 6 on Squigulator signal |
| `mru_geometry_sweep` | `bench/geometry_sweep.cpp` | key geometry over the internal generator |
| `mru_dwell_robustness` | `bench/dwell_robustness.cpp` | segmentation under dispersion |
| `mru_real_reference` | `bench/real_reference.cpp` | probe behaviour at genome scale |
| `mru_rlc_feasibility` | `bench/rlc_feasibility.cpp` | run-length-coded variant, rejected |
| `mru_brittleness` | `bench/brittleness.cpp` | perturbation response |
| `mru_offtarget` | `bench/offtarget.cpp` | superseded by `squig_sweep`'s shuffled control |

```
mru_squig_sweep <model.tsv> <ref.fa> <on.slow5> <off.slow5>
                [chunks] [detect 0|1] [min_window] [bits] [events] [accept_probe]
```

## A.7 Signal generation and the external baseline

**Squigulator.** The sweep of Chapter 6 uses Squigulator's `dna-r10-prom` profile, whose
sampling rate is 5 kHz, so a 0.4 s MinKNOW chunk is 2000 samples rather than the 1800 that
`geometry_sweep.cpp` uses at 4 kHz. Dispersion is swept with `--dwell-std`; the tool's default
is `--dwell-mean 9.0 --dwell-std 4.0`, a coefficient of variation of approximately 0.44, which
is the figure §6.1 uses when observing that the accepted simulator's own default sits where
fixed-width segmentation has already failed. The off-target set is a composition-matched
shuffle of the same reference simulated at matching dwell, so the only difference between the
two sets is index membership.

Three compatibility conditions (§6.3), each of which produces a plausible curve measuring the
wrong thing:

1. **Same pore model on both sides.** Squigulator's built-in R10.4 9-mer table disagrees with
   the one Icarust ships — 54.316 pA against 57.203 pA for `AAAAAAAAA`, a roughly 3 pA gap
   against a level standard deviation of about 1.2 pA. Encoding the reference with one model
   and generating signal from the other measures model mismatch. `tools/extract_squigulator_model.py`
   emits Squigulator's own means as a `<kmer>\t<level_pA>` TSV, parsed from
   `src/model.h`, asserting an entry count of 4⁹ and verifying that line order is lexicographic
   ACGT rather than assuming it, because the loader trusts line order and ignores the k-mer
   string.
2. **Both strands indexed.** Squigulator emits reads from both strands and the reference
   encoder encodes only the forward sequence, so the reverse complement is appended before
   encoding. Without this, half the reads are unmatchable by construction and every
   true-positive rate is halved for a reason unrelated to dwell.
3. **Chunk size derived from the profile's sampling rate.** 2000 samples, not 1800.

**RawHash2.** Built `NOHDF5=1 NOPOD5=1` against slow5lib compiled `slow5_mt=1`, run
single-threaded on the same SLOW5 files and the same 20 Mb chr20 reference, given the same
Squigulator-derived pore model:

```
-p squig_r10_9mer.tsv -k 9 --level_column 1
```

with the minimizer window `-w` set to 0 (its default), 5 and 10 for the three curves of §6.6.
Output PAF is scored by `tools/score_rawhash_paf.py`, which reads the truth Squigulator embeds
in the read identifier — `S1_1!chr20_20Mb!6574!9719!-`, that is `name!contig!start!end!strand`
— and reports two quantities: *mapped*, the fraction of reads given any target, which on the
shuffled file is the false-positive rate because those reads come from a reference not in the
index; and *correct*, the fraction mapped within 500 bases of the true locus on the matching
strand, which is meaningful only for the on-target file. The tolerance constant is `TOL = 500`
in that file.

**readfish.** Executed against Icarust on the same host with a no-operation caller, which
removes basecalling and mapping and therefore measures orchestration cost alone; see §5.8.1
for what that comparison does and does not establish. Setup notes are in
`docs/readfish-baseline.md` and the target configuration in `docs/chr20_targets.toml`.

**Icarust.** Required for the live read-until loop, which Squigulator cannot provide, and
patched as described in Appendix B. Build and TLS notes are recorded separately because the
published instructions did not work unmodified.

## A.8 Cluster environment probe

`hpc/probe.sh` is a Slurm batch script (`--exclusive`, one node, ten minutes) that installs
and changes nothing. It is submitted as a job rather than run on a login node deliberately:
the two properties that determine how everything else must be built — whether compute nodes
reach the internet, and what the CPU and kernel look like under an exclusive allocation — both
differ on a login node, which is usually a different machine and usually online.

It reports allocation context, CPU topology including SMT sibling groups and NUMA layout, the
latency-critical settings (virtualisation, clocksource, kernel command line, `isolcpus`,
`nohz_full`, `rcu_nocbs`, governor, turbo, effective affinity and cgroup cpuset), memory and
scratch filesystems with free space and filesystem type, GPU presence, toolchain versions
including whether gRPC and protobuf are reachable through pkg-config, available modules
filtered for basecalling and CUDA, whether any basecaller is present, and HTTP reachability of
three hosts.

```bash
sbatch hpc/probe.sh     # then read mru-probe-<jobid>.out
```

The three lines that determine the plan are named in the script's own closing output:
virtualisation status, the HTTP response codes, and whether a basecaller was found.

---

# Appendix B — The Icarust dwell patch

Source: `third_party_patches/icarust-dwell-variability.patch`, applied against
Icarust's `src/simulation.rs`. Rationale is in `third_party_patches/README.md` and the
measurement it supports is §6.8.

## B.1 What the stock simulator does

Icarust emits exactly `samples_per_base` samples for every k-mer:

```rust
// N sampling for each base (sample_rate / bases per second )
// This could also be worked out from profile.dwell_mean
for _ in 0..samples_per_base {
```

Its own comment acknowledges `dwell_mean` and does not use it, so every simulated read
translocates at a perfectly constant rate.

## B.2 Why this matters, and for whom it does not

The constant-rate assumption is invisible to a basecalling-based client, which is what most
read-until clients are, and decisive for any client that segments raw signal by time.
Fixed-width segmentation is *exactly* correct under constant dwell. A signal-space client can
therefore measure as accurate against Icarust and fail on an instrument.

Measured on the same daemon, same reference, same 22 s run `[VERIFY]`:

| Icarust configuration | fixed-width segmentation | event detection |
|---|---|---|
| stock (constant dwell) | 43.5% accept | 9.6% accept |
| `ICARUST_DWELL_CV=0.35` | 0.5% accept | 9.6% accept |

Fixed-width falls by a factor of 87 once dwell varies. Event detection is unchanged between
the two, which is the operational definition of dwell invariance. Without the patch neither
number is obtainable, and the stock simulator reports the worse configuration as 4.5 times
better — the figure quoted in §1.5 (C4) and §6.8.

These are Icarust accept rates on a live loop and are not the Squigulator true-positive rates
of §6.4; the two are measured on different instruments at different operating points and
should not be cross-compared.

## B.3 The change

`ICARUST_DWELL_CV` sets the coefficient of variation of a lognormal dwell about
`samples_per_base`, derived in the patch as

&nbsp;&nbsp;&nbsp;&nbsp;σ² = ln(1 + CV²),&nbsp;&nbsp; μ = ln(`samples_per_base`) − σ²/2

so the distribution's mean is the nominal sample count. Lognormal because dwell is positive
and right-skewed, and because it is the long holds that displace segmentation; published R10
variability is roughly CV 0.3–0.5 `[VERIFY]`. Draws are clamped below at one sample — a k-mer
contributing nothing is a skip, which is a different phenomenon — and above at eight times
nominal, beyond which it is a pore block rather than a dwell.

Unset, or zero, reproduces stock behaviour exactly, so prior results taken against the
unpatched tool remain valid. The patch emits a log line stating whether variability is enabled
and at what CV, and falls back to fixed dwell with a log line if the distribution is rejected,
so a run cannot silently be the wrong one.

```bash
patch -p0 < icarust-dwell-variability.patch && cargo build --release
ICARUST_DWELL_CV=0.35 ./target/release/icarust -c config.ini -s profile.toml
```

## B.4 Upstreaming

An environment variable keeps this a minimal and revertible change to a third-party tool. A
patch offered upstream should read the value from the sample profile TOML alongside
`mean_read_length` rather than from the environment, and should model skips as well, which
this patch does not.

---

# Appendix C — Frozen parameters

Each entry gives the value, the file it is defined in, and the measurement recorded in the
source comment that selected it. Figures are `[VERIFY]` throughout. Three of these parameters
were frozen more than once; §6.9 and §7.3.3 explain why, and Appendix D records the withdrawn
freezes.

## C.1 Key geometry — `src/index/quantise.hpp`

| parameter | value |
|---|---|
| `bits_per_event` | 4 |
| `events_per_key` | 12 |
| `minimizer_window` | 1 (sketching disabled) |
| `event_detection` | true |

Constraint: `bits_per_event × events_per_key ≤ 64`, so the key packs into one 64-bit word.
4 × 12 = 48.

**Why 4 × 12.** True-positive rate at the lowest zero-false-positive threshold, 400 reads:

| geometry | @5 chunks | @10 | @20 | candidates/read at 10 chunks |
|---|---|---|---|---|
| 3 × 14 | 15.5% | 18.2% | 33.0% | 7219 |
| 4 × 12 | 19.0% | 22.2% | 31.8% | 661 |

4 × 12 wins at 5 and 10 chunks, ties at 20, and costs eleven times fewer candidates.

**Why the previous freeze was wrong.** The earlier freeze chose 3 × 14 over 4-bit cells on the
grounds that 4 bits discards too many true seeds. That was true *with* minimizers: the sketch
already discards seeds, so a denser 3-bit index was needed to compensate. Without sketching
nothing is discarded, so the extra specificity of 4 bits is gain and its lower candidate count
is saving. The two parameters are not separable, and freezing them one at a time is what
produced the wrong answer twice.

**Why not shorter keys.** 3 × 8 caps 99.3% of its seeds into 65,760 distinct keys and has no
specificity left to set a threshold against. The hypothesis that shorter keys would be more
robust to boundary error was wrong; §7.3 gives the reason in terms of realised key entropy.

**Why sketching is disabled.** Under event detection every event boundary is an estimate, so a
shifted boundary makes reference and query select *different* keys as a window's minimum even
where both keys are individually correct — the sketch discards precisely the seeds detection
was added to recover. This failure mode cannot occur at constant dwell, because boundaries
never move, which is why window 10 appeared free when it was first frozen. Measured on 64 Mb
chr20 at dwell CV 0.35 with detection, 400 reads, zero-FP threshold, 10-chunk reads:

| geometry | window 10 | window 1 |
|---|---|---|
| 3 × 14 | 3.0% | 18.2% |
| 3 × 12 | 0.0% | 11.3% |

The cost of disabling it is real and is reported with the benefit: 63.9 M minimizers against
11.7 M for chr20, approximately 3.9 GB resident against 1.6 GB, and a p50 decision latency of
174 µs against 25 µs. Windows 2 and 3 were measured as intermediate points and lose most of
the accuracy (14.3% and 1.5% live accept against 97.5% at window 1 before thresholding), so
the trade is not smooth and there is no cheap middle.

**Rejected variants retained in the source.** `delta_keys` (false) with `delta_clip` 1.5 — the
difference-based encoding of row 10 of Table 8.1. An adaptive, equal-occupancy quantiser, whose
comment records the measured occupancy of uniform bucketing into 8 levels over chr20 under
Icarust's R10 9-mer table — 0.0%, 5.0%, 28.6%, 12.0%, 27.3%, 21.7%, 5.1%, 0.3% — so that one
bucket of eight is never used and another takes 0.3%, delivering about 2.3 bits per event
instead of 3. §7.8 recommends revisiting this with `a` as the dependent variable, because the
original rejection predates the model and did not measure `a`.

## C.2 Event detector — `src/index/event_detect.hpp`

| parameter | value |
|---|---|
| `window` | 4 |
| `threshold` | 3.0 |
| `min_len` | 4 |
| `max_len` | 60 |

`window` is the number of samples each side of a candidate boundary in the two-window
t-statistic. Smaller is more sensitive and noisier; at approximately 10 samples per k-mer, 4
resolves adjacent k-mers without splitting one.

`threshold` was **re-calibrated on Squigulator signal rather than on this project's own
generator**, because a calibration derived from the internal generator's noise model did not
transfer: it scored 0.6% TPR at dwell CV 0 on Squigulator data where fixed-width scored 49.2%.
This is the factor-of-thirty non-transfer reported in §6.7.

The calibration *target* also had to change, which is the less obvious half. Tuning to match
events-per-read against the ideal of one event per base is wrong. On Squigulator at CV 0.44,
4 × 12, window 1:

| | events/read | TPR |
|---|---|---|
| threshold 2.0, `min_len` 4 | 2017 (ideal 2000) | 2.0% |
| threshold 3.0, `min_len` 4 | 1562 | 14.2% |

Deliberately under-segmenting by about 20% is seven times better. The asymmetry is
mechanistic: a key packs a fixed number of *consecutive* events, so one spurious event shifts
every later key in the read, while a missed boundary merely merges two events and leaves the
rest of the frame intact. Missing boundaries is cheap; inventing them is not.

`min_len` guards against a noise spike being read as two boundaries a sample apart. 4 measured
best on Squigulator signal; 7, which was calibrated against the internal generator, costs more
than half the TPR (5.8% against 14.2% at CV 0.44).

`max_len` forces a boundary after 60 samples with no detected change. Without it a long
homopolymer becomes one enormous event and the read loses its frame; with it the segmentation
degrades gracefully to fixed-width in exactly the region where there is no level change to
detect.

The detection path takes caller-owned scratch (`EventScratch`, two prefix-sum vectors) so that
it never allocates after the first chunk.

## C.3 Decision threshold — `src/daemon/policy.hpp`

`accept_votes = 8`.

Measured on human chr20 against a composition-matched shuffled off-target control:

| threshold | on-target accept | off-target accept | |
|---|---|---|---|
| 5 | 97.5% | 72.9% | accepts almost everything |
| 8 | 8.3% | 0.75% | frozen |
| 10 | 0.0% | 0.1% | suppresses both |

8 is the only operating point measured that separates at all, at roughly 11× enrichment. The
yield is low and that is the state of the method on realistic signal rather than a tuning
artefact: 10 removes the true accepts along with the false ones. The 8.3% figure here is the
single-run value withdrawn as row 11 of Table 8.1; three replicates place it at 3.2–3.6%
(§6.4), and this comment is one of the places the correction must be propagated.

The parameter is not independent of the geometry or the minimizer window. Changing either
moves the vote distribution, so all three must be re-frozen together; doing otherwise produces
a configuration that accepts everything while appearing to have excellent recall.

## C.4 Chainer — `src/daemon/chain.hpp`

| parameter | value | meaning |
|---|---|---|
| `max_gap` | 160 | largest forward step, in events, still counted as the same chain |
| `band` | 12 | permitted disagreement between query gap and reference gap for one extension |
| `lookback` | 48 | predecessors examined per anchor by the anchor-history chainer |
| `max_occ` | 0 | occurrence filter, disabled |

`band` is the structure's purpose: 0 reproduces exact diagonal voting and its cliff. `max_gap`
is wide enough to bridge a dropped seed or a capped-occurrence gap and narrow enough that two
unrelated loci are not stitched together.

`lookback` cost is O(lookback) per anchor, paid once and never re-paid on a later chunk, which
is the structural difference from a batch chainer that re-chains every accumulated anchor on
every chunk and so does work quadratic in chunk count over a read (§4.8, and the untested
hypothesis of §5.8.2). minimap2 uses 25–50 with an early exit and RawHash's `--max-iterations`
defaults to 200 `[VERIFY]`; 48 sits inside that range.

`max_occ` = 0 disables the occurrence filter. The probe returns at most
`SeedMatches::kMaxPerSeed` positions, so a saturated count indicates an ambiguous key;
filtering on it was measured and did not help (§7.6).

Per-channel state: 24 greedy chains, or a 1024-anchor ring at 14 KB per channel for the
anchor-history chainer (§3.6, §4.8).

## C.5 Vote table — `src/daemon/diagonal_votes.hpp`

64 sets × 4 ways, `kDiagCapacity = 16384`, 3.0 KB per channel. Update is O(ways) = O(4) rather
than O(counters) = O(256); the expected frequency threshold is m/320 against classical
Misra–Gries at m/257 for the same footprint (§4.3.3) `[VERIFY]`.

Diagonals are made read-relative by passing the running count of events consumed by the read,
which is the correction of row 7 of Table 8.1:

```cpp
accumulate_chunk_votes(scratch, v, diags, dropped, scratch.events.size())
```

## C.6 Sentinel defaults in the command line

`src/daemon/main.cpp` exposes `--bits`, `--events`, `--detect`, `--no-detect` and
`--min-window`. Each numeric flag defaults to a **0 sentinel** meaning "unset", so that the
frozen `PolicyConfig` and `QuantiseConfig` values are used when the flag is absent. This is a
correction: the argument struct previously carried its own defaults and assigned them
unconditionally, so `accept_votes` was silently 5 rather than the frozen 8, producing 60.4%
on-target against 53.6% on a shuffled control — a result that is close to no discrimination at
all and that read as a working configuration. Any flag added to this parser must follow the
sentinel convention.

---

# Appendix D — Retraction log

Table 8.1 tabulates the twelve withdrawn results; this appendix gives the full entry for each,
in the same order, with the measurement that produced the withdrawn claim, the diagnosis, and
the measurement that replaced it. The purpose is stated in §1.6: the mechanism by which each
failed is the transferable part.

**D.1 — Stable 99th percentile (instrument; §5.2).** *Withdrawn:* that a thread-placement
change had produced a stable tail, two runs agreeing at 112 µs to within 0.2%. *Reported
values:* 111.93, 112.12, 223.66, 447.59 µs across four runs. *Diagnosis:* the recorder used
power-of-two bucket boundaries, so at the host's calibrated invariant TSC frequency of
2418.7 MHz the only values representable in that range were 54.19, 108.38, 216.76 and
433.53 µs. Every reported value is a bucket boundary; two runs reporting 112 µs agreed only
that the true value lay in [54.19, 108.38] µs. *Replacement:* `HdrHistogram_c` at three
significant figures, bounding relative error at 0.1% across the dynamic range; the recorder
reports its own backend on every line and labels the fallback as an upper bound. *General
form:* any instrument whose quantisation is coarse relative to the between-run variance of the
measurand will report stable values and thereby invite an inference of stability. The
diagnostic — compare reported values against the instrument's representable set — requires no
additional experiment.

**D.2 — Unpinned median of 27.0–27.3 µs (environment; §5.3).** *Withdrawn:* a four-run table.
*Diagnosis:* the host's system drive was fully exhausted; the guest filesystem resided in a
dynamically expanding image on that drive and could not extend, so writes incidental to the
measurement stalled. Approximately one third of the reported median was host state. *Replacement:*
the identical configuration — same binary, same input, same parameters — measured 17.9–18.8 µs,
with the observed maximum falling from 9.2 ms to 1.5 ms. *Why it was diagnosable at all:* the
configuration happened to be repeated; the original measurement contained no record of the
condition that invalidated it. *Replacement practice:* the harness now captures virtualisation,
SMT topology, TSC flags, clocksource, kernel command line with isolation parameters, governor,
turbo state, free storage and load average per run, and refuses to label a run publishable
unless virtualisation is absent, cores are isolated and the governor is `performance`.

**D.3 — Reader and writer on distinct physical cores (platform; §5.4).** *Withdrawn:* the
pinning plan's placement. *Diagnosis:* adjacency of logical processor identifiers was assumed
to imply distinct cores. On the measurement host CPUs 0 and 1 are SMT siblings, so the two
threads shared execution resources; the relationship is not portable, with virtual machines
commonly enumerating siblings adjacently and bare-metal Intel parts splitting them as `n` and
`n + cores`. *Replacement:* sibling sets read from
`/sys/devices/system/cpu/cpu*/topology/thread_siblings_list`, with placement derived from
topology (`cpu_sibling_groups`, `cpus_by_core_then_sibling`, `assign_distinct_cores` in
`src/core/affinity.hpp`).

**D.4 — First key-geometry freeze (model; §6.9).** *Withdrawn:* a geometry selected from an
analytical model of key-space occupancy. *Diagnosis:* the model's fit did not converge.
*Replacement:* empirical sweep.

**D.5 — Second key-geometry freeze, 4 bits × 13 events (algorithm; §4.5, §6.9).** *Withdrawn:*
the geometry and the live measurements supporting it. *Diagnosis:* those measurements were
taken while diagonals were computed chunk-relative (D.7), so they ranked geometries under a
coordinate defect. With the defect corrected the ordering of the candidate geometries reversed.
*Replacement:* 4 bits × 12 events with sketching disabled, selected on Squigulator with dwell
swept and with segmentation and sketching varied simultaneously rather than one at a time —
Appendix C.1 records why one-at-a-time freezing produced the wrong answer twice.

**D.6 — The vote table as a frequent-items estimator (algorithm; §4.4).** *Withdrawn:* the
claim that the structure estimated the most frequent diagonal. *The code:*
`if (count[weakest] <= 1) { install (d, 1) at weakest; }` with no else branch and no decrement.
*Diagnosis:* without a decrement the rule is not a frequent-items algorithm. Once every way of
a set holds two or more votes the condition is permanently false and the set is immutable for
the remainder of the read, so its contents are determined by arrival order rather than by
frequency — a cache with no eviction policy. *Scale dependence:* invisible at approximately one
candidate per chunk (a 37 kb reference) and total at approximately ninety-six (a 64 Mb
reference). Measured consequence: 0 accepts from 763 decisions at 64 Mb, where the same binary
accepted 854 of 1366 at 37 kb. *Replacement:* Misra–Gries-style replacement with decrement, and
a stress test that drives candidate density to the production value. *General form:* a bounded
structure's correctness argument must be stated in terms of the arrival density it will see,
not the density at which it was tested; `count ≤ 1` is a statement about density disguised as a
statement about a counter.

**D.7 — Diagonal votes identify a stable locus (algorithm; §4.5).** *Withdrawn:* the
precondition that repeated observations of the same locus produce the same item identity.
*Diagnosis:* seed offsets are reported relative to the query passed to the matching stage, and
the query is one chunk, so offsets restart at zero at every chunk boundary. For a read
originating at reference position r₀ with Q events per chunk, the diagonal observed in chunk c
is d_true + cQ: the item of interest was renamed on every arrival, and votes could accumulate
only *within* a chunk. *The diagnostic, which is the transferable part:* the symptom was not a
low score, which would have been attributed to weak signal, but an **inverted** relationship —
the fraction of reads whose best diagonal was correct *decreased* with read length, 49/300 at
two chunks to 7/300 at twenty. Additional evidence producing a worse answer cannot be explained
by insufficient evidence; it is the signature of accumulating a quantity other than the
intended one. *Replacement:* a running count of events consumed by the read, making diagonals
read-relative; correct-diagonal counts then rise with read length as they must, 118 → 160 →
170 → 196 of 300. Monotonicity in evidence is now asserted explicitly in the offline harness.

**D.8 — The headline framing (framing; §5.8, Chapter 7).** *Withdrawn:* the positioning of the
work as a competitive signal-space mapper. *Diagnosis:* a baseline run of RawHash2 on identical
input with the same pore model resolved 92.4% of reads against this system's 18.8%. Every prior
intervention had been measured against this system's own earlier numbers, which rose and fell
credibly and gave no indication that the ceiling lay in an earlier pipeline stage.
*Replacement:* the three contributions the measurements support — the data plane, the
measurement methodology, and the `a^E` ceiling model. *General form:* in a pipeline whose stages
are ordered, an internal improvement series measures progress against the wrong denominator
until an external reference fixes the scale. The baseline is the first experiment, not the last.
Four other framings were ruled out at the same time and are recorded in `docs/paper-direction.md`:
claiming a novel signal-space matching method, claiming that existing simulators omit dwell
variability (Squigulator models it), claiming that no benchmarking framework exists for raw
signal (RawBench), and claiming greater accuracy than existing tools.

**D.9 — Two-scale event detector (algorithm; §6.7).** *Withdrawn:* a two-scale variant
implemented on the reasoning that multi-scale change detection is standard practice.
*Diagnosis:* measured worse — 3 against 38 correct diagonals at CV 0.50 — and it regressed the
single-scale path as well (96/195/58/38 falling to 52/59/27/1 across the four read-length
points). *Replacement:* reverted to the committed single-scale two-window t-test with the
calibration of Appendix C.2.

**D.10 — Difference-based key encoding (algorithm; §7.6).** *Withdrawn:* the proposal that
encoding successive level *differences* would remove the shared per-read normalisation error.
*Diagnosis:* measured worse, 18.8% falling to 2.8–8.4%. The encoding retained one symbol per
event and changed only what the symbol encoded, so E agreements were still required while each
symbol became noisier, being a function of two noisy levels rather than one. *Replacement:*
rejected. §7.8 distinguishes it from *merging*, which reduces E and is untested.

**D.11 — Fixed-width TPR of 8.3% (replication; §6.4).** *Withdrawn:* a figure quoted from a
single 60 s run. *Diagnosis:* three replicates placed the same quantity at 3.2–3.6% against
0.76–1.08% on the composition-matched control. *Replacement:* all dwell results reported as
three-seed means with ranges (seeds 42/43/44, 500 reads per cell). The withdrawn value survives
in the `accept_votes` comment in `src/daemon/policy.hpp` and is flagged in Appendix C.3.

**D.12 — Predicted collapse of minimizer sketching under dwell (prediction; §6.5).**
*Withdrawn:* the prediction, derived from the boundary-shift mechanism of Appendix C.1, that
RawHash2's minimizers would collapse as dwell dispersion rose. *Diagnosis:* refuted by
measurement — 95.0% at CV 0 to 92.0% at CV 0.60 at its default window, with no crossover and no
cliff. *Consequence:* the refutation became §6.5, which bounds every claim in Chapter 6 and
establishes that the cliff is a property of this system's fixed-width configuration rather than
of signal-space mapping as a method class. *What survived:* the narrower and better-supported
finding that the *cost* of sketching is dwell-dependent — the w=0 minus w=10 penalty rising
12.8, 13.0, 12.4, 15.0, 15.2, 21.0 points across CV 0 to 0.60, with the false-mapping rate
rising from 8.4% to 15.2% in parallel. The tool documents that `-w` "may reduce accuracy" and
ships with sketching off, so the existence of the trade is declared; its dependence on
time-domain noise is not.

---

# Appendix E — Continuous integration and the sanitiser matrix

Source: `.github/workflows/ci.yml`. Triggers are push to `main` or `master`, pull request,
manual dispatch, and a weekly schedule at 03:00 Sunday. Concurrency is grouped by workflow and
ref with in-progress cancellation. The generator is Ninja throughout.

## E.1 The core matrix

Five configurations, `fail-fast: false`, on `ubuntu-latest` with a 30-minute timeout:

| job | compiler | build type | flags | what it is for |
|---|---|---|---|---|
| `tsan-clang` | clang | RelWithDebInfo | `-DMRU_TSAN=ON` | the one that matters: proves the lock-free ring and the arena refcount protocol are race-free |
| `asan-ubsan-clang` | clang | RelWithDebInfo | `-DMRU_ASAN=ON -DMRU_UBSAN=ON` | a broken refcount surfaces as a use-after-free rather than as silent payload corruption |
| `asan-ubsan-gcc` | gcc | RelWithDebInfo | `-DMRU_ASAN=ON -DMRU_UBSAN=ON` | the two sanitiser runtimes do not find identical sets of bugs |
| `release-warnings-gcc` | gcc | Release | `-DMRU_WERROR=ON` | keeps the optimised build clean |
| `release-warnings-clang` | clang | Release | `-DMRU_WERROR=ON` | as above |

Sanitiser options are set as environment variables on the test step:

```
TSAN_OPTIONS:  halt_on_error=1:second_deadlock_stack=1:history_size=7
ASAN_OPTIONS:  abort_on_error=1:detect_leaks=1:detect_stack_use_after_return=1:strict_string_checks=1:check_initialization_order=1
UBSAN_OPTIONS: print_stacktrace=1:halt_on_error=1
```

`halt_on_error` makes the first race fail the job rather than be buried in the log, and
`history_size=7` raises the chance of a usable stack for the *other* thread in a reported pair.

Two configuration details are recorded in the workflow as comments because both were
encountered as failures and neither is obvious.

**Submodule scope.** The jobs fetch `third_party/HdrHistogram_c` specifically, at depth 1,
rather than `submodules: recursive`. They do not build the proto tree and `minknow_api` is
large. The reason this is load-bearing rather than an optimisation is §5.2.4: before it, the
dependency was absent and the five sanitiser builds compiled the power-of-two **fallback**, so
the real percentile code — the code every reported latency figure depends on — was built under
no sanitiser and tested in no configuration. A degradation path that is correct but silent will
be the path that is tested.

**`libclang-rt-dev` is not optional.** The sanitiser runtime archives
(`libclang_rt.tsan-x86_64.a` and siblings) live in that package, not in `clang`. The GitHub
runner image happens to preinstall clang with its runtimes, so the matrix passed without it,
while a clean Ubuntu with `--no-install-recommends clang` fails at *link* time with
`cannot find libclang_rt.tsan-x86_64.a` — which is what happened on a local WSL host. Listing
it explicitly means the workflow does not depend on what the image happens to ship.

Each job records its environment before configuring: CMake version, compiler version, core
count and kernel release.

## E.2 The transport job

`transport-gcc`, separate because there is no gRPC toolchain on the development host, so this
job is where `src/transport/` is compiled at all. It is therefore the only verification that
the generated `minknow_api` bindings and the field-number assertions in `proto_compat.hpp`
hold. It checks out with `submodules: recursive`, since the protos are in a submodule, and
installs `protobuf-compiler`, `protobuf-compiler-grpc`, `libprotobuf-dev` and `libgrpc++-dev`
from Debian/Ubuntu packages, which provide gRPC through pkg-config only and ship no
`gRPCConfig.cmake`. `CMakeLists.txt` handles both that and the vcpkg CMake-config layout; this
job exercises the pkg-config path. Its environment record includes the protobuf and grpc++
versions, the `grpc_cpp_plugin` path, and the `minknow_api` submodule's `git describe`, so that
a protocol change is attributable to a pin.

## E.3 The soak job

Excluded from the per-push matrix and run on the schedule or on manual dispatch, with a
120-minute timeout, because the rare interleavings it hunts only appear over hundreds of
millions of operations.

| job | build | command |
|---|---|---|
| `soak-release` | Release | `ctest --test-dir build -L soak` — 2×10⁸ operations, 8 shards, at full speed |
| `soak-tsan` | RelWithDebInfo, `-DMRU_TSAN=ON` | `./build/mru_ring_stress 20000000 8 500000` |

The two are complementary: the release run reaches interleavings a sanitiser-slowed build never
gets to, and the TSan run is far longer than the push matrix can afford. The per-push jobs
exclude the label with `ctest -LE soak`.

The soak job deliberately carries no dynamic `name:` expression. When a job is skipped GitHub
cannot expand the matrix, so `name: ${{ matrix.name }}` renders as the literal string
`matrix.name` in the run summary; the default naming is less tidy and always correct.

## E.4 A YAML hazard worth recording

`ASAN_OPTIONS` must stay on one line. A YAML folded scalar (`>-`) joins lines with a space,
which would produce `...=1: strict_string_checks=1`, and the AddressSanitizer flag parser
rejects a flag name beginning with a space. The failure appears as a sanitiser configuration
error rather than as a YAML error, which is what makes it expensive to diagnose.
