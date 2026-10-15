# Figures

Two kinds, kept apart because they are maintained differently.

## Data figures — `fig*.pdf`, `fig*.png`

Generated. Do not edit them; edit the data or the script and regenerate:

```bash
python scripts/generate_thesis_figures.py --format pdf,png
```

Every one is produced from a file under `results/` and from nothing else, so a corrected
measurement propagates by regenerating rather than by redrawing. `MANIFEST.md` is written by
the same run and records which file each figure came from. `--only 7.2,7.3` rebuilds a subset.

The single exception is Figure 5.2, whose inputs are the four reported percentiles and the TSC
frequency quoted in §5.2.2. It illustrates a property of an instrument that no longer exists
in the code, so there is no data file to read; the constants are in the script and the
manifest says so.

## Diagrams — `diagrams/*.svg`

Hand-written SVG, because they encode structure rather than data. Edit them directly.

| file | figure | what it has to get right |
|---|---|---|
| `fig3-1-data-plane.svg` | 3.1 | where the copy boundary sits: one copy, by the protobuf runtime, into arena storage, and none after it |
| `fig3-2-arena-refcount.svg` | 3.2 | that the reader's own reference is what makes the reset-under-dispatch race unrepresentable, structurally rather than by a timing bound |
| `fig3-3-smt-map.svg` | 3.3 | that the sibling relationship is not derivable from the logical processor number, which is how the §5.4 defect arose |

Each carries its sources in an XML comment at the top. `fig3-3` encodes the measurement host's
actual topology — an i5-1135G7 with sibling sets `0-1 2-3 4-5 6-7` — so it must be redrawn, not
merely relabelled, if the host changes.

## Numbering

Figure numbers follow the chapter sections, not the order in `thesis-outline.md`, which
predates the work in Chapter 7. Figures 7.2 to 7.5 did not exist when the outline was written
and were added when §7.4.4 and §7.4.5 were measured.
