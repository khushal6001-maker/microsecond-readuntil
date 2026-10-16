#!/usr/bin/env python3
"""Mechanical consistency audit across the manuscript.

Complements scripts/verify_thesis_numbers.py, which checks that the NUMBERS agree with the
data. This one checks the manuscript's internal structure: that no [VERIFY] marker is left
unresolved, that every [TABLE n]/[FIG n] marker corresponds to something that exists, that
cross-references point at sections that exist, and that figures named in the manifest are on
disk.

It reports rather than fixes, and exits non-zero if any category is non-empty.

    python scripts/audit_manuscript.py
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
DOCS = REPO / "docs"
FIGS = REPO / "figures"

CHAPTERS = [DOCS / f"chapter{n}-draft.md" for n in range(1, 9)] + [
    DOCS / "appendices-draft.md"
]

problems: dict[str, list[str]] = {}


def add(cat: str, msg: str) -> None:
    problems.setdefault(cat, []).append(msg)


texts = {}
for p in CHAPTERS:
    if not p.exists():
        add("missing chapter file", str(p.name))
        continue
    texts[p.name] = p.read_text(encoding="utf-8")

# ---------------------------------------------------------------- 1. [VERIFY] markers
# Appendix F is the verification register and exists to explain the retired convention, so it
# necessarily names the marker it retired. Its section is excluded rather than the check being
# weakened, which would stop it catching a live marker written inside backticks elsewhere.
for name, t in texts.items():
    body = t
    if "# Appendix F" in body:
        body = body[: body.index("# Appendix F")]
    for m in re.finditer(r"\[VERIFY[^\]]*\]", body):
        line = body[: m.start()].count("\n") + 1
        add("unresolved [VERIFY]", f"{name}:{line}  {m.group(0)}")

# ---------------------------------------------------------------- 2. table / figure markers
# A [TABLE n] marker should sit beside an actual markdown table (a line starting with '|')
# within a few lines, or beside a bolded "Table n" caption.
for name, t in texts.items():
    lines = t.split("\n")
    for i, line in enumerate(lines):
        for m in re.finditer(r"\[(TABLE|FIG) ([0-9]+\.[0-9x]+)\]", line):
            kind, num = m.group(1), m.group(2)
            if num.endswith(".x"):
                continue  # the drafting-note placeholders in each chapter header
            window = "\n".join(lines[max(0, i - 14) : i + 14])
            if kind == "TABLE":
                if "|" not in window and f"Table {num}" not in window:
                    add("TABLE marker with no table",
                        f"{name}:{i + 1}  [{kind} {num}]")
            else:
                # A [FIG n] must name a file that exists, either generated or a diagram.
                stem = f"fig{num.replace('.', '-')}"
                hits = list(FIGS.glob(f"{stem}-*.pdf")) + list(
                    FIGS.glob(f"diagrams/{stem}-*.svg"))
                if not hits:
                    add("FIG marker with no figure file",
                        f"{name}:{i + 1}  [{kind} {num}] -> expected {stem}-*")

# ---------------------------------------------------------------- 3. every figure referenced
referenced = set()
for t in texts.values():
    for m in re.finditer(r"\[FIG ([0-9]+\.[0-9]+)\]", t):
        referenced.add(m.group(1))
on_disk = set()
for p in list(FIGS.glob("fig*.pdf")) + list(FIGS.glob("diagrams/fig*.svg")):
    m = re.match(r"fig(\d+)-(\d+)-", p.name)
    if m:
        on_disk.add(f"{m.group(1)}.{m.group(2)}")
for num in sorted(on_disk - referenced):
    add("figure on disk never referenced", f"figure {num}")

# ---------------------------------------------------------------- 4. cross-references
# Sections appear two ways: as markdown headings (## 5.6) and, for the third level, as a
# bolded inline run-in heading (**5.2.2 The artefact.**). Both count.
all_text = "\n".join(texts.values())
known: set[str] = set()
known.update(re.findall(r"^#{2,3} (?:Appendix )?([0-9]+\.[0-9]+(?:\.[0-9]+)?)", all_text, re.M))
known.update(re.findall(r"^\*\*([0-9]+\.[0-9]+\.[0-9]+)\s", all_text, re.M))
known.update(re.findall(r"^#{1,3} ([A-F]\.[0-9]+)", all_text, re.M))
# A reference to §n.m is satisfied by any subsection of it being present, since the parent
# heading always exists when its children do.
known.update({".".join(k.split(".")[:2]) for k in list(known) if k.count(".") == 2})

for name, t in texts.items():
    for m in re.finditer(r"§§?([0-9]+\.[0-9]+(?:\.[0-9]+)?)", t):
        ref = m.group(1)
        if ref not in known:
            line = t[: m.start()].count("\n") + 1
            add("cross-reference to a section that does not exist", f"{name}:{line}  §{ref}")

# ---------------------------------------------------------------- 5. manifest agreement
manifest = FIGS / "MANIFEST.md"
if manifest.exists():
    mt = manifest.read_text(encoding="utf-8")
    for p in sorted(FIGS.glob("fig*.pdf")):
        if p.stem not in mt:
            add("figure missing from MANIFEST.md", p.stem)
    for m in re.finditer(r"`(fig[0-9a-z\-]+)`", mt):
        if not (FIGS / f"{m.group(1)}.pdf").exists():
            add("MANIFEST.md names a figure that is not on disk", m.group(1))
else:
    add("missing chapter file", "figures/MANIFEST.md")

# ---------------------------------------------------------------- 6. stale figures
newest_data = max(
    (p.stat().st_mtime for p in (REPO / "results").rglob("*.csv")), default=0.0)
for p in sorted(FIGS.glob("fig*.pdf")):
    if p.stat().st_mtime < newest_data - 60:
        add("figure older than the data it is built from",
            f"{p.stem} (regenerate with scripts/generate_thesis_figures.py)")

# ---------------------------------------------------------------- report
print("=" * 78)
print("MANUSCRIPT AUDIT")
print("=" * 78)
words = sum(len(t.split()) for t in texts.values())
print(f"\n{len(texts)} files, {words:,} words\n")

order = [
    "missing chapter file",
    "unresolved [VERIFY]",
    "TABLE marker with no table",
    "FIG marker with no figure file",
    "figure on disk never referenced",
    "cross-reference to a section that does not exist",
    "figure missing from MANIFEST.md",
    "MANIFEST.md names a figure that is not on disk",
    "figure older than the data it is built from",
]
total = 0
for cat in order:
    items = problems.get(cat, [])
    total += len(items)
    mark = "ok  " if not items else "FAIL"
    print(f"{mark} {cat}: {len(items)}")
    for it in items[:25]:
        print(f"       {it}")
    if len(items) > 25:
        print(f"       ... and {len(items) - 25} more")

for cat, items in problems.items():
    if cat not in order:
        total += len(items)
        print(f"FAIL {cat}: {len(items)}")
        for it in items[:25]:
            print(f"       {it}")

print(f"\n{total} problem(s)")
sys.exit(1 if total else 0)
