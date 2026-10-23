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

    bib = src[src.index(r"\begin{thebibliography}"):]

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
