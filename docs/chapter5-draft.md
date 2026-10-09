# Chapter 5 — Latency Characterisation and Measurement Methodology

> **Draft status.** Structural first draft. `[VERIFY]` marks a figure to be re-derived from a
> recorded run before it enters the thesis; `[TABLE 5.x]` / `[FIG 5.x]` mark artefacts still
> to be produced.
>
> **Configuration discipline.** Three distinct measurement regimes appear in this chapter and
> must never be combined into one series. (R1) the 37 kb reference under constant-dwell
> simulation, used for the placement matrix; (R2) the 64 Mb reference under constant-dwell
> simulation; (R3) the 64 Mb reference under variable-dwell simulation with the frozen
> configuration. Medians differ by an order of magnitude between R1 and R3 for reasons that
> are entirely explicable, and quoting them as one range would be indefensible. Every table
> states its regime.

---

## 5.1 The microsecond measurement problem

Characterising a decision path whose median cost is tens of microseconds is not a matter of
timing it more carefully. At this scale the measurement is subject to at least four classes
of error whose magnitudes are comparable to, or larger than, the quantity being measured.

**Instrument resolution.** A histogram with coarse bucket boundaries does not merely report
an imprecise percentile; it reports a boundary. Because the reported value is then stable
across runs, coarse quantisation is indistinguishable from reproducibility, and it
systematically manufactures the appearance of precision. Section 5.2 documents an instance in
which this occurred in the present work.

**Summary-statistic choice.** The arithmetic mean of a latency distribution with a heavy tail
is neither a typical value nor a bound, and is dominated by the rarest observations. Under a
deadline (Chapter 3, §3.1.1), the operationally meaningful quantities are the proportion of
decisions completing within the budget and the shape of the upper tail. Means are reported in
this chapter only where they expose a discrepancy with the median.

**Environment state.** The machine's condition at the time of measurement is part of the
measurement. Section 5.3 documents a case in which an entirely external property of the host
— available storage — displaced the measured median by approximately one third, with no
change to the software under test.

**Platform mediation.** Where the measured system runs under a hypervisor, the scheduler and
the clock are not its own. Section 5.8 argues that this contaminates the upper percentiles
specifically, and quantifies which statistics survive.

The chapter's claim is therefore methodological as much as empirical: the latency figures
reported in §5.7 are defensible because each of these four error classes was identified,
measured, and either eliminated or bounded, and three of the four were discovered only after
they had already produced published-looking results that were subsequently withdrawn.

---

## 5.2 Instrument: resolution artefacts and high-dynamic-range histograms

**5.2.1 The initial instrument.** The first implementation accumulated cycle counts into a
histogram with power-of-two bucket boundaries. This structure is attractive on a decision
path: a bucket index is obtained from a leading-zero count, the table is small enough to
remain resident, and the recording operation is a single increment. Its defect is that the
relative error of a reported percentile is bounded only by the bucket width, which at
power-of-two spacing is 100% of the value.

**5.2.2 The artefact.** On the measurement host, with a calibrated invariant TSC frequency of
2418.7 MHz `[VERIFY]`, the only values the structure could report as a 99th percentile in the
relevant range were the bucket boundaries

&nbsp;&nbsp;&nbsp;&nbsp;54.19, 108.38, 216.76, 433.53 µs

The four values actually reported across four runs were 111.93, 112.12, 223.66 and 447.59 µs
`[VERIFY]`. Each is a bucket boundary. Two runs reporting "112 µs" were therefore not in
agreement to 0.2%, as the figures suggest; they were in agreement that the true value lay
somewhere in [54.19, 108.38] µs. The apparent reproducibility was an artefact of the
resolution limit, and a conclusion was drawn from it — that a particular thread-placement
change had produced a stable tail — which later measurement did not support.

This failure mode is worth stating in general form because it is not specific to
power-of-two bucketing: *any* instrument whose quantisation is coarse relative to the
between-run variance of the measurand will report stable values and thereby invite an
inference of stability. The diagnostic is to compare reported values against the
instrument's representable set, which is a calculation requiring no additional experiment.

