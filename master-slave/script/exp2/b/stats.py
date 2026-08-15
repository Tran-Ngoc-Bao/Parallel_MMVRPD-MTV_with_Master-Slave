#!/usr/bin/env python3
"""
Statistics for run2.sh (exp2/b, elite_pool_factor comparison).

Compares 3 elite_pool_factor values (0.02, 0.06, 0.10), all run with
elite-pull off, significant-best push strategy, and similarity-quality
pool replacement. For each value reports, from the elite pool still held
at program end:
  - pool size: elite_pool_size
  - pool RPD (%): mean RPD (%) of elite_pool_costs vs BKS working_time
  - pool diversity: elite_pool_diversity (normalized to [0,1] by
    customers_count)

RPD (%) = (value - BKS) / BKS * 100, same as exp1.5/stats.py.

Aggregation: for each (n, combo) instance, each pool_factor's numbers
are averaged over its runs first; each pool_factor's final numbers are
then the average over its instances (6 by default: 3 customer counts x
2 combos).

Looks for run files at
<outputs>/<n>/<n>.<combo>-pool<pool_factor>-<run_id>.json (the naming
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

POOL_FACTORS = ["0.02", "0.04", "0.06"]


def load_run(path: Path):
    with path.open() as f:
        return json.load(f)


def load_bks_working_time(path: Path):
    with path.open() as f:
        return json.load(f)["solution"]["working_time"]


def find_run_files(outputs_dir: Path, n: str, instance: str, pool_factor: str):
    """Returns [(run_id, path), ...] sorted by run_id."""
    inst_dir = outputs_dir / n
    if not inst_dir.is_dir():
        return []
    pattern = re.compile(rf"^{re.escape(instance)}-pool{re.escape(pool_factor)}-(\d+)\.json$")
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


def compute_instance(outputs_dir: Path, bks_dir: Path, n: str, combo: str, expected_runs: int):
    instance = f"{n}.{combo}"
    bks_path = bks_dir / n / f"{instance}-bks.json"
    bks_value = load_bks_working_time(bks_path) if bks_path.is_file() else None

    summary_rows = []
    detail_rows = []
    for pool_factor in POOL_FACTORS:
        run_files = find_run_files(outputs_dir, n, instance, pool_factor)

        pool_sizes, pool_diversities, pool_mean_costs = [], [], []
        for run_id, path in run_files:
            data = load_run(path)
            pool_size = data["elite_pool_size"]
            pool_diversity = data["elite_pool_diversity"]
            pool_costs = data["elite_pool_costs"]
            pool_mean_cost = statistics.mean(pool_costs) if pool_costs else None

            pool_sizes.append(pool_size)
            pool_diversities.append(pool_diversity)
            if pool_mean_cost is not None:
                pool_mean_costs.append(pool_mean_cost)

            detail_rows.append({
                "n": n, "instance": instance, "pool_factor": pool_factor, "run": run_id,
                "elite_pool_size": pool_size, "elite_pool_diversity": pool_diversity,
                "elite_pool_rpd_pct": rpd_pct(pool_mean_cost, bks_value),
            })

        avg_pool_size = statistics.mean(pool_sizes) if pool_sizes else None
        avg_pool_diversity = statistics.mean(pool_diversities) if pool_diversities else None
        avg_pool_mean_cost = statistics.mean(pool_mean_costs) if pool_mean_costs else None

        summary_rows.append({
            "n": n, "instance": instance, "pool_factor": pool_factor,
            "bks": bks_value, "runs": len(run_files), "expected_runs": expected_runs,
            "elite_pool_size": avg_pool_size,
            "elite_pool_rpd_pct": rpd_pct(avg_pool_mean_cost, bks_value),
            "elite_pool_diversity": avg_pool_diversity,
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
    ap.add_argument("--outputs", default="../../../outputs/exp2/b-m",
                     help="Dir containing run2.sh output, relative to this script "
                          "(default: ../../../outputs/exp2/b)")
    ap.add_argument("--bks", default="../../../../bks",
                     help="Dir containing BKS json files, relative to this script "
                          "(default: ../../../../bks)")
    ap.add_argument("--customers", default="200,500,1000",
                     help="Comma-separated customer counts (default: 200,500,1000)")
    ap.add_argument("--combos", default="10.2,40.1",
                     help="Comma-separated instance combos, matches run2.sh "
                          "(default: 10.2,40.1)")
    ap.add_argument("--runs", type=int, default=3,
                     help="Expected number of runs per (instance, pool_factor), used to "
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

    # ---- Per-instance-per-pool_factor summary ----
    header = (f"{'Instance':<12}{'Pool':<8}{'Runs':>6}"
              f"{'PoolSz':>8}{'PoolRPD(%)':>12}{'PoolDiv':>9}")
    out(header)
    out("-" * len(header))
    incomplete = []
    for r in summary_rows:
        out(f"{r['instance']:<12}{r['pool_factor']:<8}{r['runs']:>6}"
            f"{fmt(r['elite_pool_size'], 2):>8}{fmt(r['elite_pool_rpd_pct'], 3):>12}"
            f"{fmt(r['elite_pool_diversity'], 3):>9}")
        if r["runs"] < r["expected_runs"]:
            incomplete.append(f"{r['instance']}/pool{r['pool_factor']} ({r['runs']}/{r['expected_runs']})")
    if incomplete:
        out(f"\nNote: fewer runs found than expected for: {', '.join(incomplete)}")

    # ---- Aggregate: instance -> pool_factor (avg over instances) ----
    out(f"\n{'PoolFactor':<8}{'PoolSz':>8}{'PoolRPD(%)':>12}{'PoolDiv':>9}")
    out("-" * 37)
    pool_factor_rows = []
    for pool_factor in POOL_FACTORS:
        rows = [r for r in summary_rows if r["pool_factor"] == pool_factor]

        def avg_field(f, rows=rows):
            vals = [r[f] for r in rows if r[f] is not None]
            return statistics.mean(vals) if vals else None

        pool_factor_row = {
            "pool_factor": pool_factor,
            "elite_pool_size": avg_field("elite_pool_size"),
            "elite_pool_rpd_pct": avg_field("elite_pool_rpd_pct"),
            "elite_pool_diversity": avg_field("elite_pool_diversity"),
        }
        pool_factor_rows.append(pool_factor_row)
        out(f"{pool_factor:<8}{fmt(pool_factor_row['elite_pool_size'], 2):>8}"
            f"{fmt(pool_factor_row['elite_pool_rpd_pct'], 3):>12}"
            f"{fmt(pool_factor_row['elite_pool_diversity'], 3):>9}")

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
        detail_path = outputs_dir / "detail_pool.csv"
        write_csv(detail_path, detail_rows)
        saved.append(detail_path)
    if pool_factor_rows:
        pool_factor_path = outputs_dir / "pool_factor_summary.csv"
        write_csv(pool_factor_path, pool_factor_rows)
        saved.append(pool_factor_path)

    print("\nSaved:")
    for p in saved:
        print(f"  {p}")


if __name__ == "__main__":
    main()
