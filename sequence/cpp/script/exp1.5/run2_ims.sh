#!/usr/bin/env bash
set -euo pipefail
export LC_NUMERIC=C

# worker w, run x -> seed = x*100000 + 1000 + w
#   run x=1: worker 1 = 101001, worker 2 = 101002, worker 3 = 101003, ...
#   run x=2: worker 1 = 201001, worker 2 = 201002, worker 3 = 201003, ...

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RUN_SCRIPT="${SCRIPT_DIR}/../script.sh"
PICK_BEST="${SCRIPT_DIR}/../pick_best.py"

DEFAULT_DATA_PREFIX="$(sed -n 's/^DEFAULT_DATA_PREFIX="\([^"]\+\)"$/\1/p' "${RUN_SCRIPT}" | head -n 1)"
DATA_PREFIX="${1:-${DEFAULT_DATA_PREFIX}}"
RUNS="${2:-5}"
SLEEP_SEC="${3:-0.0}"
NUM_WORKERS="${4:-6}"
MAX_EVALUATIONS="${5:-0}"
ADAPTIVE_ITERATIONS="${6:-}"

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

OUTPUT_DIR="${OUTPUTS_DIR:-${SCRIPT_DIR}/../../outputs/exp1.5/${DATA_PREFIX}}"
mkdir -p "${OUTPUT_DIR}"

for DATA_FILE in "${DATA_FILES[@]}"; do
    DATA_FILE_NAME="$(basename "${DATA_FILE}" .txt)"
    for ((x = 1; x <= RUNS; x++)); do
        echo "=== ${DATA_FILE_NAME} run ${x}/${RUNS}: ${NUM_WORKERS} parallel workers (island model) ==="
        WORKER_DIR="$(mktemp -d)"
        PIDS=()

        for ((w = 1; w <= NUM_WORKERS; w++)); do
            SEED=$(( x * 100000 + 1000 + w ))
            (
                RUN_ID="w${w}" SEED="${SEED}" MAX_EVALUATIONS="${MAX_EVALUATIONS}" \
                    ADAPTIVE_ITERATIONS="${ADAPTIVE_ITERATIONS}" OUTPUTS_DIR="${WORKER_DIR}" \
                    bash "${RUN_SCRIPT}" "${DATA_FILE}" \
                    > "${WORKER_DIR}/w${w}.log" 2>&1
            ) &
            PIDS+=($!)
            echo "    worker ${w}: seed ${SEED} (pid ${PIDS[-1]})"
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
        for ((w = 1; w <= NUM_WORKERS; w++)); do
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
