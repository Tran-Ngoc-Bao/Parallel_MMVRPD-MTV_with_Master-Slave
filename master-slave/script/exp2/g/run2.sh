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
ELITE_PULL_QUALITY_TOLERANCE_PCT="-1.0"

# Compare 3 elite_replace_strategy values, RUNS runs each.
REPLACE_STRATEGY_LIST=("quality-only" "td-crowding" "edge-crowding")

# ... on 3 hand-picked --elite-pool-size values, grouped into named "sets"
# (po1..po3). Each set fixes a pool size per customer count:
#   po1: 200->2, 500->3
#   po2: 200->3, 500->4
#   po3: 200->4, 500->5
case "${DATA_PREFIX}" in
    200)
        POOL_SET_NAMES=("po1" "po2" "po3")
        POOL_SIZES=("2"   "3"   "4")
        ;;
    500)
        POOL_SET_NAMES=("po1" "po2" "po3")
        POOL_SIZES=("3"   "4"   "5")
        ;;
    *)
        echo "No elite_pool_size sets configured for DATA_PREFIX=${DATA_PREFIX}" >&2
        exit 1
        ;;
esac

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

OUTPUT_DIR="${OUTPUTS_DIR:-${SCRIPT_DIR}/../../../outputs/exp2/g/${DATA_PREFIX}}"
mkdir -p "${OUTPUT_DIR}"

for DATA_FILE in "${DATA_FILES[@]}"; do
    DATA_FILE_NAME="$(basename "${DATA_FILE}" .txt)"
    for ((p = 0; p < ${#POOL_SET_NAMES[@]}; p++)); do
        POOL_SET_NAME="${POOL_SET_NAMES[$p]}"
        POOL_SIZE="${POOL_SIZES[$p]}"

        for REPLACE_STRATEGY in "${REPLACE_STRATEGY_LIST[@]}"; do
            for ((x = 1; x <= RUNS; x++)); do
                RUN_ID="${POOL_SET_NAME}-${REPLACE_STRATEGY}-${x}"
                OUT_FILE="${OUTPUT_DIR}/${DATA_FILE_NAME}-${RUN_ID}.json"
                if [ -f "${OUT_FILE}" ]; then
                    echo "=== ${DATA_FILE_NAME} ${POOL_SET_NAME} (pool_size=${POOL_SIZE}) replace=${REPLACE_STRATEGY} run ${x}/${RUNS}: already have ${OUT_FILE}, skipping ==="
                    continue
                fi
                SEED=$(( x * 100 ))
                echo "=== ${DATA_FILE_NAME} ${POOL_SET_NAME} (pool_size=${POOL_SIZE}) replace=${REPLACE_STRATEGY} run ${x}/${RUNS}: master-slave, ${NUM_WORKERS} MPI ranks (1 master + $((NUM_WORKERS - 1)) workers), base seed ${SEED} ==="
                NUM_WORKERS="${NUM_WORKERS}" SEED="${SEED}" MAX_EVALUATIONS="${MAX_EVALUATIONS}" \
                    ADAPTIVE_PULL_ELITE_SEGMENTS="${ADAPTIVE_PULL_ELITE_SEGMENTS}" \
                    ELITE_POOL_SIZE="${POOL_SIZE}" \
                    ELITE_REPLACE_STRATEGY="${REPLACE_STRATEGY}" \
                    OUTPUTS_DIR="${OUTPUT_DIR}" \
                    RUN_ID="${RUN_ID}" \
                    bash "${RUN_SCRIPT}" "${DATA_FILE}"
                sleep "${SLEEP_SEC}"
            done
        done
    done
done

echo "Done. Results in ${OUTPUT_DIR}"
