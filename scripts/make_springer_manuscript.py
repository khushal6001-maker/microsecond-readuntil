#!/usr/bin/env python3
"""Derive paper/manuscript-springer.tex from paper/manuscript-ieee.tex.

Springer LNCS (llncs.cls), on the same pattern as the authors' first paper, which was
converted from IEEEtran by 3_Code/27_ieee_to_springer.py. The body text is copied through
verbatim. Only these change, and each is a requirement of the target class rather than an
edit:

  * preamble, title block and author block are rewritten for llncs
  * IEEEkeywords -> \\keywords inside the abstract, as LNCS requires
  * figure* -> figure, because LNCS is single column and there is no second column to span
  * the arena picture's node spacing is restored to the acmart geometry, since the LNCS
    text block is 4.8 in against an IEEE column of 3.5 in and the tightening is not needed

Not changed, and worth saying because the IEEE conversion needed both: there are no
hardcoded Roman section numbers to rewrite -- every cross-reference in the source uses
\\ref, which renumbers itself -- and the source is already pure ASCII.

Every sentence, number, citation and caption is byte-identical to the IEEE version.

    python scripts/make_springer_manuscript.py
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
SRC = REPO / "paper" / "manuscript-ieee.tex"
DST = REPO / "paper" / "manuscript-springer.tex"

BS = chr(92)
NL = chr(10)

PREAMBLE = r"""% =====================================================================
% A Lock-Free Data Plane and the Signal-to-Seed Bottleneck in Nanopore
% Adaptive Sampling
%
% Springer LNCS proceedings format (llncs.cls), converted from the
% IEEEtran version by scripts/make_springer_manuscript.py. Body text is
% identical to that version; edit paper/manuscript-ieee.tex and
% regenerate rather than editing this file.
%
% TODO BEFORE SUBMISSION
%   1. ORCID iDs: uncomment the \orcidID calls in \author if you have
%      them. Springer encourages these.
%   2. Confirm the page limit with the organisers. LNCS proceedings are
%      commonly 12 to 15 pages including references.
%
% IF THE VENUE PUBLISHES IN A DIFFERENT SPRINGER SERIES
%   LNCS, CCIS and LNNS all use llncs.cls, so nothing below changes.
%   Only LNEE or a Springer Nature journal template would alter the
%   class line; the body is unaffected either way.
%
% FIGURES expected in ./figures/ :
%   fig5-4-r3-distribution.pdf
%   fig7-3-model-vs-measured.pdf
% Figure 1 is drawn in TikZ below and needs no file.
% =====================================================================
\documentclass[runningheads]{llncs}

\usepackage[utf8]{inputenc}
\usepackage[T1]{fontenc}
\usepackage{amsmath,amssymb}
\usepackage{graphicx}
\usepackage{booktabs}
\usepackage{textcomp}
\usepackage{url}
\usepackage{tikz}
\usetikzlibrary{arrows.meta, positioning, shapes.geometric}

\graphicspath{{figures/}}
\renewcommand{\arraystretch}{1.15}

% Long unbreakable technical tokens -- script paths, chains of \times in maths -- can leave
% a line with nowhere to break. Letting TeX stretch the interword space on such a line keeps
% them out of the margin without touching a word.
\emergencystretch=1.5em

\begin{document}

\title{A Lock-Free Data Plane and the Signal-to-Seed Bottleneck in
Nanopore Adaptive Sampling}

% Shortened form for the running head, as llncs requires for long titles.
\titlerunning{A Lock-Free Data Plane and the Signal-to-Seed Bottleneck}

% To add ORCIDs, append \orcidID{0000-0000-0000-0000} after each name.
\author{Khushal Singh\inst{1} \and Ritu Garg\inst{1}}
\authorrunning{K. Singh and R. Garg}

\institute{Department of Computer Engineering, National Institute of
Technology Kurukshetra, Kurukshetra, Haryana 136119, India\\
\email{khushal6002@gmail.com}, \email{ritu.59@nitkkr.ac.in}}

\maketitle

