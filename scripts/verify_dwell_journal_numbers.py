#!/usr/bin/env python3
"""Re-derive every number in paper_dwell_journal/ from results/, and refuse any other.

verify_thesis_numbers.py checks the claims someone remembered to transcribe into it. This
script is stricter, because a journal manuscript has no [VERIFY] markers to fall back on: it
reads the manuscript and its Supplementary Information, each in .md and .tex, and requires
every numeral in all four files to be accounted for. A numeral is accepted only if it is

  BACKED       recomputed here from a committed file in results/, with decimal arithmetic
               and round-half-up, so 0.0915 prints as 0.092 rather than the 0.091 that a
               binary float gives;
  STRUCTURAL   a method parameter (a seed, a CV level, a read count), each of which is
               checked below against the log or file header that records it; or
  OUTSTANDING  a value no captured run backs, allowed only in the Supplementary
               Information, never in the main text, and only when marked with a dagger.

On top of that, every row of Tables 1-5 and S1-S3 is rebuilt from data and must appear verbatim
in both files of its pair, the Abstract's headline sentences must appear verbatim in both, every
replicated "mean [range]" must appear whole, and each .md/.tex pair must carry the same
data-derived numerals in the same order. A ratio written as a word ("sevenfold") is refused,
because the gate cannot check it.

    python scripts/verify_dwell_journal_numbers.py
    python scripts/verify_dwell_journal_numbers.py --md M.md --tex M.tex \\
        --supp-md S.md --supp-tex S.tex                                  # test the gate on copies
"""

from __future__ import annotations

import argparse
import csv
import difflib
import os
import re
import subprocess
import sys
from decimal import ROUND_HALF_UP, Decimal, getcontext
from pathlib import Path

getcontext().prec = 28
# The report prints section signs and en dashes; a Windows console defaults to cp1252.
sys.stdout.reconfigure(encoding="utf-8")

REPO = Path(__file__).resolve().parent.parent
RESULTS = REPO / "results"
PAPER = REPO / "paper_dwell_journal"

_ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
_ap.add_argument("--md", type=Path, default=PAPER / "manuscript.md")
_ap.add_argument("--tex", type=Path, default=PAPER / "manuscript.tex")
_ap.add_argument("--supp-md", type=Path, default=PAPER / "supplementary.md")
_ap.add_argument("--supp-tex", type=Path, default=PAPER / "supplementary.tex")
_args = _ap.parse_args()
MD, TEX = _args.md.resolve(), _args.tex.resolve()
SMD, STEX = _args.supp_md.resolve(), _args.supp_tex.resolve()

PASS: list[tuple[str, str]] = []
FAIL: list[tuple[str, str]] = []
SKIP: list[tuple[str, str]] = []   # checks that cannot apply here; they do not affect the exit code


def ok(what: str, where: str = "") -> None:
    PASS.append((what, where))


def bad(what: str, why: str) -> None:
    FAIL.append((what, why))


def rows(name: str) -> list[dict]:
    """A results CSV, skipping the leading '#' provenance commentary."""
    p = RESULTS / name
    if not p.exists():
        bad(f"data file {name}", "missing")
        return []
    with open(p, newline="", encoding="utf-8") as f:
        lines = [ln for ln in f if not ln.lstrip().startswith("#")]
    return list(csv.DictReader(lines))


def rel(path: Path) -> str:
    try:
        return str(path.relative_to(REPO))
    except ValueError:
        return str(path)


def text_of(path: Path) -> str:
    if not path.exists():
        bad(f"file {rel(path)}", "missing")
        return ""
    return path.read_text(encoding="utf-8")


def D(s) -> Decimal:
    return Decimal(str(s).strip())


def q(x: Decimal, places: str) -> str:
    return str(x.quantize(Decimal(places), rounding=ROUND_HALF_UP))


# ---------------------------------------------------------------- the registries
BACKED: dict[str, list[str]] = {}
# Hand-transcribed values, each with the document pair it must appear in: "main" for the
# manuscript, "supp" for the Supplementary Information.
CLAIMED: list[tuple[str, str]] = []


def backed(tok: str, why: str) -> None:
    BACKED.setdefault(tok, []).append(why)


def claim(written: str, value: Decimal, places: str, why: str, where: str = "main") -> None:
    """The prose says `written`; the data must say the same at that precision."""
    got = q(value, places)
    if got == written:
        backed(written, why)
        CLAIMED.append((written, where))
        ok(f"{why} = {written}")
    else:
        bad(why, f"data gives {got}, prose says {written}")


def needles(path: Path, wanted: list[str], why: str) -> None:
    """Provenance: each string must appear verbatim in the file that records it."""
    body = text_of(path)
    missing = [n for n in wanted if n not in body]
    if missing:
        bad(why, f"not in {rel(path)}: {missing}")
    else:
        ok(why, rel(path))


CVS = ["0.00", "0.10", "0.20", "0.30", "0.44", "0.60"]

# ---------------------------------------------------------------- Table 1: this pipeline
ours = {r["dwell_cv"]: r for r in rows("dwell-sweep-ours.csv")}
COLS = ["fixed_mean", "fixed_lo", "fixed_hi", "detected_mean", "detected_lo", "detected_hi"]
TABLE1 = []
if ours:
    for cv in CVS:
        r = ours[cv]
        for c in COLS:
            backed(q(D(r[c]), "0.1"), f"dwell-sweep-ours CV {cv} {c}")
        v = {c: q(D(r[c]), "0.1") for c in COLS}
        TABLE1.append([
            cv,
            f"{v['fixed_mean']} [{v['fixed_lo']}–{v['fixed_hi']}]",
            f"{v['detected_mean']} [{v['detected_lo']}–{v['detected_hi']}]",
        ])

    fm = lambda cv: D(ours[cv]["fixed_mean"])
    dm = lambda cv: D(ours[cv]["detected_mean"])
    claim("2.7", fm("0.00") / dm("0.00"), "0.1", "fixed-width lead at CV 0 (fold), §3.1")
    claim("10.8", dm("0.44") / fm("0.44"), "0.1", "fixed-width deficit at CV 0.44 (fold), Abstract")
    claim("38.2", fm("0.00") / fm("0.44"), "0.1", "fixed-width fall CV 0 to 0.44 (fold), §3.1")
    claim("8.4", fm("0.00") / fm("0.10"), "0.1", "fixed-width fall CV 0 to 0.10 (fold), §3.1")
    upto = [dm(cv) for cv in CVS if D(cv) <= D("0.44")]
    claim("14.1", min(upto), "0.1", "detected minimum up to CV 0.44, §3.1")
    claim("19.3", max(upto), "0.1", "detected maximum up to CV 0.44, §3.1")

    # "the replicate ranges do not overlap at any dispersion tested" -- a logical claim.
    # At CV 0 fixed-width leads, so its LOW must exceed detection's HIGH; elsewhere the
    # reverse. Disjoint ranges are what licence "in every seed, not only in the means".
    clash = []
    for cv in CVS:
        r = ours[cv]
        if cv == "0.00":
            if not D(r["fixed_lo"]) > D(r["detected_hi"]):
                clash.append(cv)
        elif not D(r["fixed_hi"]) < D(r["detected_lo"]):
            clash.append(cv)
    if clash:
        bad("replicate ranges disjoint at every CV (Abstract, §3.1)", f"overlap at {clash}")
    else:
        ok("replicate ranges disjoint at every CV (Abstract, §3.1)")
    if fm("0.00") > dm("0.00") and fm("0.10") < dm("0.10"):
        ok("order exchanges between CV 0 and CV 0.10 (§3.1)")
    else:
        bad("order exchanges between CV 0 and CV 0.10 (§3.1)", "it does not")

