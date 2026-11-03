#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="${SCRIPT_DIR}/.."
BUILD_DIR="${PROJECT_DIR}/build"

cd "${PROJECT_DIR}"

DEFAULT_DATA_PREFIX="${DEFAULT_DATA_PREFIX:-200}"
PROBLEM_FILE="${1:-${SCRIPT_DIR}/../../data/${DEFAULT_DATA_PREFIX}.10.2.txt}"

ADAPTIVE_ITERATIONS="${ADAPTIVE_ITERATIONS:-60}"
ADAPTIVE_PULL_ELITE_SEGMENTS="${ADAPTIVE_PULL_ELITE_SEGMENTS:-4}"
ELITE_PULL_STRATEGY="${ELITE_PULL_STRATEGY:-rank}"
ELITE_PULL_ACCEPT_STRATEGY="${ELITE_PULL_ACCEPT_STRATEGY:-selective}"
ELITE_PUSH_STRATEGY="${ELITE_PUSH_STRATEGY:-significant-best}"
ELITE_PULL_QUALITY_TOLERANCE_PCT="${ELITE_PULL_QUALITY_TOLERANCE_PCT:-1.0}"
ELITE_POOL_FACTOR="${ELITE_POOL_FACTOR:-0.036}"
ELITE_POOL_SIZE="${ELITE_POOL_SIZE:-0}"
ELITE_REPLACE_STRATEGY="${ELITE_REPLACE_STRATEGY:-edge-crowding}"
# Threads of a run, master included (as the former `mpirun -np`): the
# program gets --workers NUM_WORKERS-1. 1 runs a plain sequential search.
NUM_WORKERS="${NUM_WORKERS:-7}"
# CPU list for --cpus, e.g. "0,2,4,6,8,10,12": worker r on the r-th CPU,
# the master on the next one if any. Empty: no pinning.
CPUS="${CPUS:-}"
OUTPUTS_DIR="${OUTPUTS_DIR:-outputs}"
COMPACT_OUTPUT="${COMPACT_OUTPUT:-1}"
RUN_ID="${RUN_ID:-}"
SEED="${SEED:-}"
TIME_LIMIT="${TIME_LIMIT:-}"
MAX_EVALUATIONS="${MAX_EVALUATIONS:-}"

if [ "${NUM_WORKERS}" -lt 1 ]; then
  echo "NUM_WORKERS must be at least 1 (master included)" >&2
  exit 1
fi

CMD=(
  "${BUILD_DIR}/master_slave" run
  "${PROBLEM_FILE}"
  --workers "$(( NUM_WORKERS - 1 ))"
  --adaptive-iterations "${ADAPTIVE_ITERATIONS}"
  --adaptive-pull-elite-segments "${ADAPTIVE_PULL_ELITE_SEGMENTS}"
  --elite-pull-strategy "${ELITE_PULL_STRATEGY}"
  --elite-pull-accept-strategy "${ELITE_PULL_ACCEPT_STRATEGY}"
  --elite-push-strategy "${ELITE_PUSH_STRATEGY}"
  --elite-pull-quality-tolerance-pct "${ELITE_PULL_QUALITY_TOLERANCE_PCT}"
  --elite-pool-factor "${ELITE_POOL_FACTOR}"
  --elite-pool-size "${ELITE_POOL_SIZE}"
  --elite-replace-strategy "${ELITE_REPLACE_STRATEGY}"
  --outputs "${OUTPUTS_DIR}"
)

if [ "${COMPACT_OUTPUT}" = "1" ]; then
  CMD+=(--compact-output)
fi

if [ -n "${RUN_ID}" ]; then
  CMD+=(--run-id "${RUN_ID}")
fi

if [ -n "${SEED}" ]; then
  CMD+=(--seed "${SEED}")
fi

if [ -n "${TIME_LIMIT}" ]; then
  CMD+=(--time-limit "${TIME_LIMIT}")
fi

if [ -n "${MAX_EVALUATIONS}" ]; then
  CMD+=(--max-evaluations "${MAX_EVALUATIONS}")
fi

if [ -n "${CPUS}" ]; then
  CMD+=(--cpus "${CPUS}")
fi

"${CMD[@]}"