#!/usr/bin/env python3
"""Generate every data figure in the thesis from the recorded data in results/.

Every figure here is produced from a file under results/ and from nothing else. There are no
numbers in this script that are not either read from a file or derived arithmetically from
one, which is the property that makes the figures checkable: a corrected measurement changes
the file and the figure follows. Figures that are diagrams rather than plots -- the data-plane
block diagram, the arena state machine, the SMT map -- are not here, because they encode
structure rather than data; they are in figures/diagrams/ as hand-written SVG.

Usage:
    python scripts/generate_thesis_figures.py [--outdir figures] [--format pdf,png,svg]

Requires matplotlib and numpy. Writes one file per figure per requested format, plus
figures/MANIFEST.md listing what was produced from what.
"""

from __future__ import annotations

import argparse
import csv
import math
import os
import sys
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
from matplotlib.lines import Line2D

REPO = Path(__file__).resolve().parent.parent
RESULTS = REPO / "results"

# A deliberately plain style. A thesis figure is read in print, at one column width, probably
# in greyscale, so the encoding is line style and marker first and colour second.
plt.rcParams.update(
    {
        "figure.figsize": (6.4, 4.0),
        "figure.dpi": 150,
        "savefig.dpi": 300,
        "savefig.bbox": "tight",
        "font.size": 9,
        "axes.titlesize": 10,
        "axes.labelsize": 9,
        "legend.fontsize": 8,
        "axes.grid": True,
        "grid.alpha": 0.3,
        "grid.linewidth": 0.5,
        "axes.spines.top": False,
        "axes.spines.right": False,
        "lines.linewidth": 1.4,
        "lines.markersize": 4.5,
    }
)

OURS = "#1f3b73"
THEIRS = "#a6360a"
NEUTRAL = "#555555"

# Figure-level titles are suppressed by --no-titles, for the standalone paper, which puts
# them in the LaTeX caption instead. Panel titles on individual axes are kept either way:
# they label sub-axes and no caption replaces them.
TITLES = True


def ftitle(fig, *args, **kwargs):
    if TITLES:
        fig.suptitle(*args, **kwargs)


def atitle(ax, *args, **kwargs):
    if TITLES:
        ax.set_title(*args, **kwargs)


MANIFEST: list[tuple[str, str, str]] = []


def record(name: str, sources: str, caption: str) -> None:
    MANIFEST.append((name, sources, caption))


def save(fig, name: str, outdir: Path, formats: list[str]) -> None:
    for ext in formats:
        # PDF carries a creation timestamp by default, so regenerating unchanged figures
        # dirties the working tree and every commit touching a figure looks like a content
        # change. Suppressing it makes regeneration byte-stable, which is what lets
        # audit_manuscript.py compare figure and data timestamps meaningfully.
        kw = {"metadata": {"CreationDate": None}} if ext == "pdf" else {}
        fig.savefig(outdir / f"{name}.{ext}", **kw)
    plt.close(fig)
    print(f"  wrote {name} ({', '.join(formats)})")


def read_csv_rows(path: Path) -> list[dict]:
    """Read a CSV whose leading '#' lines are provenance commentary, not data."""
    with open(path, newline="") as f:
        lines = [ln for ln in f if not ln.lstrip().startswith("#")]
    return list(csv.DictReader(lines))


# ---------------------------------------------------------------------------------------
# Chapter 5 -- latency
# ---------------------------------------------------------------------------------------

HGRM = RESULTS / "hgrm"
MATRIX_CELLS = ["pinned-2", "unpinned-2", "pinned-4", "unpinned-4"]


def load_hgrm(path: Path) -> tuple[np.ndarray, np.ndarray]:
    """Return (value_us, percentile) from an HdrHistogram percentile dump.

    The dump's trailing lines are summary statistics rather than percentile rows, and its
    final percentile row carries 1.0, which cannot be plotted on an inverted-tail axis.
    Both are dropped here rather than filtered at the call site.
    """
    vals, pct = [], []
    with open(path) as f:
        for line in f:
            parts = line.split()
            if len(parts) < 3:
                continue
            try:
                v, p = float(parts[0]), float(parts[1])
            except ValueError:
                continue
            if not 0.0 <= p < 1.0:
                continue
            vals.append(v)
            pct.append(p)
    return np.asarray(vals), np.asarray(pct)


def fig_5_1(outdir: Path, formats: list[str]) -> None:
    """Decision-latency distribution for the four cells of the placement matrix."""
    fig, (ax, axz) = plt.subplots(1, 2, figsize=(9.0, 3.8))
    styles = {
        "pinned-2": (OURS, "-"),
        "unpinned-2": (OURS, "--"),
        "pinned-4": (THEIRS, "-"),
        "unpinned-4": (THEIRS, "--"),
    }
    for cell in MATRIX_CELLS:
        colour, ls = styles[cell]
        for rep in range(1, 5):
            p = HGRM / f"{cell}_{rep}.hgrm"
            if not p.exists():
                continue
            v, q = load_hgrm(p)
            if v.size == 0:
                continue
            tail = 1.0 / (1.0 - q)
            for a in (ax, axz):
                a.plot(
                    tail, v, color=colour, linestyle=ls, alpha=0.75,
                    linewidth=1.0, label=cell if rep == 1 else None,
                )

    # Percentiles are what the text quotes; 1/(1-p) is only how a tail is plotted legibly.
    ticks = [(2, "p50"), (10, "p90"), (100, "p99"), (1000, "p99.9"), (10000, "p99.99")]
    for a, title, keep in ((ax, "full tail", ticks), (axz, "median to p99", ticks[:3])):
        a.set_xscale("log")
        a.set_xlabel("percentile")
        a.set_ylabel("per-chunk decision latency (µs)")
        a.set_title(title)
        a.set_xticks([t for t, _ in keep])
        a.set_xticklabels([lbl for _, lbl in keep])
        a.minorticks_off()
    ax.set_yscale("log")
    axz.set_xlim(1.5, 120)
    axz.set_ylim(10, 160)

    # The budget only belongs on the full-tail panel; on the zoomed panel it is off-scale.
    ax.axhline(1560, color=NEUTRAL, linestyle=":", linewidth=1.0)
    ax.text(
        9000, 1900, "service bound 1.56 ms", fontsize=7, color=NEUTRAL, ha="right",
    )
    ax.legend(loc="lower right", frameon=False)
    ftitle(fig,
        "Figure 5.1  Decision latency by shard count and placement, 4 repetitions per cell\n"
        "37 kb reference (regime R1); p99 and above are contaminated by the hypervisor (§3.8)",
        fontsize=9,
    )
    fig.tight_layout(rect=(0, 0, 1, 0.88))
    save(fig, "fig5-1-latency-distribution", outdir, formats)
    record(
        "fig5-1-latency-distribution",
        "results/hgrm/{pinned,unpinned}-{2,4}_{1..4}.hgrm",
        "Decision-latency distributions for the four cells of the placement matrix.",
    )


