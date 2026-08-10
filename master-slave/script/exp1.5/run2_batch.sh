#!/usr/bin/env bash
set -euo pipefail
export LC_NUMERIC=C

JOBS=(
    "200   194000000   4"
    # "500   2758000000  80"
)

# Usage:
#   bash run2_batch.sh [RUNS] [NUM_WORKERS]

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RUN2_SCRIPT="${SCRIPT_DIR}/run2.sh"

RUNS="${1:-5}"
NUM_WORKERS="${2:-7}"

N_LIST=()
START_TS=$(date +%s)
for JOB in "${JOBS[@]}"; do
    read -r N MAX_EVALUATIONS SLEEP_SEC <<< "${JOB}"
    N_LIST+=("${N}")

    echo
    echo "################################################################"
    echo "# n=${N}  max_evaluations=${MAX_EVALUATIONS}  sleep_sec=${SLEEP_SEC}  [master-slave]   $(date '+%Y-%m-%d %H:%M:%S')"
    echo "################################################################"
    bash "${RUN2_SCRIPT}" "${N}" "${RUNS}" "${SLEEP_SEC}" "${NUM_WORKERS}" "${MAX_EVALUATIONS}"
done

ELAPSED=$(( $(date +%s) - START_TS ))
CUSTOMERS="$(IFS=,; echo "${N_LIST[*]}")"
echo
echo "################################################################"
echo "# All done: n=${CUSTOMERS} in ${ELAPSED}s"
echo "################################################################"