**5.2.3 Replacement and verification.** The histogram was replaced with HdrHistogram
(`HdrHistogram_c`), configured for three significant figures, which bounds relative error at
0.1% across the full dynamic range rather than at the bucket width. Two properties of the
replacement matter for the argument.

First, the recorder reports its own backend alongside every figure, and when the HdrHistogram
dependency is absent it degrades to the coarse histogram while labelling every line as an
upper bound at approximately 2× resolution. A bounded estimate can therefore not be mistaken
for a measurement in a later reading of the output.

Second, the replacement is verified against the property that matters rather than against
successful initialisation. A unit test constructs a distribution with known percentiles and
asserts that each reported percentile tracks the true one within the guaranteed relative
error; the same test, compiled against the fallback backend, demonstrates the original defect
directly by showing the coarse histogram report **the same value, 32767 cycles, for the 50th,
90th and 99th percentiles of a single distribution** whose true percentiles differ
`[VERIFY]`, with relative errors of 20% to 60%. The test additionally forces reallocation of
a container of recorders so that AddressSanitizer exercises the move constructor's pairing
with the underlying `hdr_close`, since a leak in an instrument is a slow failure in a
long-running daemon.

`[FIG 5.1]` Reported versus true percentiles for both backends on one distribution,
with representable bucket boundaries marked on the axis.

**5.2.4 Consequence for the sanitiser matrix.** The dependency was initially absent from the
continuous-integration configurations, with the result that the five sanitiser builds
compiled the fallback path. The code on which every reported percentile depended was
therefore exercised under no sanitiser. This is recorded because it is a general hazard of
optional dependencies: a degradation path that is correct but silent will be the path that is
tested.

---

## 5.3 Environment: host state as an uncontrolled variable

**5.3.1 Observation.** A set of four latency runs produced medians of 27.0–27.3 µs for the
unpinned configuration. After unrelated maintenance, the identical configuration — same
binary, same simulated input, same parameters — produced 17.9–18.8 µs, and the observed
maximum fell from 9.2 ms to 1.5 ms `[VERIFY]`. No code on that path had changed; the
thread-placement work carried out in the interval affected only the pinned configuration.

**5.3.2 Cause.** The host's system drive had been fully exhausted at the time of the first
set. The guest filesystem resided in a dynamically expanding disk image on that drive, so the
guest could not extend its backing store, and operations that required writes — including
journal activity incidental to the measurement — stalled. Approximately one third of the
originally reported median was attributable to the state of the host, not to the software.

**5.3.3 Methodological response.** Diagnosing this after the fact is possible only because
the configuration was repeated; the first measurement contained no record of the condition
that invalidated it. The measurement harness was therefore extended to capture, for every
run, the properties whose variation can displace the result: virtualisation status, SMT
sibling topology, TSC capability flags, the active clocksource, the kernel command line
including isolation parameters, the frequency governor and turbo state, available storage,
and load average. The harness additionally refuses to label a run as publishable unless
virtualisation is absent, core isolation is configured and the governor is set to
performance, so that the status of a result is carried by the result itself.

**5.3.4 Generalisation.** The two failures of §5.2 and §5.3 are instances of one error:
reporting a measurement without reporting the conditions under which the measuring apparatus
is valid. In §5.2 the apparatus was the histogram; in §5.3 it was the machine. The ordering
of checks that follows is: establish the instrument's resolution relative to the expected
variance; record the environment; only then interpret the numbers.

---

## 5.4 Topology: simultaneous multithreading and the portability of processor numbering

**5.4.1 The defect.** Thread placement initially assigned roles to consecutive logical
processor identifiers: the stream reader to processor 0, the action writer to processor 1, and
workers from processor 2 upward. On the measurement host the sibling relation is
`{0,1}, {2,3}, {4,5}, {6,7}`, so this assignment placed the reader and the writer — two of the
three hottest threads in the system — on two hardware threads of a *single* physical core,
sharing its execution resources and private cache levels, while one physical core carried no
data-plane thread at all `[VERIFY]`.

