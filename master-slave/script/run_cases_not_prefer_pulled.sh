#!/usr/bin/env bash
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BENCHMARK_RUN="${SCRIPT_DIR}/benchmark_runs.sh"

while read -r prefix ai seg strat minpull pool rand prefer_pulled workers; do
  echo "RUN CASE: $prefix $ai $seg $strat $minpull $pool $rand $prefer_pulled $workers"

  DEFAULT_DATA_PREFIX="$prefix" \
  ADAPTIVE_ITERATIONS="$ai" \
  ADAPTIVE_PULL_ELITE_SEGMENTS="$seg" \
  ELITE_PULL_STRATEGY="$strat" \
  MIN_PULL_ELITES_PER_WORKER_FACTOR="$minpull" \
  ELITE_POOL_FACTOR="$pool" \
  WORKER_HYPERPARAMS="$rand" \
  PREFER_PULLED="$prefer_pulled" \
  NUM_WORKERS="$workers" \
  OUTPUTS_DIR="${SCRIPT_DIR}/../outputs/not-prefer-pulled/${ai}-${seg}-${pool}" \
  bash "${BENCHMARK_RUN}" </dev/null
done <<'EOF'
200 8 3 random 0.6 0.02 fixed 0 10
200 8 3 random 0.6 0.06 fixed 0 10
200 8 3 random 0.6 0.10 fixed 0 10
200 8 4 random 0.6 0.02 fixed 0 10
200 8 4 random 0.6 0.06 fixed 0 10
200 8 4 random 0.6 0.10 fixed 0 10
200 8 5 random 0.6 0.02 fixed 0 10
200 8 5 random 0.6 0.06 fixed 0 10
200 8 5 random 0.6 0.10 fixed 0 10
200 10 3 random 0.6 0.02 fixed 0 10
200 10 3 random 0.6 0.06 fixed 0 10
200 10 3 random 0.6 0.10 fixed 0 10
200 10 4 random 0.6 0.02 fixed 0 10
200 10 4 random 0.6 0.06 fixed 0 10
200 10 4 random 0.6 0.10 fixed 0 10
200 10 5 random 0.6 0.02 fixed 0 10
200 10 5 random 0.6 0.06 fixed 0 10
200 10 5 random 0.6 0.10 fixed 0 10
200 12 3 random 0.6 0.02 fixed 0 10
200 12 3 random 0.6 0.06 fixed 0 10
200 12 3 random 0.6 0.10 fixed 0 10
200 12 4 random 0.6 0.02 fixed 0 10
200 12 4 random 0.6 0.06 fixed 0 10
200 12 4 random 0.6 0.10 fixed 0 10
200 12 5 random 0.6 0.02 fixed 0 10
200 12 5 random 0.6 0.06 fixed 0 10
200 12 5 random 0.6 0.10 fixed 0 10
EOF