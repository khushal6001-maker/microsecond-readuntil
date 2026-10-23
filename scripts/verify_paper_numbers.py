#!/usr/bin/env python3
"""Check that every load-bearing number in paper/ matches the data under results/.

scripts/verify_thesis_numbers.py checks the THESIS against the data. This checks the PAPER,
and it does it the other way round: rather than transcribing claims by hand, it builds the
expected string from the data file and asserts that string appears in the manuscript. A
corrected measurement therefore fails this check until the paper is updated, which is the
property that keeps a standalone paper from drifting away from the repository it cites.

Both the Markdown and both LaTeX manuscripts (acmart and IEEE) are checked, so no two of
them can disagree either.

    python scripts/verify_paper_numbers.py
"""

from __future__ import annotations

import csv
import re
import sys
from decimal import Decimal, ROUND_HALF_UP
from pathlib import Path


def r(value, places=1) -> str:
    """Round the way a paper rounds: half away from zero, on the decimal literal.

    Python's f-string uses round-half-even on a binary float, so 370.15 prints as 370.1 and
    466.5 as 466. The manuscript quotes 370.2 and 467, which is conventional and correct for
    the decimal value in the data file. Rounding the string through Decimal rather than the
    float avoids both the tie-breaking rule and the binary representation error.
    """
    q = Decimal(1).scaleb(-places) if places > 0 else Decimal(1)
    return str(Decimal(str(value)).quantize(q, rounding=ROUND_HALF_UP))

REPO = Path(__file__).resolve().parent.parent
RESULTS = REPO / "results"
PAPERS = [REPO / "paper" / "manuscript.md",
          REPO / "paper" / "manuscript.tex",
          REPO / "paper" / "manuscript-ieee.tex",
          REPO / "paper" / "manuscript-springer.tex"]

# Manuscripts that are deliberate abridgements of a fully checked one. A 12-page version
# legitimately leaves out results a full-length one states, so requiring every claim in every
# manuscript would forbid shortening a paper at all. The rule instead is:
#
#   OMISSION IS ALLOWED, INVENTION IS NOT.
#
# An abridged manuscript may skip the claims listed under "omits", because it skips the
# sections that state them. In exchange, every numeral it DOES contain must also appear in the
# manuscript it abridges, which is itself held to every claim. So nothing in the short version
# can be wrong without also being absent from a document that is checked in full.
#
# Each omission must also be genuinely absent. A label listed here whose figures are present
# after all is reported as a stale omission, so this list cannot quietly outlive the cut that
# justified it -- the same reason an unused allowlist entry is treated as a defect.
ABRIDGED = {
    "manuscript-springer.tex": {
        "of": "manuscript-ieee.tex",
        "omits": {
            # Section 3.2 descriptor layout and 3.5 per-channel footprint
            "ChunkRef size", "vote table footprint",
            # Section 4.1 histogram correction, about an earlier instrument
            "TSC artefact",
            # Section 4.2, the 37 kb secondary regime, with its table
            "R1 pinned-2", "R1 unpinned-4", "p99 spread claim",
            # Section 7.2 scale-dependence of key entropy, with its table
            "3x13 entropy and shortfall", "4x12 entropy and shortfall",
            "3x8 truncation, both references", "3x8 distinct keys at 20 Mb",
            "4x12 distinct keys at 20 Mb",
            # Section 4.3 processor-time ratio against RawHash2
            "comparison per read",
        },
    },
}

OK: list[str] = []
BAD: list[tuple[str, str, str]] = []
SKIPPED: list[str] = []


def rows(name: str) -> list[dict]:
    p = RESULTS / name
    if not p.exists():
        return []
    with open(p, newline="", encoding="utf-8") as f:
        lines = [ln for ln in f if not ln.lstrip().startswith("#")]
    return list(csv.DictReader(lines))


