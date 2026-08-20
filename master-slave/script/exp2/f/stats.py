#!/usr/bin/env python3
"""
Statistics for run2.sh (exp2/f, elite_replace_strategy comparison).

Compares 4 elite_replace_strategy values (td-crowding, edge-crowding,
quality-only, random-target) -- how the master's elite pool picks a slot
to replace when a pushed elite doesn't fit and the pool is full (see
ElitePool::consider in parallel.cpp). td-crowding and edge-crowding pick
the replacement target the same way (closest structural match to the
incoming candidate, replaced only if significantly better), differing
only in the distance used: td_distance vs edge_distance (Solution::
edge_distance / td_distance in solutions.cpp). All runs use
elite-pull-strategy=rank,
elite-pull-accept-strategy=selective, significant-best push strategy,
elite_pool_factor 0.03, adaptive_pull_elite_segments set to the best
value found in exp2/d, and elite_pull_quality_tolerance_pct set to the
best value found in exp2/e (both overridable via env var in run2.sh,
defaults 8 and 1 respectively until those experiments have concluded).
Reports 3 numbers per replace-strategy value:
  - final_rpd (%): RPD of the final best cost (solution.working_time) vs BKS
  - irpd (%): trapezoidal-rule average of R0..R8 (RPD (%) of
    best_solution_cost_by_evaluation_checkpoint at 0/8..8/8 of the
    evaluation budget vs BKS working_time), (1/8) * [(R0+R8)/2 + R1+...+R7]
  - pool_diversity: plain average of diversity_by_evaluation_checkpoint's
    D1..D8 (skips D0, since the pool is still mostly/all initial-seed
    solutions at that point and its diversity is not yet meaningful)

RPD (%) = (value - BKS) / BKS * 100, same as exp1.5/stats.py. final_rpd
uses the final best cost (solution.working_time); irpd uses the tracked
best_solution_cost_by_evaluation_checkpoint values (the true best solution
found so far at each checkpoint, not the elite pool's own best -- see
exp2/a for that one -- the two can diverge under
elite-replace-strategy=quality-only/random-target), which can carry an
infeasibility penalty at early checkpoints if the best-known solution
isn't yet feasible at that point.

Aggregation: same stratified style throughout for all 3 metrics --
computed per run first (using that run's own solution/checkpoints vs
BKS), then averaged over that instance's runs, then averaged over
instances (4 by default: 2 customer counts x 2 combos). No pooled-sum
exception here (unlike exp2/c/e's accept-rate metrics), since none of
final_rpd/irpd/pool_diversity are ratio-of-counts.

Looks for run files at
<outputs>/<n>/<n>.<combo>-repl<strategy>-<run_id>.json (the naming used
by run2.sh) and BKS files at <bks>/<n>/<n>.<combo>-bks.json.

Usage:
    python3 stats.py
    python3 stats.py --customers 200,500 --combos 10.2,40.1
    python3 stats.py --no-save
"""
import argparse
import csv
import json
import re
import statistics
from pathlib import Path

REPLACE_STRATEGY_LIST = ["td-crowding", "edge-crowding", "quality-only", "random-target"]
NUM_CHECKPOINTS = 9  # R0..R8, at k/8 of the evaluation budget


def load_run(path: Path):
    with path.open() as f:
        return json.load(f)


def load_bks_working_time(path: Path):
    with path.open() as f:
        return json.load(f)["solution"]["working_time"]


def find_run_files(outputs_dir: Path, n: str, instance: str, strategy: str):
    """Returns [(run_id, path), ...] sorted by run_id."""
    inst_dir = outputs_dir / n
    if not inst_dir.is_dir():
        return []
    pattern = re.compile(rf"^{re.escape(instance)}-repl{re.escape(strategy)}-(\d+)\.json$")
    found = []
    for f in inst_dir.iterdir():
        m = pattern.match(f.name)
        if m:
            found.append((int(m.group(1)), f))
    found.sort(key=lambda t: t[0])
    return found


def rpd_pct(value, bks_value):
    if value is None or bks_value is None or not bks_value:
        return None
    return (value - bks_value) / bks_value * 100.0


def mean_or_none(vals):
    vals = [v for v in vals if v is not None]
    return statistics.mean(vals) if vals else None


def irpd_pct(r):
    """Trapezoidal-rule average of r[0..8] over the 8 intervals."""
    if any(v is None for v in r):
        return None
    return ((r[0] + r[8]) / 2.0 + sum(r[1:8])) / 8.0


def pool_diversity_avg(d):
    """Plain average of d[1..8] (skips d[0])."""
    if any(v is None for v in d[1:NUM_CHECKPOINTS]):
        return None
    return statistics.mean(d[1:NUM_CHECKPOINTS])


