#!/bin/bash
# Re-run the Appendix F.3 items that a few minutes of compute can settle.
#
#   A. Index footprint at minimizer window 10 against window 1 (App C.1, section 3.6.3)
#   B. Realised key entropy and the 3 x 8 occurrence-cap truncation (7.3.1, 7.10)
#   C. Difference-based key encoding, three seeds (7.6, T8.1 row 10)
#   D. Per-read processor time, this pipeline against the comparison mapper (5.8.2)
#
# Everything here uses benches already in the repo at their committed state; nothing is
# reconstructed. Raw output goes to results/outstanding-raw/ and the figures the chapters
# quote are recomputed from it.
set -u
REPO="/mnt/c/Users/Khushal singh/Desktop/mtech project/microsecond-readuntil"
OUT="$REPO/results/outstanding-raw"
M=/root/squig_r10_9mer.tsv
ICM=/root/icarust/static/dna_r10.4.1_e8.2_400bps/R10_model.tsv
D=/root/squigdata
CHR20=/root/icarust/docker/squiggle_arrs/chr20_plus_wtc11.fa
RH=/root/RawHash/bin/rawhash2

mkdir -p "$OUT"
g++ -O2 -std=c++20 -I"$REPO/src" -o /root/mru_real_reference "$REPO/bench/real_reference.cpp" || exit 1
g++ -O2 -std=c++20 -I"$REPO/src" -o /root/mru_squig_sweep "$REPO/bench/squig_sweep.cpp" || exit 1
echo "built"

# ---------------------------------------------------------------- A and B
# real_reference reports, for a given geometry: minimizers kept of total, distinct keys,
# effective key space against representable, and cap survival. That covers the index
# footprint at both windows and the entropy and truncation claims in one tool.
echo
echo "=============================================================="
echo "A/B  index geometry on real chr20 DNA"
echo "=============================================================="
printf '%-26s %-14s %-14s %s\n' config minimizers "distinct keys" "effective space"
for spec in "w10:4:12:10" "w1:4:12:1" "3x8:3:8:1" "4x12:4:12:1" "3x14:3:14:10"; do
  tag="${spec%%:*}"; rest="${spec#*:}"
  bits="${rest%%:*}"; rest="${rest#*:}"
  ev="${rest%%:*}"; win="${rest##*:}"
  f="$OUT/realref_${tag}_${bits}x${ev}_w${win}.log"
  /root/mru_real_reference --model "$ICM" --fasta "$CHR20" --bases 64000000 \
      --bits "$bits" --events "$ev" --window "$win" > "$f" 2>&1
  mins=$(grep -oE 'minimizers kept *: [0-9]+ of [0-9]+ \([0-9.]+%\)' "$f" | sed 's/.*: //')
  dist=$(grep -oE 'distinct keys *: [0-9]+' "$f" | head -1 | sed 's/.*: //')
  eff=$(grep -oE 'effective space *: [0-9.e+]+' "$f" | sed 's/.*: //')
  printf '%-26s %-14s %-14s %s\n' "${tag} ${bits}x${ev} w${win}" "${mins%% *}" "$dist" "$eff"
done
echo
echo "cap survival and truncation, per geometry:"
for f in "$OUT"/realref_*.log; do
  printf '  %-34s %s\n' "$(basename "$f" .log)" \
      "$(grep -oE 'minimizer occurrences kept : [0-9]+ of [0-9]+ \([0-9.]+%\)' "$f" | sed 's/.*(//;s/)//')"
done

# ---------------------------------------------------------------- C
echo
echo "=============================================================="
echo "C  difference-based key encoding, three seeds"
echo "=============================================================="
suffix_for() { case "$1" in 42) echo "";; *) echo "_s$1";; esac; }
printf '%-20s %8s %8s %8s\n' encoding s42 s43 s44
for mode in "absolute:0" "delta:1"; do
  name="${mode%%:*}"; delta="${mode##*:}"
  vals=()
  for seed in 42 43 44; do
    sfx=$(suffix_for "$seed")
    f="$OUT/deltakeys_${name}_s${seed}.log"
    # argv: model ref on off chunks detect win bits events thr min detwin mode band gap look occ delta
    /root/mru_squig_sweep "$M" "$D/ref.fa" "$D/ref_std4.4${sfx}.slow5" \
        "$D/ref_shuf_std4.4${sfx}.slow5" 10 1 1 4 12 0 0 0 2 12 160 48 0 "$delta" \
        > "$f" 2>&1
    vals+=("$(grep -h '^RESULT' "$f" | sed -E 's/.*zero-FP: >=[0-9]+ TPR ([0-9.]+)%.*/\1/')")
  done
  printf '%-20s %8s %8s %8s\n' "$name" "${vals[0]}" "${vals[1]}" "${vals[2]}"
done

# ---------------------------------------------------------------- D
echo
echo "=============================================================="
echo "D  per-read processor time on the same 500 reads"
echo "=============================================================="
IDX=/root/chr20_w0.ind
if [ ! -f "$IDX" ]; then
  $RH -p "$M" -k 9 --level_column 1 -w 0 -q 4 -e 8 --sig-diff 0.35 \
      -d "$IDX" "$D/ref.fa" > "$OUT/rh_index.log" 2>&1
fi
/usr/bin/time -v $RH -p "$M" -k 9 --level_column 1 -w 0 -q 4 -e 8 --sig-diff 0.35 \
    -t 1 "$IDX" "$D/ref_std4.4.slow5" > /dev/null 2> "$OUT/rh_map_time.log"
rh_u=$(awk -F': ' '/User time/{u=$2} /System time/{s=$2} END{printf "%.3f", u+s}' "$OUT/rh_map_time.log")
echo "comparison mapper: ${rh_u} s of processor time for 500 reads"
awk -v t="$rh_u" 'BEGIN{printf "                   = %.2f ms per read\n", 1000*t/500}'
grep -E 'Maximum resident' "$OUT/rh_map_time.log" | sed 's/^/                   /'
echo
echo "this pipeline is quoted per CHUNK in 5.6 and derived to per read in 5.8.2;"
echo "see the note added there."
echo
echo "raw output in results/outstanding-raw/"
