#!/usr/bin/env python3
"""
Statistics for run4_ims_full.sh output (exp1-full): ims-only pipeline run
across every instance actually present in data/ for each customer count
(not just the diagonal 4 combos ../exp1 uses).

Per instance (n.combo):
  - avg result : mean working_time (seconds) over the runs found
  - avg RPD (%): (avg result - BKS) / BKS * 100
  - std dev    : sample standard deviation (ddof=1) of the per-run
                 working_time values
  - CV (%)     : std dev / avg result * 100

Aggregated per customer count n (mean over that n's instances), plus one
final "overall" row (mean over all n's).

Instances are discovered directly from <ims-outputs>/<n>/*.json file
names, so this works regardless of how many combos exist for a given n.
BKS values come from <bks>/<n>/<n>.<combo>-bks.json (see gen_bks.py).

Usage:
    python3 stats.py
    python3 stats.py --customers 6,10,12,20,50 --runs 10
"""
import argparse
import csv
import json
import re
import statistics
from pathlib import Path


def load_working_time(path: Path) -> float:
    with path.open() as f:
        data = json.load(f)
    return data["solution"]["working_time"]


def discover_instances(outputs_dir: Path, n: str):
    inst_dir = outputs_dir / n
    if not inst_dir.is_dir():
        return []
    pattern = re.compile(rf"^{re.escape(n)}\.(.+)-(\d+)\.json$")
    instances = set()
    for f in inst_dir.iterdir():
        m = pattern.match(f.name)
        if m:
            instances.add(f"{n}.{m.group(1)}")
    return sorted(instances)


def find_run_files(outputs_dir: Path, n: str, instance: str):
    inst_dir = outputs_dir / n
    pattern = re.compile(rf"^{re.escape(instance)}-(\d+)\.json$")
    found = []
    for f in inst_dir.iterdir():
        m = pattern.match(f.name)
        if m:
            found.append((int(m.group(1)), f))
    found.sort(key=lambda t: t[0])
    return [f for _, f in found]


def compute_row(ims_dir: Path, bks_dir: Path, n: str, instance: str):
    bks_path = bks_dir / n / f"{instance}-bks.json"
    bks_value = load_working_time(bks_path) if bks_path.is_file() else None

    run_files = find_run_files(ims_dir, n, instance)
    if not run_files:
        return {"n": n, "instance": instance, "bks": bks_value, "runs": 0,
                "avg_result": None, "avg_rpd_pct": None, "std_dev": None, "cv_pct": None}

    results = [load_working_time(f) for f in run_files]
    avg_result = statistics.mean(results)
    std_dev = statistics.stdev(results) if len(results) > 1 else 0.0
    cv_pct = (std_dev / avg_result * 100.0) if avg_result else None
    avg_rpd_pct = ((avg_result - bks_value) / bks_value * 100.0
                   if bks_value else None)

    return {"n": n, "instance": instance, "bks": bks_value, "runs": len(results),
            "avg_result": avg_result, "avg_rpd_pct": avg_rpd_pct,
            "std_dev": std_dev, "cv_pct": cv_pct}


def fmt(v, prec=4):
    return f"{v:.{prec}f}" if v is not None else "-"


def write_csv(path: Path, rows):
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
        writer.writeheader()
        writer.writerows(rows)


def compute_n_summary(rows_for_n):
    def avg(key):
        vals = [r[key] for r in rows_for_n if r[key] is not None]
        return statistics.mean(vals) if vals else None
    return {"avg_rpd_pct": avg("avg_rpd_pct"), "avg_cv_pct": avg("cv_pct"),
            "n_instances": len(rows_for_n)}


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--ims-outputs", default="../../outputs/exp1-full/ims",
                     help="Dir containing run4_ims_full.sh output, relative to this "
                          "script (default: ../../outputs/exp1-full/ims)")
    ap.add_argument("--bks", default="../../../../bks",
                     help="Dir containing BKS json files, relative to this script "
                          "(default: ../../../../bks)")
    ap.add_argument("--customers", default="6,10,12,20,50,100,200,500,1000",
                     help="Comma-separated customer counts (default: "
                          "6,10,12,20,50,100,200,500,1000)")
    ap.add_argument("--runs", type=int, default=10,
                     help="Expected number of runs per instance, for the 'only X/N "
                          "found' note (default: 10)")
    ap.add_argument("--outputs", default="../../outputs/exp1-full",
                     help="Root outputs dir, relative to this script -- where "
                          "summary.csv is written (default: ../../outputs/exp1-full)")
    ap.add_argument("--no-save", action="store_true",
                     help="Only print to stdout, don't write any CSV files")
    args = ap.parse_args()

    script_dir = Path(__file__).resolve().parent
    ims_dir = (script_dir / args.ims_outputs).resolve()
    bks_dir = (script_dir / args.bks).resolve()
    outputs_root = (script_dir / args.outputs).resolve()

    customers = [c.strip() for c in args.customers.split(",") if c.strip()]

    rows = []
    for n in customers:
        for instance in discover_instances(ims_dir, n):
            rows.append(compute_row(ims_dir, bks_dir, n, instance))

    print(f"ims outputs: {ims_dir}\n")
    header = f"{'Instance':<12}{'BKS(s)':>12}{'RPD ims(%)':>13}{'CV ims(%)':>12}{'runs':>8}"
    print(header)
    print("-" * len(header))
    for r in rows:
        print(f"{r['instance']:<12}{fmt(r['bks']):>12}"
              f"{fmt(r['avg_rpd_pct'], 3):>13}{fmt(r['cv_pct'], 3):>12}{r['runs']:>8}")
        if r["runs"] and r["runs"] < args.runs:
            print(f"    note: only {r['runs']}/{args.runs} runs found")

    print("\nNote: std dev is the sample standard deviation (ddof=1) of the per-run "
          "working_time values.")

    summary_rows = []
    for n in customers:
        rows_for_n = [r for r in rows if r["n"] == n]
        if not rows_for_n:
            continue
        s = compute_n_summary(rows_for_n)
        summary_rows.append({"n": n, **s})

    def avg_all(key):
        vals = [r[key] for r in summary_rows if r[key] is not None]
        return statistics.mean(vals) if vals else None

    overall = {"n": "overall", "avg_rpd_pct": avg_all("avg_rpd_pct"),
               "avg_cv_pct": avg_all("avg_cv_pct"),
               "n_instances": sum(r["n_instances"] for r in summary_rows)}
    summary_rows.append(overall)

    print(f"\n{'n':<8}{'avg_rpd_ims(%)':>16}{'avg_cv_ims(%)':>15}{'instances':>11}")
    print("-" * 50)
    for s in summary_rows:
        if s is overall:
            print("-" * 50)
        print(f"{s['n']:<8}{fmt(s['avg_rpd_pct'], 2):>16}{fmt(s['avg_cv_pct'], 2):>15}"
              f"{s['n_instances']:>11}")

    if args.no_save:
        return

    saved = []
    for n in customers:
        rows_for_n = [r for r in rows if r["n"] == n]
        if not rows_for_n:
            continue
        path = ims_dir / n / "stats.csv"
        write_csv(path, rows_for_n)
        saved.append(path)

    summary_path = outputs_root / "summary.csv"
    write_csv(summary_path, summary_rows)
    saved.append(summary_path)

    print("\nSaved:")
    for p in saved:
        print(f"  {p}")


if __name__ == "__main__":
    main()
