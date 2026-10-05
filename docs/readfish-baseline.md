# The readfish baseline, reproduced

Everything here was run on the same WSL2 host, against the same Icarust
simulation, in the same session as the numbers it is compared against. That is
the only reason the comparison is worth anything: neither side gets a friendlier
machine than the other.

It is still **not** a bare-metal measurement. `systemd-detect-virt` reports `wsl`.

## 1. Environment setup

readfish 2024.3.0 in its own venv. It pins an older `numpy`/`cattrs` world than the
daemon's tooling, so it does not share an environment with anything else.

```bash
sudo apt-get install -y python3-venv python3-dev build-essential zlib1g-dev
```

```bash
python3 -m venv /root/rfenv && source /root/rfenv/bin/activate
```

```bash
pip install --upgrade pip && pip install "readfish[all]==2024.3.0"
```

That pulls `minknow-api==6.10.3`, `mappy==2.31`, `mappy-rs==0.0.7`,
`ont-pybasecall-client-lib` and `readfish_summarise`. No separate `minimap2`
binary is needed — readfish maps through the `mappy`/`mappy-rs` bindings, and
`mappy` builds a 64 Mb chr20 index in about 3.5 s, so a prebuilt `.mmi` is not
worth the extra moving part.

## 2. TOML

`chr20_targets.toml`. The target name must match the FASTA header exactly:
`hg38_chr20.fa` begins `>NC_000020.11`, not `>chr20`, and readfish silently
targets nothing if you get this wrong — `readfish validate` is what catches it.

```toml
[caller_settings.no_op]

[mapper_settings.mappy_rs]
fn_idx_in = "/root/icarust/docker/squiggle_arrs/hg38_chr20.fa"
n_threads = 2

[[regions]]
name = "chr20_enrich"
min_chunks = 1
max_chunks = 10          # matches our PolicyConfig::max_chunks
targets = ["NC_000020.11"]
single_off = "unblock"
multi_off = "unblock"
single_on = "stop_receiving"
multi_on = "stop_receiving"
no_seq = "proceed"
no_map = "proceed"
```

Validate before running, which also prints the fraction of the reference covered:

```bash
readfish validate /root/rf/chr20_targets.toml
```

**`no_op` is a deliberate choice and it bounds what this can measure.** Our daemon
decides in signal space and never basecalls, so a basecalling readfish is not the
comparable configuration — and no basecall server exists on this host anyway.
With `no_op` readfish produces no sequence, so the mapper is configured but never
receives a query and every chunk takes the `no_seq → proceed` path. The resulting
figures are therefore a **floor** on readfish's real per-chunk cost, not the whole
pipeline. A production readfish adds basecalling and mapping on top.

## 3. Launch

```bash
source /root/rfenv/bin/activate && export MINKNOW_TRUSTED_CA=/root/icarust/static/tls_certs_fixed/ca.crt
```

```bash
readfish unblock-all --host 127.0.0.1 --port 10000 --device Bantersaurus --experiment-name rf --throttle 0.4
```

```bash
readfish targets --toml /root/rf/chr20_targets.toml --host 127.0.0.1 --port 10000 --device Bantersaurus --experiment-name rf --throttle 0.4
```

### Four things that have to be right, none of them documented together anywhere

1. **`MINKNOW_TRUSTED_CA` must point at a CA that actually signs the server
   certificate.** Icarust's shipped certs are broken three ways (no
   `subjectAltName`, expired, not signed by the shipped CA); `tls_certs_fixed`
   holds replacements minted from Icarust's own key.
2. **`--device` is Icarust's `device_id`** (`Bantersaurus`), not its `position`
   field (`FenceSitter`). The manager advertises the former.
3. **`--port` is the MANAGER port (10000), not the position port (10001).**
   Our daemon's `--target` takes the *position*, so the two tools want different
   ports against the same simulator. Pass the position port and readfish dies with
   a bare `UNIMPLEMENTED`, because `readfish/_compatibility.py` calls
   `manager.get_version_info()` on whatever port it is given.
4. **Icarust's read-classification map predates MinKNOW 6.x** and lacks `strand2`
   and `short_strand`, which readfish 2024.3.0 hardcodes in its prefilter set. It
   raises `KeyError` before streaming. Added to
   `src/impl_services/analysis_configuration.rs` as placeholder codes, since
   Icarust never emits either and MinKNOW's real codes are unpublished.

## 4. Results

Same host, same Icarust instance, ~26-32 s per run. "per chunk" is the honest unit
for both: readfish's `NNNR/T s` batch line counts one entry per read *chunk*
received in that iteration, so `T/NNN` is its per-chunk cost, directly comparable
to our per-chunk decision latency.

| | ours, chr20 | ours, 37 kb toy | readfish unblock-all | readfish targets |
|---|---|---|---|---|
| reference | 64.4 Mb | 37 kb | none | 64.4 Mb |
| per chunk (p50) | 25.6 us | 16.7 us | 124 us | 66 us |
| CPU | **182%** | **202%** | 15.6% | 25.3% |
| peak RSS | **1.49 GB** | 152 MB | 86 MB | 663 MB |
| accepted | **0** | 854 / 1366 | n/a | n/a |

### We win on latency, and lose on everything else

**Latency: 4.9x faster per chunk at chr20 scale** (25.6 vs 124 us), 7.4x on the toy
reference. That holds against readfish's *floor* — no basecalling, no mapping —
so the real gap to a production readfish is larger. This is the one claim that
survives.

**CPU: we are 8-12x worse.** 182-202% of a core against readfish's 15.6-25.3%.
This is not a defect, it is the design: our workers spin (`IdleWait` spins 2048
iterations before yielding) to avoid paying a wakeup on the critical path, while
readfish polls every 400 ms and sleeps in between. We buy latency with CPU. On a
dedicated sequencing host that is the right trade; it should be stated as a trade
rather than omitted, and "efficiency" is the wrong word for what we provide.

**Memory: 2.25x worse on the same reference.** 1.49 GB against readfish's 663 MB
for the same 64 Mb chr20. Our quantised-event index — one entry per distinct key
plus a positions array — is simply heavier than minimap2's. 1.49 GB is affordable
on a GridION and awkward on anything smaller.

### The finding that matters more than any of the above

**At chr20 scale the accept path produces ZERO accepts.** 0 of 763 decisions,
where 13 of the 29 sequenced transcripts map to chr20 and several hundred accepts
were expected. On the 37 kb toy reference the same build accepts 854 of 1366
(62.5%).

The mechanism is visible in one number: **candidates per chunk went from 0.99 to
95.1**, a 96x flood. `ChannelVotes` holds 16 diagonal slots and evicts the weakest
only when it has a single vote. At one candidate per chunk a true diagonal
accumulates undisturbed; at ninety-five it is evicted by noise before it can reach
`accept_votes = 5`. The 16-slot table and the vote threshold were both tuned
against a reference 1700x too small.

This is the project's central claim failing at realistic scale, and it was hidden
until now because every live run had used a 37 kb reference that fits entirely in
cache. The offline bench reported 89.3% TPR at 10 chunks *on chr20*, so the
offline and live paths disagree and at least one of them is measuring something
other than what it claims. That discrepancy is the next thing to resolve, and
nothing else in the paper should be written until it is.
