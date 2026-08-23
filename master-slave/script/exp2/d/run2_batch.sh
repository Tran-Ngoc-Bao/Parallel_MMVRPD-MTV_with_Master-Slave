#!/usr/bin/env bash
set -euo pipefail
export LC_NUMERIC=C

JOBS=(
    "200   194000000"
    "500   2758000000"
)

# Usage:
#   bash run2_batch.sh [RUNS] [NUM_WORKERS]
# Compares 3 adaptive_pull_elite_segments values per customer size, RUNS runs each (see run2.sh)

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RUN2_SCRIPT="${SCRIPT_DIR}/run2.sh"

RUNS="${1:-10}"
SLEEP_SEC="${SLEEP_SEC:-0.0}"
NUM_WORKERS="${2:-7}"

N_LIST=()
START_TS=$(date +%s)
for JOB in "${JOBS[@]}"; do
    read -r N MAX_EVALUATIONS <<< "${JOB}"
    N_LIST+=("${N}")

    echo
    echo "################################################################"
    echo "# n=${N}  max_evaluations=${MAX_EVALUATIONS}  [master-slave]   $(date '+%Y-%m-%d %H:%M:%S')"
    echo "################################################################"
    bash "${RUN2_SCRIPT}" "${N}" "${RUNS}" "${SLEEP_SEC}" "${NUM_WORKERS}" "${MAX_EVALUATIONS}"
done

ELAPSED=$(( $(date +%s) - START_TS ))
CUSTOMERS="$(IFS=,; echo "${N_LIST[*]}")"
echo
echo "################################################################"
echo "# All done: n=${CUSTOMERS} in ${ELAPSED}s"
echo "################################################################"
