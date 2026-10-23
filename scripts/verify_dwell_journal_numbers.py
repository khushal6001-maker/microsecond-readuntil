#!/usr/bin/env python3
"""Re-derive every number in paper_dwell_journal/ from results/, and refuse any other.

verify_thesis_numbers.py checks the claims someone remembered to transcribe into it. This
script is stricter, because a journal manuscript has no [VERIFY] markers to fall back on: it
reads both manuscript.md and manuscript.tex and requires every numeral in either to be
accounted for. A numeral is accepted only if it is

  BACKED       recomputed here from a committed file in results/, with decimal arithmetic
               and round-half-up, so 0.0915 prints as 0.092 rather than the 0.091 that a
               binary float gives;
  STRUCTURAL   a method parameter (a seed, a CV level, a read count), each of which is
               checked below against the log or file header that records it; or
  OUTSTANDING  a value no captured run backs, allowed only inside the provisional region
               of Section 3.5 and only when marked with a dagger.

On top of that, every row of Tables 1-4 is rebuilt from data and must appear verbatim in
both files, the Abstract's headline sentences must appear verbatim in both, and the two
files must carry the same set of data-derived numerals. A ratio written as a word
("sevenfold") is refused, because the gate cannot check it.

    python scripts/verify_dwell_journal_numbers.py
    python scripts/verify_dwell_journal_numbers.py --md COPY.md --tex COPY.tex   # test the gate
"""

from __future__ import annotations

import argparse
import csv
import re
import subprocess
import sys
from decimal import ROUND_HALF_UP, Decimal, getcontext
from pathlib import Path

getcontext().prec = 28
# The report prints section signs and en dashes; a Windows console defaults to cp1252.
sys.stdout.reconfigure(encoding="utf-8")

REPO = Path(__file__).resolve().parent.parent
RESULTS = REPO / "results"
PAPER = REPO / "paper_dwell_journal"

_ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
_ap.add_argument("--md", type=Path, default=PAPER / "manuscript.md")
_ap.add_argument("--tex", type=Path, default=PAPER / "manuscript.tex")
_args = _ap.parse_args()
MD, TEX = _args.md.resolve(), _args.tex.resolve()

PASS: list[tuple[str, str]] = []
FAIL: list[tuple[str, str]] = []


def ok(what: str, where: str = "") -> None:
    PASS.append((what, where))


def bad(what: str, why: str) -> None:
    FAIL.append((what, why))


def rows(name: str) -> list[dict]:
    """A results CSV, skipping the leading '#' provenance commentary."""
    p = RESULTS / name
    if not p.exists():
        bad(f"data file {name}", "missing")
        return []
    with open(p, newline="", encoding="utf-8") as f:
        lines = [ln for ln in f if not ln.lstrip().startswith("#")]
    return list(csv.DictReader(lines))


def rel(path: Path) -> str:
    try:
        return str(path.relative_to(REPO))
    except ValueError:
        return str(path)


def text_of(path: Path) -> str:
    if not path.exists():
        bad(f"file {rel(path)}", "missing")
        return ""
    return path.read_text(encoding="utf-8")


def D(s) -> Decimal:
    return Decimal(str(s).strip())


def q(x: Decimal, places: str) -> str:
    return str(x.quantize(Decimal(places), rounding=ROUND_HALF_UP))


# ---------------------------------------------------------------- the registries
BACKED: dict[str, list[str]] = {}
CLAIMED: list[str] = []          # hand-transcribed values that must appear in both files


def backed(tok: str, why: str) -> None:
    BACKED.setdefault(tok, []).append(why)


def claim(written: str, value: Decimal, places: str, why: str) -> None:
    """The prose says `written`; the data must say the same at that precision."""
    got = q(value, places)
    if got == written:
        backed(written, why)
        CLAIMED.append(written)
        ok(f"{why} = {written}")
    else:
        bad(why, f"data gives {got}, prose says {written}")


def needles(path: Path, wanted: list[str], why: str) -> None:
    """Provenance: each string must appear verbatim in the file that records it."""
    body = text_of(path)
    missing = [n for n in wanted if n not in body]
    if missing:
        bad(why, f"not in {rel(path)}: {missing}")
    else:
        ok(why, rel(path))