"""


# ---------------------------------------------------------------------------------------
# CUTS TO 12 PAGES. The venue caps submissions at 12 pages including references, and the
# straight conversion runs to 17. Single-column LNCS sets roughly twice the pages of
# two-column IEEE, so this is a third of the paper, and it cannot be met by tightening prose.
#
# The cuts are therefore WHOLE UNITS -- subsections, paragraphs, a table -- chosen in order of
# least value lost per page saved, and everything that survives is the author's text
# verbatim. Compressing every paragraph by a third would have rewritten the hand-edited
# manuscript; removing the least load-bearing units does not touch it.
#
# Each entry is (label, start marker, end marker): the span from start up to but not
# including end is removed. The script stops if either marker is missing, so a reworded
# source cannot be silently half-cut.
#
# Contribution 2 (Sections 5-7) is the stronger half of the paper and is cut last.
SPANS = [
    # An anecdote about one machine's SMT numbering. Real, and not load-bearing.
    ("3.6 thread topology",
     r"\subsection{Thread topology}",
     r"\section{Evaluation of the data plane}"),

    # Describes an EARLIER, abandoned instrument and why it was wrong. A good methodological
    # point, but about a tool this paper does not use.
    ("4.1 histogram correction",
     "We adopted this instrumentation to correct an earlier error.",
     "The harness records per run"),

    # The 37 kb secondary regime. R3, at genome scale, is the result the paper rests on.
    ("4.2 placement and thread count, with Table 1",
     r"\subsection{Placement and thread count}",
     r"\subsection{Genome scale: regime R3}"),

    # --- round two: 16 pages after round one ----------------------------------------

    # Duplicates Section 9 almost point for point, a page and a half earlier.
    ("1 what this paper does not claim",
     r"\textbf{What this paper does not claim.}",
     r"\section{Background and related work}"),

    # The struct layout and the integer narrowings. Engineering detail; the claim the
    # subsection exists to make -- one copy, then none -- is its first two sentences, kept.
    ("3.2 descriptor layout and narrowings",
     " Workers receive a 56-byte borrowed",
     "\n\n" + r"\subsection{The arena reference-counting protocol}"),

    # Memory footprint of the vote table and chainer. The O(channels) bound it supports is
    # already stated as design constraint 3.
    ("3.5 bounded per-channel state",
     r"\subsection{Bounded per-channel state}",
     r"\section{Evaluation of the data plane}"),

    # Validation of the scorer against an earlier diagnostic, and the withdrawal of results
    # obtained with a superseded estimator. Both are housekeeping about earlier work.
    ("6.2 validation and estimator-choice note",
     r"\textbf{Validation.}",
     r"\subsection{Results}"),

    # The second half of 7.2: scale-dependence of realised key entropy, and occurrence-cap
    # truncation. A real finding, and secondary to the entropy-against-agreement trade the
    # first half of the subsection makes.
    ("7.2 scale-dependence of key entropy",
     r"\textbf{Specificity also depends on scale.}",
     r"\subsection{Recoverable loss inside the chainer}"),

    # --- round three: 14 pages after round two --------------------------------------

    # What the harness records per run. Its consequence -- every run is development-only,
    # because every run was virtualised -- is stated in Section 9, which survives.
    ("4.1 harness metadata",
     "The harness records per run",
     r"\textbf{Host.}"),

    # --- round four: 13 pages after round three -------------------------------------

    # The processor-time ratio against RawHash2. The paper's own text says it "is a bound on
    # data-plane cost, not a performance result", because one system resolves reads and the
    # other largely does not. The most heavily qualified result in Section 4, and so the
    # first to go. The readfish comparison, which carries the architectural argument, stays.
    ("4.3 processor time against RawHash2",
     r"\textbf{Against a signal-space mapper.}",
     r"\section{The accuracy gap}"),
]

# Floats removed by label. Each is either a picture of numbers a surviving table carries, or
# a diagram of a mechanism the surviving prose describes.
DROP_FLOATS = {
    "fig:r3",        # plots the nine runs Table 2 already gives as median and range
    "tab:entropy",   # belongs to the 7.2 passage removed above
    "tab:rules",     # every figure the argument uses -- 9.1, 14.1, 19.0, +35%, 3.8, 7.0,
                     # 10.8 -- is already in the prose beneath it; checked before removal
    "fig:arena",     # the protocol is fully described in the 3.3 prose; the diagram
                     # illustrates it. Figure 3 is kept instead, because it is evidence --
                     # the data behind the refutation -- rather than illustration.
}

# References in Springer LNCS style (splncs04): "Surname, I.: Title. Venue Vol(No) (Year)".
# The IEEE source carries IEEE style -- initials first, titles in quotes -- which is wrong for
# a Springer submission. (The authors' first paper has the same defect: its converter copied
# the IEEE references verbatim.) Springer style is also shorter, which this version needs.
#
# Every field is taken from the IEEE bibliography; no volume, issue or page number is added
# that it did not carry. The abridgement subset check in verify_paper_numbers.py enforces
# that mechanically -- an invented volume number would be a numeral absent from the IEEE file.
SPRINGER_BIBLIOGRAPHY = r"""\begin{thebibliography}{10}

