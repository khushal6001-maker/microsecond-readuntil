# Microseconds Are Not the Problem — LaTeX source bundle

Complete, self-contained LaTeX source for the paper. Everything needed to build is in this
archive; no external style files are required beyond what TeX Live and Overleaf already ship.

| file | what it is |
|---|---|
| `manuscript-ieee.tex` | **main document** — IEEE conference format (`IEEEtran`, `[conference]`), 8 pages |
| `manuscript-acmart.tex` | the full-length paper in ACM format (`acmart`, `sigconf`, `nonacm`), 10 pages |
| `manuscript.md` | the full-length paper in Markdown, readable without a TeX installation |
| `manuscript-ieee.pdf`, `manuscript-acmart.pdf` | built output |
| `figures/` | the six data figures as PDFs; the IEEE version uses two of them, the ACM version all six, and Figure 1 is drawn in TikZ inside each `.tex` |

## Building

```bash
latexmk -pdf manuscript-ieee.tex
```

Two `pdflatex` passes suffice if `latexmk` is unavailable — the second resolves the
cross-references. The bibliography is a `thebibliography` environment, so there is no BibTeX
or Biber run and no `.bib` file to carry.

Built with TeX Live 2026. `IEEEtran` and `acmart` are both in the standard distribution.

## On Overleaf

Upload the archive with **New Project → Upload Project**. Overleaf should select
`manuscript-ieee.tex` as the main document; if it picks the ACM file instead, set it with
**Menu → Main document**. Nothing else needs configuring — the paths are flat and relative.

## Which format to submit

**`manuscript-ieee.tex` is the 8-page submission version.** It is generated from
`manuscript-acmart.tex` by `scripts/make_ieee_manuscript.py` in the repository, which also
applies the cuts that take it from 10 pages to 8. Edit the prose in the ACM file and
regenerate, or edit the IEEE file directly and stop regenerating it — but not both.

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
| abstract trimmed, 389 → 369 words | scaffolding only |

**No number was cut.** `scripts/verify_paper_numbers.py` holds all three manuscripts to the
same 144 assertions, so a cut that lost a measurement would fail the gate rather than ship.

## Before submitting

- **Authorship.** The author block names one author. A commented second `\IEEEauthorblockN`
  block sits directly below it in `manuscript-ieee.tex` if the submission is joint.
- **Title length.** The title runs to four lines in IEEE format. It is accurate, and no
  shorter form was substituted without asking.
- **Abstract length.** 369 words against IEEE guidance of nearer 250. The remainder is
  numeric content rather than scaffolding: trimming it further means dropping a measurement
  from the abstract, which is an editorial decision rather than a formatting one.
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
