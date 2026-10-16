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
import subprocess
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


def git_record(claim: str, where: str, sha: str, needles: list[str]) -> None:
    """Assert each needle appears verbatim in the commit message of `sha`.

    Some figures describe code that no longer exists -- a structure that was replaced, a
    variant that was reverted, a measurement that was withdrawn -- so no current data file
    can produce them and re-running is impossible by construction. They are not therefore
    unsupported: each was recorded in the commit message that fixed, reverted or withdrew it,
    and a commit message is a content-addressed, immutable record made at the time of the
    measurement. Citing the commit is the correct discipline for a historical claim, and this
    check keeps the citation honest by failing if the text ever stops matching.
    """
    try:
        body = subprocess.run(
            ["git", "log", "-1", "--format=%B", sha],
            cwd=REPO, capture_output=True, text=True, check=True, encoding="utf-8",
        ).stdout
    except Exception as exc:  # a missing object is a failure, not a skip
        FAIL.append((claim, where, f"git read failed: {exc}", sha))
        return
    missing = [n for n in needles if n not in body]
    if missing:
        FAIL.append((claim, where, f"not in {sha[:7]}: {missing}", sha[:7]))
    else:
        PASS.append((claim, f"{where} [commit {sha[:7]}]", None, None))


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

# ---------------------------------------------------------------- RawHash, three seeds
rhs = rows("rawhash-seeds.csv")
if rhs:
    per = {r["seed"]: r for r in rhs}
    check("comparison mapper, three-seed mean correct %", "§1.4, §7.4.5, §8.3",
          float(per["mean"]["correct_pct"]), 91.8)
    check("comparison mapper, seed 42 reproduces the 92.4 figure", "§5.8.2, §6.5",
          float(per["42"]["correct_pct"]), 92.4)
    check("comparison mapper, range across seeds", "§1.4, §8.3",
          [float(per[k]["correct_pct"]) for k in ("44", "42")], [91.2, 92.4])
    check("its default operating point is not zero-false-positive", "§6.5, §7.4.5",
          all(float(per[k]["false_mapped_pct"]) > 5.0 for k in ("42", "43", "44")), True)
else:
    MISS.append(("RawHash three-seed accuracy", "§1.4", "results/rawhash-seeds.csv absent"))

# ---------------------------------------------------------------- regime R3 latency
r3 = rows("r3-latency.csv")
if r3:
    runs = [r for r in r3 if r["run"] != "median"]
    med = next(r for r in r3 if r["run"] == "median")
    check("R3 runs captured", "§5.6 T5.2", len(runs), 9)
    check("R3 p50, median of runs", "§5.6, §8.1, §1.9, §3.1.3", float(med["p50"]), 124.77)
    check("R3 p90, median of runs", "§5.6", float(med["p90"]), 192.76)
    check("R3 p99, median of runs", "§5.6, §1.9", float(med["p99"]), 370.15)
    check("R3 p50 range across runs", "§5.6, §5.10",
          [min(float(r["p50"]) for r in runs), max(float(r["p50"]) for r in runs)],
          [115.59, 132.34])
    check("R3 p99 range across runs", "§5.6, §5.10",
          [min(float(r["p99"]) for r in runs), max(float(r["p99"]) for r in runs)],
          [346.78, 1969.29])
    check("R3 resident set, identical in every run", "§5.6, §8.1",
          len({r["rss_gb"] for r in runs}) == 1 and float(med["rss_gb"]), 3.70)
    check("R3 drops and late, zero in every run", "§5.6, §8.1, §3.5",
          all(r["dropped"] == "0" and r["late"] == "0" for r in runs), True)
    check("R3 offered chunk rate range", "§5.6, §3.1.3",
          [min(int(r["chunks_per_s"]) for r in runs),
           max(int(r["chunks_per_s"]) for r in runs)], [604, 645])
    check("R3 utilisation at the median", "§5.6, §8.1, §3.1.3",
          round(float(med["p50"]) / 1560.0, 3), 0.080, 0.02)
    # What §5.6 says about the tail against the budget. Both percentiles cross it in the
    # same single run, b1_1, whose p90 and mean are also the worst of the nine; the other
    # eight sit entirely inside the budget through the 99.9th.
    over99 = [r["run"] for r in runs if float(r["p99"]) > 1560.0]
    over999 = [r["run"] for r in runs if float(r["p99_9"]) > 1560.0]
    check("p99 exceeds the 1.56 ms budget in exactly one run", "§5.6, §8.1", over99, ["b1_1"])
    check("p99.9 exceeds the budget in the same single run", "§5.6, §8.1",
          over999, ["b1_1"])
    check("eight of nine runs are inside the budget through p99.9", "§5.6, §8.1",
          len(runs) - len(over999), 8)
    check("the crossing run is the worst at p90 and mean too", "§5.6",
          max(runs, key=lambda r: float(r["p90"]))["run"] == "b1_1"
          and max(runs, key=lambda r: float(r["mean"]))["run"] == "b1_1", True)
    # The superseded figures, asserted to be superseded.
    check("no run reaches the withdrawn p50 lower bound of 134", "§5.6",
          max(float(r["p50"]) for r in runs) < 134.0, True)
    check("exactly one run falls in the withdrawn p99 range 630-705", "§5.6",
          sum(1 for r in runs if 630.0 <= float(r["p99"]) <= 705.0), 1)