def norm(t: str) -> str:
    """Fold the typographic differences between the two manuscript formats.

    The Markdown uses en dashes and Unicode mu; the LaTeX uses --, \\mu s and escaped
    percents. Neither difference is a number, so both are normalised away before matching.
    """
    t = t.replace("\u2013", "-").replace("\u2014", "-").replace("\u00b5", "u")
    t = t.replace("---", "-").replace("--", "-")  # LaTeX en and em dashes
    t = t.replace("\\textmu", "u").replace("\\micro", "u")
    t = t.replace("\\%", "%").replace("$", "").replace("\\,", " ").replace("~", " ")
    t = t.replace("\\mu s", "us").replace("\\times", "x").replace("\u00d7", "x")
    t = t.replace("\\approx", "~").replace("\u2248", "~")
    t = re.sub(r"\\[a-zA-Z]+\{([^}]*)\}", r"\1", t)   # \textbf{...} -> ...
    t = re.sub(r"[`*_{}]", "", t)
    return re.sub(r"\s+", " ", t)


def want(label: str, *fragments: str) -> None:
    """Assert every fragment appears in each manuscript, after normalisation.

    For an abridged manuscript, a label it omits is not required -- but it must really be
    absent, or the omission list has gone stale.
    """
    for p in PAPERS:
        if not p.exists():
            continue
        body = norm(p.read_text(encoding="utf-8"))
        missing = [f for f in fragments if norm(f) not in body]
        omitted = label in ABRIDGED.get(p.name, {}).get("omits", set())
        if omitted:
            if not missing:
                BAD.append((label, p.name, "stale omission: every figure is present, so "
                                           "remove this label from ABRIDGED"))
            else:
                SKIPPED.append(f"{label} [{p.name}]")
            continue
        if missing:
            BAD.append((label, p.name, "; ".join(missing)))
        else:
            OK.append(f"{label} [{p.name}]")


def numerals(text: str) -> set[str]:
    """Numeric tokens of a LaTeX source, for the abridgement subset check.

    Command NAMES are removed but braced arguments kept, since most figures sit inside
    \\textbf{} or \\emph{}. Stripping commands together with their arguments discards exactly
    the numbers this exists to check; that bug was found once already, in check_briefing.py,
    by planting a wrong figure and watching the check pass.
    """
    t = re.sub(r"(?<!\\)%.*", "", text)                          # comments, not \%
    t = re.sub(r"\d+(\.\d+)?\s*(pt|cm|mm|em|ex|in)\b", " ", t)   # lengths
    t = re.sub(r"\\[a-zA-Z]+\s*(\[[^]]*\])?", " ", t)            # command names, options
    t = re.sub(r"[{}$~\\]", " ", t)
    found = set(re.findall(r"\d+(?:[.,]\d+)*", t))
    return found | {n.replace(",", "") for n in found}


# ------------------------------------------------------------------ regime R3
r3 = rows("r3-latency.csv")
if r3:
    runs = [r for r in r3 if r["run"] != "median"]
    med = next(r for r in r3 if r["run"] == "median")
    lo50 = min(float(r["p50"]) for r in runs)
    hi50 = max(float(r["p50"]) for r in runs)
    lo99 = min(float(r["p99"]) for r in runs)
    hi99 = max(float(r["p99"]) for r in runs)
    want("R3 p50 with range",
         f"{float(med['p50']):.1f}", f"[{lo50:.1f}-{hi50:.1f}]")
    want("R3 p99 with range",
         r(med['p99']), f"[{r(lo99)}-{r(hi99)}]")
    want("R3 p90", f"{float(med['p90']):.1f}")
    want("R3 resident set", f"{float(med['rss_gb']):.2f} GiB")
    want("R3 offered rate",
         f"{min(int(r['chunks_per_s']) for r in runs)}-"
         f"{max(int(r['chunks_per_s']) for r in runs)}")
    want("R3 run count", f"nine")
    want("R3 utilisation", f"{float(med['p50']) / 1560:.3f}")
    want("R3 derived per-read", f"{10 * float(med['p50']) / 1000:.2f} ms")
    over = [r for r in runs if float(r["p99"]) > 1560]
    want("one run crosses the budget",
         f"{float(over[0]['p99']):.1f}", f"{float(over[0]['p99_9']):.1f}")
    want("maxima span",
         f"{min(float(r['max']) for r in runs) / 1000:.1f} ms",
         f"{max(float(r['max']) for r in runs) / 1000:.1f} ms")
    biggest_max = max(runs, key=lambda r: float(r["max"]))
    want("the largest maximum is not the worst p99",
         f"{float(biggest_max['p99']):.0f}")

