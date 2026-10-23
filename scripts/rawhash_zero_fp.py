#!/usr/bin/env python3
"""RawHash2 at a zero-false-positive operating point, from the grid's own PAF output.

The dwell journal manuscript compares this pipeline's true-positive rate at the lowest score
threshold admitting no control read with RawHash2's correct-mapping rate at RawHash2's default
operating point, which maps 8-12% of control reads, and has to call the two not level-comparable.
This puts RawHash2 on the same statistic, with no new runs: a mapping-quality threshold is raised
until no control read is mapped at or above it, and the correct on-target mappings at or above that
threshold are counted. A 1% budget (at most 5 of 500 control reads) is reported beside it.

The input is results/rawhash-grid-raw/raw.tar.gz, read in place. Before anything new is reported,
every cell is scored with no threshold, exactly as tools/score_rawhash_paf.py scores it, and must
reproduce results/rawhash-grid-seeds.csv. Correctness uses the same rule as that tool: matching
strand, and the mapped interval within 500 bases of the locus squigulator records in the read id.

    python scripts/rawhash_zero_fp.py      # writes results/rawhash-grid-zero-fp.csv
"""

from __future__ import annotations

import csv
import sys
import tarfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
RAW = REPO / "results" / "rawhash-grid-raw" / "raw.tar.gz"
GRID = REPO / "results" / "rawhash-grid-seeds.csv"
OUT = REPO / "results" / "rawhash-grid-zero-fp.csv"
TOL = 500
CV = {"0": "0.00", "1": "0.10", "2": "0.20", "3": "0.30", "4.4": "0.44", "6": "0.60"}


def reads(text: str, want_truth: bool) -> list[tuple[bool, bool, int]]:
    """(mapped, correct, mapq) per read, scored as tools/score_rawhash_paf.py scores it."""
    out = []
    for line in text.splitlines():
        f = line.split("\t")
        if len(f) < 9:
            continue
        if f[5] == "*":
            out.append((False, False, -1))
            continue
        mapq = int(f[11])
        correct = False
        parts = f[0].split("!")
        if want_truth and len(parts) >= 5 and f[4] == parts[4]:
            try:
                t_start, t_end = int(parts[2]), int(parts[3])
                correct = min(abs(int(f[7]) - t_start), abs(int(f[8]) - t_end)) <= TOL
            except ValueError:
                pass
        out.append((True, correct, mapq))
    return out


def pct(a: int, b: int) -> str:
    return "%.1f" % ((100.0 * a / b) if b else 0.0)   # the formatting the scorer uses


HEADER = ["seed", "w", "dwell_cv", "reads", "off_reads", "mapq_zero_fp", "tpr_zero_fp",
          "mapq_1pct_fp", "tpr_1pct_fp"]


def compute() -> tuple[list[list[str]], list[tuple]]:
    """Every row of the output, and every cell that fails to reproduce the grid.

    verify_dwell_journal_numbers.py calls this too, so the manuscript's matched-operating-point
    numbers are recomputed from the raw PAFs on every check, not read back from the CSV.
    """
    with open(GRID, newline="") as f:
        grid = {(r["seed"], r["w"], r["dwell_cv"]): r
                for r in csv.DictReader(ln for ln in f if not ln.startswith("#"))}
    tar = tarfile.open(RAW)
    member = lambda name: tar.extractfile(f"./{name}").read().decode()

    rows, mismatches = [], []
    for w in ("0", "5", "10"):
        for std, cv in CV.items():
            for seed in ("42", "43", "44"):
                tag = f"w{w}_std{std}_s{seed}"
                on, off = reads(member(f"on_{tag}.paf"), True), reads(member(f"off_{tag}.paf"), False)
                # 1. Reproduce the grid exactly before reporting anything new.
                again = (str(len(on)), pct(sum(m for m, _, _ in on), len(on)),
                         pct(sum(c for _, c, _ in on), len(on)), str(len(off)),
                         pct(sum(m for m, _, _ in off), len(off)))
                g = grid[(seed, w, cv)]
                want = (g["reads"], g["mapped_pct"], g["correct_pct"], g["off_reads"], g["false_mapped_pct"])
                if again != want:
                    mismatches.append((tag, again, want))
                # 2. Zero false positives: one above the highest mapping quality any control read gets.
                ctrl = sorted((q for m, _, q in off if m), reverse=True)
                t0 = ctrl[0] + 1 if ctrl else 0
                tpr0 = pct(sum(c and q >= t0 for _, c, q in on), len(on))
                # 3. A 1% budget: the lowest threshold leaving at most 1% of control reads mapped.
                budget = len(off) // 100
                t1 = next(t for t in range(0, 256) if sum(q >= t for q in ctrl) <= budget)
                tpr1 = pct(sum(c and q >= t1 for _, c, q in on), len(on))
                rows.append([seed, w, cv, str(len(on)), str(len(off)), str(t0), tpr0, str(t1), tpr1])
    return rows, mismatches


def main() -> int:
    rows, mismatches = compute()
    if mismatches:
        for m in mismatches[:5]:
            print("MISMATCH", m, file=sys.stderr)
        print(f"{len(mismatches)} cells do not reproduce rawhash-grid-seeds.csv; nothing written",
              file=sys.stderr)
        return 1
    with open(OUT, "w", newline="\n", encoding="utf-8") as f:
        f.write("# RawHash2 across the dwell x window grid at matched operating points. Produced by\n"
                "# scripts/rawhash_zero_fp.py from results/rawhash-grid-raw/raw.tar.gz, after every cell\n"
                "# was rescored without a threshold and reproduced rawhash-grid-seeds.csv exactly.\n"
                "#\n"
                "# mapq_zero_fp  the lowest mapping-quality threshold at which no control read is mapped\n"
                "# tpr_zero_fp   percentage of on-target reads mapped correctly at or above that threshold\n"
                "# mapq_1pct_fp  the lowest threshold leaving at most 1% of control reads mapped\n"
                "# tpr_1pct_fp   percentage of on-target reads mapped correctly at or above that threshold\n")
        wr = csv.writer(f, lineterminator="\n")
        wr.writerow(HEADER)
        wr.writerows(rows)
    print(f"all 54 cells reproduce rawhash-grid-seeds.csv; wrote {OUT.relative_to(REPO)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