def fig_5_2(outdir: Path, formats: list[str]) -> None:
    """The power-of-two bucket-edge artefact of §5.2.2.

    This is the one figure whose inputs are constants rather than a data file, because what it
    illustrates is a property of an instrument that no longer exists in the code. The four
    reported values and the TSC frequency are quoted in §5.2.2 and are the figure's only
    inputs.
    """
    tsc_mhz = 2418.7
    reported = [111.93, 112.12, 223.66, 447.59]
    # Representable p99 values: a power-of-two cycle count divided by the TSC frequency.
    edges = [(1 << k) / tsc_mhz for k in range(16, 21)]
    edges = [e for e in edges if 20.0 <= e <= 600.0]

    fig, ax = plt.subplots(figsize=(6.4, 3.0))
    for i, e in enumerate(edges):
        ax.axvline(e, color=NEUTRAL, linestyle="--", linewidth=0.9)
        ax.text(e, 1.42, f"{e:.2f}", rotation=90, fontsize=7, color=NEUTRAL,
                ha="right", va="top")
    for i, r in enumerate(reported):
        ax.plot([r], [1.0 - 0.12 * i], marker="o", color=THEIRS, markersize=7)
        ax.text(r * 1.04, 1.0 - 0.12 * i, f"run {i + 1}: {r} µs", fontsize=8,
                va="center", color=THEIRS)

    ax.set_xscale("log")
    ax.set_xlim(35, 700)
    ax.set_ylim(0.5, 1.5)
    ax.set_yticks([])
    ax.set_xlabel("reported 99th percentile (µs, log scale)")
    ax.grid(False)
    atitle(ax,
        "Figure 5.2  Four reported p99 values against the instrument's representable set\n"
        f"power-of-two cycle buckets at an invariant TSC of {tsc_mhz} MHz",
        fontsize=9,
    )
    ax.legend(
        handles=[
            Line2D([], [], color=NEUTRAL, linestyle="--", label="representable value"),
            Line2D([], [], color=THEIRS, marker="o", linestyle="", label="reported value"),
        ],
        loc="lower right", frameon=False,
    )
    save(fig, "fig5-2-bucket-edge-artefact", outdir, formats)
    record(
        "fig5-2-bucket-edge-artefact",
        "constants quoted in §5.2.2 (no data file; the instrument was replaced)",
        "Every reported p99 coincides with a bucket boundary, so the agreement was an artefact.",
    )


def fig_5_3(outdir: Path, formats: list[str]) -> None:
    """Run-to-run spread of each percentile, which is the defensible claim about pinning."""
    rows = read_csv_rows(RESULTS / "latency-matrix-wsl2.csv")
    pcts = [("p50", "p50"), ("p90", "p90"), ("p99", "p99"), ("p99_9", "p99.9")]
    fig, ax = plt.subplots(figsize=(6.4, 3.6))
    width = 0.2
    xs = np.arange(len(pcts))
    for i, cell in enumerate(MATRIX_CELLS):
        vals = [r for r in rows if r["config"] == cell]
        spreads = []
        for key, _ in pcts:
            v = [float(r[key]) for r in vals]
            spreads.append(100.0 * (max(v) - min(v)) / min(v))
        ax.bar(
            xs + (i - 1.5) * width, spreads, width,
            label=cell,
            color=OURS if cell.endswith("-2") else THEIRS,
            alpha=1.0 if cell.startswith("pinned") else 0.45,
            edgecolor="white", linewidth=0.5,
        )
    ax.set_xticks(xs)
    ax.set_xticklabels([lbl for _, lbl in pcts])
    ax.set_ylabel("run-to-run spread (% of minimum)")
    ax.set_yscale("log")
    ax.legend(frameon=False, ncol=2)
    atitle(ax,
        "Figure 5.3  Run-to-run spread by percentile, 4 repetitions per cell\n"
        "pinning does not move the median; it makes the tail repeatable",
        fontsize=9,
    )
    save(fig, "fig5-3-percentile-spread", outdir, formats)
    record(
        "fig5-3-percentile-spread",
        "results/latency-matrix-wsl2.csv",
        "Run-to-run spread of each percentile, by shard count and placement.",
    )


