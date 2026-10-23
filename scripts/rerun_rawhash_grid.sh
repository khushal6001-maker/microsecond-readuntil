#!/bin/bash
# The comparison mapper's dwell x minimizer-window grid, over the same three seeds as
# results/dwell-sweep-ours.csv.
#
# Why: results/rawhash-dwell-sweep.csv, behind Tables 2 and 4 of paper_dwell_journal/ and
# thesis sections 6.5-6.6, is one seed per cell, and the minimizer-penalty claim rests on it.
# Seed 42 is re-run as well as 43 and 44, so every cell comes from one binary and one command
# line, and so the existing single-seed file can be checked against its own re-run.
#
# Same binary, pore model, indexes and parameters as scripts/rerun_rawhash_seeds.sh, with the
# index and -w varied: the indexes in $D were built for this grid on 2026-10-09.
#
# Cells run JOBS at a time. Each mapper process is single-threaded (-t 1) and independent, so
# running several at once changes wall time and nothing else; seed 42 reproducing the existing
# file cell for cell is the check. Control reads never map, so the mapper reads them to the end:
# about 63 s per control file against about 6 s per on-target file.
set -u
REPO="/mnt/c/Users/Khushal singh/Desktop/mtech project/microsecond-readuntil"
OUT="$REPO/results/rawhash-grid-raw"
CSV="$REPO/results/rawhash-grid-seeds.csv"
RH=/root/RawHash/bin/rawhash2
M=/root/squig_r10_9mer.tsv
D=/root/squigdata
SCORE="$REPO/tools/score_rawhash_paf.py"
JOBS=6
# 216 PAF and log files are written inside WSL, whose disk lives on D:. The Windows C: drive
# holding the repository was at 96% when this ran, and a full C: has broken WSL here before.
# Only the CSV and one compressed archive of the raw output are copied into the repository.
WORK=/root/rawhash_grid

mkdir -p "$OUT" "$WORK"
suffix_for() { case "$1" in 42) echo "";; *) echo "_s$1";; esac; }
cv_for() { case "$1" in 0) echo 0.00;; 1) echo 0.10;; 2) echo 0.20;; 3) echo 0.30;; 4.4) echo 0.44;; 6) echo 0.60;; esac; }

run_one() {
  local w=$1 std=$2 seed=$3
  local sfx tag common idx
  sfx=$(suffix_for "$seed")
  tag="w${w}_std${std}_s${seed}"
  idx="$D/ref_w${w}.ind"
  common="-p $M -k 9 --level_column 1 -w $w -q 4 -e 8 --sig-diff 0.35"
  $RH $common -t 1 "$idx" "$D/ref_std${std}${sfx}.slow5"      > "$WORK/on_$tag.paf"  2> "$WORK/on_$tag.log"  || return 1
  $RH $common -t 1 "$idx" "$D/ref_shuf_std${std}${sfx}.slow5" > "$WORK/off_$tag.paf" 2> "$WORK/off_$tag.log" || return 1
  python3 "$SCORE" "$WORK/on_$tag.paf" "$WORK/off_$tag.paf" > "$WORK/score_$tag.txt" || return 1
  printf '%-4s w=%-3s std=%-4s %s\n' "$seed" "$w" "$std" "$(cat "$WORK/score_$tag.txt")"
}
export -f run_one suffix_for
export RH M D WORK SCORE

for w in 0 5 10; do for std in 0 1 2 3 4.4 6; do for seed in 42 43 44; do
  echo "$w $std $seed"
done; done; done | xargs -P "$JOBS" -n 3 bash -c 'run_one "$@"' _ || exit 1

{
  echo "# The comparison mapper's dwell x minimizer-window grid over three seeds. Produced by"
  echo "# scripts/rerun_rawhash_grid.sh; every PAF, log and per-cell score is in"
  echo "# results/rawhash-grid-raw/raw.tar.gz."
  echo "#"
  echo "# RawHash2 v2.1 (-k 9 -q 4 -e 8 --sig-diff 0.35, -t 1) with the squigulator-derived R10.4"
  echo "# 9-mer model, the 20 Mb chr20 reference indexed on both strands at minimizer window w,"
  echo "# and the same SLOW5 files as dwell-sweep-ours.csv. Scored by tools/score_rawhash_paf.py:"
  echo "# correct = mapped within 500 bases of the true locus on the matching strand; false_mapped"
  echo "# = percentage of the composition-matched shuffled control given any mapping."
  echo "#"
  echo "# Seed 42 re-runs rawhash-dwell-sweep.csv, which it must reproduce exactly."
  echo "seed,w,dwell_cv,reads,mapped_pct,correct_pct,off_reads,false_mapped_pct"
  for w in 0 5 10; do for std in 0 1 2 3 4.4 6; do for seed in 42 43 44; do
    s=$(cat "$WORK/score_w${w}_std${std}_s${seed}.txt") || exit 1
    echo "$seed,$w,$(cv_for "$std"),$s"
  done; done; done
} > "$CSV" || exit 1
tar -czf "$OUT/raw.tar.gz" -C "$WORK" . || exit 1
echo
echo "wrote $CSV and $OUT/raw.tar.gz ($(du -h "$OUT/raw.tar.gz" | cut -f1))"
