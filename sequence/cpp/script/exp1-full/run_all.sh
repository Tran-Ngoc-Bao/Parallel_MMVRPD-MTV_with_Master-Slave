#!/usr/bin/env bash
set -euo pipefail
export LC_NUMERIC=C

# exp1-full: run_ims first (all customer counts), then run_sats (all
# customer counts), across every instance actually present in data/ for
# every customer count from 6 to 1000 (12 combos for n=6/10/12/20, 16
# combos for n=50/100/200/500/1000) -- not just the diagonal 4 combos
# ../exp1 uses. sats runs only after ims has fully finished.
#
# Requires BKS files for every instance: run gen_bks.py once beforehand
# (see ../gen_bks.py) to populate/refresh bks/<n>/<n>.<combo>-bks.json
# from bks/capacity-1400_baseline.xlsx.
#
# Per-n time limits below: 100/200/500/1000 match ../exp1/run4_batch.sh;
# 6/10/12/20/50 are PLACEHOLDERS (not yet tuned) -- adjust as needed.
JOBS=(
    "6     0.01"
    "10    0.04"
    "12    0.06"
    "20    0.1"
    "50    1.5"
    "100   10"
    "200   35"
    "500   700"
    # "1000  4500"
)

# Usage:
#   bash run_all.sh [RUNS] [SLEEP_SEC] [CPU_CORES]

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
IMS_SCRIPT="${SCRIPT_DIR}/run_ims_full.sh"
SATS_SCRIPT="${SCRIPT_DIR}/run_sats_full.sh"

RUNS="${1:-10}"
SLEEP_SEC="${2:-0.0}"
CPU_CORES="${3:-0,2,4,6,8,10}"

# n=1000 only needs 5 runs; every other n uses the RUNS above.
RUNS_1000=5

# run_phase <label> <script>
run_phase() {
    local label="$1"
    local phase_script="$2"
    local JOB N TIME_LIMIT
    for JOB in "${JOBS[@]}"; do
        read -r N TIME_LIMIT <<< "${JOB}"

        local job_runs="${RUNS}"
        if [ "${N}" = "1000" ]; then
            job_runs="${RUNS_1000}"
        fi

        echo
        echo "################################################################"
        echo "# n=${N}  time_limit=${TIME_LIMIT}s  runs=${job_runs}  [${label}]  $(date '+%Y-%m-%d %H:%M:%S')"
        echo "################################################################"
        bash "${phase_script}" "${N}" "${job_runs}" "${SLEEP_SEC}" "${CPU_CORES}" "${TIME_LIMIT}"
    done
}

START_TS=$(date +%s)

echo
echo "================================================================"
echo "= PHASE 1/2: ims"
echo "================================================================"
run_phase "ims" "${IMS_SCRIPT}"

echo
echo "================================================================"
echo "= PHASE 2/2: sats"
echo "================================================================"
run_phase "sats" "${SATS_SCRIPT}"

ELAPSED=$(( $(date +%s) - START_TS ))
CUSTOMERS="$(for JOB in "${JOBS[@]}"; do read -r N _ <<< "${JOB}"; echo "${N}"; done | paste -sd,)"
echo
echo "################################################################"
echo "# All done (ims + sats): n=${CUSTOMERS} in ${ELAPSED}s"
echo "################################################################"