def fig_5_4(outdir: Path, formats: list[str]) -> None:
    """Regime R3 across nine runs, against the service bound.

    The point is the contrast between the body of the distribution, which is reproducible, and
    the tail, which is not: eight runs sit inside the budget through the 99.9th percentile and
    one does not, and that one is the worst at every percentile including the mean.
    """
    rows = [r for r in read_csv_rows(RESULTS / "r3-latency.csv") if r["run"] != "median"]
    fig, (ax, axb) = plt.subplots(1, 2, figsize=(9.6, 3.9),
                                  gridspec_kw={"width_ratios": [1.5, 1]})

    for name in sorted(HGRM.glob("r3_frozen_*.hgrm")):
        v, q = load_hgrm(name)
        if v.size == 0:
            continue
        outlier = "b1_1" in name.name
        ax.plot(1.0 / (1.0 - q), v,
                color=THEIRS if outlier else OURS,
                linewidth=1.5 if outlier else 0.9,
                alpha=1.0 if outlier else 0.6,
                zorder=5 if outlier else 2,
                label=("run b1_1" if outlier else None))
    ax.axhline(1560, color=NEUTRAL, linestyle=":", linewidth=1.2)
    ax.text(1.6, 1760, "service bound 1.56 ms", fontsize=7, color=NEUTRAL)
    ax.set_xscale("log")
    ax.set_yscale("log")
    ticks = [(2, "p50"), (10, "p90"), (100, "p99"), (1000, "p99.9"), (10000, "p99.99")]
    ax.set_xticks([t for t, _ in ticks])
    ax.set_xticklabels([l for _, l in ticks])
    ax.minorticks_off()
    ax.set_xlabel("percentile")
    ax.set_ylabel("per-chunk decision latency (µs)")
    ax.set_title("nine runs, 64 Mb reference, frozen geometry", fontsize=9)
    ax.legend(loc="upper left", frameon=False, fontsize=7.5)

    # Per-run percentiles, so the one crossing run is identifiable rather than merely visible.
    keys = [("p50", "p50"), ("p90", "p90"), ("p99", "p99"), ("p99_9", "p99.9")]
    xs = np.arange(len(keys))
    for r in rows:
        outlier = r["run"] == "b1_1"
        axb.plot(xs, [float(r[k]) for k, _ in keys],
                 marker="o" if outlier else ".",
                 color=THEIRS if outlier else OURS,
                 linewidth=1.4 if outlier else 0.8,
                 alpha=1.0 if outlier else 0.55,
                 markersize=5 if outlier else 3,
                 zorder=5 if outlier else 2)
    axb.axhline(1560, color=NEUTRAL, linestyle=":", linewidth=1.2)
    axb.set_yscale("log")
    axb.set_xticks(xs)
    axb.set_xticklabels([l for _, l in keys])
    axb.set_ylabel("µs")
    axb.set_title("per-run percentiles", fontsize=9)

    ftitle(fig,
        "Figure 5.4  Regime R3: the body reproduces, the tail does not (§5.6)\n"
        "eight runs lie inside the budget through p99.9; one crosses it at p99 and p99.9",
        fontsize=9,
    )
    fig.tight_layout(rect=(0, 0, 1, 0.87))
    save(fig, "fig5-4-r3-distribution", outdir, formats)
    record(
        "fig5-4-r3-distribution",
        "results/hgrm/r3_frozen_*.hgrm, results/r3-latency.csv",
        "Regime R3 latency across nine runs against the 1.56 ms service bound.",
    )


# ---------------------------------------------------------------------------------------
# Chapter 4 -- decision rules
# ---------------------------------------------------------------------------------------


def fig_4_1(outdir: Path, formats: list[str]) -> None:
    """The six decision rules in triplicate, with the seed spread shown rather than hidden."""
    rows = read_csv_rows(RESULTS / "decision-rules.csv")
    labels = {
        "exact-diagonal votes (256 counters)": "votes\n(256 counters)",
        "greedy chains (control)": "greedy\nβ = 0 control",
        "greedy chains": "greedy\nβ = 12",
        "anchor-history chains": "anchor-history",
    }
    names, means, los, his, cols = [], [], [], [], []
    for r in rows:
        base = labels.get(r["rule"], r["rule"])
        extra = ""
        if r["rule"] == "anchor-history chains":
            extra = f"\nL = {r['lookback']}" + (" + occ" if r["occ_filter"] == "yes" else "")
        names.append(base + extra)
        means.append(float(r["tpr_mean"]))
        los.append(float(r["tpr_lo"]))
        his.append(float(r["tpr_hi"]))
        cols.append(NEUTRAL if "control" in r["rule"] else OURS)

    xs = np.arange(len(names))
    fig, ax = plt.subplots(figsize=(7.4, 4.0))
    ax.bar(xs, means, 0.6, color=cols, edgecolor="white", linewidth=0.6)
    ax.errorbar(xs, means,
                yerr=[np.array(means) - np.array(los), np.array(his) - np.array(means)],
                fmt="none", ecolor="#222", elinewidth=1.0, capsize=3.5, zorder=4)
    for x, m, h in zip(xs, means, his):
        ax.text(x, h + 0.35, f"{m:.1f}", ha="center", fontsize=8)
    best = int(np.argmax(means))
    ax.annotate("best measured", xy=(xs[best], his[best] + 0.9),
                xytext=(xs[best], max(his) + 3.0), ha="center", fontsize=7.5, color=OURS,
                arrowprops=dict(arrowstyle="->", color=OURS, lw=0.8))
    ax.set_xticks(xs)
    ax.set_xticklabels(names, fontsize=7)
    ax.set_ylabel("TPR at zero false positives (%)")
    ax.set_ylim(0, max(his) + 5)
    atitle(ax,
        "Figure 4.1  Decision rules at the Chapter 6 operating point, 3 seeds × 500 reads\n"
        "bars span the observed range; adjacent rules overlap, so a single run cannot "
        "separate them",
        fontsize=9,
    )
    save(fig, "fig4-1-decision-rules", outdir, formats)
    record(
        "fig4-1-decision-rules",
        "results/decision-rules.csv",
        "The six decision rules with their across-seed ranges.",
    )


# ---------------------------------------------------------------------------------------
# Chapter 6 -- dwell
# ---------------------------------------------------------------------------------------