CVS = ["0.00", "0.10", "0.20", "0.30", "0.44", "0.60"]

# ---------------------------------------------------------------- Table 1: this pipeline
ours = {r["dwell_cv"]: r for r in rows("dwell-sweep-ours.csv")}
COLS = ["fixed_mean", "fixed_lo", "fixed_hi", "detected_mean", "detected_lo", "detected_hi"]
TABLE1 = []
if ours:
    for cv in CVS:
        r = ours[cv]
        for c in COLS:
            backed(q(D(r[c]), "0.1"), f"dwell-sweep-ours CV {cv} {c}")
        v = {c: q(D(r[c]), "0.1") for c in COLS}
        TABLE1.append([
            cv,
            f"{v['fixed_mean']} [{v['fixed_lo']}–{v['fixed_hi']}]",
            f"{v['detected_mean']} [{v['detected_lo']}–{v['detected_hi']}]",
        ])

    fm = lambda cv: D(ours[cv]["fixed_mean"])
    dm = lambda cv: D(ours[cv]["detected_mean"])
    claim("2.7", fm("0.00") / dm("0.00"), "0.1", "fixed-width lead at CV 0 (fold), §3.1")
    claim("10.8", dm("0.44") / fm("0.44"), "0.1", "fixed-width deficit at CV 0.44 (fold), Abstract")
    claim("38.2", fm("0.00") / fm("0.44"), "0.1", "fixed-width fall CV 0 to 0.44 (fold), §3.1")
    claim("8.4", fm("0.00") / fm("0.10"), "0.1", "fixed-width fall CV 0 to 0.10 (fold), §3.1")
    upto = [dm(cv) for cv in CVS if D(cv) <= D("0.44")]
    claim("14.1", min(upto), "0.1", "detected minimum up to CV 0.44, §3.1")
    claim("19.3", max(upto), "0.1", "detected maximum up to CV 0.44, §3.1")

    # "the replicate ranges do not overlap at any dispersion tested" -- a logical claim.
    # At CV 0 fixed-width leads, so its LOW must exceed detection's HIGH; elsewhere the
    # reverse. Disjoint ranges are what licence "in every seed, not only in the means".
    clash = []
    for cv in CVS:
        r = ours[cv]
        if cv == "0.00":
            if not D(r["fixed_lo"]) > D(r["detected_hi"]):
                clash.append(cv)
        elif not D(r["fixed_hi"]) < D(r["detected_lo"]):
            clash.append(cv)
    if clash:
        bad("replicate ranges disjoint at every CV (Abstract, §3.1)", f"overlap at {clash}")
    else:
        ok("replicate ranges disjoint at every CV (Abstract, §3.1)")
    if fm("0.00") > dm("0.00") and fm("0.10") < dm("0.10"):
        ok("order exchanges between CV 0 and CV 0.10 (§3.1)")
    else:
        bad("order exchanges between CV 0 and CV 0.10 (§3.1)", "it does not")

