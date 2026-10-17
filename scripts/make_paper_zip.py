#!/usr/bin/env python3
"""Assemble an upload-ready LaTeX bundle for the paper.

The bundle is flat: both manuscripts and a figures/ directory sit at the archive root, so
\\graphicspath{{figures/}} resolves without editing anything and Overleaf, arXiv or a
conference submission system can take the archive as given.

Nothing is generated here. Every file is copied from the repository, so the archive cannot
contain a number that scripts/verify_paper_numbers.py has not checked.

    python scripts/make_paper_zip.py [--outdir DIR]
"""

from __future__ import annotations

import argparse
import sys
import zipfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
PAPER = REPO / "paper"
NAME = "microsecond-readuntil-paper-latex"

# archive path -> repository path
FILES = {
    "manuscript-ieee.tex": PAPER / "manuscript-ieee.tex",
    "manuscript-ieee.pdf": PAPER / "manuscript-ieee.pdf",
    "manuscript-acmart.tex": PAPER / "manuscript.tex",
    "manuscript-acmart.pdf": PAPER / "manuscript.pdf",
    "manuscript.md": PAPER / "manuscript.md",
    "README.md": PAPER / "BUNDLE.md",
}

FIGURES = sorted((PAPER / "figures").glob("*.pdf")) + [PAPER / "figures" / "MANIFEST.md"]


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--outdir", default=str(REPO / "paper" / "dist"))
    args = ap.parse_args()

    outdir = Path(args.outdir)
    outdir.mkdir(parents=True, exist_ok=True)
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

    n = len(FILES) + len(FIGURES)
    print("wrote " + str(out) + "  (" + str(n) + " files, "
          + str(round(out.stat().st_size / 1024)) + " KiB)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