**5.4.2 Why no fixed stride is correct.** The mapping from logical processor identifiers to
physical cores is established by firmware and is not portable. Two layouts are common. Under
*adjacent* numbering, siblings are consecutive, as on the measurement host and on
virtualised platforms generally. Under *split* numbering, all physical cores are enumerated
before any second hardware thread, so that processors `i` and `i + n/2` are siblings; this is
common on bare-metal server and desktop parts. An assignment with stride 1 collides on the
first layout; an assignment with stride `n/2` collides on the second. The defect is therefore
not a mis-chosen constant but the assumption that a constant exists.

**5.4.3 Resolution.** Placement is derived at startup from the sibling sets the operating
system exports per processor, from which one representative processor per physical core is
obtained. Roles are assigned over the representatives first, and only when they are exhausted
does the allocator begin using second hardware threads. Role order is deliberate: workers are
allocated before the reader and writer, because a worker executes the complete decision path
and is therefore the thread whose latency the measurement reports.

Where the data-plane thread count exceeds the physical core count the system cannot avoid
sharing, and it reports that it has done so. A run in which roles share physical cores is
marked, on the grounds that a tail latency measured under sibling contention cannot be
attributed to the software under test. On the measurement host, four physical cores admit the
four threads of a two-shard configuration and cannot admit the six of a four-shard
configuration, which is the fact that §5.5 exploits.

`[FIG 5.2]` Sibling map of the measurement host with the defective and corrected role
assignments shown against physical core boundaries.

---

## 5.5 Experimental design: un-confounding placement from thread count

The first attempt to quantify the effect of placement compared a pinned two-shard
configuration against an unpinned four-shard configuration. These differ in two variables,
so the observed difference was not attributable to either. The design was replaced with a
full factorial crossing of placement against shard count:

| | 2 shards (4 data-plane threads) | 4 shards (6 data-plane threads) |
|---|---|---|
| **unpinned** | scheduler-placed | scheduler-placed |
| **pinned** | one role per physical core | workers on cores; reader and writer on siblings |

The two comparisons the design supports are distinct and both are required. Holding shard
count at two isolates placement at a thread count that fits the host. Holding placement at
*unpinned* isolates thread count at fixed placement policy. The upper-right cell is
additionally informative because, as §5.4.3 establishes, it is the cell in which the host
cannot satisfy the placement request, and it is marked as such in the output.

Each cell was executed in four independent repetitions of 20 s, yielding approximately 17,000
decisions per repetition `[VERIFY]`. Replication is not decoration here. An earlier result in
this work was quoted from a single run and later found, on three repetitions, to be an
outlier by a factor of 2.4 `[VERIFY]`; the present chapter reports no single-run figure.
Ranges rather than standard deviations are given, since four repetitions do not support a
variance estimate and the range is the honest summary.

---

## 5.6 Results

**Regime R1** — 37 kb reference, constant-dwell simulation, four repetitions per cell, median
with observed range in microseconds.

| configuration | p50 | p90 | p99 | p99.9 |
|---|---|---|---|---|
| pinned-2 | 16.3 [15.2–16.5] | 24.7 [23.1–25.7] | 49.2 [47.0–50.9] | 158 [139–246] |
| unpinned-2 | 15.8 [15.6–16.6] | 25.1 [24.2–26.4] | 50.5 [44.9–104.5] | 166 [117–240] |
| pinned-4 | 18.7 [18.0–19.7] | 31.1 [27.9–35.4] | 70.4 [65.7–142.7] | 313 [234–614] |
| unpinned-4 | 18.4 [17.9–18.8] | 31.6 [31.3–33.4] | 83.9 [72.1–350.5] | 387 [352–2519] |

`[TABLE 5.1]`

**Thread count dominates placement.** Both two-shard cells are superior to both four-shard
cells at every percentile reported. The explanation is available without appeal to placement
policy: six data-plane threads do not fit four physical cores, so the four-shard cells
oversubscribe regardless of how threads are assigned, and the unpinned four-shard cell
produced a maximum of 12.5 ms `[VERIFY]`. The design guidance is to size shard count to the
available physical cores before considering placement.

**Placement does not reduce the median.** At a fixed two shards the pinned and unpinned
ranges overlap at both p50 (15.2–16.5 against 15.6–16.6) and p90 (23.1–25.7 against
24.2–26.4). No median improvement attributable to pinning is detectable at this sample size,
and none is claimed.

