#!/usr/bin/env python3
"""
Statistics for run2.sh (exp1.5, master-slave) output.

Same idea as sequence/cpp/script/exp1.5/stats.py, adapted for
master-slave's output layout. master-slave's run2.sh writes one file per
rep directly (a single MPI job -- 1 master + N-1 workers exchanging elite
solutions -- not an island-model best-of-N like sequence/cpp's
run2_ims.sh), but the run file naming is the same, so the stats logic is
identical. Reports two views per instance (n.combo):
  - summary: avg RPD (%) over the runs found -- one row per instance, for
    a quick overview
  - detail: RPD (%) for EACH individual run (rep) of each instance, so you
    can see run-to-run spread instead of just the average

RPD (%) = (working_time - BKS) / BKS * 100.

For each customer count n in --customers and each combo in --combos, looks
for run files at <outputs>/<n>/<n>.<combo>-<run_id>.json (the naming used
by run2.sh) and the matching BKS file at <bks>/<n>/<n>.<combo>-bks.json.
If a BKS file is missing, RPD is reported as "-" for that instance.

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


def load_working_time(path: Path) -> float:
    with path.open() as f:
        data = json.load(f)
    return data["solution"]["working_time"]


def find_run_files(outputs_dir: Path, n: str, instance: str):
    """Returns [(run_id, path), ...] sorted by run_id."""
    inst_dir = outputs_dir / n
    if not inst_dir.is_dir():
        return []
    pattern = re.compile(rf"^{re.escape(instance)}-(\d+)\.json$")
    found = []
    for f in inst_dir.iterdir():
        m = pattern.match(f.name)
        if m:
            found.append((int(m.group(1)), f))
    found.sort(key=lambda t: t[0])
    return found


def rpd_pct(working_time, bks_value):
    if bks_value is None or not bks_value:
        return None
    return (working_time - bks_value) / bks_value * 100.0


def compute_instance(outputs_dir: Path, bks_dir: Path, n: str, combo: str, expected_runs: int):
    instance = f"{n}.{combo}"
    bks_path = bks_dir / n / f"{instance}-bks.json"
    bks_value = load_working_time(bks_path) if bks_path.is_file() else None

    run_files = find_run_files(outputs_dir, n, instance)

    detail_rows = []
    results = []
    for run_id, path in run_files:
        working_time = load_working_time(path)
        results.append(working_time)
        detail_rows.append({
            "n": n, "instance": instance, "run": run_id,
            "working_time": working_time, "bks": bks_value,
            "rpd_pct": rpd_pct(working_time, bks_value),
        })

    avg_result = statistics.mean(results) if results else None
    avg_rpd = rpd_pct(avg_result, bks_value) if avg_result is not None else None

    summary_row = {
        "n": n, "instance": instance, "bks": bks_value,
        "runs": len(results), "expected_runs": expected_runs,
        "avg_result": avg_result, "avg_rpd_pct": avg_rpd,
    }
    return summary_row, detail_rows


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
    ap.add_argument("--outputs", default="../../outputs/exp1.5",
                     help="Dir containing run2.sh output, relative to this script "
                          "(default: ../../outputs/exp1.5)")
    ap.add_argument("--bks", default="../../../bks",
                     help="Dir containing BKS json files, relative to this script "
                          "(default: ../../../bks)")
    ap.add_argument("--customers", default="200,500,1000",
                     help="Comma-separated customer counts (default: 200,500,1000)")
    ap.add_argument("--combos", default="10.2,40.1",
                     help="Comma-separated instance combos, matches run2.sh "
                          "(default: 10.2,40.1)")
    ap.add_argument("--runs", type=int, default=10,
                     help="Expected number of runs per instance, used to flag "
                          "instances with fewer runs found than expected (default: 10)")
    ap.add_argument("--no-save", action="store_true",
                     help="Only print to stdout, don't write any CSV files")
    args = ap.parse_args()

    script_dir = Path(__file__).resolve().parent
    outputs_dir = (script_dir / args.outputs).resolve()
    bks_dir = (script_dir / args.bks).resolve()

    customers = [c.strip() for c in args.customers.split(",") if c.strip()]
    combos = [c.strip() for c in args.combos.split(",") if c.strip()]

    print(f"outputs: {outputs_dir}\nbks:     {bks_dir}\n")

    summary_rows = []
    detail_rows = []
    for n in customers:
        for combo in combos:
            s, d = compute_instance(outputs_dir, bks_dir, n, combo, args.runs)
            summary_rows.append(s)
            detail_rows.extend(d)

    # ---- Print: per-instance summary (avg RPD only) ----
    header = f"{'Instance':<12}{'BKS(s)':>12}{'Runs':>8}{'AvgResult(s)':>15}{'AvgRPD(%)':>12}"
    print(header)
    print("-" * len(header))
    incomplete = []
    for r in summary_rows:
        print(f"{r['instance']:<12}{fmt(r['bks']):>12}{r['runs']:>8}"
              f"{fmt(r['avg_result']):>15}{fmt(r['avg_rpd_pct'], 3):>12}")
        if r["runs"] < r["expected_runs"]:
            incomplete.append(f"{r['instance']} ({r['runs']}/{r['expected_runs']})")
    if incomplete:
        print(f"\nNote: fewer runs found than expected for: {', '.join(incomplete)}")

    # ---- Aggregate: instance -> customer count n ----
    print(f"\n{'n':<8}{'avg_rpd(%)':>14}")
    print("-" * 22)
    for n in customers:
        vals = [r["avg_rpd_pct"] for r in summary_rows if r["n"] == n and r["avg_rpd_pct"] is not None]
        avg = statistics.mean(vals) if vals else None
        print(f"{n:<8}{fmt(avg, 3):>14}")
    all_vals = [r["avg_rpd_pct"] for r in summary_rows if r["avg_rpd_pct"] is not None]
    overall = statistics.mean(all_vals) if all_vals else None
    print("-" * 22)
    print(f"{'overall':<8}{fmt(overall, 3):>14}")

    # ---- Print: per-run RPD detail ----
    print("\nPer-run RPD detail:")
    dheader = f"{'Instance':<12}{'Run':>6}{'WorkingTime(s)':>16}{'RPD(%)':>10}"
    print(dheader)
    print("-" * len(dheader))
    for r in detail_rows:
        print(f"{r['instance']:<12}{r['run']:>6}{fmt(r['working_time']):>16}{fmt(r['rpd_pct'], 3):>10}")

    if args.no_save:
        return

    saved = []
    if summary_rows:
        summary_path = outputs_dir / "summary.csv"
        write_csv(summary_path, summary_rows)
        saved.append(summary_path)
    if detail_rows:
        detail_path = outputs_dir / "detail_rpd.csv"
        write_csv(detail_path, detail_rows)
        saved.append(detail_path)

    print("\nSaved:")
    for p in saved:
        print(f"  {p}")


if __name__ == "__main__":
    main()