# ---------------------------------------------------------------- Tables 2 and 4: RawHash2 grid
# Three seeds per cell, from scripts/rerun_rawhash_grid.sh. The earlier single-seed file,
# rawhash-dwell-sweep.csv, is seed 42's run of the same grid, and the re-run must reproduce it.
SEEDS = ("42", "43", "44")
grid = {(r["seed"], r["w"], r["dwell_cv"]): r for r in rows("rawhash-grid-seeds.csv")}
single = {(r["w"], r["dwell_cv"]): r for r in rows("rawhash-dwell-sweep.csv")}
TABLE2, TABLE4 = [], []


def holds(cond: bool, what: str) -> None:
    if cond:
        ok(what)
    else:
        bad(what, "does not hold in the data")


def mr(values) -> str:
    """'mean [min–max]' at one decimal: the form the text uses for every replicated value."""
    return f"{q(sum(values) / len(values), '0.1')} [{q(min(values), '0.1')}–{q(max(values), '0.1')}]"


MR_TEXT: list[tuple[str, str, str]] = []   # (pattern, why, doc): each must appear whole in that pair


def claim_mr(text: str, values, why: str, where: str = "main") -> None:
    """A 'mean [min–max]' quoted in the text, each of its three numbers recomputed.

    The three are also required to appear together, as one phrase, in both files of the pair. The
    registry alone would accept a wrong mean that happens to equal another backed value;
    requiring the whole phrase ties the mean to its own range.
    """
    m, lo, hi = re.fullmatch(r"(\S+) \[(\S+)–(\S+)\]", text).groups()
    claim(m, sum(values) / len(values), "0.1", f"{why}, mean", where)
    claim(lo, min(values), "0.1", f"{why}, min", where)
    claim(hi, max(values), "0.1", f"{why}, max", where)
    # Words may sit between mean and range: "2.9 points [2.8–3.0]", "7.8 points on average [...]",
    # and so may a percent sign: "80.5% [76.2–84.6]".
    MR_TEXT.append((rf"(?<![\d.]){re.escape(m)}%?(?: [a-z ]+)? \[{re.escape(lo)}–{re.escape(hi)}\]", why, where))


if grid:
    COLS2 = ("reads", "mapped_pct", "correct_pct", "off_reads", "false_mapped_pct")
    diff = [(w, cv, c) for (w, cv), r in single.items() for c in COLS2
            if grid.get(("42", w, cv), {}).get(c) != r[c]]
    if diff or len(grid) != 54:
        bad("grid seed 42 reproduces rawhash-dwell-sweep.csv in every cell",
            f"{len(grid)} rows; differs at {diff[:5]}")
    else:
        ok("grid seed 42 reproduces rawhash-dwell-sweep.csv in every cell", "18 cells x 5 columns")

    corr = lambda s, w, cv: D(grid[(s, w, cv)]["correct_pct"])
    fals = lambda s, w, cv: D(grid[(s, w, cv)]["false_mapped_pct"])
    # Paired: every window of one seed maps the same reads, so the cost is taken within a seed.
    pen = lambda s, w, cv: corr(s, "0", cv) - corr(s, w, cv)

    for label, f in (("Correct", corr), ("False-mapped", fals)):
        for w in ("0", "5", "10"):
            TABLE2.append([label, w] + [q(sum(f(s, w, cv) for s in SEEDS) / 3, "0.1") for cv in CVS])
    for row in TABLE2:
        for cell in row[2:]:
            backed(cell, f"rawhash-grid {row[0]} w={row[1]}, mean of three seeds")
    for cv in CVS:
        cells = [mr([pen(s, w, cv) for s in SEEDS]) for w in ("5", "10")]
        TABLE4.append([cv] + cells)
        for c in cells:
            for tok in re.findall(r"\d+\.\d", c):
                backed(tok, f"paired sketching penalty at CV {cv}")

    loss = lambda w: [corr(s, w, "0.00") - corr(s, w, "0.60") for s in SEEDS]
    claim_mr("2.9 [2.8–3.0]", loss("0"), "RawHash2 w=0 loss over the sweep, Abstract, §3.2")
    claim_mr("7.0 [5.2–8.4]", loss("5"), "RawHash2 w=5 loss over the sweep, §3.2")
    claim_mr("10.7 [9.4–11.6]", loss("10"), "RawHash2 w=10 loss over the sweep, §3.2")
    claim_mr("11.7 [10.4–12.8]", [pen(s, "10", "0.00") for s in SEEDS], "w=10 penalty at CV 0, Abstract, §3.3")
    claim_mr("19.5 [18.2–21.0]", [pen(s, "10", "0.60") for s in SEEDS], "w=10 penalty at CV 0.60, Abstract, §3.3")
    claim_mr("15.4 [14.6–16.4]", [pen(s, "10", "0.44") for s in SEEDS], "w=10 penalty at CV 0.44, §3.3")
    claim_mr("4.7 [4.0–5.0]", [pen(s, "5", "0.00") for s in SEEDS], "w=5 penalty at CV 0, §3.3")
    claim_mr("8.7 [7.2–10.4]", [pen(s, "5", "0.60") for s in SEEDS], "w=5 penalty at CV 0.60, §3.3")
    claim_mr("7.8 [6.4–8.8]", [pen(s, "10", "0.60") - pen(s, "10", "0.00") for s in SEEDS],
             "w=10 penalty growth from CV 0 to 0.60, §3.3")
    early = [sum(pen(s, "10", cv) for s in SEEDS) / 3 for cv in CVS[:4]]
    claim("11.2", min(early), "0.1", "w=10 mean penalty, lowest up to CV 0.30, §3.3")
    claim("12.3", max(early), "0.1", "w=10 mean penalty, highest up to CV 0.30, §3.3")

    # The logical claims of the Abstract and §3.3. Each must hold in every seed, not on average.
    p = lambda w, cv: [pen(s, w, cv) for s in SEEDS]
    for w in ("5", "10"):
        holds(all(pen(s, w, "0.60") > pen(s, w, "0.00") for s in SEEDS),
              f"w={w} penalty larger at CV 0.60 than at CV 0 in every seed (Abstract, §3.3)")
    holds(max(p("10", "0.00")) < min(p("10", "0.60")), "w=10 penalty ranges at CV 0 and 0.60 disjoint (§3.3)")
    holds(max(p("10", "0.00")) < min(p("10", "0.44")), "w=10 penalty range at CV 0.44 clear of CV 0 (§3.3)")
    holds(all(min(p("10", cv)) <= max(p("10", "0.00")) for cv in CVS[1:4]),
          "w=10 penalty ranges up to CV 0.30 overlap the CV 0 range (§3.3)")
    holds(all(fals(s, "10", cv) > fals(s, "0", cv) for s in SEEDS for cv in CVS),
          "w=10 maps more control reads than w=0 in every seed at every CV (§3.3)")
    # Supp. S5: Table S3 is Table 5 before it is summarised, and only seed 44 rises at every step.
    rising = lambda s, w: all(pen(s, w, b) > pen(s, w, a) for a, b in zip(CVS, CVS[1:]))
    holds(rising("44", "5") and rising("44", "10")
          and not any(rising(s, w) for s in ("42", "43") for w in ("5", "10")),
          "only seed 44's default penalty rises at every step, at both windows (Supp. S5)")

