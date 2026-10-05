#!/bin/bash
# Decision-path latency matrix, with the environment provenance that has to appear
# beside the numbers for them to mean anything.
#
# Four cells, crossing shard count against pinning so the two are not confounded:
#
#              2 shards (4 data-plane threads)   4 shards (6 data-plane threads)
#   unpinned   scheduler places them             scheduler places them
#   pinned     every role its own PHYSICAL core  workers get cores, reader+writer
#                                                 ride SMT siblings
#
# pinned-2 against unpinned-2 isolates pinning at a fixed thread count.
# unpinned-2 against unpinned-4 isolates thread count at fixed placement.
# Without both controls, "pinned-2 is fastest" is not attributable to either.
#
# Usage:  REPS=4 SECS=20 bash matrix.sh > results.csv
#
# The provenance block goes to stderr so stdout stays a clean CSV. READ IT before
# quoting anything: a run with a non-invariant TSC, an ondemand governor, no
# isolcpus, or inside a hypervisor is a development run, not a measurement.
set -u
M=${MODEL:-/root/icarust/static/dna_r10.4.1_e8.2_400bps/R10_model.tsv}
R=${REFERENCE:-/root/icarust/docker/squiggle_arrs/gencode.v44.transcripts.wtc11.fa}
CA=${CA:-/root/icarust/static/tls_certs_fixed/ca.crt}
D=${DAEMON_DIR:-/root/mru/build-transport}
OUT=${OUT:-/root/mru/results}
TARGET=${TARGET:-127.0.0.1:10001}
REPS=${REPS:-4}
SECS=${SECS:-20}
mkdir -p "$OUT"

provenance() {
  echo "=============================================================="
  echo "ENVIRONMENT PROVENANCE  $(date -Is)"
  echo "=============================================================="
  echo "host          : $(uname -snrm)"
  echo "cpu           : $(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2- | sed 's/^ //')"
  echo "logical cpus  : $(nproc)"
  echo "smt siblings  : $(cat /sys/devices/system/cpu/cpu*/topology/thread_siblings_list 2>/dev/null | sort -u | tr '\n' ' ')"
  echo "smt active    : $(cat /sys/devices/system/cpu/smt/active 2>/dev/null || echo unknown)"
  # The single most important line: anything but "none" means the clock and the
  # scheduler belong to a hypervisor and the tail is not yours to report.
  echo "hypervisor    : $(systemd-detect-virt 2>/dev/null || echo unknown)"
  echo "tsc flags     : $(grep -m1 flags /proc/cpuinfo | tr ' ' '\n' | grep -E '^(constant_tsc|nonstop_tsc|tsc_reliable|rdtscp)$' | tr '\n' ' ')"
  echo "clocksource   : $(cat /sys/devices/system/clocksource/clocksource0/current_clocksource 2>/dev/null || echo unknown)"
  echo "kernel cmdline: $(cat /proc/cmdline)"
  echo "isolcpus      : $(grep -o 'isolcpus=[^ ]*' /proc/cmdline || echo 'ABSENT -- tail is contaminated by other work')"
  echo "nohz_full     : $(grep -o 'nohz_full=[^ ]*' /proc/cmdline || echo 'ABSENT -- timer ticks will show in the tail')"
  echo "rcu_nocbs     : $(grep -o 'rcu_nocbs=[^ ]*' /proc/cmdline || echo ABSENT)"
  echo "governor      : $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo unavailable)"
  echo "turbo (no_turbo=1 means disabled): $(cat /sys/devices/system/cpu/intel_pstate/no_turbo 2>/dev/null || echo unavailable)"
  echo "disk free     : $(df -h / | awk 'NR==2{print $4" of "$2}')"
  echo "loadavg       : $(cut -d' ' -f1-3 /proc/loadavg)"
  echo "--------------------------------------------------------------"
  local verdict="DEVELOPMENT ONLY"
  if [ "$(systemd-detect-virt 2>/dev/null || echo yes)" = "none" ] \
     && grep -q isolcpus /proc/cmdline \
     && [ "$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null)" = "performance" ]; then
    verdict="PUBLISHABLE (bare metal, isolated, performance governor)"
  fi
  echo "VERDICT: $verdict"
  echo "=============================================================="
}

run() {
  local name=$1 rep=$2; shift 2
  local line
  line=$(timeout 150 "$D/mru_daemon" --target "$TARGET" --ca "$CA" --model "$M" \
      --reference "$R" --seconds "$SECS" --latency-out "$OUT/${name}_${rep}.hgrm" "$@" 2>&1 \
    | grep -E "^decision path")
  if [ -z "$line" ]; then
    echo "$name,$rep,FAILED,,,,,,," >&2
    return
  fi
  echo "$name,$rep,$(echo "$line" | sed -E 's/.*n=([0-9]+) +p50 ([0-9.]+) +p90 ([0-9.]+) +p99 ([0-9.]+) +p99\.9 ([0-9.]+) +p99\.99 ([0-9.]+) +max ([0-9.]+) +mean ([0-9.]+).*/\1,\2,\3,\4,\5,\6,\7,\8/')"
}

provenance >&2
echo "config,rep,n,p50,p90,p99,p99_9,p99_99,max,mean"
for rep in $(seq 1 "$REPS"); do
  run unpinned-2 "$rep" --shards 2
  run pinned-2   "$rep" --shards 2 --pin
  run unpinned-4 "$rep" --shards 4
  run pinned-4   "$rep" --shards 4 --pin
done
