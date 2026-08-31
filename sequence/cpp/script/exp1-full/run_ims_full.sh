#!/usr/bin/env bash
set -euo pipefail
export LC_NUMERIC=C

# Same as ../exp1/run4_ims.sh, but instead of only the diagonal combos
# (10.1/20.2/30.3/40.4) runs every "<DATA_PREFIX>.<depots>.<variant>.txt"
# instance actually present in data/ for the given customer count (12
# combos for n=6/10/12/20, 16 combos for n=50/100/200/500/1000).
#
# Worker seed formula: worker w, run x -> seed = x*100 + w
#   run x=1: worker 1 = 101, worker 2 = 102, ...
#   run x=2: worker 1 = 201, worker 2 = 202, ...

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

OUTPUT_DIR="${OUTPUTS_DIR:-${SCRIPT_DIR}/../../outputs/exp1-full/ims/${DATA_PREFIX}}"
mkdir -p "${OUTPUT_DIR}"

for DATA_FILE in "${DATA_FILES[@]}"; do
    DATA_FILE_NAME="$(basename "${DATA_FILE}" .txt)"
    for ((x = 1; x <= RUNS; x++)); do
        OUT_FILE="${OUTPUT_DIR}/${DATA_FILE_NAME}-${x}.json"
        if [ -f "${OUT_FILE}" ]; then
            echo "=== ${DATA_FILE_NAME} run ${x}/${RUNS}: already have ${OUT_FILE}, skipping ==="
            continue
        fi

        echo "=== ${DATA_FILE_NAME} run ${x}/${RUNS}: ${NUM_WORKERS} parallel workers (island model) ==="
        WORKER_DIR="$(mktemp -d)"
        PIDS=()

        for ((w = 0; w < NUM_WORKERS; w++)); do
            SEED=$(( x * 100 + 1 + w ))
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
        # Copy the best worker's JSON, then fold in island-wide aggregates
        # over every worker (all workers have finished by here):
        #  - total_evaluations_all_workers : sum of each worker's evals
        #  - best_solution_cost_by_time_checkpoint_all_workers : per time
        #    checkpoint, the best (min) cost any worker had reached by then
        # ("total_evaluations" / "best_solution_cost_by_time_checkpoint" in
        # the copy stay the best worker's own.)
        python3 - "${OUT_FILE}" "${BEST_FILE}" "${WORKER_FILES[@]}" <<'PY'
import json, sys
out_file, best_file, *worker_files = sys.argv[1:]
with open(best_file) as f:
    result = json.load(f)

total, iters, n = 0, 0, 0
cp_series = []
for wf in worker_files:
    with open(wf) as f:
        wd = json.load(f)
    te = wd.get("total_evaluations")
    if te is not None:
        total += te
        iters += wd.get("iterations", 0)
        n += 1
    s = wd.get("best_solution_cost_by_time_checkpoint") or []
    if s:
        cp_series.append(s)

result["total_evaluations_all_workers"] = total
result["iterations_all_workers"] = iters
result["num_workers"] = n
if cp_series:
    length = min(len(s) for s in cp_series)
    result["best_solution_cost_by_time_checkpoint_all_workers"] = [
        min(s[i] for s in cp_series) for i in range(length)
    ]

with open(out_file, "w") as f:
    json.dump(result, f)
PY
        echo "    best of ${#WORKER_FILES[@]} workers: working_time=${BEST_WT} -> ${OUT_FILE}"

        rm -rf "${WORKER_DIR}"
        sleep "${SLEEP_SEC}"
    done
done

echo "Done. Results in ${OUTPUT_DIR}"
