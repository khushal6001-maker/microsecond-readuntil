#!/usr/bin/env python3
"""Draw Figure 1 of paper_dwell_journal/: how dwell dispersion reaches the keys.

This is a diagram, not a data figure, so it is not in generate_thesis_figures.py, whose rule is
that every number comes from results/. The current levels, durations, noise and key ranks below
are illustrative, and the figure caption says so. Panel B's durations are drawn lognormal with
CV 0.44, Squigulator's default dispersion, about a mean of 10 samples, from fixed seeds so that
the drawing is reproducible. Style and the byte-stable save come from the data-figure generator.

    python scripts/draw_dwell_schematic.py [--outdir paper_dwell_journal/figures]
"""

from __future__ import annotations

import argparse
import math
from pathlib import Path

import numpy as np
from matplotlib.patches import Rectangle

import generate_thesis_figures as gtf  # sets the shared rcParams on import

plt = gtf.plt
OURS, THEIRS, NEUTRAL = gtf.OURS, gtf.THEIRS, gtf.NEUTRAL

LEVELS = np.array([3.1, 1.2, 2.6, 0.8, 2.9, 1.7, 3.4, 1.0, 2.2, 0.6, 2.7, 1.4])
WINDOW = 10          # samples per fixed-width window, equal to the mean dwell
NOISE_SD = 0.10


def trace(ax, durations: np.ndarray, letter: str, caption: str, rng: np.random.Generator,
          xlabel: bool) -> None:
    signal = np.repeat(LEVELS, durations) + rng.normal(0.0, NOISE_SD, int(durations.sum()))
    n = len(signal)
    edges = np.concatenate([[0], np.cumsum(durations)])

    # Fixed-width windows that contain a k-mer boundary strictly inside them mix two levels.
    for k in range(0, n, WINDOW):
        if any(k < e < k + WINDOW for e in edges[1:-1]):
            ax.axvspan(k, min(k + WINDOW, n), color=THEIRS, alpha=0.09, linewidth=0)
        ax.axvline(k, color=THEIRS, linewidth=0.6, linestyle=(0, (2, 2)), alpha=0.55)
    ax.plot(np.arange(n), signal, color="#9a9a9a", linewidth=0.7, label="raw signal")
    for i, (a, b) in enumerate(zip(edges[:-1], edges[1:])):
        ax.hlines(signal[a:b].mean(), a, b, color=OURS, linewidth=2.2,
                  label="detected events" if i == 0 else None)
    centres = [k + min(WINDOW, n - k) / 2 for k in range(0, n, WINDOW)]
    means = [signal[k:k + WINDOW].mean() for k in range(0, n, WINDOW)]
    ax.plot(centres, means, linestyle="none", marker="s", markersize=4, color=THEIRS,
            label="fixed-width window means")
    ax.set_xlim(0, n)
    ax.set_ylim(0.0, 4.2)
    ax.set_yticks([])
    ax.set_ylabel("current", fontsize=8)
    if xlabel:
        ax.set_xlabel("samples")
    ax.grid(False)
    ax.text(-0.06, 1.04, letter, transform=ax.transAxes, fontsize=11, fontweight="bold")
    ax.text(0.0, 1.04, caption, transform=ax.transAxes, fontsize=8.5, color="#333333")


def keys(ax) -> None:
    """One window of w = 4 keys: a displaced boundary changes K2, so the minimum moves."""
    ref = [6, 2, 7, 4, 8, 5]
    qry = [6, 9, 7, 4, 8, 5]
    w = 4
    rows = (("reference", 1.15, ref, OURS, 1), ("query", 0.0, qry, THEIRS, 3))
    ax.add_patch(Rectangle((-0.12, -0.18), w - 0.04 + 0.24, 2.21, facecolor="#e9ecf2",
                           edgecolor="none", zorder=0))
    ax.text((w - 1) / 2 + 0.4, 2.12, f"one minimizer window, w = {w} keys", ha="center",
            fontsize=8, color="#333333")
    for name, y, ranks, colour, chosen in rows:
        ax.text(-0.35, y + 0.4, name, ha="right", va="center", fontsize=8.5)
        for i, r in enumerate(ranks):
            changed = name == "query" and r != ref[i]
            ax.add_patch(Rectangle((i, y), 0.86, 0.8,
                                   facecolor="#f3d9cc" if changed else "white",
                                   hatch="///" if changed else None,
                                   edgecolor=colour if i == chosen else "#777777",
                                   linewidth=2.4 if i == chosen else 0.8, zorder=2))
            # Key name and rank inside the box, so nothing sits under the row for the arrow to cross.
            ax.text(i + 0.43, y + 0.4, f"K{i + 1}\nrank {r}", ha="center", va="center", fontsize=7.5,
                    linespacing=1.25, zorder=3)
    ax.annotate("changed by a displaced boundary", xy=(1.43, 0.0), xytext=(1.43, -0.75),
                ha="center", fontsize=7.5, color=THEIRS,
                arrowprops=dict(arrowstyle="->", color=THEIRS, lw=0.8))
    ax.text(6.25, 1.55, "reference keeps K2", fontsize=8, color=OURS, va="center")
    ax.text(6.25, 0.40, "query keeps K4", fontsize=8, color=THEIRS, va="center")
    ax.text(6.25, -0.55, "K4 is identical on both sides,\nbut not the reference's choice:\n"
            "no seed in common", fontsize=7.5, color="#333333", va="center")
    ax.set_xlim(-1.6, 9.2)
    # Headroom above the window label, so the panel caption at the top does not overprint it.
    ax.set_ylim(-1.15, 2.85)
    ax.axis("off")
    ax.text(-0.06, 1.0, "C", transform=ax.transAxes, fontsize=11, fontweight="bold")
    ax.text(0.0, 1.0, "Candidate mechanism of the dwell-dependent sketching cost", transform=ax.transAxes,
            fontsize=8.5, color="#333333")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--outdir", default=str(gtf.REPO / "paper_dwell_journal" / "figures"))
    ap.add_argument("--format", default="pdf,png")
    args = ap.parse_args()
    outdir = Path(args.outdir)
    outdir.mkdir(parents=True, exist_ok=True)

    fig = plt.figure(figsize=(6.4, 6.6))
    grid = fig.add_gridspec(3, 1, height_ratios=[1.0, 1.0, 1.05], hspace=0.62)
    a, b, c = (fig.add_subplot(grid[i]) for i in range(3))

    trace(a, np.full(len(LEVELS), WINDOW), "A", "Constant dwell (CV = 0): every window covers one k-mer",
          np.random.default_rng(11), xlabel=False)
    cv = 0.44
    sigma2 = math.log(1 + cv * cv)
    mu = math.log(WINDOW) - sigma2 / 2
    dur = np.clip(np.round(np.random.default_rng(5).lognormal(mu, math.sqrt(sigma2), len(LEVELS))), 2, 80)
    trace(b, dur.astype(int), "B", "Variable dwell (CV = 0.44): windows straddle level changes",
          np.random.default_rng(11), xlabel=True)
    handles, labels = a.get_legend_handles_labels()
    handles.append(Rectangle((0, 0), 1, 1, color=THEIRS, alpha=0.15))
    labels.append("window straddles a boundary")
    fig.legend(handles, labels, loc="upper center", ncol=4, frameon=False, fontsize=7.5,
               bbox_to_anchor=(0.5, 1.0))
    keys(c)
    gtf.save(fig, "fig1-dwell-schematic", outdir, [f.strip() for f in args.format.split(",")])
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
