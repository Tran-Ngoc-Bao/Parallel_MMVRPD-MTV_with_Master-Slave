#!/usr/bin/env bash
set -euo pipefail

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
  OUTPUTS_DIR="${SCRIPT_DIR}/../outputs/num-workers/${strat}-factor${minpull}-np${workers}" \
  bash "${BENCHMARK_RUN}" </dev/null
done <<'EOF'
200 10 4 topk 1.0 0.06 fixed 1 4
200 10 4 topk 1.0 0.06 fixed 1 5
200 10 4 topk 1.0 0.06 fixed 1 6
200 10 4 topk 1.0 0.06 fixed 1 7
200 10 4 topk 1.0 0.06 fixed 1 8
200 10 4 topk 1.0 0.06 fixed 1 9
200 10 4 topk 1.0 0.06 fixed 1 10
200 10 4 rank 1.0 0.06 fixed 1 4
200 10 4 rank 1.0 0.06 fixed 1 5
200 10 4 rank 1.0 0.06 fixed 1 6
200 10 4 rank 1.0 0.06 fixed 1 7
200 10 4 rank 1.0 0.06 fixed 1 8
200 10 4 rank 1.0 0.06 fixed 1 9
200 10 4 rank 1.0 0.06 fixed 1 10
EOF