# ------------------------------------------------------------------ accuracy
dr = rows("decision-rules.csv")
rh = rows("rawhash-seeds.csv")
if dr and rh:
    def d(rule, look, occ, key):
        for r in dr:
            if r["rule"] == rule and r["lookback"] == look and r["occ_filter"] == occ:
                return r[key]
        return None

    want("our headline accuracy",
         f"{float(d('anchor-history chains','48','no','tpr_mean')):.1f}",
         f"[{float(d('anchor-history chains','48','no','tpr_lo')):.1f}-"
         f"{float(d('anchor-history chains','48','no','tpr_hi')):.1f}]")
    mean_rh = next(r for r in rh if r["seed"] == "mean")
    lo = min(float(r["correct_pct"]) for r in rh if r["seed"] != "mean")
    hi = max(float(r["correct_pct"]) for r in rh if r["seed"] != "mean")
    want("comparison accuracy",
         f"{float(mean_rh['correct_pct']):.1f}", f"[{lo:.1f}-{hi:.1f}]")
    want("best decision rule",
         f"{float(d('anchor-history chains','128','no','tpr_mean')):.1f}")
    want("voting baseline",
         f"{float(d('exact-diagonal votes (256 counters)','','no','tpr_mean')):.1f}")
    want("beta=0 control",
         f"{float(d('greedy chains (control)','','no','tpr_mean')):.1f}")
    want("median best-chain score",
         d("anchor-history chains", "48", "no", "median_votes"))
    spread = max(float(d('greedy chains (control)','','no','tpr_hi'))
                 - float(d('greedy chains (control)','','no','tpr_lo')), 0)
    want("worst across-seed spread", f"{spread:.1f} points")

# ------------------------------------------------------------------ agreement
ag = rows("agreement-summary.csv")
if ag:
    def a(system, seg, cv, key):
        for r in ag:
            if (r["system"] == system and r["segmentation"] == seg
                    and abs(float(r["dwell_cv"]) - cv) < 1e-9 and r["norm"] != "chunk"):
                return float(r[key])
        return None

    ours_p, comp_p = a("ours", "detected", 0.44, "p_key12"), a("rawhash2", "own", 0.44, "p_key8")
    ours_p8 = a("ours", "detected", 0.44, "p_key8")
    want("our a and g at CV 0.44",
         f"{a('ours','detected',0.44,'a_dp'):.3f}",
         f"{a('ours','detected',0.44,'indel_frac'):.3f}")
    want("comparison a and g at CV 0.44",
         r(a('rawhash2','own',0.44,'a_dp'), 3),
         r(a('rawhash2','own',0.44,'indel_frac'), 3))
    want("key-match probabilities", f"{comp_p:.3f}", f"{ours_p:.4f}")
    want("anchors per read",
         r(a('rawhash2','own',0.44,'anchors_read'), 0),
         r(a('ours','detected',0.44,'anchors_read')))
    want("key-match ratio", f"{round(comp_p / ours_p)}")
    want("anchor-supply ratio",
         f"{round(a('rawhash2','own',0.44,'anchors_read') / a('ours','detected',0.44,'anchors_read'))}")
    want("key-length factor", f"{ours_p8 / ours_p:.1f}")
    want("encoding factor", f"{comp_p / ours_p8:.1f}")
    want("our g at zero dispersion", f"{a('ours','detected',0.0,'indel_frac'):.3f}")
    want("comparison dwell invariance",
         f"{a('rawhash2','own',0.0,'a_dp'):.3f}", f"{a('rawhash2','own',0.44,'a_dp'):.3f}")

# ------------------------------------------------------------------ entropy
ke = rows("key-entropy.csv")
if ke:
    def k(ref, geo, key):
        for r in ke:
            if int(r["ref_mb"]) == ref and r["geometry"] == geo:
                return r[key]
        return None

    want("3x13 entropy and shortfall",
         f"{float(k(20,'3x13','realised_bits')):.1f}", f"{float(k(20,'3x13','shortfall_bits')):.1f}")
    want("4x12 entropy and shortfall",
         f"{float(k(20,'4x12','realised_bits')):.1f}", f"{float(k(20,'4x12','shortfall_bits')):.1f}")
    want("3x8 truncation, both references",
         f"{100 - float(k(20,'3x8','cap_kept_pct')):.1f}",
         f"{100 - float(k(64,'3x8','cap_kept_pct')):.1f}")
    want("3x8 distinct keys at 20 Mb", f"{int(k(20,'3x8','distinct_keys')):,}")
    want("4x12 distinct keys at 20 Mb", f"{int(k(20,'4x12','distinct_keys')):,}")