def fig_6_1(outdir: Path, formats: list[str]) -> None:
    """The crossover: fixed-width wins only at the one dispersion no pore exhibits."""
    rows = read_csv_rows(RESULTS / "dwell-sweep-ours.csv")
    cv = np.array([float(r["dwell_cv"]) for r in rows])
    fig, ax = plt.subplots()
    for prefix, label, colour, marker in (
        ("fixed", "fixed-width segmentation (3 × 14, w = 10)", THEIRS, "s"),
        ("detected", "detected events (4 × 12, w = 1)", OURS, "o"),
    ):
        mean = np.array([float(r[f"{prefix}_mean"]) for r in rows])
        lo = np.array([float(r[f"{prefix}_lo"]) for r in rows])
        hi = np.array([float(r[f"{prefix}_hi"]) for r in rows])
        ax.errorbar(
            cv, mean, yerr=[mean - lo, hi - mean], color=colour, marker=marker,
            capsize=2.5, elinewidth=0.9, label=label,
        )
    ax.axvspan(0.0, 0.10, color=NEUTRAL, alpha=0.10)
    ax.text(0.052, 46, "crossover", fontsize=8, ha="center", color=NEUTRAL)
    ax.axvline(0.44, color=NEUTRAL, linestyle=":", linewidth=1.0)
    ax.text(0.45, 46, "simulator default", fontsize=8, color=NEUTRAL)
    ax.set_xlabel("dwell-time coefficient of variation")
    ax.set_ylabel("true-positive rate at zero false positives (%)")
    ax.set_ylim(0, 55)
    ax.legend(frameon=False, loc="center right")
    atitle(ax,
        "Figure 6.1  Dwell sensitivity of two segmentation choices, 3 seeds, 500 reads/cell\n"
        "bars span the observed range across seeds",
        fontsize=9,
    )
    save(fig, "fig6-1-dwell-crossover", outdir, formats)
    record(
        "fig6-1-dwell-crossover",
        "results/dwell-sweep-ours.csv",
        "Fixed-width collapses and detection does not; the ranking inverts between CV 0 and 0.10.",
    )


def _rawhash_rows() -> dict[int, list[dict]]:
    rows = read_csv_rows(RESULTS / "rawhash-dwell-sweep.csv")
    by_w: dict[int, list[dict]] = {}
    for r in rows:
        by_w.setdefault(int(r["w"]), []).append(r)
    for v in by_w.values():
        v.sort(key=lambda r: float(r["dwell_cv"]))
    return by_w


def fig_6_2(outdir: Path, formats: list[str]) -> None:
    """The comparison mapper does not collapse -- the result that bounds Chapter 6."""
    by_w = _rawhash_rows()
    fig, ax = plt.subplots()
    styles = {0: (OURS, "o", "-"), 5: (NEUTRAL, "^", "--"), 10: (THEIRS, "s", "-.")}
    for w, rows in sorted(by_w.items()):
        colour, marker, ls = styles.get(w, (NEUTRAL, ".", "-"))
        cv = [float(r["dwell_cv"]) for r in rows]
        ax.plot(
            cv, [float(r["correct_pct"]) for r in rows], color=colour, marker=marker,
            linestyle=ls, label=f"w = {w}" + (" (its default)" if w == 0 else ""),
        )
        ax.plot(
            cv, [float(r["false_mapped_pct"]) for r in rows], color=colour, marker=marker,
            linestyle=":", alpha=0.55, linewidth=1.0,
        )
    ax.axvline(0.44, color=NEUTRAL, linestyle=":", linewidth=1.0)
    ax.set_xlabel("dwell-time coefficient of variation")
    ax.set_ylabel("per cent of reads")
    ax.set_ylim(0, 100)
    handles, labels = ax.get_legend_handles_labels()
    handles.append(Line2D([], [], color=NEUTRAL, linestyle=":", alpha=0.6))
    labels.append("false-mapped on shuffled control")
    ax.legend(handles, labels, frameon=False, loc="center left", ncol=2)
    atitle(ax,
        "Figure 6.2  Comparison mapper under the same dwell sweep, by minimizer window\n"
        "solid: correctly mapped. Its default operating point is not zero-false-positive.",
        fontsize=9,
    )
    save(fig, "fig6-2-rawhash-dwell", outdir, formats)
    record(
        "fig6-2-rawhash-dwell",
        "results/rawhash-dwell-sweep.csv",
        "Correct and false mapping rates for the comparison mapper across the dwell sweep.",
    )


def fig_6_3(outdir: Path, formats: list[str]) -> None:
    """The novel finding: the cost of sketching grows with time-domain noise."""
    by_w = _rawhash_rows()
    base = {float(r["dwell_cv"]): float(r["correct_pct"]) for r in by_w[0]}
    fig, ax = plt.subplots()
    for w, colour, marker in ((5, NEUTRAL, "^"), (10, THEIRS, "s")):
        rows = by_w[w]
        cv = [float(r["dwell_cv"]) for r in rows]
        pen = [base[c] - float(r["correct_pct"]) for c, r in zip(cv, rows)]
        ax.plot(cv, pen, color=colour, marker=marker, label=f"w = {w} against w = 0")
        if w == 10:
            ax.annotate(
                f"{pen[0]:.1f} → {pen[-1]:.1f} points",
                xy=(cv[-1], pen[-1]), xytext=(0.30, pen[-1] + 2.5),
                fontsize=8, color=colour,
                arrowprops=dict(arrowstyle="->", color=colour, lw=0.8),
            )
    ax.axvline(0.44, color=NEUTRAL, linestyle=":", linewidth=1.0)
    ax.set_xlabel("dwell-time coefficient of variation")
    ax.set_ylabel("accuracy cost of sketching (percentage points)")
    ax.set_ylim(0, 25)
    ax.legend(frameon=False, loc="upper left")
    atitle(ax,
        "Figure 6.3  The sketching trade-off is dwell-dependent\n"
        "the tool documents that -w may reduce accuracy; that the cost grows with\n"
        "time-domain noise is not documented or measured elsewhere",
        fontsize=9,
    )
    save(fig, "fig6-3-minimizer-penalty", outdir, formats)
    record(
        "fig6-3-minimizer-penalty",
        "results/rawhash-dwell-sweep.csv",
        "Accuracy cost of minimizer sketching as a function of dwell dispersion.",
    )


