#!/usr/bin/env python3
"""
Statistics for run2.sh (exp2/c, elite-pull-strategy comparison).

Compares the 4 non-off elite-pull strategies (random, topk, rank,
pullcount), all run with new-best push and similarity-quality pool
replacement. For each strategy reports:
  - RPD (%): (working_time - BKS) / BKS * 100, of the final solution
  - ETB (%): best_solution_evaluations_percent -- how far (as % of the
    evaluation budget) the worker/master had gotten when its best
    solution was last updated

Aggregation:
  - RPD: for each (n, combo) instance, working_time is averaged over its
    3 runs first, then RPD is computed from that averaged value; each
    strategy's final RPD is the average over its instances (6 by
    default: 3 customer counts x 2 combos).
  - ETB: for each instance, the MEDIAN of ETB over its 3 runs is taken
    (median instead of mean -- ETB can be skewed by a single early/late
    outlier run); each strategy's final ETB is the average of those
    per-instance medians over its instances.

Looks for run files at <outputs>/<n>/<n>.<combo>-pull<strategy>-<run_id>.json
(the naming used by run2.sh) and BKS files at <bks>/<n>/<n>.<combo>-bks.json.

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

PULL_STRATEGIES = ["random", "topk", "rank", "pullcount"]


def load_run(path: Path):
    with path.open() as f:
        return json.load(f)


def load_bks_working_time(path: Path):
    with path.open() as f:
        return json.load(f)["solution"]["working_time"]


def find_run_files(outputs_dir: Path, n: str, instance: str, pull_strategy: str):
    """Returns [(run_id, path), ...] sorted by run_id."""
    inst_dir = outputs_dir / n
    if not inst_dir.is_dir():
        return []
    pattern = re.compile(rf"^{re.escape(instance)}-pull{re.escape(pull_strategy)}-(\d+)\.json$")
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
    for pull_strategy in PULL_STRATEGIES:
        run_files = find_run_files(outputs_dir, n, instance, pull_strategy)

        working_times, etbs = [], []
        for run_id, path in run_files:
            data = load_run(path)
            wt = data["solution"]["working_time"]
            etb = data["best_solution_evaluations_percent"]
            working_times.append(wt)
            etbs.append(etb)
            detail_rows.append({
                "n": n, "instance": instance, "pull_strategy": pull_strategy, "run": run_id,
                "working_time": wt, "rpd_pct": rpd_pct(wt, bks_value),
                "etb_pct": etb,
            })

        avg_working_time = statistics.mean(working_times) if working_times else None
        avg_rpd = rpd_pct(avg_working_time, bks_value) if avg_working_time is not None else None
        median_etb = statistics.median(etbs) if etbs else None

        summary_rows.append({
            "n": n, "instance": instance, "pull_strategy": pull_strategy,
            "bks": bks_value, "runs": len(run_files), "expected_runs": expected_runs,
            "avg_working_time": avg_working_time, "avg_rpd_pct": avg_rpd,
            "median_etb_pct": median_etb,
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
    ap.add_argument("--outputs", default="../../../outputs/exp2/c",
                     help="Dir containing run2.sh output, relative to this script "
                          "(default: ../../../outputs/exp2/c)")
    ap.add_argument("--bks", default="../../../../bks",
                     help="Dir containing BKS json files, relative to this script "
                          "(default: ../../../../bks)")
    ap.add_argument("--customers", default="200,500,1000",
                     help="Comma-separated customer counts (default: 200,500,1000)")
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

    # ---- Per-instance-per-strategy summary ----
    header = (f"{'Instance':<12}{'Pull':<12}{'Runs':>6}"
              f"{'RPD(%)':>12}{'ETB(%)':>12}")
    out(header)
    out("-" * len(header))
    incomplete = []
    for r in summary_rows:
        out(f"{r['instance']:<12}{r['pull_strategy']:<12}{r['runs']:>6}"
            f"{fmt(r['avg_rpd_pct'], 3):>12}{fmt(r['median_etb_pct'], 3):>12}")
        if r["runs"] < r["expected_runs"]:
            incomplete.append(f"{r['instance']}/{r['pull_strategy']} ({r['runs']}/{r['expected_runs']})")
    if incomplete:
        out(f"\nNote: fewer runs found than expected for: {', '.join(incomplete)}")

    # ---- Aggregate: instance -> pull strategy (avg over instances) ----
    out(f"\n{'PullStrategy':<12}{'RPD(%)':>12}{'ETB(%)':>12}")
    out("-" * 36)
    pull_strategy_rows = []
    for pull_strategy in PULL_STRATEGIES:
        rows = [r for r in summary_rows if r["pull_strategy"] == pull_strategy]

        def avg_field(f, rows=rows):
            vals = [r[f] for r in rows if r[f] is not None]
            return statistics.mean(vals) if vals else None

        pull_strategy_row = {
            "pull_strategy": pull_strategy,
            "avg_rpd_pct": avg_field("avg_rpd_pct"),
            "avg_median_etb_pct": avg_field("median_etb_pct"),
        }
        pull_strategy_rows.append(pull_strategy_row)
        out(f"{pull_strategy:<12}{fmt(pull_strategy_row['avg_rpd_pct'], 3):>12}"
            f"{fmt(pull_strategy_row['avg_median_etb_pct'], 3):>12}")

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
        detail_path = outputs_dir / "detail_rpd_etb.csv"
        write_csv(detail_path, detail_rows)
        saved.append(detail_path)
    if pull_strategy_rows:
        pull_strategy_path = outputs_dir / "pull_strategy_summary.csv"
        write_csv(pull_strategy_path, pull_strategy_rows)
        saved.append(pull_strategy_path)

    print("\nSaved:")
    for p in saved:
        print(f"  {p}")


if __name__ == "__main__":
    main()
