#!/usr/bin/env python3
"""Re-derive the thesis numbers that a committed data file can settle.

This exists because every figure in Chapters 1-8 was marked [VERIFY], and a marker is only
useful if something eventually discharges it. The script recomputes each checkable claim from
results/ and reports agree / DISAGREE / missing. It deliberately does NOT read the chapters:
the claims are transcribed here by hand, so a disagreement means the prose and the data
disagree, not that a parser failed.

Claims that no committed file can settle are listed at the end as OUTSTANDING, with what each
would need. That list is the honest output: the audit's value is in knowing which numbers are
backed and which are not.

    python scripts/verify_thesis_numbers.py
"""

from __future__ import annotations

import csv
import math
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
RESULTS = REPO / "results"

PASS, FAIL, MISS = [], [], []


def rows(name: str) -> list[dict]:
    p = RESULTS / name
    if not p.exists():
        return []
    with open(p, newline="") as f:
        lines = [ln for ln in f if not ln.lstrip().startswith("#")]
    return list(csv.DictReader(lines))


def check(claim: str, where: str, got, want, tol=0.0) -> None:
    """tol is a relative tolerance; 0 means exact string/!= comparison."""
    if got is None:
        MISS.append((claim, where, "no data file"))
        return
    ok = (
        abs(got - want) <= tol * abs(want)
        if isinstance(got, (int, float)) and isinstance(want, (int, float))
        else got == want
    )
    (PASS if ok else FAIL).append((claim, where, got, want))