# ---------------------------------------------------------------- Supp. Tables S1-S2: matched points
# scripts/rawhash_zero_fp.py thresholds RawHash2's own mapping quality in the grid's PAF output.
# Its CSV is not trusted as written. The rows are recomputed from raw.tar.gz here, on every run,
# and must equal it, and that recomputation must first reproduce rawhash-grid-seeds.csv cell for
# cell, so the thresholds are applied to exactly the mappings that Table 3 scores.
zfp_rows = rows("rawhash-grid-zero-fp.csv")
zfp = {(r["seed"], r["w"], r["dwell_cv"]): r for r in zfp_rows}
TABLE_S1, TABLE_S2, TABLE_S3 = [], [], []
if zfp and grid and ours:
    sys.path.insert(0, str(REPO / "scripts"))
    import rawhash_zero_fp

    fresh, mismatches = rawhash_zero_fp.compute()
    holds(not mismatches, "rawhash_zero_fp.py rescores every grid cell to rawhash-grid-seeds.csv exactly")
    claim("54", D(len(fresh) - len(mismatches)), "1", "grid cells the rescoring reproduces, Supp. S4", "supp")
    holds([[r[h] for h in rawhash_zero_fp.HEADER] for r in zfp_rows] == fresh,
          "rawhash-grid-zero-fp.csv equals a fresh recomputation from raw.tar.gz")
    holds(all(r["reads"] == r["off_reads"] == "500" for r in zfp_rows),
          "500 on-target and 500 control reads per cell, so a 1% budget is 5 reads (Supp. S4)")
    # "set by the sixth-highest-scoring control read" needs at least six mapped control reads.
    holds(all(fals(s, w, cv) * 5 >= 6 for (s, w, cv) in grid),
          "at least 6 control reads are mapped in every cell (§3.2, Supp. S4)")

    O0, O1 = "tpr_zero_fp", "tpr_1pct_fp"
    z = lambda s, w, cv, col: D(zfp[(s, w, cv)][col])
    zpen = lambda s, w, cv, col: z(s, "0", cv, col) - z(s, w, cv, col)
    zr = lambda w, cv, col=O0: [z(s, w, cv, col) for s in SEEDS]
    zp = lambda w, cv, col: [zpen(s, w, cv, col) for s in SEEDS]
    for lab, col in (("Zero FP", O0), ("1% FP", O1)):
        for cv in CVS:
            TABLE_S1.append([lab, cv] + [mr(zr(w, cv, col)) for w in ("0", "5", "10")])
            TABLE_S2.append([lab, cv] + [mr(zp(w, cv, col)) for w in ("5", "10")])
    for cv in CVS:
        TABLE_S3.append([cv] + [q(pen(s, w, cv), "0.1") for w in ("5", "10") for s in SEEDS])
    for label, table, first in (("S1", TABLE_S1, 2), ("S2", TABLE_S2, 2), ("S3", TABLE_S3, 1)):
        for row in table:
            for cell in row[first:]:
                for tok in re.findall(r"\d+\.\d", cell):
                    backed(tok, f"Supp. Table {label}, recomputed from the grid")

    # §1 and §3.2: the level at a matched operating point, and its response to dispersion.
    claim_mr("80.5 [76.2–84.6]", zr("0", "0.44"), "RawHash2 w=0 at zero FP, CV 0.44, §1, §3.2")
    claim("5.7", sum(zr("0", "0.44")) / 3 / D(ours["0.44"]["detected_mean"]), "0.1",
          "RawHash2 at zero FP over the detected arm, CV 0.44 (fold), §3.2")
    holds(min(zr("0", "0.44")) > D(ours["0.44"]["detected_hi"]),
          "RawHash2 zero-FP range at CV 0.44 clear of the detected arm's (§3.2)")
    every = [v for cv in CVS for v in zr("0", cv)]
    claim("76.2", min(every), "0.1", "RawHash2 w=0 at zero FP, lowest replicate at any CV, §3.2")
    claim("92.8", max(every), "0.1", "RawHash2 w=0 at zero FP, highest replicate at any CV, §3.2")
    zloss = lambda col: [z(s, "0", "0.00", col) - z(s, "0", "0.60", col) for s in SEEDS]
    claim_mr("4.3 [0.4–6.6]", zloss(O0), "RawHash2 w=0 loss over the sweep at zero FP, §3.2")
    claim_mr("3.1 [2.6–3.6]", zloss(O1), "RawHash2 w=0 loss over the sweep at 1% FP, §3.2")
    means0 = [sum(zr("0", cv)) for cv in CVS]
    holds(any(b > a for a, b in zip(means0, means0[1:])) and any(b < a for a, b in zip(means0, means0[1:])),
          "RawHash2 w=0 zero-FP mean is not monotone in CV (§3.2, Supp. S4)")
    holds(min(zloss(O1)) <= max(loss("0")) and min(loss("0")) <= max(zloss(O1)),
          "w=0 loss at 1% FP overlaps the loss at the default, 'close to the 2.9' (§3.2)")

    # §3.3: the dispersion dependence of the sketching cost, at both matched points.
    claim_mr("13.4 [12.8–14.6]", zp("10", "0.00", O1), "w=10 cost at 1% FP, CV 0, §3.3")
    claim_mr("24.5 [21.4–27.0]", zp("10", "0.60", O1), "w=10 cost at 1% FP, CV 0.60, §3.3")
    claim_mr("3.8 [3.4–4.2]", zp("5", "0.00", O1), "w=5 cost at 1% FP, CV 0, §3.3")
    claim_mr("9.6 [8.6–10.6]", zp("5", "0.60", O1), "w=5 cost at 1% FP, CV 0.60, §3.3")
    for w in ("5", "10"):
        holds(max(zp(w, "0.00", O1)) < min(zp(w, "0.60", O1)),
              f"w={w} cost at 1% FP: CV 0 and 0.60 ranges disjoint (§3.3, Supp. S4)")
        holds(all(zpen(s, w, "0.60", O1) > zpen(s, w, "0.00", O1) for s in SEEDS),
              f"w={w} cost at 1% FP larger at CV 0.60 in every seed (§3.3, Supp. S4)")
    claim_mr("15.7 [13.4–19.6]", zp("10", "0.00", O0), "w=10 cost at zero FP, CV 0, §3.3")
    claim_mr("29.5 [16.6–38.6]", zp("10", "0.60", O0), "w=10 cost at zero FP, CV 0.60, §3.3")
    grow0 = [zpen(s, "10", "0.60", O0) - zpen(s, "10", "0.00", O0) for s in SEEDS]
    holds(all(g > 0 for g in grow0), "w=10 cost at zero FP larger at CV 0.60 in every seed (§3.3)")
    claim("2.4", min(grow0), "0.1", "w=10 cost growth at zero FP, smallest seed, §3.3")
    claim("20.0", max(grow0), "0.1", "w=10 cost growth at zero FP, largest seed, §3.3")
    holds(max(zp("10", "0.00", O0)) >= min(zp("10", "0.60", O0)),
          "w=10 cost at zero FP: CV 0 and 0.60 ranges overlap (§3.3)")
    grow5 = sorted(zpen(s, "5", "0.60", O0) - zpen(s, "5", "0.00", O0) for s in SEEDS)
    holds(grow5[0] == 0 and grow5[1] > 0,
          "w=5 cost at zero FP: one seed shows no increase, the other two do (§3.3, Supp. S4)")
    width = lambda xs: max(xs) - min(xs)
    holds(all(width(zp(w, cv, O0)) > max(width(p(w, cv)), width(zp(w, cv, O1)))
              for w in ("5", "10") for cv in ("0.00", "0.60")),
          "cost at zero FP varies more between seeds than at the default or at 1% FP (§3.3)")

    # Supp. S4, the three features.
    neg = [zpen(s, "5", "0.44", O0) for s in SEEDS if zpen(s, "5", "0.44", O0) < 0]
    holds(len(neg) == 1, "exactly one seed's w=5 index beats w=0 at zero FP, CV 0.44 (Supp. S4)")
    if neg:
        claim("6.6", -neg[0], "0.1", "w=5 over w=0 at zero FP, CV 0.44, one seed, Supp. S4", "supp")
    holds(all(zpen(s, w, cv, O1) > 0 and pen(s, w, cv) > 0 for s in SEEDS for w in ("5", "10") for cv in CVS),
          "no seed's sketched index beats w=0 at 1% FP or at the default (Supp. S4)")
    holds(all(sum(zp("10", cv, col)) > sum(p("10", cv)) for cv in CVS for col in (O0, O1)),
          "w=10 costs more at both matched points than at the default, at every CV (Supp. S4)")

