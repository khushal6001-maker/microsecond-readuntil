# Microseconds Are Not the Problem — LaTeX source bundle

Complete, self-contained LaTeX source for the paper. Everything needed to build is in this
archive; no external style files are required beyond what TeX Live and Overleaf already ship.

| file | what it is |
|---|---|
| `manuscript-ieee.tex` | **main document** — IEEE conference format (`IEEEtran`, `[conference]`) |
| `manuscript-acmart.tex` | the same paper in ACM format (`acmart`, `sigconf`, `nonacm`) |
| `manuscript.md` | the same paper in Markdown, readable without a TeX installation |
| `manuscript-ieee.pdf`, `manuscript-acmart.pdf` | built output, 10 pages each |
| `figures/` | the seven figures; the six data figures are PDFs, Figure 1 is drawn in TikZ inside the `.tex` |

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

The two `.tex` files contain the same prose and the same numbers. They differ in document
class, title block, reference style and table sizing, and in two small geometric details:
Figure 1's node spacing drops by 2 mm, because the ACM geometry measures 252.4 pt against an
IEEE column of 252.0 pt and overflows it by a hair; and the section cross-references read
"Section IV" rather than "§4".

`manuscript-ieee.tex` is generated from `manuscript-acmart.tex` by
`scripts/make_ieee_manuscript.py` in the repository. Edit the prose in the ACM file and
regenerate, or edit the IEEE file directly and stop regenerating it — but do not do both.

## Before submitting

- **Authorship.** The author block names one author. A commented second `\IEEEauthorblockN`
  block sits directly below it in `manuscript-ieee.tex` if the submission is joint.
- **Title length.** The title runs to four lines in IEEE format. It is accurate, and no
  shorter form was substituted without asking.
- **Abstract length.** 389 words. IEEE guidance for conference papers is nearer 250. It is
  left full-length because every figure in it is one the number gate checks; cutting it is a
  one-paragraph edit and breaks nothing.
- **Page count.** 10 pages. Many conferences cap at 8 including references. §VIII
  (Discussion) and §IX (Limitations) are the compressible sections.
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