\bibitem{minknow} Oxford Nanopore Technologies: MinKNOW API,
\url{https://github.com/nanoporetech/minknow_api}

\bibitem{readfish} Payne, A., Holmes, N., Clarke, T., Munro, R., Debebe, B.J., Loose, M.:
Readfish enables targeted nanopore sequencing of gigabase-sized genomes. Nature Biotechnology
(2021)

\bibitem{uncalled} Kovaka, S., Fan, Y., Ni, B., Timp, W., Schatz, M.C.: Targeted nanopore
sequencing by real-time mapping of raw electrical signal with UNCALLED. Nature Biotechnology
(2021)

\bibitem{sigmap} Zhang, H., Li, H., Jain, C., Cheng, H., Au, K.F., Li, H., Aluru, S.: Real-time
mapping of nanopore raw signals. Bioinformatics 37(Suppl.~1) (2021)

\bibitem{rawhash} Firtina, C., Mansouri Ghiasi, N., Lindegger, J., Singh, G., Cavlak, M.B.,
Mao, H., Mutlu, O.: RawHash: enabling fast and accurate real-time analysis of raw nanopore
signals for large genomes. Bioinformatics 39(Suppl.~1) (2023)

\bibitem{rawhash2} Firtina, C., Soysal, M., Lindegger, J., Mutlu, O.: RawHash2: mapping raw
nanopore signals using hash-based seeding and adaptive quantization. arXiv:2309.05771 (2023)

\bibitem{rawalign} Lindegger, J., Firtina, C., Mansouri Ghiasi, N., Sadrosadati, M., Alser,
M., Mutlu, O.: RawAlign: accurate, fast, and scalable raw nanopore signal mapping via combining
seeding and alignment. arXiv:2310.05037 (2023)

\bibitem{sigmoni} Shivakumar, V., Ahmed, O., Kovaka, S., Zakeri, M., Langmead, B.: Sigmoni:
classification of nanopore signal with a compressed pangenome index. Bioinformatics (2024)

\bibitem{rawbench} Eris, M., Firtina, C., Mutlu, O., et al.: RawBench: a comprehensive
benchmarking framework for raw nanopore signal analysis techniques. In: ACM-BCB (2025).
arXiv:2510.03629

\bibitem{squigulator} Gamaarachchi, H., et al.: Squigulator: simulation of nanopore sequencing
signal data. Genome Research (2024)

\bibitem{icarust} Munro, R., Payne, A., Loose, M.: Icarust: a real-time simulator for nanopore
adaptive sampling, \url{https://github.com/LooseLab/Icarust}

\bibitem{misragries} Misra, J., Gries, D.: Finding repeated elements. Science of Computer
Programming 2(2) (1982)

\bibitem{spacesaving} Metwally, A., Agrawal, D., El Abbadi, A.: Efficient computation of
frequent and top-$k$ elements in data streams. In: ICDT (2005)

\bibitem{minimap2} Li, H.: Minimap2: pairwise alignment for nucleotide sequences.
Bioinformatics 34(18) (2018)

\bibitem{hdrhistogram} Tene, G.: HdrHistogram: a high dynamic range histogram,
\url{https://github.com/HdrHistogram/HdrHistogram_c}

\bibitem{minimizers} Roberts, M., Hayes, W., Hunt, B.R., Mount, S.M., Yorke, J.A.: Reducing
storage requirements for biological sequence comparison. Bioinformatics 20(18) (2004)

\end{thebibliography}

\end{document}
"""

# Exact replacements, applied after the spans. Each tidies a sentence that a removed span
# would otherwise leave pointing at nothing.
REPLACEMENTS = [
    # R1 is no longer reported, so it is no longer defined.
    ("drop the R1 definition",
     r"\textbf{Regimes.} R1: a 37 kb reference under constant dwell. R3: a 64 Mb human chr20 reference",
     r"\textbf{Regime.} R3: a 64 Mb human chr20 reference"),

    # Figure 2 is gone; the sentence pointing at it ends one parenthesis earlier.
    ("unpoint fig:r3",
     r"p90 and at the mean (Figure~\ref{fig:r3}).",
     r"p90 and at the mean."),

    # Figure 1 is gone; the protocol sentence no longer points at it.
    ("unpoint fig:arena",
     r"The protocol removes it structurally (Figure~\ref{fig:arena}).",
     r"The protocol removes it structurally."),

    # Table 5 is gone; the sentence that introduced it now states what it contained.
    ("unpoint tab:rules",
     r"Table~\ref{tab:rules} gives six rules in triplicate.",
     r"Six decision rules were each run in triplicate at the same operating point."),

    # The reproducibility statement, shortened to what a reader of this version needs. The
    # thesis-level script and the list of five scope boundaries are in the repository.
    ("compress reproducibility",
     r"""\texttt{scripts/verify\_thesis\_numbers.py} recomputes every number from files under
\texttt{results/}, or, for figures describing code that no longer exists, checks them against the
commit message that recorded them; it exits non-zero on any disagreement, and reports 143 claims
agreeing and none disagreeing. \texttt{scripts/verify\_paper\_numbers.py} performs the same check
against this manuscript. \texttt{scripts/generate\_thesis\_figures.py} regenerates every data
figure from \texttt{results/} and nothing else, byte-stably. Five claims are not settled by any
committed file and are documented as scope boundaries: a bare-metal deadline-miss rate, real
instrument data, agreement across the full dwell sweep and across seeds, the Icarust accept rates
behind the simulator comparison, and the index resident set at minimizer window 10.""",
     r"""\texttt{scripts/verify\_paper\_numbers.py} recomputes every number in this paper from files
under \texttt{results/} and exits non-zero on any disagreement. Five claims are not settled by any
committed file and are documented as scope boundaries in the repository."""),
]


def main() -> int:
    src = SRC.read_text(encoding="utf-8")

    m_abs = re.search(r"\\begin\{abstract\}(.*?)\\end\{abstract\}", src, re.S)
    if not m_abs:
        print("could not find the abstract", file=sys.stderr)
        return 1
    abstract = m_abs.group(1).strip()

    m_kw = re.search(r"\\begin\{IEEEkeywords\}(.*?)\\end\{IEEEkeywords\}", src, re.S)
    if not m_kw:
        print("could not find IEEEkeywords", file=sys.stderr)
        return 1
    # LNCS separates keywords with \and rather than commas.
    terms = [t.strip() for t in re.sub(r"\s+", " ", m_kw.group(1)).split(",") if t.strip()]
    keywords = (" " + BS + "and ").join(terms)

    body = src[src.index(r"\section{Introduction}"):src.index(r"\begin{thebibliography}")]

    # ---- single column: nothing to span -----------------------------------------------
    before = len(re.findall(r"\\begin\{figure\*\}", body))
    body = body.replace(BS + "begin{figure*}", BS + "begin{figure}")
    body = body.replace(BS + "end{figure*}", BS + "end{figure}")
    if before == 0:
        print("expected at least one figure* to convert", file=sys.stderr)
        return 1

    # ---- the arena picture has room again ---------------------------------------------
    if "right=12mm of" in body:
        body = body.replace("right=12mm of", "right=14mm of")

    # ---- cuts to 12 pages -------------------------------------------------------------
    for label, start, end in SPANS:
        if start not in body:
            print("cut " + repr(label) + ": start marker not found", file=sys.stderr)
            return 1
        i = body.index(start)
        j = body.find(end, i + len(start))
        if j < 0:
            print("cut " + repr(label) + ": end marker not found", file=sys.stderr)
            return 1
        body = body[:i] + body[j:]

    # Floats are matched as whole environments and dropped by the label inside them, so a
    # caption edit cannot make the match miss. Each label must be found exactly once.
    for lab in sorted(DROP_FLOATS):
        hits = [m for m in re.finditer(r"\\begin\{(figure|table)\}.*?\\end\{\1\}\n*", body, re.S)
                if "\\label{" + lab + "}" in m.group(0)]
        if len(hits) != 1:
            print("float " + lab + ": expected 1 match, found " + str(len(hits)), file=sys.stderr)
            return 1
        body = body[:hits[0].start()] + body[hits[0].end():]

    for label, old, new in REPLACEMENTS:
        if old not in body:
            print("replacement " + repr(label) + " did not match", file=sys.stderr)
            return 1
        body = body.replace(old, new, 1)

    # Nothing may refer to a label a cut removed.
    labels = set(re.findall(r"\\label\{([^}]*)\}", body))
    dangling = sorted(set(re.findall(r"\\ref\{([^}]*)\}", body)) - labels)
    if dangling:
        print("dangling references after cuts: " + ", ".join(dangling), file=sys.stderr)
        return 1

    # The restyled bibliography must cite exactly the entries the source does, or a
    # reference has been dropped or invented in the restyling.
    src_bib = src[src.index(r"\begin{thebibliography}"):]
    want_keys = re.findall(r"\\bibitem\{([^}]*)\}", src_bib)
    have_keys = re.findall(r"\\bibitem\{([^}]*)\}", SPRINGER_BIBLIOGRAPHY)
    if want_keys != have_keys:
        print("bibliography keys differ from the source:", file=sys.stderr)
        print("  source  : " + ", ".join(want_keys), file=sys.stderr)
        print("  springer: " + ", ".join(have_keys), file=sys.stderr)
        return 1
    bib = SPRINGER_BIBLIOGRAPHY

    out = (PREAMBLE
           + BS + "begin{abstract}" + NL + abstract + NL + NL
           + BS + "keywords{" + keywords + "}" + NL
           + BS + "end{abstract}" + NL + NL
           + body + bib)

    DST.write_text(out, encoding="utf-8")
    d = DST.read_bytes()
    print("wrote %s (%d lines); %d figure* converted; non-ascii %d"
          % (DST.relative_to(REPO), len(out.splitlines()), before,
             len([b for b in d if b > 127])))
    return 0


if __name__ == "__main__":
    sys.exit(main())
