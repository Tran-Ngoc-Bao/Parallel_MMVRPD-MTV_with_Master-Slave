#!/usr/bin/env bash
set -euo pipefail
export LC_NUMERIC=C

# Format per line: "N  TIME_LIMIT" (TIME_LIMIT in seconds; 0 = no limit,
# run until the adaptive-search stopping criteria instead).
JOBS=(
    "100   10"
    "200   35"
    "500   700"
    "1000  4500"
)

# Usage:
#   bash run4_batch.sh [RUNS] [SLEEP_SEC] [CPU_CORES]

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SATS_SCRIPT="${SCRIPT_DIR}/run4_sats.sh"
IMS_SCRIPT="${SCRIPT_DIR}/run4_ims.sh"
STATS_SCRIPT="${SCRIPT_DIR}/stats.py"

RUNS="${1:-10}"
SLEEP_SEC="${2:-0.0}"
CPU_CORES="${3:-0,2,4,6,8,10}"

run_stats() {
    local customers_so_far="$1"
    local stage="$2"
    echo
    echo "---- stats after ${stage} (n=${customers_so_far}) ----"
    python3 "${STATS_SCRIPT}" --customers "${customers_so_far}" --runs "${RUNS}"
}

N_LIST=()
START_TS=$(date +%s)
for JOB in "${JOBS[@]}"; do
    read -r N TIME_LIMIT <<< "${JOB}"
    N_LIST+=("${N}")
    CUSTOMERS_SO_FAR="$(IFS=,; echo "${N_LIST[*]}")"

    echo
    echo "################################################################"
    echo "# n=${N}  time_limit=${TIME_LIMIT}s  [sats]  $(date '+%Y-%m-%d %H:%M:%S')"
    echo "################################################################"
    bash "${SATS_SCRIPT}" "${N}" "${RUNS}" "${SLEEP_SEC}" "${CPU_CORES}" "${TIME_LIMIT}"
    run_stats "${CUSTOMERS_SO_FAR}" "sats, n=${N}"

    echo
    echo "################################################################"
    echo "# n=${N}  time_limit=${TIME_LIMIT}s  [ims]   $(date '+%Y-%m-%d %H:%M:%S')"
    echo "################################################################"
    bash "${IMS_SCRIPT}" "${N}" "${RUNS}" "${SLEEP_SEC}" "${CPU_CORES}" "${TIME_LIMIT}"
    run_stats "${CUSTOMERS_SO_FAR}" "ims, n=${N}"
done

ELAPSED=$(( $(date +%s) - START_TS ))
CUSTOMERS="$(IFS=,; echo "${N_LIST[*]}")"
echo
echo "################################################################"
echo "# All done: n=${CUSTOMERS} in ${ELAPSED}s"
echo "################################################################"
