#!/usr/bin/env bash
set -euo pipefail
export LC_NUMERIC=C

# exp1-full for master-slave: the (single) master-slave cooperative search,
# across every instance actually present in data/ for every customer count
# from 6 to 1000 (12 combos for n=6/10/12/20, 16 combos for
# n=50/100/200/500/1000) -- see
# ../../../sequence/cpp/script/exp1-full/run_all.sh for the sequence
# equivalent (there it's ims-only; here there's only one pipeline).
#
# Requires BKS files for every instance: run
# ../../../sequence/cpp/script/gen_bks.py once beforehand (bks/ is shared
# between sequence and master-slave) to populate/refresh
# bks/<n>/<n>.<combo>-bks.json from bks/capacity-1400_baseline.xlsx.
#
# Per-n time limits below mirror ../../../sequence/cpp/script/exp1-full/run_all.sh
# for comparability; all are still PLACEHOLDERS (not yet tuned) -- adjust
# as needed.
JOBS=(
    "6     1"
    "10    1"
    "12    1"
    "20    1"
    "50    2"
    "100   10"
    "200   35"
    "500   700"
    # "1000  4500"
)

# Usage:
#   bash run_all.sh [RUNS] [SLEEP_SEC] [NUM_WORKERS]

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FULL_SCRIPT="${SCRIPT_DIR}/run_full.sh"
STATS_SCRIPT="${SCRIPT_DIR}/stats.py"

RUNS="${1:-10}"
SLEEP_SEC="${2:-0.0}"
NUM_WORKERS="${3:-7}"

# n=1000 only needs 5 runs; every other n uses the RUNS above.
RUNS_1000=5

run_stats() {
    local customers_so_far="$1"
    local stage="$2"
    local runs="$3"
    echo
    echo "---- stats after ${stage} (n=${customers_so_far}) ----"
    python3 "${STATS_SCRIPT}" --customers "${customers_so_far}" --runs "${runs}"
}

N_LIST=()
START_TS=$(date +%s)
for JOB in "${JOBS[@]}"; do
    read -r N TIME_LIMIT <<< "${JOB}"
    N_LIST+=("${N}")
    CUSTOMERS_SO_FAR="$(IFS=,; echo "${N_LIST[*]}")"

    JOB_RUNS="${RUNS}"
    if [ "${N}" = "1000" ]; then
        JOB_RUNS="${RUNS_1000}"
    fi

    echo
    echo "################################################################"
    echo "# n=${N}  time_limit=${TIME_LIMIT}s  runs=${JOB_RUNS}  [master-slave]  $(date '+%Y-%m-%d %H:%M:%S')"
    echo "################################################################"
    bash "${FULL_SCRIPT}" "${N}" "${JOB_RUNS}" "${SLEEP_SEC}" "${NUM_WORKERS}" "${TIME_LIMIT}"
    run_stats "${CUSTOMERS_SO_FAR}" "master-slave, n=${N}" "${JOB_RUNS}"
done

ELAPSED=$(( $(date +%s) - START_TS ))
CUSTOMERS="$(IFS=,; echo "${N_LIST[*]}")"
echo
echo "################################################################"
echo "# All done: n=${CUSTOMERS} in ${ELAPSED}s"
echo "################################################################"