# ---------------------------------------------------------------- Table 2: RawHash2 grid
rh = {(r["w"], r["dwell_cv"]): r for r in rows("rawhash-dwell-sweep.csv")}
TABLE2, TABLE4 = [], []
if rh:
    corr = lambda w, cv: D(rh[(w, cv)]["correct_pct"])
    fals = lambda w, cv: D(rh[(w, cv)]["false_mapped_pct"])
    for w in ("0", "5", "10"):
        TABLE2.append(["Correct", w] + [q(corr(w, cv), "0.1") for cv in CVS])
    for w in ("0", "5", "10"):
        TABLE2.append(["False-mapped", w] + [q(fals(w, cv), "0.1") for cv in CVS])
    for row in TABLE2:
        for cell in row[2:]:
            backed(cell, f"rawhash-dwell-sweep {row[0]} w={row[1]}")

    pen = lambda w, cv: corr("0", cv) - corr(w, cv)
    for w in ("5", "10"):
        cells = [q(pen(w, cv), "0.1") for cv in CVS]
        TABLE4.append([f"w = {w}"] + cells)
        for c in cells:
            backed(c, f"sketching penalty w={w}")

    claim("3.0", corr("0", "0.00") - corr("0", "0.60"), "0.1", "RawHash2 w=0 loss over sweep (points), Abstract")
    claim("8.4", corr("5", "0.00") - corr("5", "0.60"), "0.1", "RawHash2 w=5 loss over sweep (points), §3.2")
    claim("11.2", corr("10", "0.00") - corr("10", "0.60"), "0.1", "RawHash2 w=10 loss over sweep (points), §3.2")
    claim("12.8", pen("10", "0.00"), "0.1", "w=10 penalty at CV 0, Abstract")
    claim("21.0", pen("10", "0.60"), "0.1", "w=10 penalty at CV 0.60, Abstract")
    claim("8.2", pen("10", "0.60") - pen("10", "0.00"), "0.1", "w=10 penalty growth (points), §3.3")
    claim("64", (pen("10", "0.60") / pen("10", "0.00") - 1) * 100, "1", "w=10 penalty growth (%), §3.3")
    claim("0.6", pen("10", "0.10") - pen("10", "0.20"), "0.1", "w=10 penalty dip at CV 0.20, §3.3")
    claim("5.0", pen("5", "0.00"), "0.1", "w=5 penalty at CV 0, §3.3")
    claim("10.4", pen("5", "0.60"), "0.1", "w=5 penalty at CV 0.60, §3.3")
    claim("8.4", fals("0", "0.60"), "0.1", "false-mapped w=0 at CV 0.60, Abstract")
    claim("15.2", fals("10", "0.60"), "0.1", "false-mapped w=10 at CV 0.60, Abstract")
    claim("95.0", corr("0", "0.00"), "0.1", "RawHash2 w=0 correct at CV 0, §3.2")
    claim("92.0", corr("0", "0.60"), "0.1", "RawHash2 w=0 correct at CV 0.60, §3.2")

# ---------------------------------------------------------------- RawHash2, three seeds
seeds = {r["seed"]: r for r in rows("rawhash-seeds.csv")}
if seeds:
    per_c = [D(seeds[s]["correct_pct"]) for s in ("42", "43", "44")]
    per_f = [D(seeds[s]["false_mapped_pct"]) for s in ("42", "43", "44")]
    claim("91.8", sum(per_c) / 3, "0.1", "RawHash2 default correct, 3-seed mean, §2.5")
    claim("91.2", min(per_c), "0.1", "RawHash2 default correct, 3-seed min, §2.5")
    claim("92.4", max(per_c), "0.1", "RawHash2 default correct, 3-seed max, §2.5")
    claim("1.2", max(per_c) - min(per_c), "0.1", "RawHash2 default 3-seed range width, §3.3")
    claim("9.1", sum(per_f) / 3, "0.1", "RawHash2 default false-mapped, 3-seed mean, §2.5")
    claim("8.4", min(per_f), "0.1", "RawHash2 default false-mapped, 3-seed min, §2.5")
    claim("10.6", max(per_f), "0.1", "RawHash2 default false-mapped, 3-seed max, §2.5")
    for col, vals in (("correct_pct", per_c), ("false_mapped_pct", per_f)):
        if q(sum(vals) / 3, "0.1") != q(D(seeds["mean"][col]), "0.1"):
            bad(f"rawhash-seeds.csv mean row ({col})", "does not equal the mean of its seeds")
        else:
            ok(f"rawhash-seeds.csv mean row ({col}) equals mean of seeds")

    # The three seeds are at CV 0.44. Seed 42's captured score must reproduce both its row
    # here and the grid's w=0, CV 0.44 cell, which is what ties the grid to the replicates.
    sc = text_of(RESULTS / "rawhash-seeds-raw" / "score_s42.txt").strip().split(",")
    r42 = seeds["42"]
    want = [r42["reads"], r42["mapped_pct"], r42["correct_pct"], r42["off_reads"], r42["false_mapped_pct"]]
    if sc != want:
        bad("score_s42.txt matches rawhash-seeds.csv seed 42", f"{sc} vs {want}")
    else:
        ok("score_s42.txt matches rawhash-seeds.csv seed 42", "captured run")
    if rh:
        cell = rh[("0", "0.44")]
        grid = [cell["mapped_pct"], cell["correct_pct"], cell["false_mapped_pct"]]
        if [r42["mapped_pct"], r42["correct_pct"], r42["false_mapped_pct"]] != grid:
            bad("seed 42 reproduces the grid's w=0, CV 0.44 cell", f"{grid}")
        else:
            ok("seed 42 reproduces the grid's w=0, CV 0.44 cell")

