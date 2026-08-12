#!/usr/bin/env bash
set -euo pipefail
export LC_NUMERIC=C

# Format per line: "N  MAX_EVALUATIONS"
JOBS=(
    "200   194000000"
    "500   2758000000"
    "1000  13416000000"
)

# Usage:
#   bash run2_batch.sh [RUNS] [SLEEP_SEC] [NUM_WORKERS] [ADAPTIVE_ITERATIONS]

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
IMS_SCRIPT="${SCRIPT_DIR}/run2_ims.sh"

RUNS="${1:-5}"
SLEEP_SEC="${2:-0.0}"
NUM_WORKERS="${3:-6}"
ADAPTIVE_ITERATIONS="${4:-10}"

N_LIST=()
START_TS=$(date +%s)
for JOB in "${JOBS[@]}"; do
    read -r N MAX_EVALUATIONS <<< "${JOB}"
    N_LIST+=("${N}")

    echo
    echo "################################################################"
    echo "# n=${N}  max_evaluations=${MAX_EVALUATIONS}  [ims]   $(date '+%Y-%m-%d %H:%M:%S')"
    echo "################################################################"
    bash "${IMS_SCRIPT}" "${N}" "${RUNS}" "${SLEEP_SEC}" "${NUM_WORKERS}" "${MAX_EVALUATIONS}" "${ADAPTIVE_ITERATIONS}"
done

ELAPSED=$(( $(date +%s) - START_TS ))
CUSTOMERS="$(IFS=,; echo "${N_LIST[*]}")"
echo
echo "################################################################"
echo "# All done: n=${CUSTOMERS} in ${ELAPSED}s"
echo "################################################################"
