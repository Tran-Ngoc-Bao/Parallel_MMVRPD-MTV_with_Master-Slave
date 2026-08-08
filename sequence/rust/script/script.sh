#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="${SCRIPT_DIR}/.."
BIN="${PROJECT_DIR}/target/release/sequence"

DEFAULT_DATA_PREFIX="200"
PROBLEM_FILE="${1:-${PROJECT_DIR}/../../data/${DEFAULT_DATA_PREFIX}.40.4.txt}"

OUTPUTS_DIR="${OUTPUTS_DIR:-${PROJECT_DIR}/outputs}"
COMPACT_OUTPUT="${COMPACT_OUTPUT:-1}"
RUN_ID="${RUN_ID:-}"

case "${PROBLEM_FILE}" in
  /*) ;;
  *) PROBLEM_FILE="$(pwd)/${PROBLEM_FILE}" ;;
esac
case "${OUTPUTS_DIR}" in
  /*) ;;
  *) OUTPUTS_DIR="$(pwd)/${OUTPUTS_DIR}" ;;
esac

CMD=(
  "${BIN}" run
  "${PROBLEM_FILE}"
  --outputs "${OUTPUTS_DIR}"
)

if [ "${COMPACT_OUTPUT}" = "1" ]; then
  CMD+=(--compact-output)
fi

if [ -n "${RUN_ID}" ]; then
  CMD+=(--run-id "${RUN_ID}")
fi

cd "${PROJECT_DIR}"
"${CMD[@]}"
