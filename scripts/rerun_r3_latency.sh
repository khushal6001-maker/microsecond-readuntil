#!/bin/bash
# Capture regime R3: per-chunk decision latency at the FROZEN configuration against the 64 Mb
# reference, under variable dwell.
#
# Appendix F flagged this as the largest provenance gap in the thesis. The genome-scale
# histograms that survived in results/hgrm -- chr20_ours, chr20plus, fix_hg38_chr20,
# cmp_hg38_chr20 -- report medians of 21.9 to 25.6 us, which are MINIMIZER WINDOW 10 runs.
# The frozen configuration uses window 1, where the index is 5.5x larger and the probe does
# correspondingly more work, and no histogram of it was ever committed.
#
# Geometry is left entirely to the defaults on purpose: --bits, --events, --min-window all
# take a 0 sentinel and --detect a -1 sentinel, so passing none of them selects the frozen
# PolicyConfig values (4 x 12, window 1, detection on). Passing them explicitly would restate
# the frozen values in a second place, which is the shadowing defect that Appendix C.6 records.
set -u
REPO="/mnt/c/Users/Khushal singh/Desktop/mtech project/microsecond-readuntil"
OUT="$REPO/results/hgrm"
LOGS="$REPO/results/r3-raw"
IC=/root/icarust
D=/root/mru/build-transport/mru_daemon
MODEL=$IC/static/dna_r10.4.1_e8.2_400bps/R10_model.tsv
REF=$IC/docker/squiggle_arrs/chr20_plus_wtc11.fa
CA=$IC/static/tls_certs_fixed/ca.crt
TOML=$IC/mru_r3.toml
SECS=${SECS:-40}
REPS=${REPS:-6}
PREFIX=${PREFIX:-r3_frozen}
CV=${ICARUST_DWELL_CV:-0.35}

mkdir -p "$OUT" "$LOGS"

cat > "$TOML" <<TOMLEOF
output_path = "/tmp/icarust_r3"
target_yield = 100000000000
pore_type = "R10"
nucleotide_type = "DNA"

[parameters]
sample_name = "mru_r3"
experiment_name = "regime_r3_latency"
flowcell_name = "FAQ1234"
experiment_duration_set = 4800
device_id = "Bantersaurus"
position = "FenceSitter"
sample_rate = 4000

[[sample]]
name = "chr20_plus_wtc11"
input_genome = "$REF"
mean_read_length = 2000
weight = 1
TOMLEOF

echo "=============================================================="
echo "ENVIRONMENT PROVENANCE  $(date -Is)"
echo "=============================================================="
echo "host          : $(uname -snrm)"
echo "cpu           : $(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2- | sed 's/^ //')"
echo "logical cpus  : $(nproc)"
echo "smt siblings  : $(cat /sys/devices/system/cpu/cpu*/topology/thread_siblings_list 2>/dev/null | sort -u | tr '\n' ' ')"
echo "hypervisor    : $(systemd-detect-virt 2>/dev/null || echo unknown)"
echo "clocksource   : $(cat /sys/devices/system/clocksource/clocksource0/current_clocksource 2>/dev/null || echo unknown)"
echo "isolcpus      : $(grep -o 'isolcpus=[^ ]*' /proc/cmdline || echo 'ABSENT -- tail is contaminated')"
echo "governor      : $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo unavailable)"
echo "disk free     : $(df -h / | awk 'NR==2{print $4" of "$2}')"
echo "reference     : $REF ($(stat -c%s "$REF") bytes)"
echo "daemon build  : $(cd /root/mru && git log --oneline -1)"
echo "dwell CV      : $CV  (0 would be stock Icarust, i.e. constant dwell)"
echo "VERDICT: DEVELOPMENT ONLY (hypervisor present, no core isolation)"
echo "=============================================================="

pkill -f 'release/icarust' 2>/dev/null; sleep 1
echo "starting icarust with ICARUST_DWELL_CV=$CV ..."
( cd "$IC" && ICARUST_DWELL_CV="$CV" ./target/release/icarust \
      -c ./config_fixed.ini -s "$TOML" > "$LOGS/icarust.log" 2>&1 ) &
IC_PID=$!

# Icarust precomputes squiggle arrays for the whole genome before it serves anything, and
# for a 64 Mb reference that takes minutes. Wait on the port rather than on a log line, and
# check the process is still alive -- the first attempt at this run was OOM-killed here, with
# an empty log and no indication of why until the exit status was read.
echo "waiting for icarust to squiggle $(stat -c%s "$REF") bases and open the port ..."
READY=0
for i in $(seq 1 300); do
  if ! kill -0 "$IC_PID" 2>/dev/null; then
    echo "ICARUST DIED after ${i}s. Last log lines:"; tail -20 "$LOGS/icarust.log"
    echo "dmesg (look for an OOM kill):"; dmesg 2>/dev/null | tail -5
    exit 1
  fi
  if (exec 3<>/dev/tcp/127.0.0.1/10001) 2>/dev/null; then READY=1; exec 3<&- 3>&-; break; fi
  sleep 1
done
[ "$READY" = 1 ] || { echo "port 10001 never opened"; tail -20 "$LOGS/icarust.log"; exit 1; }
echo "icarust ready after ${i}s"
sleep 5
grep -iE 'dwell variability' "$LOGS/icarust.log" | head -2
free -g | head -2

for rep in $(seq 1 "$REPS"); do
  echo
  echo "---- R3 rep $rep of $REPS, ${SECS}s, frozen geometry, 2 shards, pinned ----"
  /usr/bin/time -v "$D" --target 127.0.0.1:10001 --ca "$CA" \
      --model "$MODEL" --reference "$REF" \
      --seconds "$SECS" --shards 2 --pin \
      --latency-out "$OUT/${PREFIX}_${rep}.hgrm" \
      > "$LOGS/${PREFIX}_${rep}.log" 2>&1
  grep -E '^index:|^geometry|^decision path|drop|RSS|Maximum resident' "$LOGS/${PREFIX}_${rep}.log" | head -8
done

kill "$IC_PID" 2>/dev/null; pkill -f 'release/icarust' 2>/dev/null
echo
echo "histograms in results/hgrm/${PREFIX}_*.hgrm, logs in results/r3-raw/"
