#!/usr/bin/env python3
"""Assemble an upload-ready LaTeX bundle for the paper.

EXACTLY ONE .tex sits at the archive root, with figures/ beside it, so that
\\graphicspath{{figures/}} resolves with nothing edited and a submission system choosing a
main document by itself cannot choose wrong. The 10-page ACM variant goes in a subdirectory
with its own copy of the figures: 139 KiB of duplication buys an unambiguous root, and a
reader who wants that version compiles it in place.

Nothing is generated here. Every file is copied from the repository, so the archive cannot
contain a number that scripts/verify_paper_numbers.py has not checked.

    python scripts/make_paper_zip.py [--outdir DIR]
"""

from __future__ import annotations

import argparse
import re
import sys
import zipfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
PAPER = REPO / "paper"
NAME = "microsecond-readuntil-paper-latex"
ALT = "acm-10-page"
SPRINGER = "springer-lncs"

# archive path -> repository path
FILES = {
    "manuscript-ieee.tex": PAPER / "manuscript-ieee.tex",
    "manuscript-ieee.pdf": PAPER / "manuscript-ieee.pdf",
    "manuscript.md": PAPER / "manuscript.md",
    "README.md": PAPER / "BUNDLE.md",
    ALT + "/manuscript-acmart.tex": PAPER / "manuscript.tex",
    ALT + "/manuscript-acmart.pdf": PAPER / "manuscript.pdf",
    SPRINGER + "/manuscript-springer.tex": PAPER / "manuscript-springer.tex",
    SPRINGER + "/manuscript-springer.pdf": PAPER / "manuscript-springer.pdf",
}

FIGURES = sorted((PAPER / "figures").glob("*.pdf")) + [PAPER / "figures" / "MANIFEST.md"]


def springer_only(outdir: Path) -> int:
    """A submission archive for the Springer version alone, its .tex at the root.

    The combined archive keeps the IEEE manuscript at its root, so Overleaf opens that one
    and the Springer file needs the main document switching by hand. For the version being
    submitted to a Springer venue, the archive should simply open the right paper. Only the
    figures the Springer file actually includes are packed, read from the file itself.
    """
    name = "microsecond-readuntil-springer-latex"
    tex = PAPER / "manuscript-springer.tex"
    pdf = PAPER / "manuscript-springer.pdf"
    src = tex.read_text(encoding="utf-8")
    used = re.findall(r"\\includegraphics(?:\[[^]]*\])?\{([^}]*)\}", src)
    figs = [PAPER / "figures" / u for u in used]
    missing = [str(p) for p in [tex, pdf] + figs if not p.exists()]
    if missing:
        print("missing: " + ", ".join(missing), file=sys.stderr)
        return 1
    out = outdir / (name + ".zip")
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        z.write(tex, name + "/manuscript-springer.tex")
        z.write(pdf, name + "/manuscript-springer.pdf")
        for f in figs:
            z.write(f, name + "/figures/" + f.name)
    print("wrote " + str(out) + "  (" + str(2 + len(figs)) + " files, "
          + str(round(out.stat().st_size / 1024)) + " KiB)")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--outdir", default=str(REPO / "paper" / "dist"))
    ap.add_argument("--springer", action="store_true",
                    help="build the Springer-only submission archive instead")
    args = ap.parse_args()

    outdir = Path(args.outdir)
    outdir.mkdir(parents=True, exist_ok=True)
    if args.springer:
        return springer_only(outdir)
    out = outdir / (NAME + ".zip")

    missing = [str(p) for p in list(FILES.values()) + FIGURES if not p.exists()]
    if missing:
        print("missing: " + ", ".join(missing), file=sys.stderr)
        return 1

    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        for arcname, path in FILES.items():
            z.write(path, NAME + "/" + arcname)
        for path in FIGURES:
            z.write(path, NAME + "/figures/" + path.name)
            z.write(path, NAME + "/" + ALT + "/figures/" + path.name)
            z.write(path, NAME + "/" + SPRINGER + "/figures/" + path.name)

    n = len(FILES) + 3 * len(FIGURES)
    print("wrote " + str(out) + "  (" + str(n) + " files, "
          + str(round(out.stat().st_size / 1024)) + " KiB)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
