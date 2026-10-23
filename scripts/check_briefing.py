#!/usr/bin/env python3
"""Check that paper/briefing.tex quotes no figure the manuscript does not.

The briefing explains the paper to a reader outside the field. It therefore repeats numbers
rather than deriving them, which is exactly the situation in which a digit gets changed in
transcription and nobody notices, because the briefing is read by someone who cannot check it
against the data.

So every numeric token in the briefing must also appear in paper/manuscript-ieee.tex. The
check is one-directional on purpose: the paper may carry figures the briefing leaves out, but
the briefing may not invent one.

Tokens that are structural rather than measured -- section numbers, years, the geometry of
the document itself -- are listed in ALLOWED with the reason each is there.

    python scripts/check_briefing.py
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
BRIEFING = REPO / "paper" / "briefing.tex"
PAPER = REPO / "paper" / "manuscript-ieee.tex"

# Numerals the briefing may carry that the paper does not.
#
# Kept deliberately minimal. A first draft of this list held sixteen entries covering list
# numbering, font sizes and margins; every one of them turned out to be unnecessary, because
# those numerals either appear in the paper anyway or are stripped as lengths before the
# comparison. An allowlist entry that is never needed is not harmless -- it is a standing
# exemption for a number nobody is checking. Add to this only on evidence that the check
# fails without it.
ALLOWED = {
    "144": "the assertion count reported by verify_paper_numbers.py. A property of the "
           "repository's tooling, not a measurement in the paper, and checkable by running "
           "that script.",
}


def numerals(text: str) -> list[str]:
    """Numeric tokens, with LaTeX lengths and markup stripped first.

    Command NAMES are removed but braced arguments are kept, because most of the figures in
    both documents sit inside \\textbf{...} or \\emph{...}. An earlier version of this
    function stripped commands together with their arguments, which silently discarded the
    very numbers it existed to check; it passed against a briefing with two digits
    deliberately altered.
    """
    t = re.sub(r"%.*", "", text)                                 # comments
    t = re.sub(r"\d+(\.\d+)?\s*(pt|cm|mm|em|ex|in)\b", " ", t)   # lengths, before anything
    t = re.sub(r"\\[a-zA-Z]+\s*(\[[^]]*\])?", " ", t)            # command names and options
    t = re.sub(r"[{}$~\\]", " ", t)                              # braces, maths, ties
    return re.findall(r"\d+(?:[.,]\d+)*", t)


def main() -> int:
    if not BRIEFING.exists() or not PAPER.exists():
        print("missing input file", file=sys.stderr)
        return 2

    brief = BRIEFING.read_text(encoding="utf-8")
    paper = PAPER.read_text(encoding="utf-8")

    # The paper's own numerals, plus their comma-free forms, so 63,981,218 matches 63981218.
    pool = set(numerals(paper))
    pool |= {n.replace(",", "") for n in pool}

    bad = []
    for n in sorted(set(numerals(brief)), key=lambda x: (len(x), x)):
        if n in pool or n.replace(",", "") in pool or n in ALLOWED:
            continue
        bad.append(n)

    print("=" * 70)
    print("BRIEFING NUMBER CHECK")
    print("=" * 70)
    print()
    print("briefing : " + str(BRIEFING.relative_to(REPO)))
    print("against  : " + str(PAPER.relative_to(REPO)))
    print()

    if bad:
        for n in bad:
            ctx = ""
            m = re.search(r".{0,60}" + re.escape(n) + r".{0,40}", brief)
            if m:
                ctx = re.sub(r"\s+", " ", m.group(0)).strip()
            print("FAIL  " + n + "  not in the paper")
            print("        " + ctx)
        print()
        print(str(len(bad)) + " problem(s)")
        return 1

    print("ok    every numeral in the briefing appears in the paper")
    print()
    print("0 problem(s)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
