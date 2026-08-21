#!/usr/bin/env python3
"""
Statistics for run2.sh (exp2/a, elite-push-strategy x elite_pool_size comparison).

Compares the 3 elite-push strategies (new-best, segment-best,
significant-best) on 3 hand-picked --elite-pool-size values, grouped into
named "sets" (po1..po3), same as exp2/g. Each set fixes a pool size per
customer count:
  po1: 200->2, 500->3
  po2: 200->3, 500->4
  po3: 200->4, 500->5
All runs use elite-pull off and td-crowding pool replacement. For each
(pool_set, push_strategy) pair reports, printed as one block per pool_set
(NOT averaged across pool_set -- each set is its own regime, not a
nuisance parameter to marginalize out):
  - push rate: total_elite_pushes per 1,000,000 total_evaluations
  - reduction (%): push-rate reduction vs new-best (baseline, 0%),
    within that same pool_set
  - accept (%): accepted_elite_pushes / total_elite_pushes
  - R2, R4, R8: RPD (%) of best_cost_by_evaluation_checkpoint (the elite
    pool's own best cost -- what push strategies are actually populating
    the pool with, which is what this experiment is about; see exp2/d/e/f
    for the true-best-solution-based checkpoint instead) at 2/8, 4/8, 8/8
    of the evaluation budget, vs BKS working_time
  - irpd (%): trapezoidal-rule average of R0..R8:
    (1/8) * [(R0+R8)/2 + R1+...+R7]

RPD (%) = (value - BKS) / BKS * 100, same as exp1.5/stats.py. The
"value" is the tracked cost at that checkpoint, which can carry an
infeasibility penalty at early checkpoints if the best-known solution
isn't yet feasible at that point.

Aggregation: for each (n, combo) instance, each (pool_set, push_strategy)
pair's numbers are averaged over its runs first; these per-instance
numbers are then averaged over instances (4 per pool_set: 2 customer
counts x 2 combos) to get each pair's final numbers. reduction % is the
odd one out: it's computed once per pool_set, from that pool_set's final
averaged push rates (not averaged from each instance's own detail
reduction).

Looks for run files at
<outputs>/<n>/<n>.<combo>-<pool_set>-<push_strategy>-<run_id>.json (the
naming used by run2.sh) and BKS files at <bks>/<n>/<n>.<combo>-bks.json.

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

PUSH_STRATEGIES = ["new-best", "segment-best", "significant-best"]
POOL_SETS_BY_N = {
    "200": [("po1", "2"), ("po2", "3"), ("po3", "4")],
    "500": [("po1", "3"), ("po2", "4"), ("po3", "5")],
}
POOL_SET_NAMES = ["po1", "po2", "po3"]
NUM_CHECKPOINTS = 9  # R0..R8, at k/8 of the evaluation budget


def load_run(path: Path):
    with path.open() as f:
        return json.load(f)


def load_bks_working_time(path: Path):
    with path.open() as f:
        return json.load(f)["solution"]["working_time"]


def find_run_files(outputs_dir: Path, n: str, instance: str, pool_set: str, push_strategy: str):
    """Returns [(run_id, path), ...] sorted by run_id."""
    inst_dir = outputs_dir / n
    if not inst_dir.is_dir():
        return []
    pattern = re.compile(
        rf"^{re.escape(instance)}-{re.escape(pool_set)}-{re.escape(push_strategy)}-(\d+)\.json$")
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


def irpd_pct(r):
    """Trapezoidal-rule average of r[0..8] over the 8 intervals."""
    if any(v is None for v in r):
        return None
    return ((r[0] + r[8]) / 2.0 + sum(r[1:8])) / 8.0


def mean_or_none(vals):
    vals = [v for v in vals if v is not None]
    return statistics.mean(vals) if vals else None


def compute_instance(outputs_dir: Path, bks_dir: Path, n: str, combo: str, expected_runs: int):
    instance = f"{n}.{combo}"
    bks_path = bks_dir / n / f"{instance}-bks.json"
    bks_value = load_bks_working_time(bks_path) if bks_path.is_file() else None

    summary_rows = []
    detail_rows = []
    for pool_set, pool_size in POOL_SETS_BY_N[n]:
        per_strategy = {}
        for push_strategy in PUSH_STRATEGIES:
            run_files = find_run_files(outputs_dir, n, instance, pool_set, push_strategy)

            push_rates, accept_pcts, r_per_run, irpds_per_run = [], [], [], []
            for run_id, path in run_files:
                data = load_run(path)
                p = data["total_elite_pushes"]
                a = data["accepted_elite_pushes"]
                e = data["total_evaluations"]
                cps = data["best_cost_by_evaluation_checkpoint"]

                run_push_rate = p / e * 1e6 if e else None
                run_accept_pct = a / p * 100.0 if p else None
                push_rates.append(run_push_rate)
                accept_pcts.append(run_accept_pct)
                if len(cps) == NUM_CHECKPOINTS:
                    run_r = [rpd_pct(v, bks_value) for v in cps]
                    r_per_run.append(run_r)
                    irpds_per_run.append(irpd_pct(run_r))

                detail_rows.append({
                    "n": n, "instance": instance, "pool_set": pool_set, "pool_size": pool_size,
                    "push_strategy": push_strategy, "run": run_id,
                    "total_elite_pushes": p, "accepted_elite_pushes": a, "total_evaluations": e,
                    "push_rate_per_million_evals": run_push_rate,
                    "accept_pct": run_accept_pct,
                })

            push_rate = mean_or_none(push_rates)
            accept_pct = mean_or_none(accept_pcts)

            # R0..R8 are shown as their own columns, so still averaged per
            # checkpoint index across runs. irpd is instead computed per run
            # first (from that run's own R0..R8), then averaged over runs --
            # equivalent by linearity when no run has partial-missing
            # checkpoints, but correct even if that ever stops holding.
            r = [None] * NUM_CHECKPOINTS
            if r_per_run:
                r = [mean_or_none(vals) for vals in zip(*r_per_run)]

            per_strategy[push_strategy] = {
                "runs": len(run_files), "push_rate": push_rate, "accept_pct": accept_pct,
                "r": r, "irpd": mean_or_none(irpds_per_run),
            }

        baseline_rate = per_strategy["new-best"]["push_rate"]
        for push_strategy in PUSH_STRATEGIES:
            s = per_strategy[push_strategy]
            reduction = None
            if s["push_rate"] is not None and baseline_rate:
                reduction = (baseline_rate - s["push_rate"]) / baseline_rate * 100.0
            r = s["r"]
            summary_rows.append({
                "n": n, "instance": instance, "pool_set": pool_set, "pool_size": pool_size,
                "push_strategy": push_strategy,
                "bks": bks_value, "runs": s["runs"], "expected_runs": expected_runs,
                "push_rate_per_million_evals": s["push_rate"],
                "reduction_pct": reduction,
                "accept_pct": s["accept_pct"],
                "R0": r[0], "R1": r[1], "R2": r[2], "R3": r[3], "R4": r[4],
                "R5": r[5], "R6": r[6], "R7": r[7], "R8": r[8],
                "irpd_pct": s["irpd"],
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
    ap.add_argument("--outputs", default="../../../outputs/exp2/a",
                     help="Dir containing run2.sh output, relative to this script "
                          "(default: ../../../outputs/exp2/a)")
    ap.add_argument("--bks", default="../../../../bks",
                     help="Dir containing BKS json files, relative to this script "
                          "(default: ../../../../bks)")
    ap.add_argument("--customers", default="200,500",
                     help="Comma-separated customer counts (default: 200,500)")
    ap.add_argument("--combos", default="10.2,40.1",
                     help="Comma-separated instance combos, matches run2.sh "
                          "(default: 10.2,40.1)")
    ap.add_argument("--runs", type=int, default=3,
                     help="Expected number of runs per (instance, pool_set, strategy), "
                          "used to flag incomplete ones (default: 3)")
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

    incomplete = [f"{r['instance']}/{r['pool_set']}/{r['push_strategy']} ({r['runs']}/{r['expected_runs']})"
                  for r in summary_rows if r["runs"] < r["expected_runs"]]

    def avg_field(rows, f):
        vals = [r[f] for r in rows if r[f] is not None]
        return statistics.mean(vals) if vals else None

    # ---- One block per pool_set (not averaged across pool_set): a per-
    # instance-per-strategy detail table, then the mean-over-instances
    # summary table for each (pool_set, push_strategy) pair's numbers. ----
    strategy_rows = []
    for pool_set in POOL_SET_NAMES:
        sizes = ",".join(f"{n}->{sz}" for n, combos_ in POOL_SETS_BY_N.items()
                          for name, sz in combos_ if name == pool_set)
        out(f"--- Pool set {pool_set} ({sizes}) ---")

        # Detail: one row per (instance, push_strategy).
        detail_header = (f"{'Instance':<12}{'Push':<18}{'Runs':>6}{'PushRate':>12}"
                          f"{'Reduct(%)':>11}{'Accept(%)':>11}"
                          f"{'R2(%)':>10}{'R4(%)':>10}{'R8(%)':>10}{'irpd(%)':>10}")
        out(detail_header)
        out("-" * len(detail_header))
        for r in summary_rows:
            if r["pool_set"] != pool_set:
                continue
            out(f"{r['instance']:<12}{r['push_strategy']:<18}{r['runs']:>6}"
                f"{fmt(r['push_rate_per_million_evals'], 3):>12}"
                f"{fmt(r['reduction_pct'], 3):>11}"
                f"{fmt(r['accept_pct'], 3):>11}"
                f"{fmt(r['R2'], 3):>10}{fmt(r['R4'], 3):>10}{fmt(r['R8'], 3):>10}"
                f"{fmt(r['irpd_pct'], 3):>10}")

        # Summary: mean over instances, one row per push_strategy. Reduction
        # is derived from THIS pool_set's final averaged push rates.
        fields = {}
        for push_strategy in PUSH_STRATEGIES:
            rows = [r for r in summary_rows
                    if r["pool_set"] == pool_set and r["push_strategy"] == push_strategy]
            fields[push_strategy] = {
                "push_rate_per_million_evals": avg_field(rows, "push_rate_per_million_evals"),
                "accept_pct": avg_field(rows, "accept_pct"),
                "R2": avg_field(rows, "R2"), "R4": avg_field(rows, "R4"), "R8": avg_field(rows, "R8"),
                "irpd_pct": avg_field(rows, "irpd_pct"),
            }
        baseline_final_rate = fields["new-best"]["push_rate_per_million_evals"]

        out(f"\n{'PushStrategy':<18}{'PushRate':>12}{'Reduct(%)':>11}{'Accept(%)':>11}"
            f"{'R2(%)':>10}{'R4(%)':>10}{'R8(%)':>10}{'irpd(%)':>10}")
        out("-" * 92)
        for push_strategy in PUSH_STRATEGIES:
            f = fields[push_strategy]
            reduction = None
            if f["push_rate_per_million_evals"] is not None and baseline_final_rate:
                reduction = (baseline_final_rate - f["push_rate_per_million_evals"]) / baseline_final_rate * 100.0
            strategy_row = {"pool_set": pool_set, "push_strategy": push_strategy,
                             "reduction_pct": reduction, **f}
            strategy_rows.append(strategy_row)
            out(f"{push_strategy:<18}{fmt(strategy_row['push_rate_per_million_evals'], 3):>12}"
                f"{fmt(strategy_row['reduction_pct'], 3):>11}"
                f"{fmt(strategy_row['accept_pct'], 3):>11}"
                f"{fmt(strategy_row['R2'], 3):>10}{fmt(strategy_row['R4'], 3):>10}"
                f"{fmt(strategy_row['R8'], 3):>10}{fmt(strategy_row['irpd_pct'], 3):>10}")
        out()

    if incomplete:
        out(f"Note: fewer runs found than expected for: {', '.join(incomplete)}")

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
        detail_path = outputs_dir / "detail_push.csv"
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
