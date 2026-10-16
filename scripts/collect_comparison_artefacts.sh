#!/bin/bash
# Collect the surviving artefacts behind the Chapter 5 section 5.8 comparison into results/.
#
# Appendix F listed the readfish orchestration cost and this system's processor use as
# unverifiable. They are not: the original /usr/bin/time -v output and readfish's own progress
# logs survived in /root/rf and /root/mru/results, and between them they carry every figure
# section 5.8.1 quotes. This copies them in and recomputes the quoted numbers from them.
#
# readfish reports progress as "Avg: NNNNR/S.SSSSs", meaning NNNN reads handled in S.SSSS
# seconds of batch processing. Per-read orchestration cost is therefore S.SSSS / NNNN, which
# is where the 124 us comes from -- it is not a figure this work measured with its own
# instrument, and 5.8.1 should say so.
set -u
REPO="/mnt/c/Users/Khushal singh/Desktop/mtech project/microsecond-readuntil"
OUT="$REPO/results/comparison-raw"
mkdir -p "$OUT"

cp -f /root/rf/*.time /root/rf/targets.log /root/rf/unblock-all.log "$OUT"/ 2>/dev/null
for f in /root/mru/results/*.time; do
  cp -f "$f" "$OUT/mru_$(basename "$f")" 2>/dev/null
done

cpu_pct() {  # user+sys over elapsed, to one decimal, from a /usr/bin/time -v file
  awk -F': ' '
    /User time/    {u=$2}
    /System time/  {s=$2}
    /Elapsed .wall/{split($2,a,":"); e=(length(a)==3)?a[1]*3600+a[2]*60+a[3]:a[1]*60+a[2]}
    END {if (e>0) printf "%.1f", 100*(u+s)/e; else printf "n/a"}
  ' "$1"
}
rss_mib() { awk -F': ' '/Maximum resident/{printf "%.1f", $2/1024}' "$1"; }

echo "=============================================================="
echo "readfish, against the same simulator on the same host"
echo "=============================================================="
for tag in unblock-all targets; do
  f="$OUT/${tag}.time"
  [ -f "$f" ] || continue
  avg=$(grep -oE 'Avg: [0-9]+R/[0-9.]+s' "$OUT/${tag}.log" | tail -1)
  reads=$(echo "$avg" | sed -E 's#Avg: 0*([0-9]+)R/.*#\1#')
  secs=$(echo "$avg" | sed -E 's#.*/([0-9.]+)s#\1#')
  per_us=$(awk -v r="$reads" -v s="$secs" 'BEGIN{if(r>0) printf "%.1f", 1e6*s/r}')
  printf '%-14s cpu %5s%%   rss %8s MiB   last %-22s -> %s us/read\n' \
      "$tag" "$(cpu_pct "$f")" "$(rss_mib "$f")" "$avg" "$per_us"
done

echo
echo "=============================================================="
echo "this system, same rig: processor use across the 5.8 runs"
echo "=============================================================="
for f in "$OUT"/mru_*.time; do
  [ -f "$f" ] || continue
  ref=$(grep -oE 'squiggle_arrs/[^ ]+' "$f" | head -1 | sed 's#.*/##')
  flags=$(grep -oE '\-\-(detect|min-window [0-9]+|bits [0-9]+|events [0-9]+)' "$f" | tr '\n' ' ')
  printf '%-34s cpu %5s%%   rss %9s MiB   %s %s\n' \
      "$(basename "$f" .time)" "$(cpu_pct "$f")" "$(rss_mib "$f")" "$ref" "$flags"
done

echo
echo "range of processor use across those runs:"
for f in "$OUT"/mru_*.time; do cpu_pct "$f"; echo; done | sort -g | sed -n '1p;$p' | tr '\n' ' '
echo