# ---------------------------------------------------------------------------------------
# Journal manuscript (paper_dwell_journal/): Figures 6.2 and 6.3 over three seeds. Separate
# functions with separate names, so the thesis figures above are untouched.
# ---------------------------------------------------------------------------------------

GRID_SEEDS = ("42", "43", "44")
GRID_CVS = ("0.00", "0.10", "0.20", "0.30", "0.44", "0.60")


def _grid() -> dict[tuple[str, str, str], dict]:
    rows = read_csv_rows(RESULTS / "rawhash-grid-seeds.csv")
    return {(r["seed"], r["w"], r["dwell_cv"]): r for r in rows}


def _spread(values: np.ndarray) -> tuple[np.ndarray, list[np.ndarray]]:
    """Mean over seeds, and the asymmetric error bar that spans the observed range."""
    mean = values.mean(axis=1)
    return mean, [mean - values.min(axis=1), values.max(axis=1) - mean]


def fig_j_2(outdir: Path, formats: list[str]) -> None:
    """The comparison mapper under the dwell sweep, three seeds."""
    g = _grid()
    cv = np.array([float(c) for c in GRID_CVS])
    fig, ax = plt.subplots()
    styles = {"0": (OURS, "o", "-"), "5": (NEUTRAL, "^", "--"), "10": (THEIRS, "s", "-.")}
    for w, (colour, marker, ls) in styles.items():
        for col in ("correct_pct", "false_mapped_pct"):
            v = np.array([[float(g[(s, w, c)][col]) for s in GRID_SEEDS] for c in GRID_CVS])
            mean, err = _spread(v)
            correct = col == "correct_pct"
            ax.errorbar(
                cv, mean, yerr=err, color=colour, marker=marker,
                linestyle=ls if correct else ":", alpha=1.0 if correct else 0.55,
                linewidth=None if correct else 1.0, capsize=2.5, elinewidth=0.9,
                label=(f"w = {w}" + (" (default)" if w == "0" else "")) if correct else None,
            )
    ax.axvline(0.44, color=NEUTRAL, linestyle=":", linewidth=1.0)
    ax.set_xlabel("dwell-time coefficient of variation")
    ax.set_ylabel("per cent of reads")
    ax.set_ylim(0, 100)
    handles, labels = ax.get_legend_handles_labels()
    handles.append(Line2D([], [], color=NEUTRAL, linestyle=":", alpha=0.6))
    labels.append("false-mapped on shuffled control")
    # One column, so the legend stays left of the dotted line at the simulator's default CV.
    ax.legend(handles, labels, frameon=False, loc="center left", ncol=1)
    atitle(ax, "Figure J.2  Comparison mapper under the dwell sweep, three seeds", fontsize=9)
    save(fig, "figj-2-rawhash-dwell-3seeds", outdir, formats)
    record(
        "figj-2-rawhash-dwell-3seeds",
        "results/rawhash-grid-seeds.csv",
        "Correct and false mapping rates for the comparison mapper, mean and range over three seeds.",
    )


def fig_j_3(outdir: Path, formats: list[str]) -> None:
    """The sketching penalty, paired within each seed, three seeds."""
    g = _grid()
    cv = np.array([float(c) for c in GRID_CVS])
    fig, ax = plt.subplots()
    top = 0.0
    for w, colour, marker in (("5", NEUTRAL, "^"), ("10", THEIRS, "s")):
        # Paired: every window of one seed maps the same reads, so the penalty is taken within a
        # seed and only then summarised across seeds.
        p = np.array([[float(g[(s, "0", c)]["correct_pct"]) - float(g[(s, w, c)]["correct_pct"])
                       for s in GRID_SEEDS] for c in GRID_CVS])
        mean, err = _spread(p)
        top = max(top, float(p.max()))
        ax.errorbar(cv, mean, yerr=err, color=colour, marker=marker, capsize=2.5, elinewidth=0.9,
                    label=f"w = {w} against w = 0")
    ax.axvline(0.44, color=NEUTRAL, linestyle=":", linewidth=1.0)
    ax.set_xlabel("dwell-time coefficient of variation")
    ax.set_ylabel("accuracy cost of sketching (percentage points)")
    ax.set_ylim(0, max(25.0, top + 3.0))
    ax.legend(frameon=False, loc="upper left")
    atitle(ax, "Figure J.3  The sketching penalty, paired within seed, three seeds", fontsize=9)
    save(fig, "figj-3-minimizer-penalty-3seeds", outdir, formats)
    record(
        "figj-3-minimizer-penalty-3seeds",
        "results/rawhash-grid-seeds.csv",
        "Accuracy cost of minimizer sketching, paired within seed; mean and range over three seeds.",
    )


# ---------------------------------------------------------------------------------------
# Chapter 7 -- the ceiling
# ---------------------------------------------------------------------------------------


def _agreement_rows() -> list[dict]:
    return read_csv_rows(RESULTS / "agreement-summary.csv")


