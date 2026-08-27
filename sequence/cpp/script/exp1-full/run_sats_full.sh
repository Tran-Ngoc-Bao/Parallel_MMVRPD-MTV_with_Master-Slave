#!/usr/bin/env bash
set -euo pipefail
export LC_NUMERIC=C

# Same as ../exp1/run4_sats.sh (single-run "standalone" pipeline, no
# island model), but instead of only the diagonal combos
# (10.1/20.2/30.3/40.4) runs every "<DATA_PREFIX>.<depots>.<variant>.txt"
# instance actually present in data/ for the given customer count.
#
# Missing instances are silently skipped; an already-computed run
# (<name>-<x>.json in the output dir) is skipped too, so the script is
# resumable.
#
# Seed formula: run i -> seed = i*100 + 1, i.e. the same seed worker 0
# (the first island) gets in run_ims_full.sh for the same run index.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RUN_SCRIPT="${SCRIPT_DIR}/../script.sh"

DEFAULT_DATA_PREFIX="$(sed -n 's/^DEFAULT_DATA_PREFIX="\([^"]\+\)"$/\1/p' "${RUN_SCRIPT}" | head -n 1)"
DATA_PREFIX="${1:-${DEFAULT_DATA_PREFIX}}"
RUNS="${2:-10}"
SLEEP_SEC="${3:-0.0}"
# i5-14400F: P-core hyperthread pairs are 0-1,2-3,4-5,6-7,8-9,10-11; E-cores are 12-15.
CPU_CORES="${4:-0,2,4,6,8,10}"
TIME_LIMIT="${5:-0.0}"

IFS=',' read -r -a CPU_CORE_LIST <<< "${CPU_CORES}"
NUM_CORES=${#CPU_CORE_LIST[@]}

DATA_DIR="${SCRIPT_DIR}/../../../../data"
shopt -s nullglob
DATA_FILES=("${DATA_DIR}/${DATA_PREFIX}".*.*.txt)
shopt -u nullglob
if [ ${#DATA_FILES[@]} -gt 1 ]; then
    mapfile -t DATA_FILES < <(printf '%s\n' "${DATA_FILES[@]}" | sort -V)
fi

if [ ${#DATA_FILES[@]} -eq 0 ]; then
    echo "No data files found for prefix ${DATA_PREFIX}, skipping" >&2
    exit 0
fi

OUTPUT_DIR="${OUTPUTS_DIR:-${SCRIPT_DIR}/../../outputs/exp1-full/sats/${DATA_PREFIX}}"
mkdir -p "${OUTPUT_DIR}"

for DATA_FILE in "${DATA_FILES[@]}"; do
    DATA_FILE_NAME="$(basename "${DATA_FILE}" .txt)"
    for ((i=1; i<=RUNS; i++)); do
        OUT_FILE="${OUTPUT_DIR}/${DATA_FILE_NAME}-${i}.json"
        if [ -f "${OUT_FILE}" ]; then
            echo "=== ${DATA_FILE_NAME} run ${i}/${RUNS}: already have ${OUT_FILE}, skipping ==="
            continue
        fi

        SEED=$(( i * 100 + 1 ))
        CPU_CORE="${CPU_CORE_LIST[$(( (i-1) % NUM_CORES ))]}"
        echo "=== ${DATA_FILE_NAME} run ${i}/${RUNS} (CPU ${CPU_CORE}, seed ${SEED}, time-limit ${TIME_LIMIT}) ==="
        RUN_ID="${i}" SEED="${SEED}" TIME_LIMIT="${TIME_LIMIT}" OUTPUTS_DIR="${OUTPUT_DIR}" \
            taskset -c "${CPU_CORE}" bash "${RUN_SCRIPT}" "${DATA_FILE}"
        sleep "${SLEEP_SEC}"
    done
done

echo "Done. Results in ${OUTPUT_DIR}"