# ---------------------------------------------------------------- Table 3: agreement
ag = rows("agreement-summary.csv")
TABLE3 = []
if ag:
    def arow(system, seg, cv, norm):
        hit = [r for r in ag if (r["system"], r["segmentation"], r["dwell_cv"], r["norm"]) == (system, seg, cv, norm)]
        if len(hit) != 1:
            bad(f"agreement row {system}/{seg}/{cv}/{norm}", f"{len(hit)} matches")
            return None
        return hit[0]

    for label, seg_label, system, seg, norm in (
        ("This pipeline", "fixed-width", "ours", "fixed", "read"),
        ("This pipeline", "detected", "ours", "detected", "read"),
        ("RawHash2", "own, with merging", "rawhash2", "own", "own"),
    ):
        for cv in ("0.00", "0.44"):
            r = arow(system, seg, cv, norm)
            if r is None:
                continue
            a, g = q(D(r["a_dp"]), "0.001"), q(D(r["indel_frac"]), "0.001")
            backed(a, f"agreement {system}/{seg} CV {cv}")
            backed(g, f"indel fraction {system}/{seg} CV {cv}")
            TABLE3.append([label, seg_label, cv, a, g])
            if r["reads"] != "200":
                bad(f"agreement {system}/{seg} CV {cv} read count", f"{r['reads']}, prose says 200")
            if system == "ours" and (r["bits"], r["events_per_key"]) != ("4", "12"):
                bad(f"agreement {system}/{seg} geometry", "§2.6 says 12 events at 4 bits")
    fx0, fx44 = arow("ours", "fixed", "0.00", "read"), arow("ours", "fixed", "0.44", "read")
    if fx0 and fx44:
        claim("2.8", D(fx44["indel_frac"]) / D(fx0["indel_frac"]), "0.1",
              "fixed-width indel fraction rise (fold), §3.2")

# ---------------------------------------------------------------- calibration, backed half
LOG = RESULTS / "decision-rules-raw" / "votes_s42.log"
log = text_of(LOG)
m = re.search(r"events/read on (\d+) off (\d+) \(ideal ~(\d+) at (\d+) samples/base\)", log)
t = re.search(r"RESULT\s+zero-FP: >=\d+ TPR ([\d.]+)%", log)
if m and t:
    on_ev, ideal = D(m.group(1)), D(m.group(3))
    claim("1562", on_ev, "1", "on-target events per read, frozen detector, §3.5")
    claim("21.9", (ideal - on_ev) / ideal * 100, "0.1", "under-segmentation (%), §3.5")
    claim("14.2", D(t.group(1)), "0.1", "seed 42 TPR at CV 0.44, captured log, §3.5")
    if ours and not (D(ours["0.44"]["detected_lo"]) <= D(t.group(1)) <= D(ours["0.44"]["detected_hi"])):
        bad("seed 42 TPR lies within Table 1's CV 0.44 range", t.group(1))
    else:
        ok("seed 42 TPR lies within Table 1's CV 0.44 detected range")
else:
    bad("calibration log parse", f"{rel(LOG)} lacks the events/read or RESULT line")

claim("0.44", D("4.0") / D("9.0"), "0.01", "Squigulator default CV from 4.0/9.0, §1")

# ---------------------------------------------------------------- provenance of parameters
needles(RESULTS / "dwell-sweep-ours.csv",
        ["Three independent seeds", "(42/43/44), 500 reads per cell", "10 chunks of 2000 samples",
         "0.4 s at the dna-r10-prom", "5 kHz", "20 Mb chr20 indexed on both strands",
         "composition-matched shuffle", "3 x 14 geometry, minimizer window 10, fixed-width",
         "4 x 12 geometry, minimizer window 1, two-window t-test"],
        "§2.1-2.4 sweep parameters")
