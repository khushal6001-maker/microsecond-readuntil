#!/bin/bash
# Re-run the six decision rules of Chapter 4 Table 4.2 at the Chapter 6 operating point, in
# TRIPLICATE, and capture the raw output. The 18.8% headline accuracy came from the
# anchor-history row and had, until this run, no captured artefact -- only a transcription
# (Appendix F).
#
# Three seeds rather than one because a single-run figure has already had to be withdrawn
# once in this work (Chapter 8, Table 8.1, row 11). At 500 reads per cell a one-point
# difference is four reads, which a single run cannot distinguish from noise.
#
# Operating point, from the frozen configuration: 4 bits x 12 events, minimizer window 1,
# event detection on with the frozen detector, 10 chunks of 2000 samples, squigulator's
# default dwell dispersion (--dwell-std 4.0 about --dwell-mean 9.0, CV ~0.44), 20 Mb chr20
# indexed on both strands, off-target set a composition-matched shuffle at matching dwell.
#
# squig_sweep positional arguments:
#   1 model  2 ref.fa  3 on.slow5  4 off.slow5  5 chunks  6 detect  7 min_window
#   8 bits   9 events  10 det_thr  11 det_min   12 det_win
#   13 mode (0 votes, 1 greedy chains, 2 anchor-history chains)
#   14 band  15 max_gap  16 lookback  17 max_occ
# Passing 0 for det_thr/det_min/det_win selects the frozen detector values rather than
# overriding them; that sentinel convention is why those three are zero here.
set -u
REPO="/mnt/c/Users/Khushal singh/Desktop/mtech project/microsecond-readuntil"
OUT="$REPO/results/decision-rules-raw"
M=/root/squig_r10_9mer.tsv
D=/root/squigdata
BIN=/root/mru_squig_sweep
CSV="$REPO/results/decision-rules.csv"

mkdir -p "$OUT"
g++ -O2 -std=c++20 -I"$REPO/src" -o "$BIN" "$REPO/bench/squig_sweep.cpp" || exit 1
echo "built with $(g++ --version | head -1)"

# seed 42 is the unsuffixed pair; 43 and 44 carry the suffix.
suffix_for() { case "$1" in 42) echo "";; *) echo "_s$1";; esac; }

run() { # label mode band max_gap lookback max_occ seed
  local label=$1 mode=$2 band=$3 gap=$4 look=$5 occ=$6 seed=$7
  local sfx; sfx=$(suffix_for "$seed")
  local f="$OUT/${label}_s${seed}.log"
  {
    echo "# $(date -Is)"
    echo "# label=$label seed=$seed mode=$mode band=$band max_gap=$gap lookback=$look max_occ=$occ"
    echo "# on=ref_std4.4${sfx}.slow5  off=ref_shuf_std4.4${sfx}.slow5"
    echo "# cmd: mru_squig_sweep <model> <ref> <on> <off> 10 1 1 4 12 0 0 0 $mode $band $gap $look $occ"
    "$BIN" "$M" "$D/ref.fa" "$D/ref_std4.4${sfx}.slow5" "$D/ref_shuf_std4.4${sfx}.slow5" \
           10 1 1 4 12 0 0 0 "$mode" "$band" "$gap" "$look" "$occ"
  } > "$f" 2>&1
}

RULES="votes:0:12:160:48:0
greedy-band0-control:1:0:160:48:0
greedy-band12:1:12:160:48:0
anchor-lookback48:2:12:160:48:0
anchor-lookback128:2:12:160:128:0
anchor-lookback48-occ4:2:12:160:48:4"

printf '\n%-26s %7s %7s %7s   %-16s %s\n' rule s42 s43 s44 "mean [range]" "median on-votes"
for spec in $RULES; do
  IFS=: read -r label mode band gap look occ <<< "$spec"
  vals=()
  meds=()
  for seed in 42 43 44; do
    run "$label" "$mode" "$band" "$gap" "$look" "$occ" "$seed"
    f="$OUT/${label}_s${seed}.log"
    vals+=("$(grep -h '^RESULT' "$f" | sed -E 's/.*zero-FP: >=[0-9]+ TPR ([0-9.]+)%.*/\1/')")
    meds+=("$(grep -h '^on  votes' "$f" | sed -E 's/^on  votes: med ([0-9]+).*/\1/')")
  done
  mean=$(printf '%s\n' "${vals[@]}" | awk '{s+=$1; n++} END{printf "%.1f", s/n}')
  lo=$(printf '%s\n' "${vals[@]}" | sort -g | head -1)
  hi=$(printf '%s\n' "${vals[@]}" | sort -g | tail -1)
  printf '%-26s %7s %7s %7s   %-16s %s\n' \
      "$label" "${vals[0]}" "${vals[1]}" "${vals[2]}" "$mean [$lo-$hi]" "${meds[*]}"
done

echo
echo "raw logs in results/decision-rules-raw/ (18 runs)"