**Placement makes the tail reproducible.** The 99th percentile across the four repetitions is

&nbsp;&nbsp;&nbsp;&nbsp;pinned: 46.99, 48.32, 49.97, 50.88 µs — spread 3.9 µs (8%)
&nbsp;&nbsp;&nbsp;&nbsp;unpinned: 44.86, 45.52, 55.45, 104.45 µs — spread 59.6 µs (133%)

`[VERIFY]`. The unpinned configuration is sometimes as fast and sometimes 2.3× worse, which
is the signature of migration: an unpinned worker may be moved between cores mid-run, and
each migration cold-starts its working set. The defensible statement about pinning is
therefore about variance, not about location — it does not make the system faster, it makes
the measurement repeatable. This is a weaker claim than the one originally drawn from the
withdrawn data of §5.2.2, and unlike that claim it is supported by replication.

**Regime R3** — 64 Mb reference, variable-dwell simulation, frozen configuration. Per-chunk
decision latency has a median of 134–174 µs and a 99th percentile of 630–705 µs, with
3.9–4.4 GB resident and no chunks dropped at a sustained 975 chunk/s `[VERIFY]`
`[TABLE 5.2]`. The order-of-magnitude increase over R1 is accounted for by index scale and by
the segmentation and seeding configuration frozen in Chapter 6, not by the data plane; the
decomposition belongs with that material. Relating this figure to the budget derived in
Chapter 3, §3.1.3 — an admissible mean service time of 1.56 ms at two workers and 512
channels — gives a utilisation of approximately 0.09–0.11.

---

## 5.7 Separating intrinsic from extrinsic variation

The central interpretive question of this chapter is which of the reported statistics are
properties of the implementation and which are properties of the platform.

**5.7.1 The platform.** All measurements were taken in a virtual machine under a Type-1
hypervisor on a consumer host. Two mechanisms matter. The guest's virtual processors are
scheduled by the host, so a virtual processor may be descheduled at an arbitrary point,
including within a single decision; in the guest's own timeline this is indistinguishable
from a decision that took longer. Separately, although the host exposes an invariant TSC and
the guest selects it as clocksource, timekeeping is host-mediated.

**5.7.2 Why the contamination is concentrated in the upper tail.** Let `p` be the probability
that a given decision is interrupted by a host-scheduling event and `D` the typical duration
of such an interruption. Observed latency is then approximately the intrinsic cost plus `D`
with probability `p`. For small `p`, quantiles below the `(1 − p)` point are essentially
unaffected, while quantiles above it are dominated by `D` rather than by the intrinsic
distribution. Interruptions of this kind are rare and large, so the effect is negligible at
the median and determinative at the 99th percentile and above. This is a structural argument,
not an appeal to the observed numbers, and it predicts the pattern seen in §5.6: the medians
are tight across repetitions while the upper percentiles are not.

**5.7.3 What is claimed.** The median and the 90th percentile are reported as properties of
the data plane. They are reproducible across repetitions, across placement policies at fixed
thread count, and — for the unpinned four-shard cell — across the environment change of §5.3
once that environment was corrected. The 99th percentile and above are reported as upper
bounds containing an unquantified platform contribution. The observation that the pinned
configuration's p99 spread is 8% where the unpinned is 133% bounds, but does not isolate, the
platform's contribution: pinning removes guest-side migration while leaving host-side
preemption untouched, so the residual 8% is an upper bound on intrinsic tail variance under
this platform and the 133% is a lower bound on the combined effect.

**5.7.4 What would be required to measure the tail.** A defensible tail measurement requires
the measured cores to be removed from the general scheduler's remit, tickless operation on
those cores, device interrupt affinity moved away from them, a fixed frequency governor, and
absence of virtualisation. These are kernel-boot and host-configuration properties rather
than application settings, which is why the harness treats them as a precondition for
labelling a result rather than as parameters. A further consideration is that a mobile part
with aggressive thermal management is unsuitable even bare metal, since thermal excursions
appear in the tail; a platform with cores to spare for isolation is preferable.

---

## 5.8 Comparison with existing clients

Comparisons are reported with their boundaries stated, because the three systems do not
divide the work identically and a ratio between differently scoped quantities is not a
performance claim.