else:
    MISS.append(("regime R3 latency", "§5.6 T5.2", "results/r3-latency.csv absent"))

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
    def d2(rule, lookback, occ, key):
        for r in dr:
            if r["rule"] == rule and r["lookback"] == lookback and r["occ_filter"] == occ:
                v = r[key]
                return v if key == "median_votes" else float(v)
        return None

    def d(rule, lookback="", occ="no"):
        return d2(rule, lookback, occ, "tpr_mean")

    votes = d("exact-diagonal votes (256 counters)")
    anchor48 = d("anchor-history chains", "48", "no")
    anchor128 = d("anchor-history chains", "128", "no")
    check("headline accuracy, three-seed mean", "§1.4, §7.1, §7.4.5, §8.3", anchor48, 17.6)
    check("headline accuracy, range", "§1.4, §7.1, §8.3",
          [d2("anchor-history chains", "48", "no", k) for k in ("tpr_lo", "tpr_hi")],
          [16.6, 18.8])
    check("18.8 is the seed-42 value, not the mean", "§4.7.3",
          d2("anchor-history chains", "48", "no", "tpr_s42"), 18.8)
    check("votes, three-seed mean", "§4.7.3, §7.6", votes, 14.1)
    check("best intervention is +35% relative over votes", "§7.6, §1.5, §8.3",
          round(100 * (anchor128 / votes - 1)), 35, 0.04)
    check("longer look-back DOES help", "§4.7.3, T7.3", anchor128 > anchor48, True)
    check("longer look-back helps in every seed", "§4.7.3",
          all(d2("anchor-history chains", "128", "no", k)
              > d2("anchor-history chains", "48", "no", k)
              for k in ("tpr_s42", "tpr_s43", "tpr_s44")), True)
    check("longer look-back, three-seed mean", "§4.7.3, T7.3", anchor128, 19.0)
    check("beta=0 control is worse than votes", "§4.7.3",
          d("greedy chains (control)") < votes, True)
    check("occurrence filtering does not help", "§4.7.3, T7.3",
          d("anchor-history chains", "48", "yes"), 16.3)
    check("median best-chain score on on-target reads", "§7.5, §8.3",
          d2("anchor-history chains", "48", "no", "median_votes"), "4")
    # Cross-file agreement on the two rows both files carry, at seed 42.
    if gx:
        check("14.2 agrees across the two files at seed 42", "§4.7.3 vs §7.6",
              d2("exact-diagonal votes (256 counters)", "", "no", "tpr_s42"),
              g("4x12", "votes"))
        check("17.4 agrees across the two files at seed 42", "§4.7.3 vs §7.6",
              d2("greedy chains", "", "no", "tpr_s42"), g("4x12", "chain"))
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
# From the daemon's own index line, reproduced in every results/r3-raw/r3_frozen_*.log.
check("minimizers at window 1 on chr20", "App C.1, §5.6",
      round(63981218 / 1e6, 1), 63.9, 0.01)
check("reference bases indexed", "§5.6", round(63981237 / 1e6), 64, 0.01)

