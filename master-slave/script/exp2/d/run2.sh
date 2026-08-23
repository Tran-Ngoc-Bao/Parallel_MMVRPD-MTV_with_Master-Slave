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

# Compare 3 adaptive_pull_elite_segments values, RUNS runs each
SEGMENTS_LIST=("2" "4" "8")

COMBOS=("10.2" "40.1")
# Fixed --elite-pool-size, same as exp2/g's po3 (200->4, 500->5).
case "${DATA_PREFIX}" in
    200) COMBOS+=("20.3"); POOL_SIZE="4" ;;
    500) COMBOS+=("30.4"); POOL_SIZE="5" ;;
    *)
        echo "No elite_pool_size configured for DATA_PREFIX=${DATA_PREFIX}" >&2
        exit 1
        ;;
esac

DATA_DIR="${SCRIPT_DIR}/../../../../data"
DATA_FILES=()
for COMBO in "${COMBOS[@]}"; do
    DATA_FILE="${DATA_DIR}/${DATA_PREFIX}.${COMBO}.txt"
    if [ -f "${DATA_FILE}" ]; then
        DATA_FILES+=("${DATA_FILE}")
    fi
done

if [ ${#DATA_FILES[@]} -eq 0 ]; then
    echo "No data files found for prefix ${DATA_PREFIX}" >&2
    exit 1
fi

OUTPUT_DIR="${OUTPUTS_DIR:-${SCRIPT_DIR}/../../../outputs/exp2/d/${DATA_PREFIX}}"
mkdir -p "${OUTPUT_DIR}"

for DATA_FILE in "${DATA_FILES[@]}"; do
    DATA_FILE_NAME="$(basename "${DATA_FILE}" .txt)"
    for SEGMENTS in "${SEGMENTS_LIST[@]}"; do
        for ((x = 1; x <= RUNS; x++)); do
            RUN_ID="seg${SEGMENTS}-${x}"
            OUT_FILE="${OUTPUT_DIR}/${DATA_FILE_NAME}-${RUN_ID}.json"
            if [ -f "${OUT_FILE}" ]; then
                echo "=== ${DATA_FILE_NAME} segments=${SEGMENTS} run ${x}/${RUNS}: already have ${OUT_FILE}, skipping ==="
                continue
            fi
            SEED=$(( x * 100 ))
            echo "=== ${DATA_FILE_NAME} segments=${SEGMENTS} run ${x}/${RUNS}: master-slave, ${NUM_WORKERS} MPI ranks (1 master + $((NUM_WORKERS - 1)) workers), base seed ${SEED} ==="
            NUM_WORKERS="${NUM_WORKERS}" SEED="${SEED}" MAX_EVALUATIONS="${MAX_EVALUATIONS}" \
                ELITE_POOL_SIZE="${POOL_SIZE}" \
                ADAPTIVE_PULL_ELITE_SEGMENTS="${SEGMENTS}" \
                OUTPUTS_DIR="${OUTPUT_DIR}" \
                RUN_ID="${RUN_ID}" \
                bash "${RUN_SCRIPT}" "${DATA_FILE}"
            sleep "${SLEEP_SEC}"
        done
    done
done

echo "Done. Results in ${OUTPUT_DIR}"