def fig_7_1(outdir: Path, formats: list[str]) -> None:
    """Key match probability over (a, E), with the measured operating points marked."""
    a = np.linspace(0.40, 0.98, 320)
    e = np.arange(4, 21)
    A, E = np.meshgrid(a, e)
    P = A**E

    fig, ax = plt.subplots(figsize=(6.8, 4.2))
    levels = [1e-5, 1e-4, 1e-3, 1e-2, 1e-1, 0.3, 0.6]
    cs = ax.contourf(A, E, P, levels=levels, norm=matplotlib.colors.LogNorm(),
                     cmap="BuPu", extend="both", alpha=0.65)
    cl = ax.contour(A, E, P, levels=levels, colors=NEUTRAL, linewidths=0.7)
    ax.clabel(cl, fmt=lambda v: f"{v:g}", fontsize=7)

    # Only the two configurations each system actually ships, at the simulator's default
    # dispersion. The remaining cells are in Figures 7.2 and 7.3, where they are legible;
    # crowding six annotated points onto a contour plot makes none of them readable.
    want = {("ours", 0.44), ("rawhash2", 0.44)}
    for r in _agreement_rows():
        if r["norm"] == "chunk" or r["segmentation"] == "fixed":
            continue
        if (r["system"], float(r["dwell_cv"])) not in want:
            continue
        is_ours = r["system"] == "ours"
        a_m, e_m = float(r["a_dp"]), int(r["events_per_key"])
        p_m = float(r["p_key12" if is_ours else "p_key8"])
        ax.plot(
            a_m, e_m, marker="o" if is_ours else "D",
            color=OURS if is_ours else THEIRS,
            markersize=9, markeredgecolor="white", markeredgewidth=1.0, zorder=5,
        )
        ax.annotate(
            f"{'this pipeline' if is_ours else 'comparison mapper'}\n"
            f"$a$={a_m:.3f}, $E$={e_m}\n"
            f"$a^E$={a_m**e_m:.3f}, measured $P$={p_m:.3f}",
            xy=(a_m, e_m), xytext=(-12, 26) if is_ours else (-12, -52),
            textcoords="offset points", fontsize=7.5,
            ha="right", color=OURS if is_ours else THEIRS,
            arrowprops=dict(arrowstyle="-", color=OURS if is_ours else THEIRS, lw=0.7),
        )

    cbar = fig.colorbar(cs, ax=ax, pad=0.02)
    cbar.set_label("$a^E$, modelled key match probability")
    ax.set_xlabel("per-event bucket agreement $a$")
    ax.set_ylabel("events per key $E$")
    atitle(ax,
        "Figure 7.1  Modelled key match probability over the $(a, E)$ plane, at CV 0.44\n"
        "measured $P$ sits below $a^E$ in both cases, because $a^E$ omits the\n"
        "correspondence-failure factor $(1-g)^E$ (§7.2.3)",
        fontsize=9,
    )
    ax.grid(False)
    save(fig, "fig7-1-ae-surface", outdir, formats)
    record(
        "fig7-1-ae-surface",
        "arithmetic, with operating points from results/agreement-summary.csv",
        "The a^E surface with each system's measured operating point marked.",
    )


def fig_7_2(outdir: Path, formats: list[str]) -> None:
    """The comparison that closes Chapter 7: agreement, correspondence, yield."""
    rows = [r for r in _agreement_rows() if r["norm"] != "chunk"]
    labels, a_vals, g_vals, p_vals, cols = [], [], [], [], []
    for r in rows:
        is_ours = r["system"] == "ours"
        labels.append(
            f"{'ours' if is_ours else 'comp.'}\n{r['segmentation'][:5]} E={r['events_per_key']}\n"
            f"CV {float(r['dwell_cv']):.2f}"
        )
        a_vals.append(float(r["a_dp"]))
        g_vals.append(float(r["indel_frac"]))
        p_vals.append(float(r["p_key8"] if not is_ours else r["p_key12"]))
        cols.append(OURS if is_ours else THEIRS)

    xs = np.arange(len(labels))
    fig, axes = plt.subplots(1, 3, figsize=(10.5, 3.9))
    for ax, vals, title, ylab, logy in (
        (axes[0], a_vals, "per-event agreement $a$", "$a$", False),
        (axes[1], g_vals, "correspondence failure $g$", "indel fraction", False),
        (axes[2], p_vals, "measured key match probability", "$P$(key) at own $E$", True),
    ):
        ax.bar(xs, vals, color=cols, edgecolor="white", linewidth=0.6)
        ax.set_xticks(xs)
        ax.set_xticklabels(labels, fontsize=6.5, rotation=45, ha="right")
        ax.set_title(title, fontsize=9)
        ax.set_ylabel(ylab)
        if logy:
            ax.set_yscale("log")
        for x, v in zip(xs, vals):
            ax.text(x, v, f"{v:.3f}" if not logy else f"{v:.4f}",
                    ha="center", va="bottom", fontsize=7)
        if not logy:
            ax.set_ylim(0, max(vals) * 1.28)
    axes[2].set_ylim(min(p_vals) * 0.4, max(p_vals) * 3.0)
    ftitle(fig,
        "Figure 7.2  Both systems under one estimator, identical input (§7.4.5)\n"
        "a 31-fold difference in anchor supply accounts for 18.8% against 92.4% accuracy",
        fontsize=9,
    )
    fig.tight_layout(rect=(0, 0, 1, 0.86))
    save(fig, "fig7-2-agreement-comparison", outdir, formats)
    record(
        "fig7-2-agreement-comparison",
        "results/agreement-summary.csv",
        "Agreement, correspondence-failure rate and measured key match probability, both systems.",
    )