needles(LOG,
        ["seed=42", "on=ref_std4.4.slow5", "ref 40000000 bases (both strands)", "9-mer model",
         "geometry 4 x 12", "window 1, EVENT-DETECTED", "10 chunks of 2000 samples",
         "ideal ~2000 at 10 samples/base"],
        "§2.1-2.4 parameters in the captured log")
needles(RESULTS / "decision-rules.csv",
        ["threshold 3.0, min_len 4, window 4", "--dwell-mean 9.0 --dwell-std 4.0, CV ~0.44"],
        "§1, §2.4 detector parameters and Squigulator default")
needles(RESULTS / "rawhash-seeds.csv",
        ["-q 4 -e 8 -w 0 --sig-diff 0.35", "within 500 bases", "matching strand",
         "same three seeds as decision-rules.csv"],
        "§2.5 RawHash2 parameters and scoring")
needles(RESULTS / "rawhash-seeds-raw" / "on_s42.log",
        ["Version: 2.1", "pore kmer size: 9"], "§2.5 RawHash2 version and k")
needles(RESULTS / "agreement-summary.csv",
        ["200 reads per cell", "--sig-diff merging", "--out-quantize-ref",
         "matches / aligned pairs under the maximising alignment",
         "gap columns as a fraction of alignment columns", "from all samples of the read"],
        "§2.6 agreement estimator")
needles(REPO / "third_party_patches" / "icarust-dwell-variability.patch",
        ["ICARUST_DWELL_CV", "LogNormal", "(1.0 + dwell_cv * dwell_cv).ln()", "- 0.5 * sigma2",
         "samples_per_base * 8", "clamp(1.0, hi)", "This could also be worked out from profile.dwell_mean",
         "Unset or 0 reproduces the original fixed-count behaviour exactly"],
        "§3.4 Icarust patch behaviour")
needles(REPO / "third_party_patches" / "rawhash2-reference-quantise-dump.patch",
        ["out-quantize-ref"], "§2.6 RawHash2 reference-side dump")

# Method parameters and their provenance, above. A numeral here is accepted anywhere.
STRUCTURAL = {
    "0", "1", "2", "3", "4", "5", "6", "8", "9", "10", "12", "14", "20", "40",
    "42", "43", "44", "200", "500", "2000",
    *CVS, "4.4", "10.0", "9.0", "4.0", "0.4", "0.35", "3.0",
    "136119",                                    # postcode in the affiliation
}
YEARS = {"2004", "2021", "2023", "2024", "2025", "2026"}   # author-year citations, md only

# ---------------------------------------------------------------- outstanding values
# Each needs a captured run that does not exist. They are allowed only inside the
# provisional region and only with a dagger. Where a transcription exists it is checked.
OUTSTANDING: list[tuple[str, str, str]] = []


def outstanding(key: str, why: str, needs: str) -> None:
    OUTSTANDING.append((key, why, needs))


ica = {r["icarust"]: r for r in rows("icarust-stock-vs-patched.csv")}
if ica:
    st, pa = ica["stock"], ica["patched"]
    if (st["fixed_accept_pct"], st["detected_accept_pct"], pa["fixed_accept_pct"],
            pa["detected_accept_pct"], pa["dwell_cv"]) == ("43.5", "9.6", "0.5", "9.6", "0.35"):
        ok("Icarust values transcribed as icarust-stock-vs-patched.csv records them")
    else:
        bad("Icarust transcription", "text and CSV disagree")
    ratio = q(D(st["fixed_accept_pct"]) / D(st["detected_accept_pct"]), "0.1")
    if ratio != "4.5":
        bad("Icarust stock ratio", f"CSV gives {ratio}, prose says 4.5")
needs_ica = ("a re-run of stock and patched Icarust against the client; thesis Appendix F.3 "
             "item 4 records that the CSV is a transcription, not a captured run")
for k in ("43.5%", "9.6%", "0.5%", "4.5"):
    outstanding(k, "Icarust live accept rates, §3.5", needs_ica)