# ---------------------------------------------------------------- RawHash2 default, three seeds
seeds = {r["seed"]: r for r in rows("rawhash-seeds.csv")}
if seeds:
    per_c = [D(seeds[s]["correct_pct"]) for s in ("42", "43", "44")]
    per_f = [D(seeds[s]["false_mapped_pct"]) for s in ("42", "43", "44")]
    claim("91.8", sum(per_c) / 3, "0.1", "RawHash2 default correct, 3-seed mean, §2.5")
    claim("91.2", min(per_c), "0.1", "RawHash2 default correct, 3-seed min, §2.5")
    claim("92.4", max(per_c), "0.1", "RawHash2 default correct, 3-seed max, §2.5")
    claim("9.1", sum(per_f) / 3, "0.1", "RawHash2 default false-mapped, 3-seed mean, §2.5")
    claim("8.4", min(per_f), "0.1", "RawHash2 default false-mapped, 3-seed min, §2.5")
    claim("10.6", max(per_f), "0.1", "RawHash2 default false-mapped, 3-seed max, §2.5")
    for col, vals in (("correct_pct", per_c), ("false_mapped_pct", per_f)):
        if q(sum(vals) / 3, "0.1") != q(D(seeds["mean"][col]), "0.1"):
            bad(f"rawhash-seeds.csv mean row ({col})", "does not equal the mean of its seeds")
        else:
            ok(f"rawhash-seeds.csv mean row ({col}) equals mean of seeds")

    # The three seeds are at CV 0.44. Seed 42's captured score must reproduce its row here, and
    # each seed must equal the grid's w=0, CV 0.44 cell: two separate runs, from two separately
    # built indexes, that must agree exactly.
    sc = text_of(RESULTS / "rawhash-seeds-raw" / "score_s42.txt").strip().split(",")
    r42 = seeds["42"]
    want = [r42["reads"], r42["mapped_pct"], r42["correct_pct"], r42["off_reads"], r42["false_mapped_pct"]]
    if sc != want:
        bad("score_s42.txt matches rawhash-seeds.csv seed 42", f"{sc} vs {want}")
    else:
        ok("score_s42.txt matches rawhash-seeds.csv seed 42", "captured run")
    if grid:
        cols = ("mapped_pct", "correct_pct", "false_mapped_pct")
        holds(all([seeds[s][c] for c in cols] == [grid[(s, "0", "0.44")][c] for c in cols] for s in SEEDS),
              "rawhash-seeds.csv equals the grid's w=0, CV 0.44 cell for every seed")

# ---------------------------------------------------------------- Table 3: agreement
ag = rows("agreement-summary.csv")
TABLE3 = []
if ag:
    def arow(system, seg, cv, norm):
        hit = [r for r in ag if (r["system"], r["segmentation"], r["dwell_cv"], r["norm"]) == (system, seg, cv, norm)]
        if len(hit) != 1:
            bad(f"agreement row {system}/{seg}/{cv}/{norm}", f"{len(hit)} matches")
            return None
        return hit[0]

    for label, seg_label, system, seg, norm in (
        ("This pipeline", "fixed-width", "ours", "fixed", "read"),
        ("This pipeline", "detected", "ours", "detected", "read"),
        ("RawHash2", "own, with merging", "rawhash2", "own", "own"),
    ):
        for cv in ("0.00", "0.44"):
            r = arow(system, seg, cv, norm)
            if r is None:
                continue
            a, g = q(D(r["a_dp"]), "0.001"), q(D(r["indel_frac"]), "0.001")
            backed(a, f"agreement {system}/{seg} CV {cv}")
            backed(g, f"indel fraction {system}/{seg} CV {cv}")
            TABLE3.append([label, seg_label, cv, a, g])
            if r["reads"] != "200":
                bad(f"agreement {system}/{seg} CV {cv} read count", f"{r['reads']}, prose says 200")
            if system == "ours" and (r["bits"], r["events_per_key"]) != ("4", "12"):
                bad(f"agreement {system}/{seg} geometry", "§2.6 says 12 events at 4 bits")
    fx0, fx44 = arow("ours", "fixed", "0.00", "read"), arow("ours", "fixed", "0.44", "read")
    if fx0 and fx44:
        claim("2.8", D(fx44["indel_frac"]) / D(fx0["indel_frac"]), "0.1",
              "fixed-width indel fraction rise (fold), §3.2")

