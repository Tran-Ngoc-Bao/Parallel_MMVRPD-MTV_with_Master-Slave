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
# Best value from exp2/d -- override via env var until that's been run.
ADAPTIVE_PULL_ELITE_SEGMENTS="8"
ELITE_PULL_STRATEGY="rank"
ELITE_PULL_ACCEPT_STRATEGY="selective"
ELITE_PUSH_STRATEGY="significant-best"
ELITE_POOL_FACTOR="0.03"
ELITE_REPLACE_STRATEGY="similarity-aware"

# Compare 3 elite_pull_quality_tolerance_pct values, RUNS runs each.
# elite-pull-accept-strategy is fixed to "selective" above -- this
# tolerance only has any effect under Selective (Always ignores it).
TOLERANCE_LIST=("0.5" "1" "2")

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

OUTPUT_DIR="${OUTPUTS_DIR:-${SCRIPT_DIR}/../../../outputs/exp2/e/${DATA_PREFIX}}"
mkdir -p "${OUTPUT_DIR}"

for DATA_FILE in "${DATA_FILES[@]}"; do
    DATA_FILE_NAME="$(basename "${DATA_FILE}" .txt)"
    for TOLERANCE in "${TOLERANCE_LIST[@]}"; do
        for ((x = 1; x <= RUNS; x++)); do
            RUN_ID="tol${TOLERANCE}-${x}"
            OUT_FILE="${OUTPUT_DIR}/${DATA_FILE_NAME}-${RUN_ID}.json"
            if [ -f "${OUT_FILE}" ]; then
                echo "=== ${DATA_FILE_NAME} tolerance=${TOLERANCE}% run ${x}/${RUNS}: already have ${OUT_FILE}, skipping ==="
                continue
            fi
            SEED=$(( x * 100 ))
            echo "=== ${DATA_FILE_NAME} tolerance=${TOLERANCE}% run ${x}/${RUNS}: master-slave, ${NUM_WORKERS} MPI ranks (1 master + $((NUM_WORKERS - 1)) workers), base seed ${SEED} ==="
            NUM_WORKERS="${NUM_WORKERS}" SEED="${SEED}" MAX_EVALUATIONS="${MAX_EVALUATIONS}" \
                ADAPTIVE_ITERATIONS="${ADAPTIVE_ITERATIONS}" \
                ADAPTIVE_PULL_ELITE_SEGMENTS="${ADAPTIVE_PULL_ELITE_SEGMENTS}" \
                ELITE_PULL_STRATEGY="${ELITE_PULL_STRATEGY}" \
                ELITE_PULL_ACCEPT_STRATEGY="${ELITE_PULL_ACCEPT_STRATEGY}" \
                ELITE_PUSH_STRATEGY="${ELITE_PUSH_STRATEGY}" \
                ELITE_POOL_FACTOR="${ELITE_POOL_FACTOR}" \
                ELITE_REPLACE_STRATEGY="${ELITE_REPLACE_STRATEGY}" \
                ELITE_PULL_QUALITY_TOLERANCE_PCT="${TOLERANCE}" \
                OUTPUTS_DIR="${OUTPUT_DIR}" \
                RUN_ID="${RUN_ID}" \
                bash "${RUN_SCRIPT}" "${DATA_FILE}"
            sleep "${SLEEP_SEC}"
        done
    done
done

echo "Done. Results in ${OUTPUT_DIR}"
