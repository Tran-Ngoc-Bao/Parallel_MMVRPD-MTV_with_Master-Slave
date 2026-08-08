#!/usr/bin/env python3
"""
Statistics for run4_sats.sh and run4_ims.sh output, plus a head-to-head
comparison between the two.

Per instance (n.combo), for each of sats and ims independently:
  - avg result : mean working_time (seconds) over the runs found
  - avg RPD (%): (avg result - BKS) / BKS * 100
  - std dev    : sample standard deviation (ddof=1) of the per-run
                 working_time values
  - CV (%)     : std dev / avg result * 100

Comparison, per instance (only when both sats and ims have an avg RPD):
  - delta_rpd (%) : avg_rpd(sats) - avg_rpd(ims)
                     (positive => ims has the lower/better RPD)
  - ims_win        : 1 if delta_rpd > RPD_TIE_EPSILON, else 0
                     (a delta smaller than the epsilon is a tie, not a win --
                     without this, two runs that both land exactly on BKS can
                     show a "win" purely from floating-point summation-order
                     noise on the order of 1e-13/1e-14 percent)

Aggregated per customer count n (mean over that n's instances, except
ims_wins which is a "wins/total" fraction):
  - avg_rpd_sats(n), avg_cv_sats(n), avg_rpd_ims(n), avg_cv_ims(n)
  - avg_delta_rpd(n): mean of the instances' delta_rpd
  - ims_wins(n): (# instances where ims_win) / (# instances for that n),
    shown as e.g. "3/4"

Plus one final "overall" row aggregating across all n's the same way
(mean of the n's, ims_wins = total wins / total instances across all n,
e.g. "6/16").

For each customer count n in --customers and each combo in --combos, looks
for run files at <outputs>/<n>/<n>.<combo>-<run_id>.json (the naming used
by script.sh/run4_sats.sh/run4_ims.sh) and the matching BKS file at
<bks>/<n>/<n>.<combo>-bks.json.

Usage:
    python3 stats.py
    python3 stats.py --customers 100 --csv instances.csv --csv-summary summary.csv
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
    single pipeline (sats or ims)."""
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


def compute_row(sats_dir: Path, ims_dir: Path, bks_dir: Path, n: str, combo: str):
    instance = f"{n}.{combo}"

    bks_path = bks_dir / n / f"{instance}-bks.json"
    bks_value = load_working_time(bks_path) if bks_path.is_file() else None

    sats = compute_pipeline_stats(sats_dir, bks_value, n, instance)
    ims = compute_pipeline_stats(ims_dir, bks_value, n, instance)

    delta_rpd = None
    ims_win = None
    if sats["avg_rpd_pct"] is not None and ims["avg_rpd_pct"] is not None:
        delta_rpd = sats["avg_rpd_pct"] - ims["avg_rpd_pct"]
        ims_win = 1 if delta_rpd > RPD_TIE_EPSILON else 0

    return {
        "n": n, "instance": instance, "bks": bks_value,
        "sats_runs": sats["runs"], "sats_avg_result": sats["avg_result"],
        "sats_avg_rpd_pct": sats["avg_rpd_pct"], "sats_std_dev": sats["std_dev"],
        "sats_cv_pct": sats["cv_pct"],
        "ims_runs": ims["runs"], "ims_avg_result": ims["avg_result"],
        "ims_avg_rpd_pct": ims["avg_rpd_pct"], "ims_std_dev": ims["std_dev"],
        "ims_cv_pct": ims["cv_pct"],
        "delta_rpd_pct": delta_rpd, "ims_win": ims_win,
    }


def fmt(v, prec=4):
    return f"{v:.{prec}f}" if v is not None else "-"


def fmt_fraction(count, total):
    return f"{count}/{total}"


