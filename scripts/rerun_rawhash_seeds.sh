#!/bin/bash
# Map the comparison mapper over the same three seeds as scripts/rerun_decision_rules.sh, at
# squigulator's default dwell dispersion and the mapper's own defaults.
#
# Why: the headline comparison in 1.4, 5.8.2, 7.1 and 8.3 sets this pipeline's accuracy
# against this mapper's. Once this pipeline's figure is a three-seed mean, a single-seed
# figure on the other side is not a like-for-like comparison, and the whole point of the
# comparison is that it is like-for-like.
set -u
REPO="/mnt/c/Users/Khushal singh/Desktop/mtech project/microsecond-readuntil"
OUT="$REPO/results/rawhash-seeds-raw"
RH=/root/RawHash/bin/rawhash2
M=/root/squig_r10_9mer.tsv
D=/root/squigdata
IDX=/root/chr20_w0.ind
SCORE="$REPO/tools/score_rawhash_paf.py"

mkdir -p "$OUT"
COMMON="-p $M -k 9 --level_column 1 -w 0 -q 4 -e 8 --sig-diff 0.35"

if [ ! -f "$IDX" ]; then
  echo "building index at the mapper's own defaults ..."
  $RH $COMMON -d "$IDX" "$D/ref.fa" > "$OUT/index-build.log" 2>&1 || exit 1
fi

suffix_for() { case "$1" in 42) echo "";; *) echo "_s$1";; esac; }

printf '\n%-6s %-10s %9s %9s %9s\n' seed set mapped% correct% false%
for seed in 42 43 44; do
  sfx=$(suffix_for "$seed")
  on="$D/ref_std4.4${sfx}.slow5"
  off="$D/ref_shuf_std4.4${sfx}.slow5"
  $RH $COMMON -t 1 "$IDX" "$on"  > "$OUT/on_s${seed}.paf"  2> "$OUT/on_s${seed}.log"
  $RH $COMMON -t 1 "$IDX" "$off" > "$OUT/off_s${seed}.paf" 2> "$OUT/off_s${seed}.log"
  python3 "$SCORE" "$OUT/on_s${seed}.paf" "$OUT/off_s${seed}.paf" \
      > "$OUT/score_s${seed}.txt" 2>&1
  cat "$OUT/score_s${seed}.txt"
done

echo
echo "raw PAF and scores in results/rawhash-seeds-raw/"
