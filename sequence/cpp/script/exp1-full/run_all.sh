#!/usr/bin/env bash
set -euo pipefail
export LC_NUMERIC=C

# exp1-full: run4_ims only (no sats), across every instance actually
# present in data/ for every customer count from 6 to 1000 (12 combos for
# n=6/10/12/20, 16 combos for n=50/100/200/500/1000) -- not just the
# diagonal 4 combos ../exp1 uses.
#
# Requires BKS files for every instance: run gen_bks.py once beforehand
# (see ../gen_bks.py) to populate/refresh bks/<n>/<n>.<combo>-bks.json
# from bks/capacity-1400_baseline.xlsx.
#
# Per-n time limits below: 100/200/500/1000 match ../exp1/run4_batch.sh;
# 6/10/12/20/50 are PLACEHOLDERS (not yet tuned) -- adjust as needed.
JOBS=(
    "6     0.01"
    "10    0.05"
    "12    0.08"
    "20    0.13"
    "50    2"
    "100   14"
    "200   50"
    "500   1000"
    "1000  6300"
)

# Usage:
#   bash run_all.sh [RUNS] [SLEEP_SEC] [CPU_CORES]

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
IMS_SCRIPT="${SCRIPT_DIR}/run4_ims_full.sh"
STATS_SCRIPT="${SCRIPT_DIR}/stats.py"

RUNS="${1:-10}"
SLEEP_SEC="${2:-0.0}"
CPU_CORES="${3:-0,2,4,6,8,10}"

# n=1000 only needs 5 runs; every other n uses the RUNS above (default 10).
RUNS_1000=5

run_stats() {
    local customers_so_far="$1"
    local stage="$2"
    local runs="$3"
    echo
    echo "---- stats after ${stage} (n=${customers_so_far}) ----"
    python3 "${STATS_SCRIPT}" --customers "${customers_so_far}" --runs "${runs}"
}

N_LIST=()
START_TS=$(date +%s)
for JOB in "${JOBS[@]}"; do
    read -r N TIME_LIMIT <<< "${JOB}"
    N_LIST+=("${N}")
    CUSTOMERS_SO_FAR="$(IFS=,; echo "${N_LIST[*]}")"

    JOB_RUNS="${RUNS}"
    if [ "${N}" = "1000" ]; then
        JOB_RUNS="${RUNS_1000}"
    fi

    echo
    echo "################################################################"
    echo "# n=${N}  time_limit=${TIME_LIMIT}s  runs=${JOB_RUNS}  [ims]  $(date '+%Y-%m-%d %H:%M:%S')"
    echo "################################################################"
    bash "${IMS_SCRIPT}" "${N}" "${JOB_RUNS}" "${SLEEP_SEC}" "${CPU_CORES}" "${TIME_LIMIT}"
    run_stats "${CUSTOMERS_SO_FAR}" "ims, n=${N}" "${JOB_RUNS}"
done

ELAPSED=$(( $(date +%s) - START_TS ))
CUSTOMERS="$(IFS=,; echo "${N_LIST[*]}")"
echo
echo "################################################################"
echo "# All done: n=${CUSTOMERS} in ${ELAPSED}s"
echo "################################################################"