needles(REPO / "docs" / "appendices-draft.md",
        ["The Icarust accept rates behind Table 6.2", "not from a captured run"],
        "thesis F.3 records the Icarust values as outstanding")

APP = REPO / "docs" / "appendices-draft.md"
needles(APP, ["| threshold 2.0, `min_len` 4 | 2017 (ideal 2000) | 2.0% |"],
        "threshold-2.0 values as the thesis records them")
needs_cal = "a captured run of the threshold-2.0 detector at CV 0.44"
for k in ("2017", "2.0%"):
    outstanding(k, "threshold-2.0 development run, §3.5", needs_cal)
if t:
    r71 = q(D(t.group(1)) / D("2.0"), "0.1")
    if r71 != "7.1":
        bad("calibration ratio", f"14.2/2.0 gives {r71}, prose says 7.1")
outstanding("7.1", "ratio on the threshold-2.0 run, §3.5", needs_cal)

needles(APP, ["it scored 0.6% TPR at dwell CV 0 on Squigulator"],
        "cross-generator value as the thesis records it")
needs_x = "a captured run of the own-generator calibration on Squigulator signal at CV 0"
outstanding("0.6%", "cross-generator calibration, §3.5", needs_x)
if ours:
    r307 = q(D(ours["0.00"]["detected_mean"]) / D("0.6"), "0.1")
    if r307 != "30.7":
        bad("cross-generator ratio", f"18.4/0.6 gives {r307}, prose says 30.7")
outstanding("30.7", "ratio on the cross-generator run, §3.5", needs_x)
OUT_KEYS = {k for k, _, _ in OUTSTANDING}

# ---------------------------------------------------------------- reading the manuscripts
NUM = re.compile(r"(?<![\w.])(\d+(?:\.\d+)?)(%?)", re.ASCII)
WORD_RATIO = re.compile(
    r"\b(?:two|three|four|five|six|seven|eight|nine|ten|eleven|twelve|twenty|thirty|forty|"
    r"fifty|hundred)[- ]?(?:fold|times)\b", re.IGNORECASE)
REFS = re.compile(r"\b(?:Sections?|Tables?|Figures?)\s+\d+(?:\.\d+)?(?:\s+(?:to|and)\s+\d+(?:\.\d+)?)?")


def collapse(s: str) -> str:
    return re.sub(r"\s+", " ", s)


def split_regions(raw: str, begin: str, end: str) -> list[tuple[str, bool]]:
    out, prov, pos = [], False, 0
    for mm in re.finditer(re.escape(begin) + "|" + re.escape(end), raw):
        out.append((raw[pos:mm.start()], prov))
        prov = mm.group(0) == begin
        pos = mm.end()
    out.append((raw[pos:], prov))
    return out


def md_segments(raw: str) -> list[tuple[str, bool]]:
    raw = raw.split("\n## References")[0]
    segs = []
    for seg, prov in split_regions(raw, "<!-- provisional:begin -->", "<!-- provisional:end -->"):
        seg = re.sub(r"<!--.*?-->", "", seg, flags=re.S)
        segs.append((collapse(seg), prov))
    return segs


def md_scan(s: str) -> str:
    s = re.sub(r"!\[[^\]]*\]\([^)]*\)", "", s)
    s = re.sub(r"https?://\S+", "", s)
    s = re.sub(r"\S+@\S+", "", s)
    s = re.sub(r"(^| )#+ \d+(?:\.\d+)* ", r"\1", s)
    s = re.sub(r"(^| )\d+\. (?=\S)", r"\1", s)
    return REFS.sub("", s)


def tex_segments(raw: str) -> list[tuple[str, bool]]:
    body = raw.split(r"\begin{document}", 1)[-1].split(r"\bibliographystyle", 1)[0]
    segs = []
    for seg, prov in split_regions(body, "% provisional:begin", "% provisional:end"):
        seg = re.sub(r"(?<!\\)%.*", "", seg)
        seg = seg.replace(r"\prov{}", "†").replace(r"\prov", "†").replace(r"\%", "%")
        seg = seg.replace("---", "—").replace("--", "–").replace("~", " ")
        seg = seg.replace("$", "").replace(r"\_", "_")
        # Whole control words only: a bare replace of \in would also eat \includegraphics.
        seg = re.sub(r"\\times(?![A-Za-z])", "×", seg)
        seg = re.sub(r"\\in(?![A-Za-z])", "∈", seg)
        segs.append((collapse(seg), prov))
    return segs


