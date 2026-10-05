# Patches to third-party tools

## `icarust-dwell-variability.patch`

Adds dwell-time variability to [Icarust](https://github.com/LooseLab/Icarust), applied
against `src/simulation.rs`.

### Why

Icarust emits exactly `samples_per_base` samples for every k-mer:

```rust
// N sampling for each base (sample_rate / bases per second )
// This could also be worked out from profile.dwell_mean
for _ in 0..samples_per_base {
```

Its own comment acknowledges `dwell_mean` and does not use it, so every simulated read
translocates at a perfectly constant rate. Real pores do not: dwell per k-mer is variable
and right-skewed.

The assumption is invisible to a basecaller-based client, which is most of them, and
decisive for any client that segments raw signal by time. Fixed-width segmentation is
*exactly* correct under constant dwell, so a signal-space client can look accurate against
Icarust and fail on a sequencer. Measured here, same daemon, same reference, same 22 s run:

| Icarust | fixed-width segmentation | event detection |
|---|---|---|
| stock (constant dwell) | 43.5% accept | 9.6% accept |
| `ICARUST_DWELL_CV=0.35` | **0.5% accept** | **9.6% accept** |

Fixed-width falls 87x once dwell varies. Event detection is unchanged between the two,
which is the definition of dwell invariance and the reason it is the right segmentation.
Without this patch neither number is measurable and the stock simulator reports the
*worse* method as 4.5x better.

### Usage

```bash
patch -p0 < icarust-dwell-variability.patch && cargo build --release
```

```bash
ICARUST_DWELL_CV=0.35 ./target/release/icarust -c config.ini -s profile.toml
```

Unset, or 0, reproduces stock behaviour exactly, so existing results are unaffected.
Lognormal about `samples_per_base`, since dwell is positive and right-skewed and it is the
long holds that do the damage; published R10 variability is roughly CV 0.3-0.5. Clamped to
at least one sample (a k-mer contributing nothing is a skip, a different phenomenon) and
at most eight times nominal (beyond that it is a pore block).

### Note for upstreaming

An environment variable keeps this a minimal, revertible change to a third-party tool.
A patch offered upstream should read the value from the sample profile TOML instead,
along`mean_read_length`, and ideally model skips as well.
