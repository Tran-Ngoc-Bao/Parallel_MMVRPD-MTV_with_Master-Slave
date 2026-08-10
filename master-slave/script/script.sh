#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="${SCRIPT_DIR}/.."
BUILD_DIR="${PROJECT_DIR}/build"

cd "${PROJECT_DIR}"

DEFAULT_DATA_PREFIX="${DEFAULT_DATA_PREFIX:-200}"
PROBLEM_FILE="${1:-${SCRIPT_DIR}/../../data/${DEFAULT_DATA_PREFIX}.10.2.txt}"

ADAPTIVE_ITERATIONS="${ADAPTIVE_ITERATIONS:-10}"
ADAPTIVE_PULL_ELITE_SEGMENTS="${ADAPTIVE_PULL_ELITE_SEGMENTS:-4}"
ELITE_PULL_STRATEGY="${ELITE_PULL_STRATEGY:-topk}"
MIN_PULL_ELITES_PER_WORKER_FACTOR="${MIN_PULL_ELITES_PER_WORKER_FACTOR:-1.0}"
ELITE_POOL_FACTOR="${ELITE_POOL_FACTOR:-0.06}"
WORKER_HYPERPARAMS="${WORKER_HYPERPARAMS:-fixed}"
PREFER_PULLED="${PREFER_PULLED:-1}"
NUM_WORKERS="${NUM_WORKERS:-4}"
OUTPUTS_DIR="${OUTPUTS_DIR:-outputs}"
COMPACT_OUTPUT="${COMPACT_OUTPUT:-1}"
RUN_ID="${RUN_ID:-}"
SEED="${SEED:-}"
MAX_EVALUATIONS="${MAX_EVALUATIONS:-}"

CMD=(
  mpirun --allow-run-as-root -np "${NUM_WORKERS}"
  "${BUILD_DIR}/tabu_search" run
  "${PROBLEM_FILE}"
  --adaptive-iterations "${ADAPTIVE_ITERATIONS}"
  --adaptive-pull-elite-segments "${ADAPTIVE_PULL_ELITE_SEGMENTS}"
  --elite-pull-strategy "${ELITE_PULL_STRATEGY}"
  --min-pull-elites-per-worker-factor "${MIN_PULL_ELITES_PER_WORKER_FACTOR}"
  --elite-pool-factor "${ELITE_POOL_FACTOR}"
  --outputs "${OUTPUTS_DIR}"
)

if [ "${WORKER_HYPERPARAMS}" != "fixed" ]; then
  CMD+=(--worker-hyperparams "${WORKER_HYPERPARAMS}")
fi

if [ "${PREFER_PULLED}" = "1" ]; then
  CMD+=(--prefer-pulled)
fi

if [ "${COMPACT_OUTPUT}" = "1" ]; then
  CMD+=(--compact-output)
fi

if [ -n "${RUN_ID}" ]; then
  CMD+=(--run-id "${RUN_ID}")
fi

if [ -n "${SEED}" ]; then
  CMD+=(--seed "${SEED}")
fi

if [ -n "${MAX_EVALUATIONS}" ]; then
  CMD+=(--max-evaluations "${MAX_EVALUATIONS}")
fi

"${CMD[@]}"