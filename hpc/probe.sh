#!/bin/bash
#SBATCH --job-name=mru-probe
#SBATCH --exclusive
#SBATCH --nodes=1
#SBATCH --time=00:10:00
#SBATCH --output=mru-probe-%j.out
#
# Environment probe. Run this FIRST and send back the whole output.
#
# Submit it as a job rather than running it on the login node:
#
#     sbatch hpc/probe.sh          # then read mru-probe-<jobid>.out
#
# Why it has to be a job: the two things that decide how everything else is built --
# whether compute nodes reach the internet, and what the CPU/kernel actually look
# like under an exclusive allocation -- are both different on a login node. A login
# node is usually online and usually a different machine. Running this on the login
# node answers the wrong questions.
#
# It changes nothing and installs nothing. Add --gres=gpu:1 (or your site's GPU flag)
# if GPUs are not allocated by default, otherwise the nvidia-smi section will be empty.

set -u
line() { printf '\n========== %s ==========\n' "$1"; }
have() { command -v "$1" >/dev/null 2>&1; }

line "WHERE AM I"
echo "hostname     : $(hostname)"
echo "date         : $(date -Is)"
echo "under slurm  : ${SLURM_JOB_ID:-NO (running outside a job -- results may not reflect a compute node)}"
echo "slurm node   : ${SLURMD_NODENAME:-n/a}"
echo "slurm cpus   : ${SLURM_CPUS_ON_NODE:-n/a}  tasks=${SLURM_NTASKS:-n/a}"
echo "partition    : ${SLURM_JOB_PARTITION:-n/a}"
echo "exclusive    : ${SLURM_JOB_EXCLUSIVE:-unset (check scontrol below)}"
if have scontrol && [ -n "${SLURM_JOB_ID:-}" ]; then
  scontrol show job "$SLURM_JOB_ID" 2>/dev/null | grep -E "NumCPUs|NumNodes|OverSubscribe|TRES=|Shared" | sed 's/^/  /'
fi

line "CPU"
if have lscpu; then lscpu | grep -viE "flags" ; fi
echo "-- TSC and related flags --"
grep -m1 flags /proc/cpuinfo | tr ' ' '\n' | grep -E '^(constant_tsc|nonstop_tsc|tsc_reliable|rdtscp|tsc_adjust|invariant)$' | tr '\n' ' '; echo
echo "-- SMT sibling groups (this is what the pinning plan reads) --"
cat /sys/devices/system/cpu/cpu*/topology/thread_siblings_list 2>/dev/null | sort -u | tr '\n' ' '; echo
echo "smt active   : $(cat /sys/devices/system/cpu/smt/active 2>/dev/null || echo unknown)"
echo "-- NUMA --"
if have numactl; then numactl --hardware | head -20; else echo "numactl absent"; fi

line "THE LATENCY-CRITICAL SETTINGS"
echo "virtualised  : $(systemd-detect-virt 2>/dev/null || echo unknown)   <-- must be 'none' for a publishable tail"
echo "clocksource  : $(cat /sys/devices/system/clocksource/clocksource0/current_clocksource 2>/dev/null || echo unknown)"
echo "cmdline      : $(cat /proc/cmdline)"
echo "isolcpus     : $(grep -o 'isolcpus=[^ ]*' /proc/cmdline || echo ABSENT)"
echo "nohz_full    : $(grep -o 'nohz_full=[^ ]*' /proc/cmdline || echo ABSENT)"
echo "rcu_nocbs    : $(grep -o 'rcu_nocbs=[^ ]*' /proc/cmdline || echo ABSENT)"
echo "governor     : $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo unavailable)"
echo "no_turbo     : $(cat /sys/devices/system/cpu/intel_pstate/no_turbo 2>/dev/null || echo unavailable)"
echo "-- what this job is actually allowed to use --"
echo "affinity     : $(taskset -pc $$ 2>/dev/null || echo unknown)"
for f in /sys/fs/cgroup/cpuset.cpus.effective /sys/fs/cgroup/cpuset/cpuset.cpus; do
  [ -r "$f" ] && echo "cgroup cpuset: $(cat $f)  ($f)"
done
echo "am I root    : $([ "$(id -u)" = 0 ] && echo YES || echo no)"

line "MEMORY AND SCRATCH"
free -g 2>/dev/null | head -2
echo "-- the daemon needs ~1.5 GB resident for a 64 Mb reference index --"
for d in "$HOME" "${SCRATCH:-}" "${TMPDIR:-/tmp}" /tmp /dev/shm /scratch /lustre; do
  [ -n "$d" ] && [ -d "$d" ] && printf "%-22s %s  (%s)\n" "$d" \
      "$(df -h "$d" 2>/dev/null | awk 'NR==2{print $4" free of "$2}')" \
      "$(stat -f -c %T "$d" 2>/dev/null)"
done

line "GPU"
if have nvidia-smi; then nvidia-smi; else echo "nvidia-smi absent (no GPU allocated to this job?)"; fi

line "TOOLCHAIN"
for t in gcc g++ clang clang++ cmake ninja make git python3 cargo rustc conda mamba micromamba spack apptainer singularity docker podman pkg-config protoc; do
  if have "$t"; then printf "%-14s %s\n" "$t" "$($t --version 2>&1 | head -1)"; else printf "%-14s ABSENT\n" "$t"; fi
done
echo "-- gRPC/protobuf already present? (the transport build needs these) --"
if have pkg-config; then
  for p in grpc++ protobuf; do printf "  %-10s %s\n" "$p" "$(pkg-config --modversion $p 2>/dev/null || echo 'not via pkg-config')"; done
fi

line "MODULES"
for init in /etc/profile.d/modules.sh /etc/profile.d/lmod.sh /usr/share/lmod/lmod/init/bash; do
  [ -r "$init" ] && . "$init" 2>/dev/null && break
done
if have module || declare -F module >/dev/null 2>&1; then
  echo "-- anything matching dorado / guppy / ont / nanopore / minimap --"
  module -t avail 2>&1 | grep -iE "dorado|guppy|ont|nanopore|minimap|samtools" | head -30
  echo "-- cuda / gcc / cmake / rust --"
  module -t avail 2>&1 | grep -iE "^(cuda|gcc|cmake|rust|anaconda|miniconda|python)" | head -30
  echo "(if those are empty, run 'module avail' yourself and send the list)"
else
  echo "no module system found in this shell"
fi

line "DORADO / BASECALLER PRESENT ANYWHERE?"
for b in dorado dorado_basecall_server guppy_basecall_server ont_basecall_client; do
  if have "$b"; then printf "%-26s %s\n" "$b" "$(command -v $b)"; else printf "%-26s not on PATH\n" "$b"; fi
done
ls -d /opt/ont /usr/local/ont /opt/dorado 2>/dev/null

line "INTERNET FROM THIS NODE  (the answer that decides how we install things)"
for url in https://github.com https://pypi.org https://cdn.oxfordnanoportal.com; do
  code=$(curl -sS -o /dev/null -w '%{http_code}' --max-time 8 "$url" 2>&1)
  printf "%-36s %s\n" "$url" "$code"
done
echo "proxy vars   : ${http_proxy:-none} ${https_proxy:-none}"
echo "dns          : $(getent hosts github.com 2>/dev/null | head -1 || echo 'cannot resolve')"

line "DONE"
echo "Send this entire file back. The three lines that decide the plan are:"
echo "  1. 'virtualised' under THE LATENCY-CRITICAL SETTINGS"
echo "  2. the HTTP codes under INTERNET FROM THIS NODE"
echo "  3. whether anything turned up under DORADO / BASECALLER"
