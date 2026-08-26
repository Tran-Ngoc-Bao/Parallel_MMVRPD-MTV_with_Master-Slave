#!/usr/bin/env bash
set -euo pipefail
export LC_NUMERIC=C

# exp1-full for master-slave: runs the (single) master-slave cooperative
# search across every "<DATA_PREFIX>.<depots>.<variant>.txt" instance
# actually present in data/ for the given customer count (12 combos for
# n=6/10/12/20, 16 combos for n=50/100/200/500/1000) -- see
# ../../../sequence/cpp/script/exp1-full/run4_ims_full.sh for the sequence
# equivalent.
#
# Unlike sequence/cpp's island model (N independent OS processes, best of
# N picked afterwards), one master-slave run already IS a full cooperative
# pool of MPI ranks (1 master + NUM_WORKERS-1 slaves) producing a single
# result, so there is no separate pick_best step here.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RUN_SCRIPT="${SCRIPT_DIR}/../script.sh"

DEFAULT_DATA_PREFIX="$(sed -n 's/^DEFAULT_DATA_PREFIX="\${DEFAULT_DATA_PREFIX:-\([^\}]\+\)}"$/\1/p' "${RUN_SCRIPT}" | head -n 1)"
DATA_PREFIX="${1:-${DEFAULT_DATA_PREFIX}}"
RUNS="${2:-10}"
SLEEP_SEC="${3:-0.0}"
NUM_WORKERS="${4:-7}"
TIME_LIMIT="${5:-0.0}"

DATA_DIR="${SCRIPT_DIR}/../../../data"
shopt -s nullglob
DATA_FILES=("${DATA_DIR}/${DATA_PREFIX}".*.*.txt)
shopt -u nullglob
if [ ${#DATA_FILES[@]} -gt 1 ]; then
    mapfile -t DATA_FILES < <(printf '%s\n' "${DATA_FILES[@]}" | sort -V)
fi

if [ ${#DATA_FILES[@]} -eq 0 ]; then
    echo "No data files found for prefix ${DATA_PREFIX}" >&2
    exit 1
fi

OUTPUT_DIR="${OUTPUTS_DIR:-${SCRIPT_DIR}/../../outputs/exp1-full/ms/${DATA_PREFIX}}"
mkdir -p "${OUTPUT_DIR}"

for DATA_FILE in "${DATA_FILES[@]}"; do
    DATA_FILE_NAME="$(basename "${DATA_FILE}" .txt)"
    for ((x = 1; x <= RUNS; x++)); do
        OUT_FILE="${OUTPUT_DIR}/${DATA_FILE_NAME}-${x}.json"
        if [ -f "${OUT_FILE}" ]; then
            echo "=== ${DATA_FILE_NAME} run ${x}/${RUNS}: already have ${OUT_FILE}, skipping ==="
            continue
        fi

        SEED=$(( x * 100 ))
        echo "=== ${DATA_FILE_NAME} run ${x}/${RUNS}: master-slave, ${NUM_WORKERS} MPI ranks (1 master + $((NUM_WORKERS - 1)) workers), seed ${SEED} ==="
        NUM_WORKERS="${NUM_WORKERS}" SEED="${SEED}" TIME_LIMIT="${TIME_LIMIT}" \
            OUTPUTS_DIR="${OUTPUT_DIR}" RUN_ID="${x}" \
            bash "${RUN_SCRIPT}" "${DATA_FILE}"

        sleep "${SLEEP_SEC}"
    done
done

echo "Done. Results in ${OUTPUT_DIR}"
