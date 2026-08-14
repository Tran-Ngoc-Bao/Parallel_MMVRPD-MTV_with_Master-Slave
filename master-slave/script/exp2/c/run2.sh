#!/usr/bin/env bash
set -euo pipefail
export LC_NUMERIC=C

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RUN_SCRIPT="${SCRIPT_DIR}/../../script.sh"

DEFAULT_DATA_PREFIX_FROM_FILE="$(
  sed -n 's/^DEFAULT_DATA_PREFIX="\${DEFAULT_DATA_PREFIX:-\([^\}]\+\)}"$/\1/p' "${RUN_SCRIPT}" | head -n 1
)"
DEFAULT_DATA_PREFIX="${DEFAULT_DATA_PREFIX:-${DEFAULT_DATA_PREFIX_FROM_FILE}}"
DATA_PREFIX="${1:-${DEFAULT_DATA_PREFIX}}"
RUNS="${2:-3}"
SLEEP_SEC="${3:-0.0}"
NUM_WORKERS="${4:-7}"
MAX_EVALUATIONS="${5:-0}"

# Fixed settings for this experiment
ADAPTIVE_ITERATIONS="60"
ELITE_PUSH_STRATEGY="new-best"
ELITE_REPLACE_STRATEGY="similarity-quality"
ELITE_POOL_FACTOR="0.04"

# Compare the 4 non-off pull strategies, RUNS runs each
PULL_STRATEGIES=("random" "topk" "rank" "pullcount")

DATA_DIR="${SCRIPT_DIR}/../../../../data"
DATA_FILES=()
for COMBO in "10.2" "40.1"; do
    DATA_FILE="${DATA_DIR}/${DATA_PREFIX}.${COMBO}.txt"
    if [ -f "${DATA_FILE}" ]; then
        DATA_FILES+=("${DATA_FILE}")
    fi
done

if [ ${#DATA_FILES[@]} -eq 0 ]; then
    echo "No data files found for prefix ${DATA_PREFIX}" >&2
    exit 1
fi

OUTPUT_DIR="${OUTPUTS_DIR:-${SCRIPT_DIR}/../../../outputs/exp2/c/${DATA_PREFIX}}"
mkdir -p "${OUTPUT_DIR}"

for DATA_FILE in "${DATA_FILES[@]}"; do
    DATA_FILE_NAME="$(basename "${DATA_FILE}" .txt)"
    for PULL_STRATEGY in "${PULL_STRATEGIES[@]}"; do
        for ((x = 1; x <= RUNS; x++)); do
            SEED=$(( x * 100000 + 1001 ))
            echo "=== ${DATA_FILE_NAME} pull=${PULL_STRATEGY} run ${x}/${RUNS}: master-slave, ${NUM_WORKERS} MPI ranks (1 master + $((NUM_WORKERS - 1)) workers), base seed ${SEED} ==="
            NUM_WORKERS="${NUM_WORKERS}" SEED="${SEED}" MAX_EVALUATIONS="${MAX_EVALUATIONS}" \
                ADAPTIVE_ITERATIONS="${ADAPTIVE_ITERATIONS}" \
                ELITE_PULL_STRATEGY="${PULL_STRATEGY}" \
                ELITE_PUSH_STRATEGY="${ELITE_PUSH_STRATEGY}" \
                ELITE_POOL_FACTOR="${ELITE_POOL_FACTOR}" \
                ELITE_REPLACE_STRATEGY="${ELITE_REPLACE_STRATEGY}" \
                OUTPUTS_DIR="${OUTPUT_DIR}" \
                RUN_ID="pull${PULL_STRATEGY}-${x}" \
                bash "${RUN_SCRIPT}" "${DATA_FILE}"
            sleep "${SLEEP_SEC}"
        done
    done
done

echo "Done. Results in ${OUTPUT_DIR}"
