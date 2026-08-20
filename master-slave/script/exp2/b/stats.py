#!/usr/bin/env python3
"""
Statistics for run2.sh (exp2/b, elite_pool_size comparison).

Compares hand-picked --elite-pool-size values, grouped into named "sets"
(bo1..bo4), each fixing a pool size per customer count:
  bo1: 200->2, 500->3
  bo2: 200->3, 500->4
  bo3: 200->3, 500->5
  bo4: 200->4, 500->5
All runs use elite-pull off, significant-best push strategy, and
td-crowding pool replacement. For each set reports, from the elite
pool still held at program end:
  - pool size: elite_pool_size
  - pool RPD (%): mean RPD (%) of elite_pool_costs vs BKS working_time
  - pool diversity: elite_pool_diversity (normalized to [0,1] by
    customers_count)

RPD (%) = (value - BKS) / BKS * 100, same as exp1.5/stats.py.

Aggregation: for each (n, combo) instance, each set's numbers are
averaged over its runs first; each set's final numbers are then the
average over all 6 instances (3 customer counts x 2 combos).

Looks for run files at
<outputs>/<n>/<n>.<combo>-<set>-<run_id>.json (the naming used by
run2.sh) and BKS files at <bks>/<n>/<n>.<combo>-bks.json.

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

# Sets covering each customer count, as (set_name, pool_size).
SETS_BY_N = {
    "200": [("bo1", "2"), ("bo2", "3"), ("bo3", "3"), ("bo4", "4")],
    "500": [("bo1", "3"), ("bo2", "4"), ("bo3", "5"), ("bo4", "5")],
}
ALL_SET_NAMES = ["bo1", "bo2", "bo3", "bo4"]


def load_run(path: Path):
    with path.open() as f:
        return json.load(f)


def load_bks_working_time(path: Path):
    with path.open() as f:
        return json.load(f)["solution"]["working_time"]


def find_run_files(outputs_dir: Path, n: str, instance: str, set_name: str):
    """Returns [(run_id, path), ...] sorted by run_id."""
    inst_dir = outputs_dir / n
    if not inst_dir.is_dir():
        return []
    pattern = re.compile(rf"^{re.escape(instance)}-{re.escape(set_name)}-(\d+)\.json$")
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
    for set_name, pool_size_hint in SETS_BY_N.get(n, []):
        run_files = find_run_files(outputs_dir, n, instance, set_name)

        pool_sizes, pool_diversities, pool_rpds = [], [], []
        for run_id, path in run_files:
            data = load_run(path)
            pool_size = data["elite_pool_size"]
            pool_diversity = data["elite_pool_diversity"]
            pool_costs = data["elite_pool_costs"]
            pool_mean_cost = statistics.mean(pool_costs) if pool_costs else None
            pool_rpd = rpd_pct(pool_mean_cost, bks_value)

            pool_sizes.append(pool_size)
            pool_diversities.append(pool_diversity)
            if pool_rpd is not None:
                pool_rpds.append(pool_rpd)

            detail_rows.append({
                "n": n, "instance": instance, "set": set_name, "pool_size": pool_size_hint, "run": run_id,
                "elite_pool_size": pool_size, "elite_pool_diversity": pool_diversity,
                "elite_pool_rpd_pct": pool_rpd,
            })

        def mean_or_none(vals):
            vals = [v for v in vals if v is not None]
            return statistics.mean(vals) if vals else None

        avg_pool_size = mean_or_none(pool_sizes)
        avg_pool_diversity = mean_or_none(pool_diversities)
        avg_pool_rpd = mean_or_none(pool_rpds)

        summary_rows.append({
            "n": n, "instance": instance, "set": set_name, "pool_size": pool_size_hint,
            "bks": bks_value, "runs": len(run_files), "expected_runs": expected_runs,
            "elite_pool_size": avg_pool_size,
            "elite_pool_rpd_pct": avg_pool_rpd,
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
    ap.add_argument("--outputs", default="../../../outputs/exp2/b",
                     help="Dir containing run2.sh output, relative to this script "
                          "(default: ../../../outputs/exp2/b)")
    ap.add_argument("--bks", default="../../../../bks",
                     help="Dir containing BKS json files, relative to this script "
                          "(default: ../../../../bks)")
    ap.add_argument("--customers", default="200,500",
                     help="Comma-separated customer counts (default: 200,500)")
    ap.add_argument("--combos", default="10.2,40.1",
                     help="Comma-separated instance combos, matches run2.sh "
                          "(default: 10.2,40.1)")
    ap.add_argument("--runs", type=int, default=3,
                     help="Expected number of runs per (instance, set), used to "
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

    # ---- Per-instance-per-set summary ----
    header = (f"{'Instance':<12}{'Set':<6}{'Sz':<4}{'Runs':>6}"
              f"{'PoolSz':>8}{'PoolRPD(%)':>12}{'PoolDiv':>9}")
    out(header)
    out("-" * len(header))
    incomplete = []
    for r in summary_rows:
        out(f"{r['instance']:<12}{r['set']:<6}{r['pool_size']:<4}{r['runs']:>6}"
            f"{fmt(r['elite_pool_size'], 2):>8}{fmt(r['elite_pool_rpd_pct'], 3):>12}"
            f"{fmt(r['elite_pool_diversity'], 3):>9}")
        if r["runs"] < r["expected_runs"]:
            incomplete.append(f"{r['instance']}/{r['set']} ({r['runs']}/{r['expected_runs']})")
    if incomplete:
        out(f"\nNote: fewer runs found than expected for: {', '.join(incomplete)}")

    # ---- Aggregate: set -> avg over all instances ----
    out(f"\n{'Set':<6}{'PoolSz':>8}{'PoolRPD(%)':>12}{'PoolDiv':>9}")
    out("-" * 35)
    set_rows = []
    for set_name in ALL_SET_NAMES:
        rows = [r for r in summary_rows if r["set"] == set_name]
        if not rows:
            continue

        def avg_field(f, rows=rows):
            vals = [r[f] for r in rows if r[f] is not None]
            return statistics.mean(vals) if vals else None

        set_row = {
            "set": set_name,
            "elite_pool_size": avg_field("elite_pool_size"),
            "elite_pool_rpd_pct": avg_field("elite_pool_rpd_pct"),
            "elite_pool_diversity": avg_field("elite_pool_diversity"),
        }
        set_rows.append(set_row)
        out(f"{set_name:<6}{fmt(set_row['elite_pool_size'], 2):>8}"
            f"{fmt(set_row['elite_pool_rpd_pct'], 3):>12}"
            f"{fmt(set_row['elite_pool_diversity'], 3):>9}")

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
    if set_rows:
        set_path = outputs_dir / "set_summary.csv"
        write_csv(set_path, set_rows)
        saved.append(set_path)

    print("\nSaved:")
    for p in saved:
        print(f"  {p}")


if __name__ == "__main__":
    main()