# ---------------------------------------------------------------- calibration, backed half
LOG = RESULTS / "decision-rules-raw" / "votes_s42.log"
log = text_of(LOG)
m = re.search(r"events/read on (\d+) off (\d+) \(ideal ~(\d+) at (\d+) samples/base\)", log)
t = re.search(r"RESULT\s+zero-FP: >=\d+ TPR ([\d.]+)%", log)
if m and t:
    on_ev, ideal = D(m.group(1)), D(m.group(3))
    claim("1562", on_ev, "1", "on-target events per read, frozen detector, Supp. S2", "supp")
    claim("21.9", (ideal - on_ev) / ideal * 100, "0.1", "under-segmentation (%), Supp. S2", "supp")
    claim("14.2", D(t.group(1)), "0.1", "seed 42 TPR at CV 0.44, captured log, Supp. S2", "supp")
    if ours and not (D(ours["0.44"]["detected_lo"]) <= D(t.group(1)) <= D(ours["0.44"]["detected_hi"])):
        bad("seed 42 TPR lies within Table 1's CV 0.44 range", t.group(1))
    else:
        ok("seed 42 TPR lies within Table 1's CV 0.44 detected range")
else:
    bad("calibration log parse", f"{rel(LOG)} lacks the events/read or RESULT line")

claim("0.44", D("4.0") / D("9.0"), "0.01", "Squigulator default CV from 4.0/9.0, §1")

# ---------------------------------------------------------------- provenance of parameters
needles(RESULTS / "dwell-sweep-ours.csv",
        ["Three independent seeds", "(42/43/44), 500 reads per cell", "10 chunks of 2000 samples",
         "0.4 s at the dna-r10-prom", "5 kHz", "20 Mb chr20 indexed on both strands",
         "composition-matched shuffle", "3 x 14 geometry, minimizer window 10, fixed-width",
         "4 x 12 geometry, minimizer window 1, two-window t-test"],
        "§2.1-2.4 sweep parameters")
needles(LOG,
        ["seed=42", "on=ref_std4.4.slow5", "ref 40000000 bases (both strands)", "9-mer model",
         "geometry 4 x 12", "window 1, EVENT-DETECTED", "10 chunks of 2000 samples",
         "ideal ~2000 at 10 samples/base"],
        "§2.1-2.4 parameters in the captured log")
needles(RESULTS / "decision-rules.csv",
        ["threshold 3.0, min_len 4, window 4", "--dwell-mean 9.0 --dwell-std 4.0, CV ~0.44"],
        "§1, §2.4 detector parameters and Squigulator default")
needles(RESULTS / "rawhash-seeds.csv",
        ["-q 4 -e 8 -w 0 --sig-diff 0.35", "within 500 bases", "matching strand",
         "same three seeds as decision-rules.csv"],
        "§2.5 RawHash2 parameters and scoring")
needles(RESULTS / "rawhash-seeds-raw" / "on_s42.log",
        ["Version: 2.1", "pore kmer size: 9", "concatanated events: 8; quantization bits: 4"],
        "§2.5 RawHash2 version, k and key geometry (Table 1)")
needles(RESULTS / "rawhash-grid-seeds.csv",
        ["scripts/rerun_rawhash_grid.sh", "-k 9 -q 4 -e 8 --sig-diff 0.35", "within 500 bases",
         "matching strand", "Seed 42 re-runs rawhash-dwell-sweep.csv"],
        "§2.5 three-seed grid parameters and scoring")
holds((RESULTS / "rawhash-grid-raw" / "raw.tar.gz").exists(),
      "the grid's raw PAF and logs are committed (rawhash-grid-raw/raw.tar.gz)")
needles(RESULTS / "rawhash-grid-zero-fp.csv",
        ["Produced by", "scripts/rawhash_zero_fp.py from results/rawhash-grid-raw/raw.tar.gz",
         "reproduced rawhash-grid-seeds.csv exactly", ",".join(["seed", "w", "dwell_cv", "reads"])],
        "Supp. S4 matched-operating-point file and its provenance")
# The threshold rules as Supp. S4 states them: the twelfth PAF column, one above the highest
# control read, and a budget of len // 100 control reads.
needles(REPO / "scripts" / "rawhash_zero_fp.py",
        ["mapq = int(f[11])", "t0 = ctrl[0] + 1 if ctrl else 0", "budget = len(off) // 100",
         "sum(q >= t for q in ctrl) <= budget", "c and q >= t0", "c and q >= t1"],
        "Supp. S4 threshold rules as rawhash_zero_fp.py applies them")
needles(RESULTS / "agreement-summary.csv",
        ["200 reads per cell", "--sig-diff merging", "--out-quantize-ref",
         "matches / aligned pairs under the maximising alignment",
         "gap columns as a fraction of alignment columns", "from all samples of the read"],
        "§2.6 agreement estimator")
needles(REPO / "third_party_patches" / "icarust-dwell-variability.patch",
        ["ICARUST_DWELL_CV", "LogNormal", "(1.0 + dwell_cv * dwell_cv).ln()", "- 0.5 * sigma2",
         "samples_per_base * 8", "clamp(1.0, hi)", "This could also be worked out from profile.dwell_mean",
         "Unset or 0 reproduces the original fixed-count behaviour exactly"],
        "§3.4 Icarust patch behaviour")
needles(REPO / "third_party_patches" / "rawhash2-reference-quantise-dump.patch",
        ["out-quantize-ref"], "§2.6 RawHash2 reference-side dump")

# Method parameters and their provenance, above. A numeral here is accepted anywhere.
STRUCTURAL = {
    "0", "1", "2", "3", "4", "5", "6", "8", "9", "10", "12", "14", "20", "40",
    "42", "43", "44", "200", "500", "2000",
    *CVS, "4.4", "10.0", "9.0", "4.0", "0.4", "0.35", "3.0",
    "136119",                                    # postcode in the affiliation
}
YEARS = {"2004", "2021", "2023", "2024", "2025", "2026"}   # author-year citations, md only

# ---------------------------------------------------------------- outstanding values
# Each needs a captured run that does not exist. They are allowed only in the Supplementary
# Information, never in the main text, and only with a dagger. Where a transcription exists
# it is checked.
OUTSTANDING: list[tuple[str, str, str]] = []


def outstanding(key: str, why: str, needs: str) -> None:
    OUTSTANDING.append((key, why, needs))