def tex_scan(s: str) -> str:
    for pat in (r"\\(?:label|ref|citep?|citet|url|includegraphics(?:\[[^\]]*\])?)\{[^}]*\}",
                r"\\begin\{tabular\}\{[^}]*\}", r"\\multicolumn\{\d+\}", r"\\cmidrule\([^)]*\)\{[^}]*\}",
                r"\\rule\{[^}]*\}\{[^}]*\}", r"[\d.]+\\(?:line|text|column)width"):
        s = re.sub(pat, "", s)
    return s


md_raw, tex_raw = text_of(MD), text_of(TEX)
FILES = {
    "manuscript.md": [(md_scan(s), p, s) for s, p in md_segments(md_raw)],
    "manuscript.tex": [(tex_scan(s), p, s) for s, p in tex_segments(tex_raw)],
}
NORM = {name: " ".join(full for _, _, full in segs) for name, segs in FILES.items()}

# ---- tables, cell by cell from data
for label, table in (("Table 1", TABLE1), ("Table 2", TABLE2), ("Table 3", TABLE3), ("Table 4", TABLE4)):
    for row in table:
        md_row = "| " + " | ".join(row) + " |"
        tex_row = " & ".join(row) + r" \\"
        for name, want in (("manuscript.md", md_row), ("manuscript.tex", tex_row)):
            if want in NORM[name]:
                ok(f"{label} row {' | '.join(row[:3])}", name)
            else:
                bad(f"{label} row in {name}", f"expected '{want}'")

# ---- Abstract headline sentences, verbatim in both
ANCHORS = [
    "49.7% against 18.4% true-positive rate at zero false positives",
    "trails it 10.8-fold at the simulator's default CV of 0.44, 1.3% against 14.1%",
    "the replicate ranges do not overlap at any dispersion tested",
    "loses 3.0 points at its default setting",
    "is 12.8 points at CV 0 and 21.0 at CV 0.60",
    "false-mapping rate on the control from 8.4% to 15.2%",
]
for a in ANCHORS:
    for name in FILES:
        if a in NORM[name]:
            ok(f"Abstract anchor in {name}: {a[:40]}…")
        else:
            bad(f"Abstract anchor in {name}", f"missing '{a}'")

# ---- every numeral accounted for
DATA_TOKENS: dict[str, set[str]] = {}
for name, segs in FILES.items():
    seen: set[str] = set()
    before = len(FAIL)
    for scan, prov, _ in segs:
        for wm in WORD_RATIO.finditer(scan):
            bad(f"ratio written as a word in {name}", f"'{wm.group(0)}': write it as digits")
        for mm in NUM.finditer(scan):
            num, pct = mm.group(1), mm.group(2)
            key = num + pct
            ctx = scan[max(0, mm.start() - 30):mm.end() + 20]
            if key in OUT_KEYS:
                after = scan[mm.end():mm.end() + 8]
                if not prov:
                    bad(f"outstanding value {key} outside §3.5 in {name}", f"…{ctx}…")
                elif not re.match(r"(?:-fold)?\s*†", after):
                    bad(f"outstanding value {key} not marked † in {name}", f"…{ctx}…")
                seen.add(key)
            elif num in BACKED:
                seen.add(key)
            elif num in STRUCTURAL or (name.endswith(".md") and num in YEARS):
                pass
            else:
                bad(f"unregistered numeral {key} in {name}", f"…{ctx}…")
    DATA_TOKENS[name] = seen
    if len(FAIL) == before:
        ok(f"every numeral in {name} is backed, structural or marked outstanding")

# ---- the two files carry the same data-derived numerals
only_md = DATA_TOKENS["manuscript.md"] - DATA_TOKENS["manuscript.tex"]
only_tex = DATA_TOKENS["manuscript.tex"] - DATA_TOKENS["manuscript.md"]
if only_md or only_tex:
    bad("md and tex carry the same data-derived numerals",
        f"only in md: {sorted(only_md)}; only in tex: {sorted(only_tex)}")