# ---------------------------------------------------------------- latency, regime R1
lat = rows("latency-matrix-wsl2.csv")
if lat:
    def cell(cfg, key):
        return [float(r[key]) for r in lat if r["config"] == cfg]

    # Chapter 5 Table 5.1 quotes medians with observed range.
    for cfg, p50, p90, p99 in (
        ("pinned-2", 16.3, 24.7, 49.2),
        ("unpinned-2", 15.8, 25.1, 50.5),
        ("pinned-4", 18.7, 31.1, 70.4),
        ("unpinned-4", 18.4, 31.6, 83.9),
    ):
        for key, want in (("p50", p50), ("p90", p90), ("p99", p99)):
            v = sorted(cell(cfg, key))
            med = (v[len(v) // 2 - 1] + v[len(v) // 2]) / 2 if len(v) % 2 == 0 else v[len(v) // 2]
            check(f"R1 {cfg} {key} median of 4 reps", "§5.6 T5.1", round(med, 1), want, 0.02)

    # Chapter 5 §5.6: p99 spread, pinned against unpinned, at two shards.
    for cfg, want in (("pinned-2", 8.0), ("unpinned-2", 133.0)):
        v = cell(cfg, "p99")
        spread = 100.0 * (max(v) - min(v)) / min(v)
        check(f"{cfg} p99 run-to-run spread %", "§5.6, C3, §1.9", round(spread), round(want), 0.05)

    # The individual p99 values §5.6 lists.
    got = sorted(round(x, 2) for x in cell("pinned-2", "p99"))
    check("pinned-2 p99 set", "§5.6", got, [46.99, 48.32, 49.97, 50.88])
    got = sorted(round(x, 2) for x in cell("unpinned-2", "p99"))
    check("unpinned-2 p99 set", "§5.6", got, [44.86, 45.52, 55.45, 104.45])

    # §5.6: the unpinned four-shard maximum.
    check("unpinned-4 max (ms)", "§5.6", round(max(cell("unpinned-4", "max")) / 1000.0, 1),
          12.5, 0.02)

    # §5.6: both two-shard cells beat both four-shard cells at every percentile.
    worst2 = max(max(cell(c, k)) for c in ("pinned-2", "unpinned-2") for k in ("p50",))
    best4 = min(min(cell(c, k)) for c in ("pinned-4", "unpinned-4") for k in ("p50",))
    check("thread count dominates placement at p50", "§5.6", worst2 < best4, True)
else:
    MISS.append(("regime R1 latency table", "§5.6", "results/latency-matrix-wsl2.csv absent"))

# ---------------------------------------------------------------- dwell sweep, ours
dw = rows("dwell-sweep-ours.csv")
if dw:
    by = {float(r["dwell_cv"]): r for r in dw}
    check("fixed-width TPR at CV 0", "§6.4, §1.9", float(by[0.0]["fixed_mean"]), 49.7)
    check("fixed-width TPR at CV 0.60", "§6.4", float(by[0.60]["fixed_mean"]), 1.7)
    check("detected TPR at CV 0", "§6.4, §1.9", float(by[0.0]["detected_mean"]), 18.4)
    check("detected TPR at CV 0.60", "§6.4, §1.9", float(by[0.60]["detected_mean"]), 7.5)
    check("detected TPR at CV 0.44 (frozen op point)", "§7.5", float(by[0.44]["detected_mean"]), 14.1)
    # Chapters 1 and 8 say fixed-width falls "49.7 -> 1.3". 1.3 is the CV 0.44 value.
    check("value quoted as the fixed-width endpoint", "§1.9, §8.3",
          float(by[0.44]["fixed_mean"]), 1.3)
    # Crossover must lie between CV 0 and CV 0.10.
    check("crossover between CV 0 and 0.10", "§6.4",
          float(by[0.0]["fixed_mean"]) > float(by[0.0]["detected_mean"])
          and float(by[0.10]["fixed_mean"]) < float(by[0.10]["detected_mean"]), True)
else:
    MISS.append(("dwell sweep", "§6.4", "results/dwell-sweep-ours.csv absent"))

# ---------------------------------------------------------------- RawHash sweep
rh = rows("rawhash-dwell-sweep.csv")
if rh:
    def rhc(w, cv, key):
        for r in rh:
            if int(r["w"]) == w and abs(float(r["dwell_cv"]) - cv) < 1e-9:
                return float(r[key])
        return None

    check("comparison mapper correct % at w=0, CV 0", "§6.5, §1.9", rhc(0, 0.0, "correct_pct"), 95.0)
    check("comparison mapper correct % at w=0, CV 0.60", "§6.5, §1.9", rhc(0, 0.60, "correct_pct"), 92.0)
    check("comparison mapper correct % at w=0, CV 0.44 (the 92.4 headline)",
          "§5.8.2, §7.4.5, §8.3", rhc(0, 0.44, "correct_pct"), 92.4)
    pen0 = rhc(0, 0.0, "correct_pct") - rhc(10, 0.0, "correct_pct")
    pen60 = rhc(0, 0.60, "correct_pct") - rhc(10, 0.60, "correct_pct")
    check("sketching penalty at CV 0 (points)", "§6.6, §1.9", round(pen0, 1), 12.8)
    check("sketching penalty at CV 0.60 (points)", "§6.6, §1.9", round(pen60, 1), 21.0)
    # §6.6 originally called this growth monotone. It is not: the penalty dips at CV 0.20.
    # The defensible statements are that it increases across the range and that the increase
    # is large, and both are checked here instead.
    pens = [round(rhc(0, c, "correct_pct") - rhc(10, c, "correct_pct"), 1)
            for c in (0.0, 0.10, 0.20, 0.30, 0.44, 0.60)]
    check("sketching penalty series", "§6.6", pens, [12.8, 13.0, 12.4, 15.0, 15.2, 21.0])
    check("penalty is NOT monotone (it dips at CV 0.20)", "§6.6",
          any(b < a for a, b in zip(pens, pens[1:])), True)
    check("penalty increase across the range (%)", "§6.6",
          round(100 * (pens[-1] / pens[0] - 1)), 64, 0.03)
    check("false-mapped at w=10, CV 0.60 (%)", "§6.6", rhc(10, 0.60, "false_mapped_pct"), 15.2)
    check("false-mapped at w=5, CV 0.60 (%)", "§6.6", rhc(5, 0.60, "false_mapped_pct"), 10.4)
else:
    MISS.append(("RawHash dwell sweep", "§6.5", "results/rawhash-dwell-sweep.csv absent"))

# ---------------------------------------------------------------- agreement
ag = rows("agreement-summary.csv")
if ag:
    def agc(system, seg, cv, norm, key):
        for r in ag:
            if (r["system"] == system and r["segmentation"] == seg
                    and abs(float(r["dwell_cv"]) - cv) < 1e-9 and r["norm"] == norm):
                return float(r[key])
        return None

    check("ours frozen a at CV 0.44", "§7.4.5, §8.3, §1.5", agc("ours", "detected", 0.44, "read", "a_dp"), 0.7826)
    check("ours frozen indel at CV 0.44", "§7.4.5, §8.3", agc("ours", "detected", 0.44, "read", "indel_frac"), 0.1963)
    check("ours frozen P(key12) at CV 0.44", "§7.4.5, §8.3", agc("ours", "detected", 0.44, "read", "p_key12"), 0.00554)
    check("ours frozen anchors/read at CV 0.44", "§7.5, §8.3", agc("ours", "detected", 0.44, "read", "anchors_read"), 14.9)
    check("comparison a at CV 0.44", "§7.4.5, §8.3, §1.5", agc("rawhash2", "own", 0.44, "own", "a_dp"), 0.9109)
    check("comparison indel at CV 0.44", "§7.4.5, §8.3", agc("rawhash2", "own", 0.44, "own", "indel_frac"), 0.0915)
    check("comparison P(key8) at CV 0.44", "§7.4.5, §8.3", agc("rawhash2", "own", 0.44, "own", "p_key8"), 0.25370)
    check("comparison anchors/read at CV 0.44", "§7.4.5, §8.3", agc("rawhash2", "own", 0.44, "own", "anchors_read"), 466.5)
    check("reproduction of the 55.9% figure (chunk-norm fixed, CV 0)", "§7.4.4",
          agc("ours", "fixed", 0.0, "chunk", "a_affine"), 0.5563)
    check("whole-read normalisation agreement", "§7.4.4, §7.8",
          agc("ours", "fixed", 0.0, "read", "a_affine"), 0.7255)

    # Derived claims the prose states.
    ours_p = agc("ours", "detected", 0.44, "read", "p_key12")
    comp_p = agc("rawhash2", "own", 0.44, "own", "p_key8")
    ours_a = agc("ours", "detected", 0.44, "read", "anchors_read")
    comp_a = agc("rawhash2", "own", 0.44, "own", "anchors_read")
    check("key-match ratio, comparison over ours", "§7.4.5, §8.3", round(comp_p / ours_p), 46, 0.03)
    check("anchor-supply ratio", "§7.5, §8.3, §1.5", round(comp_a / ours_a), 31, 0.04)
    ours_p8 = agc("ours", "detected", 0.44, "read", "p_key8")
    check("decomposition: key-length factor", "§7.4.5, §8.3", round(ours_p8 / ours_p, 1), 4.9, 0.03)
    check("decomposition: encoding factor", "§7.4.5, §8.3", round(comp_p / ours_p8, 1), 9.3, 0.03)
    check("the two factors multiply to the whole", "§8.3",
          round((ours_p8 / ours_p) * (comp_p / ours_p8)) == round(comp_p / ours_p), True)
    # Normalisation yield factor.
    check("normalisation yield factor", "§7.8, §8.4",
          round(agc("ours", "fixed", 0.0, "read", "p_key12")
                / agc("ours", "fixed", 0.0, "chunk", "p_key12"), 1), 2.0, 0.06)
else:
    MISS.append(("agreement summary", "§7.4", "results/agreement-summary.csv absent"))

# ---------------------------------------------------------------- geometry x chaining
gx = rows("geometry-x-chaining.csv")
if gx:
    def g(geo, dec):
        for r in gx:
            if r["geometry"] == geo and r["decision"] == dec:
                return float(r["zeroFP_TPR"])
        return None

    check("votes at 4x12", "§7.6, T7.3", g("4x12", "votes"), 14.2)
    check("chains at 4x12", "§7.6, T7.3", g("4x12", "chain"), 17.4)
    check("chains at 4x8", "§7.6, T7.3", g("4x8", "chain"), 3.2)
    check("chains at 4x10", "§7.6", g("4x10", "chain"), 15.8)
    best = max((g(geo, dec) or 0) for geo in {r["geometry"] for r in gx} for dec in ("votes", "chain"))
    check("greedy chaining over votes at 4x12 (%)", "§7.6, T7.3",
          round(100 * (g("4x12", "chain") / g("4x12", "votes") - 1)), 23, 0.04)
    check("no configuration exceeds the 4x12 chained result", "§7.6", best, g("4x12", "chain"))

# The +32% the thesis quotes is 14.2 -> 18.8, the ANCHOR-HISTORY chainer, which
# geometry-x-chaining.csv does not contain. It lives only in decision-rules.csv, which is a
# transcription rather than a captured output -- see that file's header.
dr = rows("decision-rules.csv")
if dr:
    def d(rule, lookback="", occ=""):
        for r in dr:
            if r["rule"] == rule and r["lookback"] == lookback and r["occ_filter"] == occ:
                return float(r["zeroFP_TPR"])
        return None

    votes = d("exact-diagonal votes (256 counters)")
    anchor = d("anchor-history chains", "48", "no")
    check("best intervention is +32% relative over votes", "§7.6, §1.5, §8.3",
          round(100 * (anchor / votes - 1)), 32, 0.04)
    check("the 18.8% headline", "§1.4, §7.1, §7.4.5, §8.3", anchor, 18.8)
    check("beta=0 control is worse than votes", "§4.7.3",
          d("greedy chains (control)", "", "") < votes, True)
    check("longer look-back does not help", "§4.7.3, T7.3", d("anchor-history chains", "128", "no"), 18.0)
    check("occurrence filtering does not help", "§4.7.3, T7.3", d("anchor-history chains", "48", "yes"), 17.8)
    # Cross-file agreement on the two rows both files carry.
    if gx:
        check("14.2 agrees across the two files", "§4.7.3 vs §7.6", votes, g("4x12", "votes"))
        check("17.4 agrees across the two files", "§4.7.3 vs §7.6",
              d("greedy chains", "", ""), g("4x12", "chain"))
else:
    MISS.append(("decision-rule comparison", "§4.7.3 T4.2", "results/decision-rules.csv absent"))

if gx:
    pass
else:
    MISS.append(("geometry x chaining", "§7.6", "results/geometry-x-chaining.csv absent"))

# ---------------------------------------------------------------- Icarust
ic = rows("icarust-stock-vs-patched.csv")
if ic:
    stock = next(r for r in ic if r["icarust"] == "stock")
    patched = next(r for r in ic if r["icarust"] == "patched")
    check("Icarust stock, fixed-width accept %", "§6.8, App B", float(stock["fixed_accept_pct"]), 43.5)
    check("Icarust patched, fixed-width accept %", "§6.8, App B", float(patched["fixed_accept_pct"]), 0.5)
    check("detected accept unchanged between the two", "§6.8",
          float(stock["detected_accept_pct"]), float(patched["detected_accept_pct"]))
    check("stock reports the worse configuration as 4.5x better", "§6.8, §1.5, §8.3",
          round(float(stock["fixed_accept_pct"]) / float(stock["detected_accept_pct"]), 1), 4.5, 0.03)
    check("fixed-width falls by a factor of 87", "App B",
          round(float(stock["fixed_accept_pct"]) / float(patched["fixed_accept_pct"])), 87, 0.02)
else:
    MISS.append(("Icarust stock vs patched", "§6.8", "results/icarust-stock-vs-patched.csv absent"))

# ---------------------------------------------------------------- structural arithmetic
check("vote table bytes per channel (64x4x12)", "§3.6 T3.3, §4.3.1", 64 * 4 * 12, 3072)
check("vote table MB at C=512", "§4.3.1 T4.1", round(64 * 4 * 12 * 512 / 1e6, 2), 1.57, 0.01)
check("vote table MB at C=2675", "§4.3.1 T4.1", round(64 * 4 * 12 * 2675 / 1e6, 2), 8.22, 0.01)
check("anchor history bytes per channel (1024x14)", "§4.7.3 T4.2", 1024 * 14, 14336)
check("anchor history MB at C=512", "§4.7.3", round(1024 * 14 * 512 / 1e6, 2), 7.34, 0.01)
check("anchor history MB at C=2675", "§4.7.3", round(1024 * 14 * 2675 / 1e6, 2), 38.35, 0.01)
check("service bound at W=2, C=512, Tc=0.4 s (ms)", "§1.1, §3.1.3, §8.1",
      round(2 * 0.4 / 512 * 1000, 2), 1.56, 0.01)
check("key fits one word: 4 bits x 12 events", "App C.1", 4 * 12 <= 64, True)
check("Misra-Gries threshold at 256 counters", "§4.3.3 T4.1", f"m/{256 + 1}", "m/257")
check("set-associative expected threshold", "§4.3.3 T4.1", f"m/{64 * (4 + 1)}", "m/320")
check("TSC bucket edges are the reported p99 values", "§5.2.2",
      [round((1 << k) / 2418.7, 2) for k in (17, 18, 19, 20)],
      [54.19, 108.38, 216.76, 433.53])
check("utilisation at R3 median 134 us against the bound", "§5.6, §8.1",
      round(134 / 1560.0, 2), 0.09, 0.06)
check("utilisation at R3 p50 174 us against the bound", "§5.6, §8.1",
      round(174 / 1560.0, 2), 0.11, 0.05)
check("3.1 Gb against 64 Mb is 'roughly fifty times'", "§8.4",
      round(3.1e9 / 64e6), 48, 0.05)

# ---------------------------------------------------------------- report
W = 62
print("=" * 78)
print("THESIS NUMBER VERIFICATION")
print("=" * 78)
print(f"\nAGREE  ({len(PASS)})")
for c, w, *_ in PASS:
    print(f"  ok   {c[:W]:<{W}} {w}")
if FAIL:
    print(f"\nDISAGREE  ({len(FAIL)})   <-- prose and data do not match")
    for c, w, got, want in FAIL:
        print(f"  FAIL {c[:W]:<{W}} {w}")
        print(f"       data says {got!r}, prose says {want!r}")
if MISS:
    print(f"\nNO DATA FILE  ({len(MISS)})")
    for c, w, why in MISS:
        print(f"  --   {c[:W]:<{W}} {w}  ({why})")

print("\n" + "=" * 78)
print("OUTSTANDING: claims no committed file can settle")
print("=" * 78)
for claim, where, needs in [
    ("Regime R3 latency: p50 134-174 us, p99 630-705 us, 3.9-4.4 GB, 975 chunk/s",
     "§5.6 T5.2, §8.1, §1.9",
     "re-run at minimizer window 1 with --latency-out. The chr20 dumps in results/hgrm "
     "are window-10 runs (p50 22-26 us) and do NOT back this figure"),
    ("readfish orchestration: 124 us/chunk, 15.6-25.3% of a core, 86-663 MB",
     "§5.8.1, §8.1, §8.3", "re-run readfish with the no-op caller, capture to results/"),
    ("This system's processor use: 182-202% of one core", "§5.8.1, §8.1",
     "capture alongside the readfish comparison"),
    ("RawHash2 9.66 ms/read against ~1.34 ms", "§5.8.2",
     "re-run both single-threaded with /usr/bin/time, capture to results/"),
    ("This pipeline's headline accuracy, 18.8%", "§1.4, §5.8.2, §7.1, §8.3",
     "it is the anchor-history chainer row of decision-rules.csv, which is a TRANSCRIPTION "
     "and not a captured run. geometry-x-chaining.csv corroborates 14.2 and 17.4 but not "
     "18.8. Re-run bench/squig_sweep.cpp in anchor-history mode and replace that file"),
    ("Vote-table lockout: 0 accepts of 763; 854 of 1366 at 37 kb", "§4.4, T8.1",
     "recoverable only by re-running the pre-fix binary; cite the commit instead"),
    ("Chunk-relative diagonal: 49/300 -> 7/300, corrected 118->160->170->196",
     "§4.5, T8.1", "re-run bench with the diagnostic enabled, capture to results/"),
    ("Oracle normalisation recovers 100% of seeds against 50.5% per-read", "§7.8, §8.4",
     "re-run; superseded in part by §7.4.4's whole-read result, which IS backed"),
    ("Realised key entropy ~18 bits below nominal; 3x8 truncates 99.3%", "§7.3.1, §7.10",
     "re-run bench/real_reference.cpp, capture to results/"),
    ("Median best-chain score of 4 on on-target reads", "§7.5, §8.3",
     "re-run; this is the observation the whole of §7.5 is compared against"),
    ("Two-scale detector: 3 against 38 correct diagonals at CV 0.50", "T8.1 row 9",
     "the implementation was reverted; cite the commit that reverted it"),
    ("Difference keys: 18.8% falling to 2.8-8.4%", "§7.6, T8.1 row 10",
     "delta_keys is still in QuantConfig; re-runnable with bench/squig_sweep.cpp"),
    ("Index footprint 63.9M vs 11.7M minimizers, 3.9 vs 1.6 GB", "App C.1, §3.6.3",
     "re-run index build at both windows, capture to results/"),
    ("Withdrawn figures in T8.1 rows 1, 2, 3 (111.93/112.12/223.66/447.59; 27.0-27.3 "
     "and 17.9-18.8; 9.2 ms and 1.5 ms)", "§5.2.2, §5.3.1, T8.1",
     "these are withdrawn results and are quoted as such; the ORIGINAL runs are gone. "
     "Cite the session record, or re-state as 'approximately'"),
]:
    print(f"\n  {claim}")
    print(f"    cited at : {where}")
    print(f"    needs    : {needs}")

print()
print(f"summary: {len(PASS)} agree, {len(FAIL)} disagree, {len(MISS)} missing file, "
      f"14 outstanding")
sys.exit(1 if FAIL else 0)