ica = {r["icarust"]: r for r in rows("icarust-stock-vs-patched.csv")}
if ica:
    st, pa = ica["stock"], ica["patched"]
    if (st["fixed_accept_pct"], st["detected_accept_pct"], pa["fixed_accept_pct"],
            pa["detected_accept_pct"], pa["dwell_cv"]) == ("43.5", "9.6", "0.5", "9.6", "0.35"):
        ok("Icarust values transcribed as icarust-stock-vs-patched.csv records them")
    else:
        bad("Icarust transcription", "text and CSV disagree")
    ratio = q(D(st["fixed_accept_pct"]) / D(st["detected_accept_pct"]), "0.1")
    if ratio != "4.5":
        bad("Icarust stock ratio", f"CSV gives {ratio}, prose says 4.5")
needs_ica = ("a re-run of stock and patched Icarust against the client; thesis Appendix F.3 "
             "item 4 records that the CSV is a transcription, not a captured run")
for k in ("43.5%", "9.6%", "0.5%", "4.5"):
    outstanding(k, "Icarust live accept rates, Supp. S1", needs_ica)
needles(REPO / "docs" / "appendices-draft.md",
        ["The Icarust accept rates behind Table 6.2", "not from a captured run"],
        "thesis F.3 records the Icarust values as outstanding")

APP = REPO / "docs" / "appendices-draft.md"
needles(APP, ["| threshold 2.0, `min_len` 4 | 2017 (ideal 2000) | 2.0% |"],
        "threshold-2.0 values as the thesis records them")
needs_cal = "a captured run of the threshold-2.0 detector at CV 0.44"
for k in ("2017", "2.0%"):
    outstanding(k, "threshold-2.0 development run, Supp. S2", needs_cal)
if t:
    r71 = q(D(t.group(1)) / D("2.0"), "0.1")
    if r71 != "7.1":
        bad("calibration ratio", f"14.2/2.0 gives {r71}, prose says 7.1")
outstanding("7.1", "ratio on the threshold-2.0 run, Supp. S2", needs_cal)

needles(APP, ["it scored 0.6% TPR at dwell CV 0 on Squigulator"],
        "cross-generator value as the thesis records it")
needs_x = "a captured run of the own-generator calibration on Squigulator signal at CV 0"
outstanding("0.6%", "cross-generator calibration, Supp. S3", needs_x)
if ours:
    r307 = q(D(ours["0.00"]["detected_mean"]) / D("0.6"), "0.1")
    if r307 != "30.7":
        bad("cross-generator ratio", f"18.4/0.6 gives {r307}, prose says 30.7")
outstanding("30.7", "ratio on the cross-generator run, Supp. S3", needs_x)
OUT_KEYS = {k for k, _, _ in OUTSTANDING}

# ---------------------------------------------------------------- reading the manuscripts
NUM = re.compile(r"(?<![\w.])(\d+(?:\.\d+)?)(%?)", re.ASCII)
WORD_RATIO = re.compile(
    r"\b(?:two|three|four|five|six|seven|eight|nine|ten|eleven|twelve|twenty|thirty|forty|"
    r"fifty|hundred)[- ]?(?:fold|times)\b", re.IGNORECASE)
REFS = re.compile(r"\b(?:Sections?|Tables?|Figures?)\s+\d+(?:\.\d+)?(?:\s+(?:to|and)\s+\d+(?:\.\d+)?)?")


def collapse(s: str) -> str:
    return re.sub(r"\s+", " ", s)


def split_regions(raw: str, begin: str, end: str) -> list[tuple[str, bool]]:
    out, prov, pos = [], False, 0
    for mm in re.finditer(re.escape(begin) + "|" + re.escape(end), raw):
        out.append((raw[pos:mm.start()], prov))
        prov = mm.group(0) == begin
        pos = mm.end()
    out.append((raw[pos:], prov))
    return out


def md_segments(raw: str) -> list[tuple[str, bool]]:
    raw = raw.split("\n## References")[0]
    segs = []
    for seg, prov in split_regions(raw, "<!-- provisional:begin -->", "<!-- provisional:end -->"):
        seg = re.sub(r"<!--.*?-->", "", seg, flags=re.S)
        segs.append((collapse(seg), prov))
    return segs


def md_scan(s: str) -> str:
    s = re.sub(r"!\[[^\]]*\]\([^)]*\)", "", s)
    s = re.sub(r"https?://\S+", "", s)
    s = re.sub(r"\S+@\S+", "", s)
    s = re.sub(r"(^| )#+ \d+(?:\.\d+)* ", r"\1", s)
    s = re.sub(r"(^| )\d+\. (?=\S)", r"\1", s)
    return REFS.sub("", s)


def tex_segments(raw: str) -> list[tuple[str, bool]]:
    body = raw.split(r"\begin{document}", 1)[-1].split(r"\bibliographystyle", 1)[0]
    segs = []
    for seg, prov in split_regions(body, "% provisional:begin", "% provisional:end"):
        seg = re.sub(r"(?<!\\)%.*", "", seg)
        seg = seg.replace(r"\prov{}", "†").replace(r"\prov", "†").replace(r"\%", "%")
        seg = seg.replace("---", "—").replace("--", "–").replace("~", " ")
        seg = seg.replace("$", "").replace(r"\_", "_")
        # Whole control words only: a bare replace of \in would also eat \includegraphics.
        seg = re.sub(r"\\times(?![A-Za-z])", "×", seg)
        seg = re.sub(r"\\in(?![A-Za-z])", "∈", seg)
        segs.append((collapse(seg), prov))
    return segs


def tex_scan(s: str) -> str:
    for pat in (r"\\(?:label|ref|citep?|citet|url|includegraphics(?:\[[^\]]*\])?)\{[^}]*\}",
                # A column spec may nest one level of braces, as in {p{2.0cm}p{3.2cm}}.
                r"\\begin\{tabular\}\{(?:[^{}]|\{[^{}]*\})*\}",
                r"\\multicolumn\{\d+\}", r"\\cmidrule\([^)]*\)\{[^}]*\}",
                r"\\rule\{[^}]*\}\{[^}]*\}", r"[\d.]+\\(?:line|text|column)width",
                # A number glued to a TeX length unit is layout, never data ("12.8 points" has a space).
                r"(?<![\w.])\d+(?:\.\d+)?(?:cm|mm|pt|em|ex|in|bp)\b"):
        s = re.sub(pat, "", s)
    # The manuscript cites its own sections with \ref, but the supplement can only name the
    # manuscript's sections in plain text ("Section~3.1"), so those are stripped here as in md.
    return REFS.sub("", s)


md_raw, tex_raw = text_of(MD), text_of(TEX)
smd_raw, stex_raw = text_of(SMD), text_of(STEX)
# The middle field of each segment says whether outstanding values may appear in it: nowhere in
# the manuscript, anywhere in the Supplementary Information (still marked with a dagger).
FILES = {
    "manuscript.md": [(md_scan(s), False, s) for s, _ in md_segments(md_raw)],
    "manuscript.tex": [(tex_scan(s), False, s) for s, _ in tex_segments(tex_raw)],
}
SUPP_FILES = {
    "supplementary.md": [(md_scan(s), True, s) for s, _ in md_segments(smd_raw)],
    "supplementary.tex": [(tex_scan(s), True, s) for s, _ in tex_segments(stex_raw)],
}
ALL_FILES = {**FILES, **SUPP_FILES}
NORM = {name: " ".join(full for _, _, full in segs) for name, segs in ALL_FILES.items()}
PAIRS = {"main": ("manuscript.md", "manuscript.tex"), "supp": ("supplementary.md", "supplementary.tex")}