**5.8.1 A basecalling-based client.** readfish was executed against the same simulator on the
same host with a no-operation caller, which removes basecalling and mapping and therefore
measures its orchestration cost alone. Per-chunk orchestration was 124 µs, at 15.6–25.3% of
one core and 86–663 MB resident `[VERIFY]`. The present system is faster per chunk against
this lower bound on readfish's cost, and consumes 8–12× more processor time: 182–202% of one
core, because workers spin before yielding (Chapter 3, §3.7.4). Both halves are reported.
The comparison establishes that the orchestration layer of a basecalling client is already
substantial relative to a complete signal-space decision, which is the architectural point;
it does not establish superiority, since a production readfish performs work this
configuration omits.

**5.8.2 A signal-space mapper.** RawHash2 was executed on identical simulated reads with the
same pore model, single-threaded, requiring 9.66 ms of processor time per read against
approximately 1.34 ms for the present system `[VERIFY]`. This ratio must be read together
with the accuracy figures of Chapter 6: the present system resolves 18.8% of reads where
RawHash2 resolves 92.4% on the same data. **A latency ratio between a system that resolves
reads and one that largely does not is a bound on the cost of the data plane, not a
performance result, and it is not presented as one.** Its legitimate use is the one made in
Chapter 7: the decision path is not where the system's deficiency lies.

A mechanism has been proposed for part of the difference — that a batch chainer operating in
read-until mode re-chains all accumulated anchors on each arriving chunk, giving work
quadratic in chunk count, whereas the present system's per-channel state is extended
incrementally and is linear. This mechanism is consistent with the architecture of both
systems but **was not instrumented in this work and is stated as a hypothesis.** Establishing
it requires measuring RawHash2's per-chunk cost as a function of accumulated chunk count,
which is left to further work.

---

## 5.9 Threats to validity

**Platform.** Addressed in §5.7; p99 and above are not claimed.

**Single-host measurement.** All figures derive from one machine, a four-core mobile part.
Core count interacts with the principal finding of §5.6 — that thread count dominates
placement — and a host with more physical cores would move the threshold at which
oversubscription occurs without, on the argument given, changing its nature.

**Simulated input.** Arrival is generated by a simulator and is more regular than a flow
cell's. In particular the simulator used for R1 and R2 emits a fixed number of samples per
base (Chapter 6), so inter-arrival variability is understated. The effect on a loss system is
to understate drop probability; no drops were observed, so the figure reported is an
optimistic bound on that property.

**Decision-path scope.** Reported latency covers the interval from the transport read
returning to the decision being reached, and excludes the network, the server and the action
round trip. The deadline-miss counter is the complementary measurement and is reported
separately; a complete end-to-end deadline analysis requires hardware.

**Absence of a deadline-miss result.** No decisions were observed to miss their deadline in
the configurations reported, so the counter establishes only that misses were absent at
utilisation ≈ 0.1. The quantity is reported for completeness and is not evidence of margin
at higher utilisation.

---

## 5.10 Summary

The per-chunk decision latency of the data plane has a median of 16.3 µs and a 90th
percentile of 24.7 µs in the two-shard pinned configuration on a 37 kb reference, and a
median of 134–174 µs in the frozen configuration on a 64 Mb reference, the latter
corresponding to a utilisation of approximately 0.1 against the budget derived in Chapter 3.
Thread count dominates placement: configurations whose data-plane thread count exceeds the
host's physical core count are inferior at every percentile. Pinning does not reduce the
median; it reduces the run-to-run spread of the 99th percentile from 133% to 8%.

Three measurement errors were identified in the course of obtaining these figures, each of
which had already produced a result that was subsequently withdrawn: a histogram whose
resolution manufactured apparent reproducibility, an exhausted host filesystem that displaced
the median by approximately one third, and a thread-placement rule that assumed a portable
relationship between logical processor identifiers and physical cores. The methodological
conclusion is that at this timescale the apparatus and its environment require the same
scrutiny as the system under test, and that a measurement reported without its environment is
not reproducible in principle. The harness constructed in response records that environment
with every run and withholds the label of a publishable result unless the platform satisfies
the conditions of §5.7.4.
