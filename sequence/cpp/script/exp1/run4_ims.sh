#!/usr/bin/env bash
set -euo pipefail
export LC_NUMERIC=C

# Worker seed formula: worker w, run x -> seed = w*10000 + 1000 + x
#   run x=1: worker 0 = 1001, worker 1 = 11001, worker 2 = 21001, ...
#   run x=2: worker 0 = 1002, worker 1 = 11002, worker 2 = 21002, ...

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RUN_SCRIPT="${SCRIPT_DIR}/../script.sh"
PICK_BEST="${SCRIPT_DIR}/../pick_best.py"

DEFAULT_DATA_PREFIX="$(sed -n 's/^DEFAULT_DATA_PREFIX="\([^"]\+\)"$/\1/p' "${RUN_SCRIPT}" | head -n 1)"
DATA_PREFIX="${1:-${DEFAULT_DATA_PREFIX}}"
RUNS="${2:-10}"
SLEEP_SEC="${3:-0.0}"
# i5-14400F P-core hyperthread pairs are 0-1,2-3,4-5,6-7,8-9,10-11;
# E-cores are 12-15. Default below picks all 6 P-cores.
CPU_CORES="${4:-0,2,4,6,8,10}"
TIME_LIMIT="${5:-0.0}"

IFS=',' read -r -a CPU_CORE_LIST <<< "${CPU_CORES}"
NUM_WORKERS=${#CPU_CORE_LIST[@]}

DATA_DIR="${SCRIPT_DIR}/../../../../data"
DATA_FILES=()
for COMBO in "10.1" "20.2" "30.3" "40.4"; do
    DATA_FILE="${DATA_DIR}/${DATA_PREFIX}.${COMBO}.txt"
    if [ -f "${DATA_FILE}" ]; then
        DATA_FILES+=("${DATA_FILE}")
    fi
done

if [ ${#DATA_FILES[@]} -eq 0 ]; then
    echo "No data files found for prefix ${DATA_PREFIX}" >&2
    exit 1
fi

OUTPUT_DIR="${OUTPUTS_DIR:-${SCRIPT_DIR}/../../outputs/exp1/ims/${DATA_PREFIX}}"
mkdir -p "${OUTPUT_DIR}"

for DATA_FILE in "${DATA_FILES[@]}"; do
    DATA_FILE_NAME="$(basename "${DATA_FILE}" .txt)"
    for ((x = 1; x <= RUNS; x++)); do
        echo "=== ${DATA_FILE_NAME} run ${x}/${RUNS}: ${NUM_WORKERS} parallel workers (island model) ==="
        WORKER_DIR="$(mktemp -d)"
        PIDS=()

        for ((w = 0; w < NUM_WORKERS; w++)); do
            SEED=$(( w * 10000 + 1000 + x ))
            CPU_CORE="${CPU_CORE_LIST[$w]}"
            (
                RUN_ID="w${w}" SEED="${SEED}" TIME_LIMIT="${TIME_LIMIT}" OUTPUTS_DIR="${WORKER_DIR}" \
                    taskset -c "${CPU_CORE}" bash "${RUN_SCRIPT}" "${DATA_FILE}" \
                    > "${WORKER_DIR}/w${w}.log" 2>&1
            ) &
            PIDS+=($!)
            echo "    worker ${w}: core ${CPU_CORE}, seed ${SEED} (pid ${PIDS[-1]})"
        done

        FAILED=0
        for PID in "${PIDS[@]}"; do
            if ! wait "${PID}"; then
                FAILED=$((FAILED + 1))
            fi
        done
        if [ "${FAILED}" -gt 0 ]; then
            echo "    WARNING: ${FAILED}/${NUM_WORKERS} worker(s) failed, see ${WORKER_DIR}/w*.log" >&2
        fi

        WORKER_FILES=()
        for ((w = 0; w < NUM_WORKERS; w++)); do
            F="${WORKER_DIR}/${DATA_FILE_NAME}-w${w}.json"
            [ -f "${F}" ] && WORKER_FILES+=("${F}")
        done

        if [ ${#WORKER_FILES[@]} -eq 0 ]; then
            echo "    ERROR: no worker produced output for run ${x}, skipping" >&2
            rm -rf "${WORKER_DIR}"
            continue
        fi

        BEST_FILE="$(python3 "${PICK_BEST}" "${WORKER_FILES[@]}")"
        BEST_WT="$(python3 -c "import json;print(json.load(open('${BEST_FILE}'))['solution']['working_time'])")"
        cp "${BEST_FILE}" "${OUTPUT_DIR}/${DATA_FILE_NAME}-${x}.json"
        echo "    best of ${#WORKER_FILES[@]} workers: working_time=${BEST_WT} -> ${OUTPUT_DIR}/${DATA_FILE_NAME}-${x}.json"

        rm -rf "${WORKER_DIR}"
        sleep "${SLEEP_SEC}"
    done
done

echo "Done. Results in ${OUTPUT_DIR}"
