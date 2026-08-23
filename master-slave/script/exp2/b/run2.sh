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
ELITE_PULL_STRATEGY="off"

# Compare hand-picked elite_pool_size values via --elite-pool-size, grouped
# into named "sets" (bo1..bo4). Each set fixes a pool size per customer
# count:
#   bo1: 200->2, 500->3
#   bo2: 200->3, 500->4
#   bo3: 200->3, 500->5
#   bo4: 200->4, 500->5
case "${DATA_PREFIX}" in
    200)
        SET_NAMES=("bo1" "bo2" "bo3" "bo4")
        POOL_SIZES=("2"   "3"   "3"   "4")
        ;;
    500)
        SET_NAMES=("bo1" "bo2" "bo3" "bo4")
        POOL_SIZES=("3"   "4"   "5"   "5")
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

OUTPUT_DIR="${OUTPUTS_DIR:-${SCRIPT_DIR}/../../../outputs/exp2/b/${DATA_PREFIX}}"
mkdir -p "${OUTPUT_DIR}"

for DATA_FILE in "${DATA_FILES[@]}"; do
    DATA_FILE_NAME="$(basename "${DATA_FILE}" .txt)"
    for ((i = 0; i < ${#SET_NAMES[@]}; i++)); do
        SET_NAME="${SET_NAMES[$i]}"
        POOL_SIZE="${POOL_SIZES[$i]}"

        CANONICAL_SET=""
        for ((j = 0; j < i; j++)); do
            if [ "${POOL_SIZES[$j]}" = "${POOL_SIZE}" ]; then
                CANONICAL_SET="${SET_NAMES[$j]}"
                break
            fi
        done

        for ((x = 1; x <= RUNS; x++)); do
            RUN_ID="${SET_NAME}-${x}"
            OUT_FILE="${OUTPUT_DIR}/${DATA_FILE_NAME}-${RUN_ID}.json"
            if [ -f "${OUT_FILE}" ]; then
                echo "=== ${DATA_FILE_NAME} ${SET_NAME} (pool_size=${POOL_SIZE}) run ${x}/${RUNS}: already have ${OUT_FILE}, skipping ==="
                continue
            fi

            if [ -n "${CANONICAL_SET}" ]; then
                CANONICAL_FILE="${OUTPUT_DIR}/${DATA_FILE_NAME}-${CANONICAL_SET}-${x}.json"
                if [ -f "${CANONICAL_FILE}" ]; then
                    echo "=== ${DATA_FILE_NAME} ${SET_NAME} (pool_size=${POOL_SIZE}) run ${x}/${RUNS}: same config as ${CANONICAL_SET}, reusing its result ==="
                    python3 - "${CANONICAL_FILE}" "${OUT_FILE}" "${RUN_ID}" <<'PYEOF'
import json, sys
src, dst, run_id = sys.argv[1], sys.argv[2], sys.argv[3]
with open(src) as f:
    data = json.load(f)
data["config"]["run_id"] = run_id
with open(dst, "w") as f:
    json.dump(data, f)
PYEOF
                    continue
                fi
            fi

            SEED=$(( x * 100 ))
            echo "=== ${DATA_FILE_NAME} ${SET_NAME} (pool_size=${POOL_SIZE}) run ${x}/${RUNS}: master-slave, ${NUM_WORKERS} MPI ranks (1 master + $((NUM_WORKERS - 1)) workers), base seed ${SEED} ==="
            NUM_WORKERS="${NUM_WORKERS}" SEED="${SEED}" MAX_EVALUATIONS="${MAX_EVALUATIONS}" \
                ELITE_PULL_STRATEGY="${ELITE_PULL_STRATEGY}" \
                ELITE_POOL_SIZE="${POOL_SIZE}" \
                OUTPUTS_DIR="${OUTPUT_DIR}" \
                RUN_ID="${RUN_ID}" \
                bash "${RUN_SCRIPT}" "${DATA_FILE}"
            sleep "${SLEEP_SEC}"
        done
    done
done

echo "Done. Results in ${OUTPUT_DIR}"