else:
    ok("md and tex carry the same data-derived numerals", f"{len(DATA_TOKENS['manuscript.md'])} values")
for c in sorted(set(CLAIMED)):
    for name in FILES:
        if not any(t.rstrip("%") == c for t in DATA_TOKENS[name]):
            bad(f"claimed value {c} absent from {name}", "the script checks a number the text no longer uses")
for k in sorted(OUT_KEYS):
    for name in FILES:
        if k not in DATA_TOKENS[name]:
            bad(f"outstanding value {k} absent from {name}", "drop it from OUTSTANDING or restore it")

# ---------------------------------------------------------------- figures and references
for fig in ("fig6-1-dwell-crossover", "fig6-2-rawhash-dwell", "fig6-3-minimizer-penalty"):
    pdf, png = PAPER / "figures" / f"{fig}.pdf", PAPER / "figures" / f"{fig}.png"
    if not (pdf.exists() and png.exists()):
        bad(f"figure {fig}", "pdf or png missing; run generate_thesis_figures.py --no-titles")
        continue
    src = RESULTS / ("dwell-sweep-ours.csv" if fig.startswith("fig6-1") else "rawhash-dwell-sweep.csv")
    if pdf.stat().st_mtime < src.stat().st_mtime:
        bad(f"figure {fig}", f"older than {src.name}; regenerate")
    try:
        from pypdf import PdfReader
        words = "".join(p.extract_text() or "" for p in PdfReader(pdf).pages)
        if "Figure 6." in words:
            bad(f"figure {fig}", "carries a figure-level title; regenerate with --no-titles")
        else:
            ok(f"figure {fig} exists, is current and is untitled")
    except ImportError:
        ok(f"figure {fig} exists and is current (pypdf absent: title not checked)")
    if f"figures/{fig}.png" not in md_raw or f"figures/{fig}.pdf" not in tex_raw:
        bad(f"figure {fig}", "not referenced by both manuscripts")

try:
    dirty = subprocess.run(["git", "status", "--porcelain", "--", "figures"], cwd=REPO,
                           capture_output=True, text=True, check=True).stdout.strip()
    (bad("thesis figures/ untouched", dirty) if dirty else ok("thesis figures/ untouched", "git status"))
except Exception as exc:
    bad("thesis figures/ untouched", f"git failed: {exc}")

bib = text_of(PAPER / "refs.bib")
keys = set(re.findall(r"@\w+\{([^,]+),", bib))
cited = {k.strip() for grp in re.findall(r"\\cite[pt]?\{([^}]*)\}", tex_raw) for k in grp.split(",")}
if cited - keys:
    bad("every \\cite key is in refs.bib", f"missing {sorted(cited - keys)}")
else:
    ok("every \\cite key is in refs.bib", f"{len(cited)} cited")
if keys - cited:
    bad("every refs.bib entry is cited", f"uncited {sorted(keys - cited)}")

# ---------------------------------------------------------------- report
W = 66
print("=" * 78)
print("DWELL JOURNAL MANUSCRIPT: NUMBER VERIFICATION")
print("=" * 78)
print(f"\nAGREE  ({len(PASS)})")
for what, where in PASS:
    print(f"  ok   {what[:W]:<{W}} {where}")
if FAIL:
    print(f"\nDISAGREE  ({len(FAIL)})   <-- manuscript and data do not match")
    for what, why in FAIL:
        print(f"  FAIL {what}")
        print(f"       {why}")

print("\n" + "=" * 78)
print("OUTSTANDING: values in §3.5 that no captured run backs")
print("=" * 78)
for key, why, needs in OUTSTANDING:
    print(f"\n  {key:<7} {why}")
    print(f"          needs: {needs}")

print(f"\nsummary: {len(PASS)} agree, {len(FAIL)} disagree, {len(OUTSTANDING)} outstanding, "
      f"{len(BACKED)} distinct backed values")
sys.exit(1 if FAIL else 0)
