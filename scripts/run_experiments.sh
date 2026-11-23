#!/usr/bin/env bash
# Reproduce every experiment in the report. ~6-8 minutes on a 12-thread laptop.
# Usage: scripts/run_experiments.sh [results_dir]
set -euo pipefail
cd "$(dirname "$0")/.."
OUT=${1:-results}
mkdir -p "$OUT"
make -s all

NCPU=$(nproc)
H=$NCPU            # "all hardware threads"
H2=$((NCPU * 2))   # 2x oversubscribed
H4=$((NCPU * 4))   # 4x oversubscribed

{
  echo "date: $(date -Iseconds)"
  echo "uname: $(uname -srm)"
  echo "compiler: $(${CC:-gcc} --version | head -1)"
  echo "nproc: $NCPU"
  echo "load_before: $(cut -d' ' -f1-3 /proc/loadavg)"
  lscpu | grep -E "Model name|Thread\(s\) per core|Core\(s\) per socket|L1d|L2|L3|MHz"
  # Hybrid CPUs (Intel P-/E-cores) expose per-core max frequency:
  for c in /sys/devices/system/cpu/cpu[0-9]*; do
    printf "%s max_khz=%s\n" "$(basename "$c")" "$(cat "$c"/cpufreq/cpuinfo_max_freq 2>/dev/null || echo ?)"
  done
} > "$OUT/env.txt"

echo "[C] hardware calibration"
./calibrate 0,1 0,2 0,4 4,5 4,8 > "$OUT/calibration.csv"

echo "[E1] lock scaling, empty critical section (paper Fig. 6 style)"
./lockbench -l all -t 1,2,4,6,8,10,$H -d 0.5 -r 3 > "$OUT/e1_scaling_empty.csv"

echo "[E2] lock scaling, small critical section + think time (paper Fig. 7 style)"
./lockbench -l ttas,tas_backoff,ticket,anderson,mcs,mcs_sp,mcs_spw,pthread -t 1,2,4,6,8,10,$H \
  -c 4 -k 200 -d 0.5 -r 3 > "$OUT/e2_scaling_cs.csv"

echo "[E3] oversubscription: threads > hardware threads"
./lockbench -l tas_backoff,ticket,anderson,mcs,mcs_sp,mcs_spw,pthread -t $((H / 2)),$H,$H2,$H4 \
  -c 4 -k 200 -d 1.0 -r 3 > "$OUT/e3_oversub.csv"

echo "[E4] MCS-SP / MCS-SPW spin-limit sensitivity"
first=1
for s in 0 16 64 256 1024 4096 16384 65536; do
  if [ $first = 1 ]; then hdr=""; first=0; else hdr="-H"; fi
  ./lockbench -l mcs_sp,mcs_spw -t $((H * 2 / 3)),$H2 -c 4 -k 200 -d 0.5 -r 3 -s $s $hdr |
    awk -F, -v s=$s 'NR==1 && $1=="lock"{print $0",spin_limit"; next}{print $0","s}'
done > "$OUT/e4_spin_sweep.csv"

echo "[E7] MCS-SPW wake-ahead depth"
first=1
for w in 0 1 2 3 4 8; do
  if [ $first = 1 ]; then hdr=""; first=0; else hdr="-H"; fi
  ./lockbench -l mcs_spw -t $((H * 2 / 3)),$H,$H2,$H4 -c 4 -k 200 -d 0.5 -r 3 -w $w $hdr |
    awk -F, -v w=$w 'NR==1 && $1=="lock"{print $0",wake_depth"; next}{print $0","w}'
done > "$OUT/e7_wake_depth.csv"

echo "[E5] barriers, threads <= hardware threads"
./barrierbench -t 2,4,6,8,10,$H -e 50000 -r 3 > "$OUT/e5_barriers.csv"

echo "[E6] barriers, oversubscribed (few episodes: spin barriers take ~a time slice each)"
./barrierbench -t $((H + H / 3)),$H2 -e 300 -r 3 > "$OUT/e6_barriers_oversub.csv"

echo "load_after: $(cut -d' ' -f1-3 /proc/loadavg)" >> "$OUT/env.txt"
grep -l FAIL "$OUT"/*.csv && { echo "CORRECTNESS FAILURE"; exit 1; } || echo "all runs passed correctness checks"
