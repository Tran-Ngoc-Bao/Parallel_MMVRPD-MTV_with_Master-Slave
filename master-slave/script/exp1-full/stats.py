#!/usr/bin/env python3
"""
Statistics for run_full.sh output (exp1-full), plus a head-to-head
comparison against sequence/cpp's exp1-full (run4_ims_full.sh) output --
same idea as ../../../sequence/cpp/script/exp1/stats.py's sats-vs-ims
comparison, but here it's master-slave vs sequence's ims.

Per instance (n.combo), for each of seq_ims and ms independently:
  - avg result : mean working_time (seconds) over the runs found
  - avg RPD (%): (avg result - BKS) / BKS * 100
  - std dev    : sample standard deviation (ddof=1) of the per-run
                 working_time values
  - CV (%)     : std dev / avg result * 100

Comparison, per instance (only when both pipelines have an avg RPD):
  - delta_rpd (%) : avg_rpd(seq_ims) - avg_rpd(ms)
                     (positive => ms has the lower/better RPD)
  - ms_win         : 1 if delta_rpd > RPD_TIE_EPSILON, else 0
                     (a delta smaller than the epsilon is a tie, not a win --
                     without this, two runs that both land exactly on BKS can
                     show a "win" purely from floating-point summation-order
                     noise on the order of 1e-13/1e-14 percent)

Aggregated per customer count n (mean over that n's instances, except
ms_wins which is a "wins/total" fraction):
  - avg_rpd_seq_ims(n), avg_cv_seq_ims(n), avg_rpd_ms(n), avg_cv_ms(n)
  - avg_delta_rpd(n): mean of the instances' delta_rpd
  - ms_wins(n): (# instances where ms_win) / (# instances for that n),
    shown as e.g. "3/4"

Plus one final "overall" row aggregating across all n's the same way.

Instances are discovered from <ms-outputs>/<n>/*.json and
<seq-ims-outputs>/<n>/*.json file names (union of both, so an instance
missing from one side still shows up with "-" for that side), so this
works regardless of how many combos exist for a given n. BKS values come
from <bks>/<n>/<n>.<combo>-bks.json (shared between sequence and
master-slave, see ../../../sequence/cpp/script/gen_bks.py).

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


# Deltas smaller than this (in RPD percentage points) are treated as a tie
# rather than a win -- guards against floating-point summation-order noise
# (~1e-13/1e-14 %) when both pipelines land exactly on BKS being counted as
# a "win" for whichever side happens to round a hair lower.
RPD_TIE_EPSILON = 1e-6


def load_working_time(path: Path) -> float:
    with path.open() as f:
        data = json.load(f)
    return data["solution"]["working_time"]


def discover_instances(outputs_dir: Path, n: str):
    inst_dir = outputs_dir / n
    if not inst_dir.is_dir():
        return set()
    pattern = re.compile(rf"^{re.escape(n)}\.(.+)-(\d+)\.json$")
    instances = set()
    for f in inst_dir.iterdir():
        m = pattern.match(f.name)
        if m:
            instances.add(f"{n}.{m.group(1)}")
    return instances


def find_run_files(outputs_dir: Path, n: str, instance: str):
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
    return [f for _, f in found]


def compute_pipeline_stats(outputs_dir: Path, bks_value, n: str, instance: str):
    """avg result / avg RPD (%) / std dev / CV (%) for one instance, for a
    single pipeline (seq_ims or ms)."""
    run_files = find_run_files(outputs_dir, n, instance)

    if not run_files:
        return {
            "runs": 0, "avg_result": None, "avg_rpd_pct": None,
            "std_dev": None, "cv_pct": None,
        }

    results = [load_working_time(f) for f in run_files]
    avg_result = statistics.mean(results)
    std_dev = statistics.stdev(results) if len(results) > 1 else 0.0
    cv_pct = (std_dev / avg_result * 100.0) if avg_result else None
    avg_rpd_pct = ((avg_result - bks_value) / bks_value * 100.0
                   if bks_value else None)

    return {
        "runs": len(results), "avg_result": avg_result,
        "avg_rpd_pct": avg_rpd_pct, "std_dev": std_dev, "cv_pct": cv_pct,
    }


def compute_row(seq_ims_dir: Path, ms_dir: Path, bks_dir: Path, n: str, instance: str):
    bks_path = bks_dir / n / f"{instance}-bks.json"
    bks_value = load_working_time(bks_path) if bks_path.is_file() else None

    seq_ims = compute_pipeline_stats(seq_ims_dir, bks_value, n, instance)
    ms = compute_pipeline_stats(ms_dir, bks_value, n, instance)

    delta_rpd = None
    ms_win = None
    if seq_ims["avg_rpd_pct"] is not None and ms["avg_rpd_pct"] is not None:
        delta_rpd = seq_ims["avg_rpd_pct"] - ms["avg_rpd_pct"]
        ms_win = 1 if delta_rpd > RPD_TIE_EPSILON else 0

    return {
        "n": n, "instance": instance, "bks": bks_value,
        "seq_ims_runs": seq_ims["runs"], "seq_ims_avg_result": seq_ims["avg_result"],
        "seq_ims_avg_rpd_pct": seq_ims["avg_rpd_pct"], "seq_ims_std_dev": seq_ims["std_dev"],
        "seq_ims_cv_pct": seq_ims["cv_pct"],
        "ms_runs": ms["runs"], "ms_avg_result": ms["avg_result"],
        "ms_avg_rpd_pct": ms["avg_rpd_pct"], "ms_std_dev": ms["std_dev"],
        "ms_cv_pct": ms["cv_pct"],
        "delta_rpd_pct": delta_rpd, "ms_win": ms_win,
    }


def fmt(v, prec=4):
    return f"{v:.{prec}f}" if v is not None else "-"


def fmt_fraction(count, total):
    return f"{count}/{total}"


def round2(v):
    return round(v, 2) if v is not None else None


def rounded_summary(s):
    """Rounds a summary row's avg_* fields to 2dp and recomputes
    avg_delta_rpd_pct from the already-rounded seq_ims/ms values, so the
    displayed delta always exactly equals (rounded seq_ims - rounded ms)
    instead of drifting a cent off from rounding a separately-averaged
    delta on its own."""
    seq_ims_r = round2(s["avg_rpd_seq_ims_pct"])
    ms_r = round2(s["avg_rpd_ms_pct"])
    delta_r = round2(seq_ims_r - ms_r) if seq_ims_r is not None and ms_r is not None else None
    return {
        "avg_rpd_seq_ims_pct": seq_ims_r, "avg_cv_seq_ims_pct": round2(s["avg_cv_seq_ims_pct"]),
        "avg_rpd_ms_pct": ms_r, "avg_cv_ms_pct": round2(s["avg_cv_ms_pct"]),
        "avg_delta_rpd_pct": delta_r,
    }


def pipeline_view(row, pipeline: str):
    """Extract the seq_ims-only or ms-only columns of a combined row, in the
    single-pipeline shape (instance, runs, avg_result, bks, avg_rpd_pct,
    std_dev, cv_pct) -- what gets saved into <outputs>/<n>/stats.csv."""
    p = pipeline + "_"
    return {
        "n": row["n"], "instance": row["instance"],
        "runs": row[p + "runs"], "avg_result": row[p + "avg_result"],
        "bks": row["bks"], "avg_rpd_pct": row[p + "avg_rpd_pct"],
        "std_dev": row[p + "std_dev"], "cv_pct": row[p + "cv_pct"],
    }


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

    wins = sum(r["ms_win"] for r in rows_for_n if r["ms_win"] is not None)
    n_instances = len(rows_for_n)

    return {
        "avg_rpd_seq_ims_pct": avg("seq_ims_avg_rpd_pct"),
        "avg_cv_seq_ims_pct": avg("seq_ims_cv_pct"),
        "avg_rpd_ms_pct": avg("ms_avg_rpd_pct"),
        "avg_cv_ms_pct": avg("ms_cv_pct"),
        "avg_delta_rpd_pct": avg("delta_rpd_pct"),
        "ms_wins_count": wins,
        "ms_wins_total": n_instances,
    }


def compute_overall_summary(summary_rows):
    """One extra row aggregating across all n's, same pattern as
    compute_n_summary (instance -> n) applied one level up (n -> overall):
    mean of each n's value, and ms_wins as total wins / total instances
    across all n's."""
    def avg(key):
        vals = [r[key] for r in summary_rows if r[key] is not None]
        return statistics.mean(vals) if vals else None

    return {
        "n": "overall",
        "avg_rpd_seq_ims_pct": avg("avg_rpd_seq_ims_pct"),
        "avg_cv_seq_ims_pct": avg("avg_cv_seq_ims_pct"),
        "avg_rpd_ms_pct": avg("avg_rpd_ms_pct"),
        "avg_cv_ms_pct": avg("avg_cv_ms_pct"),
        "avg_delta_rpd_pct": avg("avg_delta_rpd_pct"),
        "ms_wins_count": sum(r["ms_wins_count"] for r in summary_rows),
        "ms_wins_total": sum(r["ms_wins_total"] for r in summary_rows),
    }


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--seq-ims-outputs", default="../../../sequence/cpp/outputs/exp1-full/ims",
                     help="Dir containing sequence/cpp's run4_ims_full.sh output, relative "
                          "to this script (default: "
                          "../../../sequence/cpp/outputs/exp1-full/ims)")
    ap.add_argument("--ms-outputs", default="../../outputs/exp1-full/ms",
                     help="Dir containing run_full.sh output, relative to this script "
                          "(default: ../../outputs/exp1-full/ms)")
    ap.add_argument("--bks", default="../../../bks",
                     help="Dir containing BKS json files, relative to this script "
                          "(default: ../../../bks)")
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
    seq_ims_dir = (script_dir / args.seq_ims_outputs).resolve()
    ms_dir = (script_dir / args.ms_outputs).resolve()
    bks_dir = (script_dir / args.bks).resolve()
    outputs_root = (script_dir / args.outputs).resolve()

    customers = [c.strip() for c in args.customers.split(",") if c.strip()]

    rows = []
    for n in customers:
        instances = sorted(discover_instances(seq_ims_dir, n) | discover_instances(ms_dir, n))
        for instance in instances:
            rows.append(compute_row(seq_ims_dir, ms_dir, bks_dir, n, instance))

    print(f"seq_ims outputs: {seq_ims_dir}\nms outputs:      {ms_dir}\n")

    header = (f"{'Instance':<12}{'BKS(s)':>12}"
              f"{'RPD seq_ims(%)':>16}{'CV seq_ims(%)':>15}"
              f"{'RPD ms(%)':>13}{'CV ms(%)':>12}"
              f"{'delta_rpd(%)':>13}{'ms_win':>8}")
    print(header)
    print("-" * len(header))
    for r in rows:
        win = "-" if r["ms_win"] is None else str(r["ms_win"])
        print(f"{r['instance']:<12}{fmt(r['bks']):>12}"
              f"{fmt(r['seq_ims_avg_rpd_pct'], 3):>16}{fmt(r['seq_ims_cv_pct'], 3):>15}"
              f"{fmt(r['ms_avg_rpd_pct'], 3):>13}{fmt(r['ms_cv_pct'], 3):>12}"
              f"{fmt(r['delta_rpd_pct'], 3):>13}{win:>8}")
        for label, key in (("seq_ims", "seq_ims_runs"), ("ms", "ms_runs")):
            if r[key] and r[key] < args.runs:
                print(f"    note: {label} only {r[key]}/{args.runs} runs found")

    print("\nNote: std dev is the sample standard deviation (ddof=1) of the per-run "
          "working_time values. delta_rpd = avg_rpd(seq_ims) - avg_rpd(ms); positive "
          f"means ms had the lower (better) RPD. ms_win = 1 if delta_rpd > {RPD_TIE_EPSILON:g} "
          "(smaller deltas count as a tie, not a win).")

    # ---- Aggregate: instance -> customer count n ----
    summary_rows = []
    for n in customers:
        rows_for_n = [r for r in rows if r["n"] == n]
        if not rows_for_n:
            continue
        s = compute_n_summary(rows_for_n)
        summary_rows.append({"n": n, **s})

    # ---- Aggregate: customer count n -> overall (all n's combined) ----
    overall_row = compute_overall_summary(summary_rows)
    summary_rows.append(overall_row)

    print(f"\n{'n':<8}{'avg_rpd_seq_ims(%)':>20}{'avg_cv_seq_ims(%)':>19}"
          f"{'avg_rpd_ms(%)':>15}{'avg_cv_ms(%)':>14}"
          f"{'avg_delta_rpd(%)':>18}{'ms_wins':>10}")
    print("-" * 117)
    for s in summary_rows:
        if s is overall_row:
            print("-" * 117)
        ms_wins_str = fmt_fraction(s["ms_wins_count"], s["ms_wins_total"])
        rs = rounded_summary(s)
        print(f"{s['n']:<8}{fmt(rs['avg_rpd_seq_ims_pct'], 2):>20}{fmt(rs['avg_cv_seq_ims_pct'], 2):>19}"
              f"{fmt(rs['avg_rpd_ms_pct'], 2):>15}{fmt(rs['avg_cv_ms_pct'], 2):>14}"
              f"{fmt(rs['avg_delta_rpd_pct'], 2):>18}{ms_wins_str:>10}")

    if args.no_save:
        return

    # ---- Save: per-n stats.csv inside each pipeline's own <n>/ dir ----
    saved = []
    for n in customers:
        rows_for_n = [r for r in rows if r["n"] == n]
        if not rows_for_n:
            continue

        seq_ims_rows = [pipeline_view(r, "seq_ims") for r in rows_for_n]
        seq_ims_path = seq_ims_dir / n / "stats_vs_ms.csv"
        write_csv(seq_ims_path, seq_ims_rows)
        saved.append(seq_ims_path)

        ms_rows = [pipeline_view(r, "ms") for r in rows_for_n]
        ms_path = ms_dir / n / "stats.csv"
        write_csv(ms_path, ms_rows)
        saved.append(ms_path)

    # ---- Save: one combined summary.csv (per-n comparison) in outputs/ ----
    summary_path = outputs_root / "summary.csv"
    summary_csv_rows = [
        {"n": s["n"], **rounded_summary(s),
         "ms_wins": fmt_fraction(s["ms_wins_count"], s["ms_wins_total"])}
        for s in summary_rows
    ]
    write_csv(summary_path, summary_csv_rows)
    saved.append(summary_path)

    print("\nSaved:")
    for p in saved:
        print(f"  {p}")


if __name__ == "__main__":
    main()
