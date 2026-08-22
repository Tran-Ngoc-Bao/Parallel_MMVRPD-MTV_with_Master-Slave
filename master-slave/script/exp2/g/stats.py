#!/usr/bin/env python3
"""
Statistics for run2.sh (exp2/g, elite_replace_strategy x elite_pool_size comparison).

Compares 3 elite_replace_strategy values (quality-only, td-crowding,
edge-crowding) on 3 hand-picked --elite-pool-size values, grouped into
named "sets" (po1..po3). Each set fixes a pool size per customer count:
  po1: 200->2, 500->3
  po2: 200->3, 500->4
  po3: 200->4, 500->5
All runs use elite-pull-strategy=rank, elite-pull-accept-strategy=selective,
adaptive_pull_elite_segments set to the best value found in exp2/d, and
elite_pull_quality_tolerance_pct set to the best value found in exp2/e
(both overridable via env var in run2.sh, defaults 4 and 1 respectively
until those experiments have concluded).

Reports 4 numbers per (pool_set, strategy) pair, printed as one table per
pool_set -- NOT averaged across pool_set (each set is its own regime, not
a nuisance parameter to marginalize out):
  - final_rpd (%): RPD of the final best cost (solution.working_time) vs BKS
  - irpd (%): trapezoidal-rule average of R0..R8 (RPD (%) of
    best_solution_cost_by_evaluation_checkpoint at 0/8..8/8 of the
    evaluation budget vs BKS working_time), (1/8) * [(R0+R8)/2 + R1+...+R7]
  - AR / accepted_transfer (%): pull_accept_count / pull_offer_count --
    of the offers a worker's pull request could get (offered = at least
    one elite in the pool from a worker other than the requester), how
    many actually get sent back to it (see pick_for_dispatch in
    parallel.cpp)
  - ER / effective_transfer (%): pull_round_improved_count / pull_accept_count
    -- of those accepted transfers, how many are followed by an
    improvement over the personal best held at pull time, before the next
    pull (see hooks->pull_round_improved in solutions.cpp)

RPD (%) = (value - BKS) / BKS * 100, same as exp1.5/stats.py. final_rpd
uses the final best cost (solution.working_time); irpd uses the tracked
best_solution_cost_by_evaluation_checkpoint values (the true best solution
found so far at each checkpoint, not the elite pool's own best -- see
exp2/a for that one), which can carry an infeasibility penalty at early
checkpoints if the best-known solution isn't yet feasible at that point.

Aggregation: same stratified style as exp2/f for final_rpd and irpd --
computed per run first (using that run's own solution/checkpoints vs BKS),
then averaged over that instance's runs, then averaged over instances (4
per pool_set: 2 customer counts x 2 combos). AR and ER instead follow
exp2/c's pooling: N_accept summed over the instance's runs / N_offer (or
N_accept, for ER) summed over the instance's runs * 100 (not averaged
per-run), since offer/accept counts can vary a lot run to run -- pooling
weights each pull attempt equally instead of each run equally. These
per-instance numbers are then averaged over instances to get each
(pool_set, strategy) pair's final numbers.

Also writes a separate stats-sub.txt with 3 end-of-run pool-state numbers
per (pool_set, strategy) pair (same per-pool_set block layout, same
stratified run -> instance -> pair aggregation as final_rpd/irpd above):
  - mean_pool_rpd (%): RPD of the mean(elite_pool_costs) held at program
    end vs BKS working_time (same style as exp2/b's pool RPD)
  - pool_diversity: elite_pool_diversity held at program end (mean pairwise
    td_distance across the final pool, normalized to [0,1])
  - replacement rate: accepted_elite_pushes per 1,000,000 total_evaluations
    -- accepted_elite_pushes counts every push that actually got inserted
    into or replaced a slot in the pool (see ElitePool::consider in
    parallel.cpp); with these pool sizes (2-5) the pool fills almost
    immediately, so this is effectively a replacement rate

Looks for run files at
<outputs>/<n>/<n>.<combo>-<pool_set>-<strategy>-<run_id>.json (the naming
used by run2.sh) and BKS files at <bks>/<n>/<n>.<combo>-bks.json.

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

REPLACE_STRATEGY_LIST = ["quality-only", "td-crowding", "edge-crowding"]
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


def find_run_files(outputs_dir: Path, n: str, instance: str, pool_set: str, strategy: str):
    """Returns [(run_id, path), ...] sorted by run_id."""
    inst_dir = outputs_dir / n
    if not inst_dir.is_dir():
        return []
    pattern = re.compile(
        rf"^{re.escape(instance)}-{re.escape(pool_set)}-{re.escape(strategy)}-(\d+)\.json$")
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


def compute_instance(outputs_dir: Path, bks_dir: Path, n: str, combo: str, expected_runs: int):
    instance = f"{n}.{combo}"
    bks_path = bks_dir / n / f"{instance}-bks.json"
    bks_value = load_bks_working_time(bks_path) if bks_path.is_file() else None

    summary_rows = []
    detail_rows = []
    for pool_set, pool_size in POOL_SETS_BY_N[n]:
        for strategy in REPLACE_STRATEGY_LIST:
            run_files = find_run_files(outputs_dir, n, instance, pool_set, strategy)

            rpds, irpds_per_run = [], []
            accepted_pulls, offered_pulls = [], []
            improved_rounds = []
            pool_rpds, pool_diversities, replacement_rates = [], [], []
            for run_id, path in run_files:
                data = load_run(path)
                final_cost = data["solution"]["working_time"]
                pac = data["pull_accept_count"]
                poc = data["pull_offer_count"]
                pri = data["pull_round_improved_count"]
                cps = data.get("best_solution_cost_by_evaluation_checkpoint")

                run_rpd = rpd_pct(final_cost, bks_value)
                run_ar = pac / poc * 100.0 if poc else None
                run_er = pri / pac * 100.0 if pac else None

                pool_costs = data.get("elite_pool_costs")
                pool_mean_cost = statistics.mean(pool_costs) if pool_costs else None
                run_pool_rpd = rpd_pct(pool_mean_cost, bks_value)
                run_pool_diversity = data.get("elite_pool_diversity")
                accepted_pushes = data.get("accepted_elite_pushes")
                total_evals = data.get("total_evaluations")
                run_replacement_rate = (
                    accepted_pushes / total_evals * 1e6 if total_evals else None)

                if run_rpd is not None:
                    rpds.append(run_rpd)
                if cps is not None and len(cps) == NUM_CHECKPOINTS:
                    run_r = [rpd_pct(v, bks_value) for v in cps]
                    irpds_per_run.append(irpd_pct(run_r))
                accepted_pulls.append(pac)
                offered_pulls.append(poc)
                improved_rounds.append(pri)
                if run_pool_rpd is not None:
                    pool_rpds.append(run_pool_rpd)
                if run_pool_diversity is not None:
                    pool_diversities.append(run_pool_diversity)
                if run_replacement_rate is not None:
                    replacement_rates.append(run_replacement_rate)

                detail_rows.append({
                    "n": n, "instance": instance, "pool_set": pool_set, "pool_size": pool_size,
                    "strategy": strategy, "run": run_id,
                    "final_cost": final_cost, "final_rpd_pct": run_rpd,
                    "pull_accept_count": pac, "pull_offer_count": poc, "ar_pct": run_ar,
                    "pull_round_improved_count": pri, "er_pct": run_er,
                    "pool_rpd_pct": run_pool_rpd, "pool_diversity": run_pool_diversity,
                    "replacement_rate_per_million_evals": run_replacement_rate,
                })

            total_poc = sum(offered_pulls)
            ar_pct = sum(accepted_pulls) / total_poc * 100.0 if total_poc else None
            total_pac = sum(accepted_pulls)
            er_pct = sum(improved_rounds) / total_pac * 100.0 if total_pac else None

            summary_rows.append({
                "n": n, "instance": instance, "pool_set": pool_set, "pool_size": pool_size,
                "strategy": strategy, "bks": bks_value,
                "runs": len(run_files), "expected_runs": expected_runs,
                "final_rpd_pct": mean_or_none(rpds),
                "irpd_pct": mean_or_none(irpds_per_run),
                "ar_pct": ar_pct,
                "er_pct": er_pct,
                "mean_pool_rpd_pct": mean_or_none(pool_rpds),
                "pool_diversity": mean_or_none(pool_diversities),
                "replacement_rate_per_million_evals": mean_or_none(replacement_rates),
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
    ap.add_argument("--outputs", default="../../../outputs/exp2/g",
                     help="Dir containing run2.sh output, relative to this script "
                          "(default: ../../../outputs/exp2/g)")
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

    incomplete = [f"{r['instance']}/{r['pool_set']}/{r['strategy']} ({r['runs']}/{r['expected_runs']})"
                  for r in summary_rows if r["runs"] < r["expected_runs"]]

    def avg_field(rows, f):
        vals = [r[f] for r in rows if r[f] is not None]
        return statistics.mean(vals) if vals else None

    # ---- One block per pool_set (not averaged across pool_set): a per-
    # instance-per-strategy detail table, then the mean-over-instances
    # summary table for each (pool_set, strategy) pair's final_rpd/irpd/AR/ER. ----
    pool_set_rows = []
    for pool_set in POOL_SET_NAMES:
        sizes = ",".join(f"{n}->{sz}" for n, combos_ in POOL_SETS_BY_N.items()
                          for name, sz in combos_ if name == pool_set)
        out(f"--- Pool set {pool_set} ({sizes}) ---")

        # Detail: one row per (instance, strategy).
        detail_header = (f"{'Instance':<12}{'Strategy':<14}{'Runs':>6}"
                          f"{'final_RPD(%)':>14}{'irpd(%)':>10}{'AR(%)':>10}{'ER(%)':>10}")
        out(detail_header)
        out("-" * len(detail_header))
        for r in summary_rows:
            if r["pool_set"] != pool_set:
                continue
            out(f"{r['instance']:<12}{r['strategy']:<14}{r['runs']:>6}"
                f"{fmt(r['final_rpd_pct'], 3):>14}{fmt(r['irpd_pct'], 3):>10}"
                f"{fmt(r['ar_pct'], 3):>10}{fmt(r['er_pct'], 3):>10}")

        # Summary: mean over instances, one row per strategy.
        out(f"\n{'Strategy':<14}{'final_RPD(%)':>14}{'irpd(%)':>10}{'AR(%)':>10}{'ER(%)':>10}")
        out("-" * 58)
        for strategy in REPLACE_STRATEGY_LIST:
            rows = [r for r in summary_rows if r["pool_set"] == pool_set and r["strategy"] == strategy]
            row = {
                "pool_set": pool_set,
                "strategy": strategy,
                "final_rpd_pct": avg_field(rows, "final_rpd_pct"),
                "irpd_pct": avg_field(rows, "irpd_pct"),
                "ar_pct": avg_field(rows, "ar_pct"),
                "er_pct": avg_field(rows, "er_pct"),
            }
            pool_set_rows.append(row)
            out(f"{strategy:<14}{fmt(row['final_rpd_pct'], 3):>14}"
                f"{fmt(row['irpd_pct'], 3):>10}{fmt(row['ar_pct'], 3):>10}{fmt(row['er_pct'], 3):>10}")
        out()

    if incomplete:
        out(f"Note: fewer runs found than expected for: {', '.join(incomplete)}")

    report = "\n".join(lines)
    print(report)

    # ---- stats-sub.txt: end-of-run pool-state numbers (mean_pool_rpd,
    # pool_diversity, replacement rate), same per-pool_set block layout. ----
    sub_lines = []

    def sub_out(s=""):
        sub_lines.append(s)

    sub_out(f"outputs: {outputs_dir}")
    sub_out(f"bks:     {bks_dir}")
    sub_out()

    pool_set_sub_rows = []
    for pool_set in POOL_SET_NAMES:
        sizes = ",".join(f"{n}->{sz}" for n, combos_ in POOL_SETS_BY_N.items()
                          for name, sz in combos_ if name == pool_set)
        sub_out(f"--- Pool set {pool_set} ({sizes}) ---")

        detail_header = (f"{'Instance':<12}{'Strategy':<14}{'Runs':>6}"
                          f"{'MeanPoolRPD(%)':>16}{'PoolDiv':>10}{'ReplRate':>12}")
        sub_out(detail_header)
        sub_out("-" * len(detail_header))
        for r in summary_rows:
            if r["pool_set"] != pool_set:
                continue
            sub_out(f"{r['instance']:<12}{r['strategy']:<14}{r['runs']:>6}"
                    f"{fmt(r['mean_pool_rpd_pct'], 3):>16}{fmt(r['pool_diversity'], 3):>10}"
                    f"{fmt(r['replacement_rate_per_million_evals'], 3):>12}")

        sub_out(f"\n{'Strategy':<14}{'MeanPoolRPD(%)':>16}{'PoolDiv':>10}{'ReplRate':>12}")
        sub_out("-" * 52)
        for strategy in REPLACE_STRATEGY_LIST:
            rows = [r for r in summary_rows if r["pool_set"] == pool_set and r["strategy"] == strategy]
            row = {
                "pool_set": pool_set,
                "strategy": strategy,
                "mean_pool_rpd_pct": avg_field(rows, "mean_pool_rpd_pct"),
                "pool_diversity": avg_field(rows, "pool_diversity"),
                "replacement_rate_per_million_evals": avg_field(rows, "replacement_rate_per_million_evals"),
            }
            pool_set_sub_rows.append(row)
            sub_out(f"{strategy:<14}{fmt(row['mean_pool_rpd_pct'], 3):>16}"
                    f"{fmt(row['pool_diversity'], 3):>10}"
                    f"{fmt(row['replacement_rate_per_million_evals'], 3):>12}")
        sub_out()

    if incomplete:
        sub_out(f"Note: fewer runs found than expected for: {', '.join(incomplete)}")

    sub_report = "\n".join(sub_lines)
    print(sub_report)

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
        detail_path = outputs_dir / "detail_transfer.csv"
        write_csv(detail_path, detail_rows)
        saved.append(detail_path)
    if pool_set_rows:
        pool_set_path = outputs_dir / "pool_set_summary.csv"
        write_csv(pool_set_path, pool_set_rows)
        saved.append(pool_set_path)
    sub_report_path = outputs_dir / "stats-sub.txt"
    sub_report_path.write_text(sub_report + "\n")
    saved.append(sub_report_path)
    if pool_set_sub_rows:
        pool_set_sub_path = outputs_dir / "pool_set_sub_summary.csv"
        write_csv(pool_set_sub_path, pool_set_sub_rows)
        saved.append(pool_set_sub_path)

    print("\nSaved:")
    for p in saved:
        print(f"  {p}")


if __name__ == "__main__":
    main()