# ---------------------------------------------------------------- realised key entropy
ke = rows("key-entropy.csv")
if ke:
    def k(ref, geo, key):
        for r in ke:
            if int(r["ref_mb"]) == ref and r["geometry"] == geo:
                return float(r[key]) if key != "geometry" else r[key]
        return None

    check("3x13 realised entropy on the 20 Mb reference", "§7.3.1 T7.1",
          k(20, "3x13", "realised_bits"), 22.3)
    check("3x13 shortfall below nominal", "§7.3.1, §7.10",
          k(20, "3x13", "shortfall_bits"), 16.7)
    check("4x12 realised entropy, frozen geometry", "§7.3.1 T7.1",
          k(20, "4x12", "realised_bits"), 23.8)
    check("4x12 shortfall below nominal", "§7.3.1, §7.10",
          k(20, "4x12", "shortfall_bits"), 24.2)
    check("3x8 distinct keys on the 20 Mb reference", "§7.3.2",
          k(20, "3x8", "distinct_keys"), 92482)
    check("3x8 truncation on the 20 Mb reference (%)", "§7.3.2, §7.10",
          round(100 - k(20, "3x8", "cap_kept_pct"), 1), 97.2)
    check("3x8 truncation on the 64 Mb reference (%)", "§7.3.2, §7.10",
          round(100 - k(64, "3x8", "cap_kept_pct"), 1), 99.3)
    check("4x12 cap survival on the 20 Mb reference (%)", "§7.3.2",
          k(20, "4x12", "cap_kept_pct"), 93.5)
    check("entropy is monotone in nominal width", "§7.3.1",
          all(k(20, a, "realised_bits") < k(20, b, "realised_bits")
              for a, b in (("3x8", "4x8"), ("4x8", "3x13"), ("3x13", "3x14"), ("3x14", "4x12"))),
          True)
    check("shortfall grows faster than nominal width", "§7.3.1",
          k(20, "4x12", "shortfall_bits") > 3 * k(20, "3x8", "shortfall_bits"), True)
    check("minimizers at window 10 on the 64 Mb reference", "App C.1, §3.6.3",
          round(11665467 / 1e6, 1), 11.7, 0.01)
else:
    MISS.append(("realised key entropy", "§7.3.1", "results/key-entropy.csv absent"))

# ---------------------------------------------------------------- difference keys
dk = rows("delta-keys.csv")
if dk:
    enc = {r["encoding"]: r for r in dk}
    check("difference keys, three-seed mean", "§7.6 T7.3, T8.1 row 10",
          float(enc["delta"]["tpr_mean"]), 8.3)
    check("difference keys, range", "§7.6, T8.1 row 10",
          [float(enc["delta"][k]) for k in ("tpr_lo", "tpr_hi")], [3.6, 11.8])
    check("the absolute control reproduces decision-rules.csv", "§7.6",
          float(enc["absolute"]["tpr_mean"]), 17.6)
    check("difference keys are worse in every seed", "§7.6",
          all(float(enc["delta"][k]) < float(enc["absolute"][k])
              for k in ("tpr_s42", "tpr_s43", "tpr_s44")), True)
else:
    MISS.append(("difference keys", "§7.6", "results/delta-keys.csv absent"))

# ---------------------------------------------------------------- comparison costs
cc = rows("comparison-costs.csv")
if cc:
    def c(system, frag, key):
        for r in cc:
            if r["system"] == system and frag in r["mode"]:
                return r[key]
        return None

    check("readfish orchestration, per read (us)", "§5.8.1, §8.1, §8.3",
          round(float(c("readfish", "no-op", "per_read_ms")) * 1000, 1), 124.3)
    check("readfish no-op caller, processor use (%)", "§5.8.1",
          float(c("readfish", "no-op", "cpu_pct")), 15.6)
    check("readfish with caller and mapper, processor use (%)", "§5.8.1",
          float(c("readfish", "targets", "cpu_pct")), 25.3)
    check("readfish resident set range (MiB)", "§5.8.1",
          [float(c("readfish", "no-op", "rss_mib")), float(c("readfish", "targets", "rss_mib"))],
          [86.0, 663.4])
    check("this system's processor use", "§5.8.1, §8.1",
          c("ours", "frozen", "cpu_pct"), "181.5-202.3")
    check("comparison mapper, per read incl. index load (ms)", "§5.8.2",
          float(c("rawhash2", "incl", "per_read_ms")), 10.64)
    check("comparison mapper, per read excl. index load (ms)", "§5.8.2",
          float(c("rawhash2", "excl", "per_read_ms")), 10.18)
    check("this system's per-read figure is 10 chunks of the R3 median", "§5.8.2",
          round(10 * 124.77 / 1000.0, 2), float(c("ours", "frozen", "per_read_ms")), 0.01)
