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
ADAPTIVE_PULL_ELITE_SEGMENTS="4"
ELITE_POOL_FACTOR="0.03"

# Compare 4 elite-pull strategies (excluding off) x 2 elite-pull-accept strategies, RUNS runs each
PULL_STRATEGIES=("random" "topk" "rank" "pullcount")
ACCEPT_STRATEGIES=("always" "selective")

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
        for ACCEPT_STRATEGY in "${ACCEPT_STRATEGIES[@]}"; do
            for ((x = 1; x <= RUNS; x++)); do
                RUN_ID="${PULL_STRATEGY}-${ACCEPT_STRATEGY}-${x}"
                OUT_FILE="${OUTPUT_DIR}/${DATA_FILE_NAME}-${RUN_ID}.json"
                if [ -f "${OUT_FILE}" ]; then
                    echo "=== ${DATA_FILE_NAME} pull=${PULL_STRATEGY} accept=${ACCEPT_STRATEGY} run ${x}/${RUNS}: already have ${OUT_FILE}, skipping ==="
                    continue
                fi
                SEED=$(( x * 100 ))
                echo "=== ${DATA_FILE_NAME} pull=${PULL_STRATEGY} accept=${ACCEPT_STRATEGY} run ${x}/${RUNS}: master-slave, ${NUM_WORKERS} MPI ranks (1 master + $((NUM_WORKERS - 1)) workers), base seed ${SEED} ==="
                NUM_WORKERS="${NUM_WORKERS}" SEED="${SEED}" MAX_EVALUATIONS="${MAX_EVALUATIONS}" \
                    ADAPTIVE_PULL_ELITE_SEGMENTS="${ADAPTIVE_PULL_ELITE_SEGMENTS}" \
                    ELITE_PULL_STRATEGY="${PULL_STRATEGY}" \
                    ELITE_PULL_ACCEPT_STRATEGY="${ACCEPT_STRATEGY}" \
                    ELITE_POOL_FACTOR="${ELITE_POOL_FACTOR}" \
                    OUTPUTS_DIR="${OUTPUT_DIR}" \
                    RUN_ID="${RUN_ID}" \
                    bash "${RUN_SCRIPT}" "${DATA_FILE}"
                sleep "${SLEEP_SEC}"
            done
        done
    done
done

echo "Done. Results in ${OUTPUT_DIR}"