def pipeline_view(row, pipeline: str):
    """Extract the sats-only or ims-only columns of a combined row, in the
    single-pipeline shape (instance, runs, avg_result, bks, avg_rpd_pct,
    std_dev, cv_pct) -- what gets saved into <outputs>/<pipeline>/<n>/stats.csv."""
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

    wins = sum(r["ims_win"] for r in rows_for_n if r["ims_win"] is not None)
    n_instances = len(rows_for_n)

    return {
        "avg_rpd_sats_pct": avg("sats_avg_rpd_pct"),
        "avg_cv_sats_pct": avg("sats_cv_pct"),
        "avg_rpd_ims_pct": avg("ims_avg_rpd_pct"),
        "avg_cv_ims_pct": avg("ims_cv_pct"),
        "avg_delta_rpd_pct": avg("delta_rpd_pct"),
        "ims_wins_count": wins,
        "ims_wins_total": n_instances,
    }


def compute_overall_summary(summary_rows):
    """One extra row aggregating across all n's, same pattern as
    compute_n_summary (instance -> n) applied one level up (n -> overall):
    mean of each n's value, and ims_wins as total wins / total instances
    across all n's (e.g. "6/16" for 4 n's x 4 instances)."""
    def avg(key):
        vals = [r[key] for r in summary_rows if r[key] is not None]
        return statistics.mean(vals) if vals else None

    return {
        "n": "overall",
        "avg_rpd_sats_pct": avg("avg_rpd_sats_pct"),
        "avg_cv_sats_pct": avg("avg_cv_sats_pct"),
        "avg_rpd_ims_pct": avg("avg_rpd_ims_pct"),
        "avg_cv_ims_pct": avg("avg_cv_ims_pct"),
        "avg_delta_rpd_pct": avg("avg_delta_rpd_pct"),
        "ims_wins_count": sum(r["ims_wins_count"] for r in summary_rows),
        "ims_wins_total": sum(r["ims_wins_total"] for r in summary_rows),
    }


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--sats-outputs", default="../outputs/sats",
                     help="Dir containing run4_sats.sh output, relative to this script "
                          "(default: ../outputs/sats)")
    ap.add_argument("--ims-outputs", default="../outputs/ims",
                     help="Dir containing run4_ims.sh output, relative to this script "
                          "(default: ../outputs/ims)")
    ap.add_argument("--bks", default="../../../bks",
                     help="Dir containing BKS json files, relative to this script "
                          "(default: ../../../bks)")
    ap.add_argument("--customers", default="100,200,500,1000",
                     help="Comma-separated customer counts (default: 100,200,500,1000)")
    ap.add_argument("--combos", default="10.1,20.2,30.3,40.4",
                     help="Comma-separated instance combos, matches run4_sats.sh/run4_ims.sh "
                          "(default: 10.1,20.2,30.3,40.4)")
    ap.add_argument("--runs", type=int, default=10,
                     help="Expected number of runs per instance, for the 'only X/N "
                          "found' note (default: 10)")
    ap.add_argument("--outputs", default="../outputs",
                     help="Root outputs dir (parent of sats/ and ims/), relative to this "
                          "script -- where summary.csv is written (default: ../outputs)")
    ap.add_argument("--no-save", action="store_true",
                     help="Only print to stdout, don't write any CSV files")
    args = ap.parse_args()

    script_dir = Path(__file__).resolve().parent
    sats_dir = (script_dir / args.sats_outputs).resolve()
    ims_dir = (script_dir / args.ims_outputs).resolve()
    bks_dir = (script_dir / args.bks).resolve()
    outputs_root = (script_dir / args.outputs).resolve()

    customers = [c.strip() for c in args.customers.split(",") if c.strip()]
    combos = [c.strip() for c in args.combos.split(",") if c.strip()]

    rows = []
    for n in customers:
        for combo in combos:
            rows.append(compute_row(sats_dir, ims_dir, bks_dir, n, combo))

    print(f"sats outputs: {sats_dir}\nims outputs:  {ims_dir}\n")

    header = (f"{'Instance':<12}{'BKS(s)':>12}"
              f"{'RPD sats(%)':>13}{'CV sats(%)':>12}"
              f"{'RPD ims(%)':>13}{'CV ims(%)':>12}"
              f"{'delta_rpd(%)':>13}{'ims_win':>8}")
    print(header)
    print("-" * len(header))
    for r in rows:
        win = "-" if r["ims_win"] is None else str(r["ims_win"])
        print(f"{r['instance']:<12}{fmt(r['bks']):>12}"
              f"{fmt(r['sats_avg_rpd_pct'], 3):>13}{fmt(r['sats_cv_pct'], 3):>12}"
              f"{fmt(r['ims_avg_rpd_pct'], 3):>13}{fmt(r['ims_cv_pct'], 3):>12}"
              f"{fmt(r['delta_rpd_pct'], 3):>13}{win:>8}")

    print("\nNote: std dev is the sample standard deviation (ddof=1) of the per-run "
          "working_time values. delta_rpd = avg_rpd(sats) - avg_rpd(ims); positive "
          f"means ims had the lower (better) RPD. ims_win = 1 if delta_rpd > {RPD_TIE_EPSILON:g} "
          "(smaller deltas count as a tie, not a win).")

    # ---- Aggregate: instance -> customer count n ----
    summary_rows = []
    for n in customers:
        rows_for_n = [r for r in rows if r["n"] == n]
        s = compute_n_summary(rows_for_n)
        summary_rows.append({"n": n, **s})

    # ---- Aggregate: customer count n -> overall (all n's combined) ----
    overall_row = compute_overall_summary(summary_rows)
    summary_rows.append(overall_row)

    print(f"\n{'n':<8}{'avg_rpd_sats(%)':>17}{'avg_cv_sats(%)':>16}"
          f"{'avg_rpd_ims(%)':>16}{'avg_cv_ims(%)':>15}"
          f"{'avg_delta_rpd(%)':>18}{'ims_wins':>10}")
    print("-" * 111)
    for s in summary_rows:
        if s is overall_row:
            print("-" * 111)
        ims_wins_str = fmt_fraction(s["ims_wins_count"], s["ims_wins_total"])
        print(f"{s['n']:<8}{fmt(s['avg_rpd_sats_pct'], 3):>17}{fmt(s['avg_cv_sats_pct'], 3):>16}"
              f"{fmt(s['avg_rpd_ims_pct'], 3):>16}{fmt(s['avg_cv_ims_pct'], 3):>15}"
              f"{fmt(s['avg_delta_rpd_pct'], 3):>18}{ims_wins_str:>10}")

    if args.no_save:
        return

    # ---- Save: per-n stats.csv inside each pipeline's own <n>/ dir ----
    saved = []
    for n in customers:
        rows_for_n = [r for r in rows if r["n"] == n]

        sats_rows = [pipeline_view(r, "sats") for r in rows_for_n]
        sats_path = sats_dir / n / "stats.csv"
        write_csv(sats_path, sats_rows)
        saved.append(sats_path)

        ims_rows = [pipeline_view(r, "ims") for r in rows_for_n]
        ims_path = ims_dir / n / "stats.csv"
        write_csv(ims_path, ims_rows)
        saved.append(ims_path)

    # ---- Save: one combined summary.csv (per-n comparison) in outputs/ ----
    summary_path = outputs_root / "summary.csv"
    summary_csv_rows = [
        {**{k: v for k, v in s.items() if k not in ("ims_wins_count", "ims_wins_total")},
         "ims_wins": fmt_fraction(s["ims_wins_count"], s["ims_wins_total"])}
        for s in summary_rows
    ]
    write_csv(summary_path, summary_csv_rows)
    saved.append(summary_path)

    print("\nSaved:")
    for p in saved:
        print(f"  {p}")


if __name__ == "__main__":
    main()