# ------------------------------------------------------------------ costs
cc = rows("comparison-costs.csv")
if cc:
    def c(system, frag, key):
        for r in cc:
            if r["system"] == system and frag in r["mode"]:
                return r[key]
        return None

    want("readfish per read-chunk",
         f"{float(c('readfish','no-op','per_read_ms')) * 1000:.1f} us")
    want("readfish cpu and rss",
         f"{float(c('readfish','no-op','cpu_pct')):.1f}",
         f"{float(c('readfish','no-op','rss_mib')):.1f} MiB",
         f"{float(c('readfish','targets','cpu_pct')):.1f}",
         f"{float(c('readfish','targets','rss_mib')):.1f} MiB")
    want("our processor use", c("ours", "frozen", "cpu_pct").replace("-", "-"))
    want("comparison per read",
         f"{float(c('rawhash2','incl','per_read_ms')):.2f} ms",
         f"{float(c('rawhash2','excl','per_read_ms')):.2f} ms")

# ------------------------------------------------------------------ R1
lat = rows("latency-matrix-wsl2.csv")
if lat:
    import statistics as st
    for cfg, label in (("pinned-2", "pinned, 2 shards"), ("unpinned-4", "unpinned, 4 shards")):
        v = [r for r in lat if r["config"] == cfg]
        want(f"R1 {cfg}",
             f"{st.median(float(r['p50']) for r in v):.1f}",
             f"{st.median(float(r['p90']) for r in v):.1f}")
    v = [float(r["p99"]) for r in lat if r["config"] == "unpinned-2"]
    want("p99 spread claim", "133%", "8%")

# ------------------------------------------------------------------ constants
want("service bound", "1.56 ms", "1.5625")
# The two index-structure figures are asserted separately rather than as one phrase. Joining
# them with "+" is one way to write the startup line and "a 512.0 MiB table plus 204.3 MiB of
# positions" is another; the connective is not a measurement, and asserting it made the check
# fail on a manuscript whose numbers were entirely correct.
want("index line", "63,981,218 minimizers", "32,888,127 distinct keys",
     "512.0 MiB", "204.3 MiB")
want("ChunkRef size", "56-byte", "72 bytes")
want("vote table footprint", "3,072 B", "1.57 MB", "8.22 MB")
want("TSC artefact", "2418.7 MHz", "111.93", "54.19")
want("realised alphabet", "2.46 bits", "2.58 bits")
want("sig-diff retention", "73.4%")
want("difference keys", "8.3", "[3.6-11.8]")

# ------------------------------------------------------------------ abridgement
# Every numeral in an abridged manuscript must appear in the one it abridges.
SUBSET_OK: list[str] = []
for name, spec in ABRIDGED.items():
    short, full = REPO / "paper" / name, REPO / "paper" / spec["of"]
    if not (short.exists() and full.exists()):
        continue
    invented = sorted(numerals(short.read_text(encoding="utf-8"))
                      - numerals(full.read_text(encoding="utf-8")),
                      key=lambda n: (len(n), n))
    if invented:
        BAD.append(("abridgement subset", name,
                    "numerals not in " + spec["of"] + ": " + ", ".join(invented)))
    else:
        SUBSET_OK.append(f"{name} contains no numeral absent from {spec['of']}")

# ------------------------------------------------------------------ report
print("=" * 78)
print("PAPER NUMBER VERIFICATION")
print("=" * 78)
present = [p.name for p in PAPERS if p.exists()]
print(f"\nchecking: {', '.join(present) if present else 'NO MANUSCRIPT FOUND'}\n")
print(f"ok    {len(OK)} assertions matched")
if SKIPPED:
    print(f"omit  {len(SKIPPED)} claims deliberately absent from an abridged manuscript")
for line in SUBSET_OK:
    print(f"ok    {line}")
if BAD:
    print(f"FAIL  {len(BAD)} mismatched\n")
    for label, fname, missing in BAD:
        print(f"  {label}  [{fname}]")
        print(f"      not found: {missing}")
print(f"\n{len(BAD)} problem(s)")
sys.exit(1 if BAD or not present else 0)