else:
    MISS.append(("comparison costs", "§5.8", "results/comparison-costs.csv absent"))

# ---------------------------------------------------------------- the historical record
# Figures describing code that no longer exists. Each is cited to the commit that fixed,
# reverted or withdrew it, and the commit message is the primary record.
git_record("Vote-table lockout: 0 accepts of 763 at 64 Mb", "§4.4, T8.1 row 6",
           "284908b", ["0 accepts out of 763", "votes[weakest] <= 1"])
git_record("Vote-table lockout: 854 of 1366 at 37 kb", "§4.4, T8.1 row 6",
           "ba18428", ["accepts 854 of 1366"])
git_record("Chunk-relative diagonal: the inverted metric", "§4.5, T8.1 row 7",
           "b6e939a", ["49/300 at 2 chunks down\nto 7/300 at 20"])
git_record("Chunk-relative diagonal: the corrected series", "§4.5, T8.1 row 7",
           "b6e939a", ["118 -> 160 -> 170 -> 196 of 300"])
git_record("Two-scale detector: 3 against 38 correct diagonals at CV 0.50",
           "T8.1 row 9", "e58d6fe", ["0.50          38             3", "52/59/27/1"])
git_record("Oracle normalisation: 100% of seeds against 50.5%", "§7.8, §8.4",
           "6cb90bb", ["oracle normalisation recovers 100% of seeds and per-read",
                       "normalisation recovers 50.5%"])
git_record("Withdrawn p99 values and the TSC frequency", "§5.2.2, T8.1 row 1",
           "0c620fa", ["111.93", "2418.7"])
git_record("Withdrawn unpinned medians and maxima", "§5.3.1, T8.1 row 2",
           "ab4614f", ["27.0", "17.9", "9.2 ms"])
git_record("Single-run dwell figure withdrawn by replication", "T8.1 row 11",
           "668a6a8", ["seed"])

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
OUTSTANDING = [
    ("Icarust accept rates behind Table 6.2: 43.5 / 0.5 / 9.6 per cent",
     "§6.8, App B",
     "live read-until rates against Icarust, recorded in third_party_patches/README.md and "
     "results/icarust-stock-vs-patched.csv but not from a captured run. Needs a re-run of "
     "the patched and stock simulator with the daemon, roughly an hour including the "
     "Icarust rebuild"),
    ("Index resident set at minimizer window 10: ~1.5 GiB",
     "App C.1, §3.6.3",
     "the minimizer COUNT is measured (11.67 M, results/key-entropy.csv) and the resident "
     "set is read from a surviving /usr/bin/time -v in results/comparison-raw/, which is a "
     "window-10 run of an older binary. A current window-10 daemon run would settle it"),
    ("Bare-metal deadline-miss rate against the 1.56 ms bound",
     "§3.8, §5.9, §8.4",
     "requires hardware this work does not have: isolated cores, tickless operation, "
     "interrupt affinity moved off the measured cores, fixed governor. Every latency "
     "figure in the thesis is scoped to a hypervisor because of this"),
    ("Per-event agreement across the full dwell sweep and across seeds",
     "§6.10, §7.9",
     "agreement is measured at two dwell levels with one seed and 200 reads (§7.4.4-5). "
     "The tooling exists; extending it to six levels in triplicate is a few hours of "
     "compute, not a missing capability"),
    ("Real instrument data for any measurement in this thesis",
     "§6.10, §7.9, §8.4",
     "all signal is simulated, from two generators. Needs a sequencing run"),
]
for claim, where, needs in OUTSTANDING:
    print(f"\n  {claim}")
    print(f"    cited at : {where}")
    print(f"    needs    : {needs}")

print()
print(f"summary: {len(PASS)} agree, {len(FAIL)} disagree, {len(MISS)} missing file, "
      f"{len(OUTSTANDING)} outstanding")
sys.exit(1 if FAIL else 0)