# ---- tables, cell by cell from data
# Table 1 lists configurations, not results: every number in it is a method parameter whose
# provenance is checked above, so its rows are fixed here and required verbatim in both files.
CONFIG = [
    ["Fixed-width arm", "constant-width windows", "3 × 14", "10", "TPR at zero false positives", "first 10 chunks"],
    ["Detected arm", "two-window t-test, threshold 3.0", "4 × 12", "1", "TPR at zero false positives",
     "first 10 chunks"],
    ["RawHash2", "its own detection, with event merging", "4 × 8", "0, 5 and 10",
     "correct mapping at its default operating point", "whole read"],
]
for label, table in (("Table 1", CONFIG), ("Table 2", TABLE1), ("Table 3", TABLE2), ("Table 4", TABLE3),
                     ("Table 5", TABLE4)):
    for row in table:
        md_row = "| " + " | ".join(row) + " |"
        tex_row = " & ".join(row) + r" \\"
        for name, want in (("manuscript.md", md_row), ("manuscript.tex", tex_row)):
            if want in NORM[name]:
                ok(f"{label} row {' | '.join(row[:3])}", name)
            else:
                bad(f"{label} row in {name}", f"expected '{want}'")
# The supplement's tables, likewise rebuilt from data and required verbatim in its two files.
# A negative cell is typeset $-$6.6, which the .tex normalisation reads as -6.6.
for label, table in (("Table S1", TABLE_S1), ("Table S2", TABLE_S2), ("Table S3", TABLE_S3)):
    if not table:
        bad(f"{label} rows", "not rebuilt: a data file is missing")
    for row in table:
        md_row = "| " + " | ".join(row) + " |"
        tex_row = " & ".join(row) + r" \\"
        for name, want in (("supplementary.md", md_row), ("supplementary.tex", tex_row)):
            if want in NORM[name]:
                ok(f"{label} row {' | '.join(row[:3])}", name)
            else:
                bad(f"{label} row in {name}", f"expected '{want}'")

# ---- Abstract headline sentences, verbatim in both
ANCHORS = [
    "49.7% against 18.4% true-positive rate at zero false positives",
    "trails it 10.8-fold at the simulator's default CV of 0.44, 1.3% against 14.1%",
    "the replicate ranges do not overlap at any dispersion tested",
    "loses 2.9 points at its default setting",
    "rises from 11.7 points at CV 0 to 19.5 at CV 0.60, and is larger at CV 0.60 in every seed",
    # §3.3: the two single values that are not quoted as a mean with its range.
    "the mean w = 10 cost stays between 11.2 and 12.3 points",
    # §2.7: the one sentence that stands in the main text for the Supplementary Information.
    "they are reported, marked, in the Supplementary Information and are used in no claim here",
    # Figure 1 is a schematic, and its caption must say so.
    "the current levels, durations and key ranks are illustrative, not data",
    # §3.2 and §3.3 at matched operating points: the logical claims the holds() checks back.
    "every replicate at every CV lies between 76.2% and 92.8%",
    "at both windows the ranges are disjoint and the cost is larger at CV 0.60 in every seed",
    "larger in every seed by 2.4 to 20.0 points, but the ranges overlap, and at w = 5 one seed shows no increase",
]
SUPP_ANCHORS = [
    "which all 54 cells do",
    "maps 6.6 points more of its reads correctly than its w = 0 index",
    "at w = 10 sketching costs more at both matched operating points than at the default, at every CV",
    "Only seed 44's cost rises at every step of the sweep, at both windows",
]
for anchors, files in ((ANCHORS, FILES), (SUPP_ANCHORS, SUPP_FILES)):
    for a in anchors:
        for name in files:
            if a in NORM[name]:
                ok(f"anchor in {name}: {a[:40]}…")
            else:
                bad(f"anchor in {name}", f"missing '{a}'")

# ---- every replicated 'mean [range]' appears whole, in both files of its pair
for pat, why, doc in MR_TEXT:
    for name in PAIRS[doc]:
        if re.search(pat, NORM[name]):
            ok(f"mean [range] in {name}: {why[:45]}")
        else:
            bad(f"mean [range] phrase in {name}", f"{why}: no match for {pat}")

# ---- every numeral accounted for
DATA_TOKENS: dict[str, set[str]] = {}
DATA_SEQ: dict[str, list[str]] = {}   # data numerals in reading order, for the md/tex comparison
for name, segs in ALL_FILES.items():
    seen: set[str] = set()
    order: list[str] = []
    before = len(FAIL)
    for scan, prov, _ in segs:
        for wm in WORD_RATIO.finditer(scan):
            bad(f"ratio written as a word in {name}", f"'{wm.group(0)}': write it as digits")
        for mm in NUM.finditer(scan):
            num, pct = mm.group(1), mm.group(2)
            key = num + pct
            ctx = scan[max(0, mm.start() - 30):mm.end() + 20]
            if key in OUT_KEYS:
                after = scan[mm.end():mm.end() + 8]
                if not prov:
                    bad(f"outstanding value {key} in the main text ({name}); "
                        "it belongs in the Supplementary Information", f"…{ctx}…")
                elif not re.match(r"(?:-fold)?\s*†", after):
                    bad(f"outstanding value {key} not marked † in {name}", f"…{ctx}…")
                seen.add(key)
                order.append(key)
            elif num in BACKED:
                seen.add(key)
                order.append(key)
            elif num in STRUCTURAL or (name.endswith(".md") and num in YEARS):
                pass
            else:
                bad(f"unregistered numeral {key} in {name}", f"…{ctx}…")
    DATA_TOKENS[name] = seen
    DATA_SEQ[name] = order
    if len(FAIL) == before:
        ok(f"every numeral in {name} is backed, structural or marked outstanding")

# ---- the two files carry the same data-derived numerals, in the same order
# A set comparison alone misses a number changed to another value that is backed elsewhere: both
# files would still hold the same set. The .tex transcribes the .md sentence for sentence, so the
# data numerals must also appear in one identical sequence, which locates any drift exactly.
for doc, (mdn, texn) in PAIRS.items():
    label = "md and tex" if doc == "main" else "supplement md and tex"
    only_md = DATA_TOKENS[mdn] - DATA_TOKENS[texn]
    only_tex = DATA_TOKENS[texn] - DATA_TOKENS[mdn]
    if only_md or only_tex:
        bad(f"{label} carry the same data-derived numerals",
            f"only in md: {sorted(only_md)}; only in tex: {sorted(only_tex)}")
    else:
        ok(f"{label} carry the same data-derived numerals", f"{len(DATA_TOKENS[mdn])} values")
    seq_md, seq_tex = DATA_SEQ[mdn], DATA_SEQ[texn]
    if seq_md == seq_tex:
        ok(f"{label} carry them in the same order", f"{len(seq_md)} numerals")
    else:
        sm = difflib.SequenceMatcher(a=seq_md, b=seq_tex, autojunk=False)
        where = [f"md[{i1}] {seq_md[max(0, i1 - 2):i2 + 1]} vs tex[{j1}] {seq_tex[max(0, j1 - 2):j2 + 1]}"
                 for tag, i1, i2, j1, j2 in sm.get_opcodes() if tag != "equal"]
        bad(f"{label} carry them in the same order", f"{len(where)} difference(s); first: {where[:3]}")
