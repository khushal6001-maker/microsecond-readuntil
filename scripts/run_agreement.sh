#!/bin/bash
set -u
REPO="/mnt/c/Users/Khushal singh/Desktop/mtech project/microsecond-readuntil"
M=/root/squig_r10_9mer.tsv
D=/root/squigdata
AG=/root/mru_symbol_agreement
DU=/root/mru_symbol_dump
OUT=/root/agree

mkdir -p "$OUT"
g++ -O2 -std=c++20 -I"$REPO/src" -o "$DU" "$REPO/bench/symbol_dump.cpp" || exit 1
g++ -O2 -std=c++17 -o "$AG" "$REPO/bench/symbol_agreement.cpp" || exit 1
echo "built"

summarise() { # label logfile
  printf '%-34s' "$1"
  awk '
    /^a_dp     pooled/   {adp=$4}
    /^a_affine pooled/   {aaf=$4}
    /^indel fraction/    {ind=$4}
    /^  E = 8   :/       {if(k8=="")k8=$4}
    /^  E = 12  :/       {if(k12=="")k12=$4}
    /^  E = 8   : [0-9]/ {last8=$4}
    END{printf "a_dp=%-7s a_aff=%-7s indel=%-7s P(key8)=%-9s P(key12)=%-9s\n", adp, aaf, ind, k8, k12}
  ' "$2"
}

# ---- this pipeline, four cells -------------------------------------------------
for cell in "detect:1:std0" "detect:1:std4.4" "fixed:0:std0" "fixed:0:std4.4"; do
  name="${cell%%:*}"; rest="${cell#*:}"; det="${rest%%:*}"; tag="${rest#*:}"
  "$DU" "$M" "$D/ref.fa" "$D/ref_${tag}.slow5" chr20_20Mb "$det" 4 \
      /root/our_ref_${name}.txt /root/our_q_${name}_${tag}.txt 200 0 \
      2> "$OUT/dump_our_${name}_${tag}.log"
  "$AG" /root/our_ref_${name}.txt /root/our_q_${name}_${tag}.txt 20000000 400 200 \
      > "$OUT/our_${name}_${tag}.csv" 2> "$OUT/our_${name}_${tag}.log"
  summarise "ours ${name} CV=${tag}" "$OUT/our_${name}_${tag}.log"
done

# ---- normalisation variant: scale from the first chunk only, as production does -
"$DU" "$M" "$D/ref.fa" "$D/ref_std0.slow5" chr20_20Mb 0 4 \
    /root/our_ref_fixed.txt /root/our_q_fixed1_std0.txt 200 1 \
    2> "$OUT/dump_our_fixed1_std0.log"
"$AG" /root/our_ref_fixed.txt /root/our_q_fixed1_std0.txt 20000000 400 200 \
    > "$OUT/our_fixed1_std0.csv" 2> "$OUT/our_fixed1_std0.log"
summarise "ours fixed CV=std0 chunk-norm" "$OUT/our_fixed1_std0.log"

# ---- RawHash2, two cells -------------------------------------------------------
for tag in std0 std4.4; do
  "$AG" /root/rh_ref_sym.txt /root/rh_q_${tag}.txt 20000000 200 200 \
      > "$OUT/rh_${tag}.csv" 2> "$OUT/rh_${tag}.log"
  summarise "rawhash CV=${tag}" "$OUT/rh_${tag}.log"
done

cp "$OUT"/*.csv "$REPO/results/" 2>/dev/null
echo
echo "=== full logs ==="
for f in "$OUT"/*.log; do
  case "$f" in *dump*) continue;; esac
  echo "---------------- $f"; cat "$f"
done
