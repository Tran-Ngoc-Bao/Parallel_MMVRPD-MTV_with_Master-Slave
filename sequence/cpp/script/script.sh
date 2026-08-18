#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="${SCRIPT_DIR}/.."
BUILD_DIR="${PROJECT_DIR}/build"

DEFAULT_DATA_PREFIX="200"
PROBLEM_FILE="${1:-${PROJECT_DIR}/../../data/${DEFAULT_DATA_PREFIX}.10.2.txt}"

OUTPUTS_DIR="${OUTPUTS_DIR:-${PROJECT_DIR}/outputs}"
COMPACT_OUTPUT="${COMPACT_OUTPUT:-1}"
RUN_ID="${RUN_ID:-}"
SEED="${SEED:-}"
TIME_LIMIT="${TIME_LIMIT:-}"

case "${PROBLEM_FILE}" in
  /*) ;;
  *) PROBLEM_FILE="$(pwd)/${PROBLEM_FILE}" ;;
esac
case "${OUTPUTS_DIR}" in
  /*) ;;
  *) OUTPUTS_DIR="$(pwd)/${OUTPUTS_DIR}" ;;
esac

CMD=(
  "${BUILD_DIR}/sequence" run
  "${PROBLEM_FILE}"
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

cd "${PROJECT_DIR}"
"${CMD[@]}"