def fig_7_3(outdir: Path, formats: list[str]) -> None:
    """Measured key match probability against what a^E predicts, which is the correction."""
    rows = _agreement_rows()
    fig, ax = plt.subplots(figsize=(6.0, 4.6))
    for r in rows:
        is_ours = r["system"] == "ours"
        E = int(r["events_per_key"])
        a = float(r["a_dp"])
        modelled = a**E
        measured = float(r["p_key12"] if is_ours else r["p_key8"])
        g = float(r["indel_frac"])
        ax.plot(
            modelled, measured, marker="o" if is_ours else "D",
            color=OURS if is_ours else THEIRS, markersize=6 + 18 * g,
            alpha=0.85, markeredgecolor="white", markeredgewidth=0.7,
        )
        tag = f"{'ours' if is_ours else 'comp.'} {r['segmentation'][:5]} CV{float(r['dwell_cv']):.2f}"
        if r["norm"] == "chunk":
            tag += " chunk-norm"
        # Points near the right edge get their labels on the left, or they run off.
        right = modelled > 0.08
        ax.annotate(tag, xy=(modelled, measured),
                    xytext=(-7 if right else 7, 7 if right else -3),
                    textcoords="offset points", fontsize=6.5,
                    ha="right" if right else "left",
                    color=OURS if is_ours else THEIRS)

    lims = [2e-4, 1.0]
    ax.plot(lims, lims, color=NEUTRAL, linestyle="--", linewidth=1.0)
    ax.text(1.2e-3, 2.4e-3, "measured = modelled", fontsize=7, color=NEUTRAL, rotation=38)
    ax.fill_between(lims, lims, [1.0, 1.0], color=OURS, alpha=0.05)
    ax.fill_between(lims, [2e-4, 2e-4], lims, color=THEIRS, alpha=0.05)
    ax.text(3e-4, 0.35, "$a^E$ is a lower bound\n(correspondence exact)",
            fontsize=7.5, color=OURS)
    ax.text(0.10, 6e-4, "$a^E$ over-predicts\n(indels break key windows)",
            fontsize=7.5, color=THEIRS, ha="center")
    ax.set_xscale("log")
    ax.set_yscale("log")
    ax.set_xlim(*lims)
    ax.set_ylim(*lims)
    ax.set_xlabel("$a^E$, modelled from measured $a$")
    ax.set_ylabel("$P$(key), measured directly from the alignment")
    atitle(ax,
        "Figure 7.3  Why $a^E$ is not a one-sided bound in general (§7.2.3)\n"
        "marker area grows with the correspondence-failure rate $g$",
        fontsize=9,
    )
    save(fig, "fig7-3-model-vs-measured", outdir, formats)
    record(
        "fig7-3-model-vs-measured",
        "results/agreement-summary.csv",
        "Measured key match probability against the a^E prediction, sized by indel rate.",
    )


def fig_7_4(outdir: Path, formats: list[str]) -> None:
    """Per-read agreement distributions, so the pooled figures are not taken on trust."""
    cells = [
        ("agreement-our_detect_std4.4.csv", "ours, detected, CV 0.44", OURS, "-"),
        ("agreement-our_fixed_std4.4.csv", "ours, fixed-width, CV 0.44", OURS, "--"),
        ("agreement-rh_std4.4.csv", "comparison, CV 0.44", THEIRS, "-"),
        ("agreement-rh_std0.csv", "comparison, CV 0.00", THEIRS, "--"),
    ]
    fig, (ax, ax2) = plt.subplots(1, 2, figsize=(9.4, 3.8))
    for fname, label, colour, ls in cells:
        path = RESULTS / fname
        if not path.exists():
            continue
        rows = read_csv_rows(path)
        a = np.sort(np.array([float(r["a_dp"]) for r in rows]))
        g = np.sort(np.array([float(r["indel_frac"]) for r in rows]))
        y = np.arange(1, a.size + 1) / a.size
        ax.plot(a, y, color=colour, linestyle=ls, label=label)
        ax2.plot(g, y, color=colour, linestyle=ls, label=label)
    ax.set_xlabel("per-read agreement $a$")
    ax.set_ylabel("cumulative fraction of reads")
    ax.set_title("agreement", fontsize=9)
    ax2.set_xlabel("per-read correspondence failure $g$")
    ax2.set_title("indel fraction", fontsize=9)
    ax.legend(frameon=False, loc="upper left", fontsize=7)
    ftitle(fig,
        "Figure 7.4  Per-read distributions behind the pooled figures of Table 7.5\n"
        "200 reads per cell; the separation is in the distribution, not only in the mean",
        fontsize=9,
    )
    fig.tight_layout(rect=(0, 0, 1, 0.86))
    save(fig, "fig7-4-per-read-distributions", outdir, formats)
    record(
        "fig7-4-per-read-distributions",
        "results/agreement-{our_detect,our_fixed,rh}_std*.csv",
        "Empirical CDFs of per-read agreement and indel fraction for four cells.",
    )


def fig_7_6(outdir: Path, formats: list[str]) -> None:
    """Realised entropy against nominal key width, and cap survival, at both reference sizes.

    Two things the table states but a reader has to do arithmetic to see: that the shortfall
    grows faster than the nominal width, and that the occurrence cap collapses for short keys
    far more sharply than the reference grows.
    """
    rows = read_csv_rows(RESULTS / "key-entropy.csv")
    fig, (ax, axc) = plt.subplots(1, 2, figsize=(9.6, 3.9))

    for ref, colour, marker, ls in ((20, OURS, "o", "-"), (64, THEIRS, "D", "--")):
        rs = sorted((r for r in rows if int(r["ref_mb"]) == ref),
                    key=lambda r: int(r["nominal_bits"]))
        nom = [int(r["nominal_bits"]) for r in rs]
        real = [float(r["realised_bits"]) for r in rs]
        ax.plot(nom, real, color=colour, marker=marker, linestyle=ls,
                label=f"{ref} Mb reference")
        axc.plot(nom, [float(r["cap_kept_pct"]) for r in rs],
                 color=colour, marker=marker, linestyle=ls, label=f"{ref} Mb reference")
        for r, x, y in zip(rs, nom, real):
            if ref == 20:
                ax.annotate(r["geometry"], xy=(x, y), xytext=(0, -13),
                            textcoords="offset points", fontsize=7, ha="center", color=colour)

    lim = [20, 52]
    ax.plot(lim, lim, color=NEUTRAL, linestyle=":", linewidth=1.0)
    ax.text(44, 45.5, "realised = nominal", fontsize=7, color=NEUTRAL, rotation=31, ha="center")
    ax.set_xlim(*lim)
    ax.set_ylim(14, 52)
    ax.set_xlabel("nominal key width (bits)")
    ax.set_ylabel("realised entropy (bits)")
    ax.set_title("the shortfall grows with the key", fontsize=9)
    ax.legend(frameon=False, loc="upper left", fontsize=7.5)

    axc.axhline(50, color=NEUTRAL, linestyle=":", linewidth=1.0)
    axc.set_xlabel("nominal key width (bits)")
    axc.set_ylabel("minimizer occurrences surviving the cap (%)")
    axc.set_ylim(0, 100)
    axc.set_title("short keys saturate the occurrence cap", fontsize=9)
    axc.legend(frameon=False, loc="lower right", fontsize=7.5)

    ftitle(fig,
        "Figure 7.6  Realised key entropy and cap survival by geometry (§7.3)\n"
        "both depend on reference size, which is why Table 7.1 states it",
        fontsize=9,
    )
    fig.tight_layout(rect=(0, 0, 1, 0.86))
    save(fig, "fig7-6-key-entropy", outdir, formats)
    record(
        "fig7-6-key-entropy",
        "results/key-entropy.csv",
        "Realised entropy and occurrence-cap survival by geometry, at both reference sizes.",
    )


