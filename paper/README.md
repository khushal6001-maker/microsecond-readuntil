# Standalone paper

A self-contained research paper drawn from the thesis, built on the same data and checked by
the same discipline.

| file | what it is |
|---|---|
| `manuscript.md` | the paper in Markdown, readable without a TeX installation |
| `manuscript.tex` | the same paper for `acmart` (sigconf, `nonacm`) |
| `manuscript-ieee.tex` | the submission manuscript: 8 pages, `IEEEtran` (`conference`), hand-maintained |
| `manuscript.pdf` (10 pages), `manuscript-ieee.pdf` (8 pages) | built output |
| `figures/` | the paper's figures, generated without figure-level titles |
| `BUNDLE.md` | the README that ships inside the distributable source archive |

## Building

```bash
cd paper && latexmk -pdf manuscript.tex
```

```bash
cd paper && latexmk -pdf manuscript-ieee.tex
```

Two `pdflatex` passes suffice if `latexmk` is unavailable; the bibliography is a
`thebibliography` environment, so no BibTeX run is needed. Built with TeX Live 2026; both
`acmart` and `IEEEtran` are in the standard distribution.

## The IEEE version is now hand-maintained

**`manuscript-ieee.tex` is the author's manuscript and is edited directly.** It began as the
generator's output, but the author has since rewritten its prose, and regenerating would
replace that work with a derivation of the acmart source.

`scripts/make_ieee_manuscript.py` therefore refuses to run while the file on disk carries the
`HAND-MAINTAINED` marker in its header, and exits non-zero saying so. Delete the file first if
you genuinely want it regenerated.

The practical consequence is that **the two manuscripts have diverged in prose** and will stay
diverged. They have not diverged in numbers, and cannot: `verify_paper_numbers.py` holds both
to the same assertions, which is now the only thing keeping them in step.

### What the generator did, while it was in use

The historical behaviour is recorded here because the current file still bears its marks. It
rewrote the preamble, the title block and the bibliography wholesale, and made four kinds of
formatting change to the body:
`\S\ref` becomes `Section~\ref`, two `\paragraph` run-in headings become the `\textbf` form
used elsewhere in the paper, table bodies drop from `\small` to `\footnotesize` or
`\scriptsize` for the narrower column, and Figure 1's node spacing tightens by 2 mm. That
last one is measured rather than guessed: the acmart picture is 252.42 pt wide against an
IEEE column of 252.0 pt, so it overflows by 0.42 pt.

It also **cuts the paper from 10 pages to 8**, which is the cap at most conferences. The cuts
are listed as data in `DROP_FIGURES`, `FIGURE_WIDTHS`, `PROSE_CUTS` and `SECTION_REWRITES`
rather than applied by hand, so what was removed is auditable; the script exits non-zero if
any target string has moved. Measured at `0.86\textwidth` in a 516 pt text block, the six
data figures occupy 1328 pt of full-width band before captions — 2.2 of the 10 pages — which
is why four of them, the four that plot a table already present or draw a displayed equation,
are the first thing to go. The other cuts are the scaffolding around §VIII and §IX, and an
abstract rewritten from 389 words to IEEE's 250 in a single paragraph.

**No measurement was cut.** `verify_paper_numbers.py` holds the IEEE file to the same 144
assertions as the other two, so a cut that dropped one fails the gate.

## A distributable archive

```bash
python scripts/make_paper_zip.py
```

Writes `paper/dist/microsecond-readuntil-paper-latex.zip`: the IEEE manuscript and `figures/`
at the archive root, the Markdown beside them, and the 10-page ACM variant in
`acm-10-page/` with its own copy of the figures. Exactly one `.tex` sits at the root, so a
submission system choosing a main document by itself cannot choose wrong, and
`\graphicspath{{figures/}}` resolves with nothing edited.

Both `.tex` files are pure ASCII and declare `inputenc`, so neither depends on the compiler's
default input encoding — TeX Live has defaulted to UTF-8 only since 2018, and a bundle that
relies on that default builds locally and fails on an older submission system.

The script only copies files, so the archive cannot contain a number the gate below has not
checked. The archive is not committed; it is rebuilt from committed files on demand.

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
stops a standalone paper drifting away from the repository it cites. The Markdown and both
LaTeX manuscripts are checked, so no two of them can disagree with each other either.

At the committed state it reports 144 assertions matched across the three formats.

## Relationship to the thesis

The paper is not an abridgement. It presents the two contributions the measurements support —
the data plane and the signal-to-seed bottleneck — and omits the thesis material that serves a
different purpose: the measurement-methodology chapter's full retraction log, the dwell-axis
evaluation chapter, and the appendices. Where the thesis separates regimes R1 and R3 across
several sections, the paper reports both in one section.

Numbers are identical in both. Five claims are not settled by any committed file and are
documented as scope boundaries in the paper's reproducibility statement and in Appendix F.3 of
the thesis.