def compute_instance(outputs_dir: Path, bks_dir: Path, n: str, combo: str, expected_runs: int):
    instance = f"{n}.{combo}"
    bks_path = bks_dir / n / f"{instance}-bks.json"
    bks_value = load_bks_working_time(bks_path) if bks_path.is_file() else None

    summary_rows = []
    detail_rows = []
    for strategy in REPLACE_STRATEGY_LIST:
        run_files = find_run_files(outputs_dir, n, instance, strategy)

        rpds, irpds_per_run, diversities_per_run = [], [], []
        for run_id, path in run_files:
            data = load_run(path)
            final_cost = data["solution"]["working_time"]
            cps = data.get("best_solution_cost_by_evaluation_checkpoint")
            divs = data.get("diversity_by_evaluation_checkpoint")

            run_rpd = rpd_pct(final_cost, bks_value)
            run_irpd = None
            if cps is not None and len(cps) == NUM_CHECKPOINTS:
                run_r = [rpd_pct(v, bks_value) for v in cps]
                run_irpd = irpd_pct(run_r)
            run_diversity = None
            if divs is not None and len(divs) == NUM_CHECKPOINTS:
                run_diversity = pool_diversity_avg(divs)

            if run_rpd is not None:
                rpds.append(run_rpd)
            if run_irpd is not None:
                irpds_per_run.append(run_irpd)
            if run_diversity is not None:
                diversities_per_run.append(run_diversity)

            detail_rows.append({
                "n": n, "instance": instance, "strategy": strategy, "run": run_id,
                "final_cost": final_cost, "final_rpd_pct": run_rpd,
                "irpd_pct": run_irpd, "pool_diversity": run_diversity,
            })

        summary_rows.append({
            "n": n, "instance": instance, "strategy": strategy,
            "bks": bks_value, "runs": len(run_files), "expected_runs": expected_runs,
            "final_rpd_pct": mean_or_none(rpds),
            "irpd_pct": mean_or_none(irpds_per_run),
            "pool_diversity": mean_or_none(diversities_per_run),
        })
    return summary_rows, detail_rows


def fmt(v, prec=4):
    return f"{v:.{prec}f}" if v is not None else "-"


def write_csv(path: Path, rows):
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
        writer.writeheader()
        writer.writerows(rows)


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--outputs", default="../../../outputs/exp2/f",
                     help="Dir containing run2.sh output, relative to this script "
                          "(default: ../../../outputs/exp2/f)")
    ap.add_argument("--bks", default="../../../../bks",
                     help="Dir containing BKS json files, relative to this script "
                          "(default: ../../../../bks)")
    ap.add_argument("--customers", default="200,500",
                     help="Comma-separated customer counts (default: 200,500)")
    ap.add_argument("--combos", default="10.2,40.1",
                     help="Comma-separated instance combos, matches run2.sh "
                          "(default: 10.2,40.1)")
    ap.add_argument("--runs", type=int, default=3,
                     help="Expected number of runs per (instance, strategy), used to "
                          "flag incomplete ones (default: 3)")
    ap.add_argument("--no-save", action="store_true",
                     help="Only print to stdout, don't write any CSV files")
    args = ap.parse_args()

    script_dir = Path(__file__).resolve().parent
    outputs_dir = (script_dir / args.outputs).resolve()
    bks_dir = (script_dir / args.bks).resolve()

    customers = [c.strip() for c in args.customers.split(",") if c.strip()]
    combos = [c.strip() for c in args.combos.split(",") if c.strip()]

    lines = []

    def out(s=""):
        lines.append(s)

    out(f"outputs: {outputs_dir}")
    out(f"bks:     {bks_dir}")
    out()

    summary_rows = []
    detail_rows = []
    for n in customers:
        for combo in combos:
            s, d = compute_instance(outputs_dir, bks_dir, n, combo, args.runs)
            summary_rows.extend(s)
            detail_rows.extend(d)

    incomplete = [f"{r['instance']}/{r['strategy']} ({r['runs']}/{r['expected_runs']})"
                  for r in summary_rows if r["runs"] < r["expected_runs"]]

    def avg_field(rows, f):
        vals = [r[f] for r in rows if r[f] is not None]
        return statistics.mean(vals) if vals else None

    # ---- final_rpd(%), irpd(%), and pool_diversity per replace-strategy value ----
    # All 3 metrics: stratified average, per run -> mean over runs (per
    # instance, done in compute_instance) -> mean over instances (here).
    out(f"{'Strategy':<18}{'final_RPD(%)':>14}{'irpd(%)':>10}{'PoolDiv':>10}")
    out("-" * 52)
    strategy_rows = []
    for strategy in REPLACE_STRATEGY_LIST:
        rows = [r for r in summary_rows if r["strategy"] == strategy]
        strategy_row = {
            "strategy": strategy,
            "final_rpd_pct": avg_field(rows, "final_rpd_pct"),
            "irpd_pct": avg_field(rows, "irpd_pct"),
            "pool_diversity": avg_field(rows, "pool_diversity"),
        }
        strategy_rows.append(strategy_row)
        out(f"{strategy:<18}{fmt(strategy_row['final_rpd_pct'], 3):>14}"
            f"{fmt(strategy_row['irpd_pct'], 3):>10}"
            f"{fmt(strategy_row['pool_diversity'], 3):>10}")
    if incomplete:
        out(f"\nNote: fewer runs found than expected for: {', '.join(incomplete)}")

    report = "\n".join(lines)
    print(report)

    if args.no_save:
        return

    saved = []
    report_path = outputs_dir / "stats.txt"
    outputs_dir.mkdir(parents=True, exist_ok=True)
    report_path.write_text(report + "\n")
    saved.append(report_path)
    if summary_rows:
        summary_path = outputs_dir / "summary.csv"
        write_csv(summary_path, summary_rows)
        saved.append(summary_path)
    if detail_rows:
        detail_path = outputs_dir / "detail_replace.csv"
        write_csv(detail_path, detail_rows)
        saved.append(detail_path)
    if strategy_rows:
        strategy_path = outputs_dir / "strategy_summary.csv"
        write_csv(strategy_path, strategy_rows)
        saved.append(strategy_path)

    print("\nSaved:")
    for p in saved:
        print(f"  {p}")


if __name__ == "__main__":
    main()
