# Standalone paper

A self-contained research paper drawn from the thesis, built on the same data and checked by
the same discipline.

| file | what it is |
|---|---|
| `manuscript.md` | the paper in Markdown, readable without a TeX installation |
| `manuscript.tex` | the same paper for `acmart` (sigconf, `nonacm`) |
| `manuscript.pdf` | built output, 10 pages |
| `figures/` | the paper's figures, generated without figure-level titles |

## Building

```bash
cd paper && latexmk -pdf manuscript.tex
```

Two `pdflatex` passes suffice if `latexmk` is unavailable; the bibliography is a
`thebibliography` environment, so no BibTeX run is needed.

## Figures

The paper's figures come from the same generator and the same data as the thesis figures, with
one difference:

```bash
python scripts/generate_thesis_figures.py --format pdf --outdir paper/figures --no-titles --only 4.1,5.4,7.1,7.2,7.3,7.6
```

`--only` restricts the build to the six figures this paper uses, so the directory matches the
command exactly; the thesis uses fourteen.

`--no-titles` suppresses the figure-level titles, because a two-column paper puts them in the
LaTeX caption and a matplotlib title would both duplicate the caption and carry *thesis*
figure numbers beside the paper's own. Panel titles on individual axes are kept, since they
label sub-axes and no caption replaces them.

Wide figures span both columns (`figure*`). They were sized for a thesis page, and scaling them
into one column makes their axis labels unreadable; regenerating them at column width was
rejected because the same script builds the thesis figures and two differently sized sets would
drift apart.

## Verification

```bash
python scripts/verify_paper_numbers.py
```

This checks the paper rather than the thesis, and does it in the opposite direction from
`verify_thesis_numbers.py`: instead of transcribing claims by hand, it builds the expected
string from the data file under `results/` and asserts that string appears in the manuscript.
A corrected measurement therefore fails the check until the paper is updated, which is what
stops a standalone paper drifting away from the repository it cites. Both the Markdown and the
LaTeX are checked, so the two cannot disagree with each other either.

At the committed state it reports 96 assertions matched across the two formats.

## Relationship to the thesis

The paper is not an abridgement. It presents the two contributions the measurements support —
the data plane and the signal-to-seed bottleneck — and omits the thesis material that serves a
different purpose: the measurement-methodology chapter's full retraction log, the dwell-axis
evaluation chapter, and the appendices. Where the thesis separates regimes R1 and R3 across
several sections, the paper reports both in one table.

Numbers are identical in both. Five claims are not settled by any committed file and are
documented as scope boundaries in the paper's reproducibility statement and in Appendix F.3 of
the thesis.
