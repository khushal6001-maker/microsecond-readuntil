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

## `rawhash2-reference-quantise-dump.patch`

Exposes RawHash2's reference-side per-event quantised symbols, applied against
`src/{roptions.h,rsketch.c,rindex.c,main.cpp}` of RawHash2 v2.1. Needed for Chapter 7
§7.4.5, which measures per-event bucket agreement for RawHash2 on the same input as this
pipeline.

### Why

RawHash2 already ships `--out-quantize`, which dumps the quantised symbol stream for
**signal** input. The reference path in `rindex.c` passes `out = 0` to `ri_sketch`
unconditionally, so the reference half of the comparison is not obtainable. Agreement is by
definition a property of the two sides together, so without this there is no measurement.

`--out-quantize-ref` dumps the reference stream during index construction as
`>NAME<TAB>{+,-}` followed by comma-separated `POS:SYMBOL` pairs. Positions are required,
not decorative: `--sig-diff` makes event retention data-dependent, so without them there is
no way to locate the reference window corresponding to a read's true locus. The new `out == 2`
mode carries that format and leaves the stock `out == 1` output byte-identical.

One further change is a defect fix rather than a feature. The stock `--out-quantize` prints
`"%s\n"` for the read name and `ri_sketch_reg` does not newline-terminate the symbol list, so
with more than one read the records run together and the output cannot be parsed. The patch
prints `"\n%s\n"`.

### Usage

```bash
patch -p0 < rawhash2-reference-quantise-dump.patch && make NOHDF5=1 NOPOD5=1
```

```bash
rawhash2 -d /tmp/discard.ind -p squig_r10_9mer.tsv -k 9 --level_column 1 \
         -w 0 -q 4 -e 8 --sig-diff 0.35 --out-quantize-ref ref.fa > ref_symbols.txt
rawhash2 -p squig_r10_9mer.tsv -k 9 --level_column 1 \
         -w 0 -q 4 -e 8 --sig-diff 0.35 --out-quantize reads.slow5 > query_symbols.txt
```

`ri_sketch_reg` does not populate the index when `out` is set, so the index file named by
`-d` is written empty and is discarded. Nothing else changes: with neither flag set the
binary is behaviourally identical to stock.

### Note for upstreaming

The reference dump is the natural counterpart of a facility the tool already has, and the
newline fix is a bug fix worth offering on its own. A cleaner version would share one
`--out-quantize[=target|query|both]` option rather than adding a second flag.