def fig_7_5(outdir: Path, formats: list[str]) -> None:
    """Six interventions against the anchor supply they could not change."""
    rows = read_csv_rows(RESULTS / "geometry-x-chaining.csv")
    geos = ["3x8", "3x10", "4x8", "4x10", "4x12"]
    present = [g for g in geos if any(r["geometry"] == g for r in rows)]
    xs = np.arange(len(present))
    width = 0.38
    fig, ax = plt.subplots(figsize=(6.4, 3.8))
    for i, (dec, colour, label) in enumerate(
        (("votes", NEUTRAL, "exact diagonal voting"), ("chain", OURS, "gap-tolerant chaining"))
    ):
        vals = []
        for g in present:
            m = [r for r in rows if r["geometry"] == g and r["decision"] == dec]
            vals.append(float(m[0]["zeroFP_TPR"]) if m else 0.0)
        ax.bar(xs + (i - 0.5) * width, vals, width, color=colour, label=label,
               edgecolor="white", linewidth=0.5)
        for x, v in zip(xs + (i - 0.5) * width, vals):
            ax.text(x, v + 0.25, f"{v:.1f}", ha="center", fontsize=7)
    ax.set_xticks(xs)
    ax.set_xticklabels(present)
    ax.set_xlabel("key geometry (bits × events)")
    ax.set_ylabel("TPR at zero false positives (%)")
    ax.legend(frameon=False, loc="upper left")
    atitle(ax,
        "Figure 7.5  Changing the decision rule reorganises a fixed anchor supply\n"
        "the best improvement across all six interventions is +32% relative (§7.6)",
        fontsize=9,
    )
    save(fig, "fig7-5-geometry-x-chaining", outdir, formats)
    record(
        "fig7-5-geometry-x-chaining",
        "results/geometry-x-chaining.csv",
        "Voting against chaining across key geometries; the decision rule moves little.",
    )


FIGURES = [
    ("4.1", fig_4_1),
    ("5.1", fig_5_1),
    ("5.2", fig_5_2),
    ("5.3", fig_5_3),
    ("5.4", fig_5_4),
    ("6.1", fig_6_1),
    ("6.2", fig_6_2),
    ("6.3", fig_6_3),
    ("J6.2", fig_j_2),
    ("J6.3", fig_j_3),
    ("7.1", fig_7_1),
    ("7.2", fig_7_2),
    ("7.3", fig_7_3),
    ("7.4", fig_7_4),
    ("7.5", fig_7_5),
    ("7.6", fig_7_6),
]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--outdir", default=str(REPO / "figures"))
    ap.add_argument("--format", default="pdf,png",
                    help="comma-separated: pdf, png, svg")
    ap.add_argument("--no-titles", action="store_true",
                    help="omit figure-level titles; for a paper that captions them instead")
    ap.add_argument("--only", default="", help="comma-separated figure numbers, e.g. 7.2,7.3")
    args = ap.parse_args()

    global TITLES
    TITLES = not args.no_titles

    outdir = Path(args.outdir)
    outdir.mkdir(parents=True, exist_ok=True)
    formats = [f.strip() for f in args.format.split(",") if f.strip()]
    only = {s.strip() for s in args.only.split(",") if s.strip()}

    failures = []
    for num, fn in FIGURES:
        if only and num not in only:
            continue
        # Journal-only figures (J...) are drawn only when named, so a full regeneration of the
        # thesis figures/ directory never acquires figures the thesis does not reference.
        if not only and num.startswith("J"):
            continue
        print(f"figure {num}")
        try:
            fn(outdir, formats)
        except Exception as exc:  # a missing input should not stop the rest
            print(f"  FAILED: {type(exc).__name__}: {exc}", file=sys.stderr)
            failures.append((num, f"{type(exc).__name__}: {exc}"))

    with open(outdir / "MANIFEST.md", "w", encoding="utf-8") as f:
        f.write("# Generated figures\n\n")
        f.write("Produced by `scripts/generate_thesis_figures.py` from the files named\n")
        f.write("below and from nothing else. Regenerate after any correction to a\n")
        f.write("results file; do not edit a figure by hand.\n\n")
        f.write("| figure | source data | content |\n|---|---|---|\n")
        for name, src, cap in MANIFEST:
            f.write(f"| `{name}` | `{src}` | {cap} |\n")
        if failures:
            f.write("\n## Not produced\n\n")
            for num, why in failures:
                f.write(f"- Figure {num}: {why}\n")
    print(f"\nmanifest: {outdir / 'MANIFEST.md'}")
    if failures:
        print(f"{len(failures)} figure(s) failed", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
