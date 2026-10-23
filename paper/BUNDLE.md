# A Lock-Free Data Plane and the Signal-to-Seed Bottleneck — LaTeX source bundle

Complete, self-contained LaTeX source for the paper. Everything needed to build is in this
archive; no external style files are required beyond what TeX Live and Overleaf already ship.

| file | what it is |
|---|---|
| `manuscript-ieee.tex` | **main document** — IEEE conference format (`IEEEtran`, `[conference]`), 8 pages |
| `manuscript-ieee.pdf` | built output |
| `figures/` | the six data figures as PDFs; the IEEE version uses two of them, and Figure 1 is drawn in TikZ inside the `.tex` |
| `manuscript.md` | the full-length paper in Markdown, readable without a TeX installation |
| `acm-10-page/` | the full-length paper in ACM format (`acmart`, `sigconf`, `nonacm`), with its own copy of the figures so it compiles in place |

There is deliberately **one `.tex` at the archive root**, so that a submission system picking
a main document for itself cannot pick wrong.

## Building

```bash
latexmk -pdf manuscript-ieee.tex
```

Two `pdflatex` passes suffice if `latexmk` is unavailable — the second resolves the
cross-references. The bibliography is a `thebibliography` environment, so there is no BibTeX
or Biber run and no `.bib` file to carry.

Both `.tex` files are **pure ASCII** and declare `inputenc`, so neither depends on the
compiler's default input encoding. Built and checked with TeX Live 2026; `IEEEtran` and
`acmart` are both in the standard distribution, and nothing here needs a package a submission
system would not already have.

## On Overleaf

Upload the archive itself — do not unzip it first — with **New Project → Upload Project**.
`manuscript-ieee.tex` is the only `.tex` at the root, so it is selected automatically; if
Overleaf ever disagrees, set it under **Menu → Main document**. Nothing else needs
configuring, and if Overleaf keeps the enclosing folder rather than flattening it, that is
fine: every path in the `.tex` is relative to the `.tex`.

To build the 10-page version instead, set the main document to
`acm-10-page/manuscript-acmart.tex`.

## Which format to submit

**`manuscript-ieee.tex` is the 8-page submission version, and it is hand-maintained.** Edit it
directly. It began as output from `scripts/make_ieee_manuscript.py`, derived from the ACM
file, but its prose has since been rewritten by the author; that script now refuses to
overwrite it.

The two `.tex` files therefore differ in wording as well as in format. They do not differ in
any measurement — `verify_paper_numbers.py` holds both to the same assertions.

Formatting differences: document class, title block, reference style, table sizing, and two
geometric details — Figure 1's node spacing drops by 2 mm, because the ACM geometry measures
252.4 pt against an IEEE column of 252.0 pt and overflows it by a hair, and the section
cross-references read "Section IV" rather than "§4".

Content differences, all in the IEEE version, all listed as data in the generator:

| what | why |
|---|---|
| four data figures dropped | three plotted a table that is still present; one drew a surface that is given as a displayed equation |
| Figure 3 drawn at `0.70\textwidth` | at `0.86` an IEEE text block scales it *up* 11% past its natural width |
| §VIII and §IX rewritten shorter | the two discursive sections; every number in them is carried over |
| abstract cut, 389 → 250 words | IEEE's guidance, and one paragraph rather than two |

The abstract no longer quotes the peak resident set, the index size, the offered chunk rate,
the run count or the 46-fold key-match decomposition; §IV and §VI still do. The one phrasing
that leaves the IEEE paper entirely is the rounded "63.9 M minimizers", which the body gives
exactly as 63,981,218.

**No measurement was cut.** `scripts/verify_paper_numbers.py` holds all three manuscripts to
the same 144 assertions, so a cut that lost one would fail the gate rather than ship. The
generator additionally refuses to write a short abstract containing any numeral the
full-length abstract did not have, since a retyped abstract is where a digit gets quietly
changed.

## Before submitting

- **Authorship.** The author block names one author. A commented second `\IEEEauthorblockN`
  block sits directly below it in `manuscript-ieee.tex` if the submission is joint.
- **Title length.** The title runs to four lines in IEEE format. It is accurate, and no
  shorter form was substituted without asking.
- **Abstract length.** 250 words, one paragraph, which is IEEE's guidance. Cutting below that
  means dropping a further measurement, not scaffolding.
- **Page count.** 8 pages including references, which is the common conference cap. If a
  venue allows 10, use `manuscript-acmart.tex` as the content source — it has the four extra
  figures and the full-length §VIII and §IX.
- **Anonymisation.** Neither file is anonymised. For a double-blind venue, replace the author
  block, and note that §X and the reproducibility paragraph name the repository commit.

## Verification

Every load-bearing number in all three manuscripts is checked against the measurement data by
`scripts/verify_paper_numbers.py` in the repository, which builds the expected string from the
CSV files under `results/` and asserts it appears in each manuscript. At the committed state
it reports 144 assertions matched across the three formats, so no two of them can disagree
with each other or with the data.

Five claims are not settled by any committed measurement file and are named as scope
boundaries in the paper's own reproducibility paragraph.
