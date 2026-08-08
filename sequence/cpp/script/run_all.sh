#!/usr/bin/env bash
set -euo pipefail
export LC_NUMERIC=C

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RUN_SCRIPT="${SCRIPT_DIR}/script.sh"

DEFAULT_DATA_PREFIX="$(sed -n 's/^DEFAULT_DATA_PREFIX="\([^"]\+\)"$/\1/p' "${RUN_SCRIPT}" | head -n 1)"
DATA_PREFIX="${1:-${DEFAULT_DATA_PREFIX}}"
RUNS="${2:-5}"
SLEEP_SEC="${3:-0.0}"

DATA_DIR="${SCRIPT_DIR}/../../../data"
DATA_FILES=()
for DATA_FILE in "${DATA_DIR}/${DATA_PREFIX}".*.*.txt; do
    if [ -f "${DATA_FILE}" ]; then
        DATA_FILES+=("${DATA_FILE}")
    fi
done

if [ ${#DATA_FILES[@]} -eq 0 ]; then
    echo "No data files found for prefix ${DATA_PREFIX}" >&2
    exit 1
fi

OUTPUT_DIR="${OUTPUTS_DIR:-${SCRIPT_DIR}/../outputs/${DATA_PREFIX}}"
mkdir -p "${OUTPUT_DIR}"

STAMP="$(TZ='Asia/Bangkok' date +%Y%m%d-%H%M%S)"
CSV_FILE="${OUTPUT_DIR}/benchmark-${DATA_PREFIX}-${STAMP}.csv"
SUMMARY_FILE="${OUTPUT_DIR}/benchmark-${DATA_PREFIX}-${STAMP}-summary.txt"

printf "data_file,run,result,timing_total_sec\n" > "${CSV_FILE}"

for DATA_FILE in "${DATA_FILES[@]}"; do
    DATA_FILE_NAME="$(basename "${DATA_FILE}" .txt)"
    for ((i=1; i<=RUNS; i++)); do
        TMP_LOG="$(mktemp)"
        if ! RUN_ID="${i}" OUTPUTS_DIR="${OUTPUT_DIR}" bash "${RUN_SCRIPT}" "${DATA_FILE}" > "${TMP_LOG}" 2>&1; then
            echo "Error: run failed for ${DATA_FILE_NAME} (run ${i}). Output:" >&2
            cat "${TMP_LOG}" >&2
            rm -f "${TMP_LOG}"
            exit 1
        fi

        RESULT="$(sed -n 's/^Result = \([0-9.][0-9.]*\)$/\1/p' "${TMP_LOG}" | tail -n 1 | awk '{printf "%.6f", $1 / 60}')"
        TOTAL_TIME="$(sed -n 's/^Timing .* total=\([0-9.][0-9.]*\)$/\1/p' "${TMP_LOG}" | tail -n 1 | awk '{printf "%.3f", $1}')"

        rm -f "${TMP_LOG}"

        printf "%s,%d,%s,%s\n" "${DATA_FILE_NAME}" "${i}" "${RESULT}" "${TOTAL_TIME}" >> "${CSV_FILE}"
        sleep "${SLEEP_SEC}"
    done
done

AVG_RESULT="$(awk -F, 'NR>1 {sum+=$3; n++} END {if (n>0) printf "%.6f", sum/n; else print "NaN"}' "${CSV_FILE}")"
AVG_TIME="$(awk -F, 'NR>1 {sum+=$4; n++} END {if (n>0) printf "%.2f", sum/n; else print "NaN"}' "${CSV_FILE}")"

BEST_INFO="$(awk -F, '
NR>1 {
    f=$1; r=$2+0; val=$3+0; t=$4+0
    if ((f in minval) == 0) { minval[f]=val; mintime[f]=t; minrun[f]=r }
    else if (val < minval[f]) { minval[f]=val; mintime[f]=t; minrun[f]=r }
}
END {
    sum=0; sumt=0; m=0
    for (f in minval) { sum += minval[f]; sumt += mintime[f]; m++ }
    if (m>0) printf "%d,%.6f,%.2f", m, sum/m, sumt/m
}' "${CSV_FILE}")"

BEST_INSTANCES="$(awk -F, '
NR>1 {
    f=$1; r=$2+0; val=$3+0
    if ((f in minval) == 0) { minval[f]=val; minrun[f]=r }
    else if (val < minval[f])  { minval[f]=val; minrun[f]=r }
    else if (val == minval[f]) { minrun[f]=minrun[f] ", " r }
}
END {
    for (f in minval) print f "\t" minrun[f]
}' "${CSV_FILE}" | sort | awk -F'\t' '{printf "  %-20s run %s\n", $1, $2}')"

FILE_COUNT="${BEST_INFO%%,*}"
REST="${BEST_INFO#*,}"
BEST_RESULT="${REST%%,*}"
BEST_TIME="${REST#*,}"

{
    echo "data_prefix=${DATA_PREFIX}"
    echo "runs=${RUNS}"
    echo "sleep_sec=${SLEEP_SEC}"
    echo "average_result=${AVG_RESULT}"
    echo "average_total_time_sec=${AVG_TIME}"
    echo "best_result=${BEST_RESULT}"
    echo "file_count=${FILE_COUNT}"
    echo "best_result_total_time_sec=${BEST_TIME}"
    printf "best_result_instances=\n%s\n" "${BEST_INSTANCES}"
    echo "csv_file=${CSV_FILE}"
} > "${SUMMARY_FILE}"

STD_INFO="$(awk -F, '
    NR>1 {
        f=$1; v=$3+0; t=$4+0
        rsum[f]+=v; rsumsq[f]+=v*v; rcnt[f]++
        tsum[f]+=t; tsumsq[f]+=t*t
    }
    END {
        n_files=0; sum_rvar=0; sum_tvar=0
        for (f in rcnt) {
            n=rcnt[f]; n_files++
            if (n>1) {
                sum_rvar += (rsumsq[f] - rsum[f]*rsum[f]/n) / (n-1)
                sum_tvar += (tsumsq[f] - tsum[f]*tsum[f]/n) / (n-1)
            }
        }
        if (n_files>0) printf "%.6f,%.6f", sqrt(sum_rvar)/n_files, sqrt(sum_tvar)/n_files
    }' "${CSV_FILE}")"

COMBINED_STD_RESULT="${STD_INFO%%,*}"
COMBINED_STD_TIME="${STD_INFO#*,}"
CV_PERCENT="$(awk -v std="${COMBINED_STD_RESULT}" -v avg="${AVG_RESULT}" 'BEGIN { cv = (avg+0>0) ? std/avg*100 : 0; printf "%.6f", cv }')"

{
    echo "combined_std_result=${COMBINED_STD_RESULT}"
    echo "combined_std_timing_total_sec=${COMBINED_STD_TIME}"
    echo "cv_percent=${CV_PERCENT}"
} >> "${SUMMARY_FILE}"

echo "Average total time (s): ${AVG_TIME}"
echo "Average result: ${AVG_RESULT}"
echo "Best result: ${BEST_RESULT}"
echo "Combined std result: ${COMBINED_STD_RESULT}  (CV%: ${CV_PERCENT})"
echo "Saved: ${CSV_FILE}"
echo "Saved: ${SUMMARY_FILE}"