for c, doc in sorted(set(CLAIMED)):
    for name in PAIRS[doc]:
        if not any(t.rstrip("%") == c for t in DATA_TOKENS[name]):
            bad(f"claimed value {c} absent from {name}", "the script checks a number the text no longer uses")
for k in sorted(OUT_KEYS):
    for name in PAIRS["supp"]:
        if k not in DATA_TOKENS[name]:
            bad(f"outstanding value {k} absent from {name}", "drop it from OUTSTANDING or restore it")

# ---------------------------------------------------------------- figures and references
# Each figure and what it is drawn from: a results file, or for the schematic its own script.
SCHEMATIC = REPO / "scripts" / "draw_dwell_schematic.py"
PAPER_FIGURES = {
    "fig1-dwell-schematic": SCHEMATIC,
    "fig6-1-dwell-crossover": RESULTS / "dwell-sweep-ours.csv",
    "figj-2-rawhash-dwell-3seeds": RESULTS / "rawhash-grid-seeds.csv",
    "figj-3-minimizer-penalty-3seeds": RESULTS / "rawhash-grid-seeds.csv",
}
needles(SCHEMATIC, ["noise and key ranks below", "are illustrative, and the figure caption says so"],
        "Figure 1 is drawn as a schematic, from values its script declares illustrative")
for fig, source in PAPER_FIGURES.items():
    pdf, png = PAPER / "figures" / f"{fig}.pdf", PAPER / "figures" / f"{fig}.png"
    if not (pdf.exists() and png.exists()):
        bad(f"figure {fig}", "pdf or png missing; run draw_dwell_schematic.py and "
                             "generate_thesis_figures.py --no-titles --only 6.1,J6.2,J6.3 "
                             "--outdir paper_dwell_journal/figures")
        continue
    src = source
    if pdf.stat().st_mtime < src.stat().st_mtime:
        bad(f"figure {fig}", f"older than {src.name}; regenerate")
    try:
        from pypdf import PdfReader
        words = "".join(p.extract_text() or "" for p in PdfReader(pdf).pages)
        if re.search(r"Figure [0-9J]\.", words):
            bad(f"figure {fig}", "carries a figure-level title; regenerate with --no-titles")
        else:
            ok(f"figure {fig} exists, is current and is untitled")
    except ImportError:
        ok(f"figure {fig} exists and is current (pypdf absent: title not checked)")
    if f"figures/{fig}.png" not in md_raw or f"figures/{fig}.pdf" not in tex_raw:
        bad(f"figure {fig}", "not referenced by both manuscripts")

def not_this_checkout() -> str | None:
    """None if REPO is this project's own git checkout, otherwise why it is not.

    A release archive, which Zenodo preserves, has no .git, so asking git whether the thesis
    figures are untouched has no answer there. Skipping is right only when the question cannot
    be asked: inside a real checkout the check runs, and a git error there still fails.
    """
    try:
        top = subprocess.run(["git", "rev-parse", "--show-toplevel"], cwd=REPO,
                             capture_output=True, text=True)
    except FileNotFoundError:
        return "git is not installed"
    if top.returncode != 0:
        return "not a git checkout (a release archive, for instance)"
    same = os.path.normcase(str(Path(top.stdout.strip()).resolve())) == os.path.normcase(str(REPO.resolve()))
    if not same:
        return "inside another repository's work tree"
    tracked = subprocess.run(["git", "ls-files", "--error-unmatch", "scripts/verify_dwell_journal_numbers.py"],
                             cwd=REPO, capture_output=True, text=True)
    if tracked.returncode != 0:
        return "a git work tree that does not track this project"
    return None


why_not = not_this_checkout()
if why_not:
    SKIP.append(("thesis figures/ untouched", why_not))
else:
    try:
        dirty = subprocess.run(["git", "status", "--porcelain", "--", "figures"], cwd=REPO,
                               capture_output=True, text=True, check=True).stdout.strip()
        (bad("thesis figures/ untouched", dirty) if dirty else ok("thesis figures/ untouched", "git status"))
    except Exception as exc:
        bad("thesis figures/ untouched", f"git failed: {exc}")

bib = text_of(PAPER / "refs.bib")
keys = set(re.findall(r"@\w+\{([^,]+),", bib))
cited = {k.strip() for grp in re.findall(r"\\cite[pt]?\{([^}]*)\}", tex_raw) for k in grp.split(",")}
if cited - keys:
    bad("every \\cite key is in refs.bib", f"missing {sorted(cited - keys)}")
else:
    ok("every \\cite key is in refs.bib", f"{len(cited)} cited")
if keys - cited:
    bad("every refs.bib entry is cited", f"uncited {sorted(keys - cited)}")
# The supplement has no bibliography of its own, so it must not cite: a \cite there would
# print as "?" in its PDF.
s_cited = re.findall(r"\\cite[pt]?\{([^}]*)\}", stex_raw)
holds(not s_cited, "the Supplementary Information cites nothing it cannot resolve")

# ---------------------------------------------------------------- report
W = 66
print("=" * 78)
print("DWELL JOURNAL MANUSCRIPT: NUMBER VERIFICATION")
print("=" * 78)
print(f"\nAGREE  ({len(PASS)})")
for what, where in PASS:
    print(f"  ok   {what[:W]:<{W}} {where}")
if FAIL:
    print(f"\nDISAGREE  ({len(FAIL)})   <-- manuscript and data do not match")
    for what, why in FAIL:
        print(f"  FAIL {what}")
        print(f"       {why}")
if SKIP:
    print(f"\nSKIPPED  ({len(SKIP)})   not applicable here; does not affect the exit code")
    for what, why in SKIP:
        print(f"  skip {what}: {why}")

print("\n" + "=" * 78)
print("OUTSTANDING: values in the Supplementary Information that no captured run backs")
print("=" * 78)
for key, why, needs in OUTSTANDING:
    print(f"\n  {key:<7} {why}")
    print(f"          needs: {needs}")

print(f"\nsummary: {len(PASS)} agree, {len(FAIL)} disagree, {len(SKIP)} skipped, "
      f"{len(OUTSTANDING)} outstanding, {len(BACKED)} distinct backed values")
sys.exit(1 if FAIL else 0)
