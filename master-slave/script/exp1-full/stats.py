#!/usr/bin/env python3
"""
Statistics for master-slave exp1-full (run_full.sh, the cooperative
pipeline tagged "coop"), side by side with sequence/cpp's exp1-full sats
and ims pipelines.

Per instance (n.combo):
  - RPD sats (%)  : mean final RPD of sequence/cpp sats
  - RPD ims (%)   : mean final RPD of sequence/cpp ims
  - RPD coop (%)  : mean final RPD of master-slave coop
    where final RPD of a run = (working_time - BKS) / BKS * 100, and the
    per-instance value is the mean over the runs found.
  - delta_coop (%): RPD ims - RPD coop   (bigger is better: positive means
                    coop's RPD is lower, i.e. coop is better than ims)
  - result        : coop vs ims -- "win" if delta_coop > RPD_TIE_EPSILON,
                    "loss" if delta_coop < -RPD_TIE_EPSILON, else "tie"
  - evals coop/ims: per run, coop run k's all-worker eval total divided by
                    ims run k's (paired by run index) -- coop uses
                    total_evaluations, ims uses total_evaluations_all_workers,
                    both being the sum over every worker of that run. Those
                    per-run ratios are then meaned over the instance's paired
                    runs.

Aggregated per customer count n as the unweighted mean over that n's
instances of each per-instance value (win/tie/loss as "count/total"),
i.e. the same run -> instance -> customer order as every other column.
Plus one final "overall" row: the unweighted mean over all instances.

Instances are discovered from the union of the three pipelines' file names
under <dir>/<n>/. BKS values come from <bks>/<n>/<n>.<combo>-bks.json
(shared, see ../../../sequence/cpp/script/gen_bks.py).

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


# Deltas smaller than this (in RPD percentage points) are a tie, not a
# win/loss -- guards against floating-point summation-order noise
# (~1e-13/1e-14 %) when both pipelines land exactly on BKS.
RPD_TIE_EPSILON = 1e-6

COOP_EVALS_FIELD = "total_evaluations"
IMS_EVALS_FIELD = "total_evaluations_all_workers"

# Instances used to tune parameters; the "held-out" summary row aggregates
# every other instance.
TUNING_INSTANCES = {"200.10.2", "200.40.1", "500.10.2", "500.40.1"}


def load_working_time(path: Path) -> float:
    with path.open() as f:
        data = json.load(f)
    return data["solution"]["working_time"]


def load_field(path: Path, field: str):
    with path.open() as f:
        data = json.load(f)
    return data.get(field)


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


def find_run_files_indexed(outputs_dir: Path, n: str, instance: str):
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


def find_run_files(outputs_dir: Path, n: str, instance: str):
    return [f for _, f in find_run_files_indexed(outputs_dir, n, instance)]


def load_evals_by_run(outputs_dir: Path, n, instance, field):
    """{run_index: that run's all-worker evaluation total} for the runs that
    carry `field`."""
    out = {}
    for idx, path in find_run_files_indexed(outputs_dir, n, instance):
        v = load_field(path, field)
        if v is not None:
            out[idx] = v
    return out


def compute_pipeline_stats(outputs_dir: Path, bks_value, n, instance):
    """For one instance + one pipeline: mean final RPD (%) over the runs
    found (run -> instance averaging, same as everything else)."""
    run_files = find_run_files(outputs_dir, n, instance)
    if not run_files:
        return {"runs": 0, "avg_rpd_pct": None}

    results = [load_working_time(f) for f in run_files]
    avg_result = statistics.mean(results)
    avg_rpd_pct = ((avg_result - bks_value) / bks_value * 100.0
                   if bks_value else None)
    return {"runs": len(results), "avg_rpd_pct": avg_rpd_pct}


def compute_row(sats_dir, ims_dir, coop_dir, bks_dir, n, instance):
    bks_path = bks_dir / n / f"{instance}-bks.json"
    bks_value = load_working_time(bks_path) if bks_path.is_file() else None

    sats = compute_pipeline_stats(sats_dir, bks_value, n, instance)
    ims = compute_pipeline_stats(ims_dir, bks_value, n, instance)
    coop = compute_pipeline_stats(coop_dir, bks_value, n, instance)

    delta_coop = None
    coop_result = None
    if ims["avg_rpd_pct"] is not None and coop["avg_rpd_pct"] is not None:
        delta_coop = ims["avg_rpd_pct"] - coop["avg_rpd_pct"]
        if delta_coop > RPD_TIE_EPSILON:
            coop_result = "win"
        elif delta_coop < -RPD_TIE_EPSILON:
            coop_result = "loss"
        else:
            coop_result = "tie"

    # evals ratio: divide per run (coop run k / ims run k, paired by run
    # index), then mean those ratios over the instance's paired runs.
    coop_by_run = load_evals_by_run(coop_dir, n, instance, COOP_EVALS_FIELD)
    ims_by_run = load_evals_by_run(ims_dir, n, instance, IMS_EVALS_FIELD)
    paired = sorted(k for k in coop_by_run if k in ims_by_run and ims_by_run[k])
    per_run_ratios = [coop_by_run[k] / ims_by_run[k] for k in paired]
    evals_ratio = statistics.mean(per_run_ratios) if per_run_ratios else None
    coop_evals = statistics.mean(coop_by_run[k] for k in paired) if paired else None
    ims_evals = statistics.mean(ims_by_run[k] for k in paired) if paired else None

    return {
        "n": n, "instance": instance, "bks": bks_value,
        "sats_runs": sats["runs"], "sats_rpd_pct": sats["avg_rpd_pct"],
        "ims_runs": ims["runs"], "ims_rpd_pct": ims["avg_rpd_pct"],
        "coop_runs": coop["runs"], "coop_rpd_pct": coop["avg_rpd_pct"],
        "delta_coop_pct": delta_coop, "coop_result": coop_result,
        "evals_paired_runs": len(paired),
        "coop_evals": coop_evals, "ims_evals": ims_evals,
        "evals_ratio": evals_ratio,
    }


def fmt(v, prec=4):
    return f"{v:.{prec}f}" if v is not None else "-"


def fmt_fraction(count, total):
    return f"{count}/{total}"


def mean_or_none(vals):
    vals = [v for v in vals if v is not None]
    return statistics.mean(vals) if vals else None


def summarize(rows):
    """One summary dict for a set of per-instance rows (used per-n and
    overall): unweighted mean over instances of each per-instance value --
    same run -> instance -> bucket order as every other column here."""
    results = [r["coop_result"] for r in rows if r["coop_result"] is not None]
    return {
        "avg_rpd_sats_pct": mean_or_none(r["sats_rpd_pct"] for r in rows),
        "avg_rpd_ims_pct": mean_or_none(r["ims_rpd_pct"] for r in rows),
        "avg_rpd_coop_pct": mean_or_none(r["coop_rpd_pct"] for r in rows),
        "avg_delta_coop_pct": mean_or_none(r["delta_coop_pct"] for r in rows),
        "coop_wins": results.count("win"),
        "coop_ties": results.count("tie"),
        "coop_losses": results.count("loss"),
        "n_compared": len(results),
        "evals_ratio": mean_or_none(r["evals_ratio"] for r in rows),
    }


def write_csv(path: Path, rows):
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
        writer.writeheader()
        writer.writerows(rows)


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--seq-sats-outputs", default="../../../sequence/cpp/outputs/exp1-full/sats",
                    help="sequence/cpp run_sats_full.sh output dir, relative to this "
                         "script (default: ../../../sequence/cpp/outputs/exp1-full/sats)")
    ap.add_argument("--seq-ims-outputs", default="../../../sequence/cpp/outputs/exp1-full/ims",
                    help="sequence/cpp run_ims_full.sh output dir, relative to this "
                         "script (default: ../../../sequence/cpp/outputs/exp1-full/ims)")
    ap.add_argument("--coop-outputs", default="../../outputs/exp1-full/coop",
                    help="master-slave run_full.sh output dir, relative to this "
                         "script (default: ../../outputs/exp1-full/coop)")
    ap.add_argument("--bks", default="../../../bks",
                    help="BKS json dir, relative to this script (default: ../../../bks)")
    ap.add_argument("--customers", default="6,10,12,20,50,100,200,500,1000",
                    help="Comma-separated customer counts")
    ap.add_argument("--runs", type=int, default=10,
                    help="Expected runs per instance, for the 'only X/N found' note")
    ap.add_argument("--outputs", default="../../outputs/exp1-full",
                    help="Root outputs dir -- where summary.csv / instances.csv are written")
    ap.add_argument("--no-save", action="store_true",
                    help="Only print to stdout, don't write CSV files")
    args = ap.parse_args()

    script_dir = Path(__file__).resolve().parent
    sats_dir = (script_dir / args.seq_sats_outputs).resolve()
    ims_dir = (script_dir / args.seq_ims_outputs).resolve()
    coop_dir = (script_dir / args.coop_outputs).resolve()
    bks_dir = (script_dir / args.bks).resolve()
    outputs_root = (script_dir / args.outputs).resolve()

    customers = [c.strip() for c in args.customers.split(",") if c.strip()]

    rows = []
    for n in customers:
        instances = sorted(discover_instances(sats_dir, n)
                           | discover_instances(ims_dir, n)
                           | discover_instances(coop_dir, n))
        for instance in instances:
            rows.append(compute_row(sats_dir, ims_dir, coop_dir, bks_dir, n, instance))

    print(f"sats outputs: {sats_dir}\nims  outputs: {ims_dir}\ncoop outputs: {coop_dir}\n")

    header = (f"{'Instance':<12}{'BKS(s)':>12}"
              f"{'RPD sats(%)':>14}{'RPD ims(%)':>13}{'RPD coop(%)':>14}"
              f"{'delta_coop(%)':>15}{'result':>8}{'evals coop/ims':>16}")
    print(header)
    print("-" * len(header))
    for r in rows:
        result = "-" if r["coop_result"] is None else r["coop_result"]
        print(f"{r['instance']:<12}{fmt(r['bks']):>12}"
              f"{fmt(r['sats_rpd_pct'], 3):>14}{fmt(r['ims_rpd_pct'], 3):>13}"
              f"{fmt(r['coop_rpd_pct'], 3):>14}{fmt(r['delta_coop_pct'], 3):>15}"
              f"{result:>8}{fmt(r['evals_ratio'], 3):>16}")
        for label, key in (("sats", "sats_runs"), ("ims", "ims_runs"), ("coop", "coop_runs")):
            if r[key] and r[key] < args.runs:
                print(f"    note: {label} only {r[key]}/{args.runs} runs found")

    print("\nNote: RPD = (working_time - BKS) / BKS * 100, meaned over runs. "
          "delta_coop = RPD ims - RPD coop; bigger is better -- positive means "
          f"coop had the lower (better) RPD. result: win if delta_coop > "
          f"{RPD_TIE_EPSILON:g}, loss if < {-RPD_TIE_EPSILON:g}, else tie.")

    # ---- Aggregate: per customer count n, then overall + held-out ----
    summary_rows = []
    for n in customers:
        rows_for_n = [r for r in rows if r["n"] == n]
        if rows_for_n:
            summary_rows.append({"n": n, **summarize(rows_for_n)})
    overall_row = {"n": "overall", **summarize(rows)}
    held_out_row = {"n": "held-out",
                    **summarize([r for r in rows if r["instance"] not in TUNING_INSTANCES])}
    summary_rows.append(overall_row)
    summary_rows.append(held_out_row)

    sh = (f"{'n':<9}{'avg_rpd_sats(%)':>17}{'avg_rpd_ims(%)':>16}{'avg_rpd_coop(%)':>17}"
          f"{'avg_delta_coop(%)':>19}{'wins':>7}{'ties':>7}{'losses':>8}"
          f"{'evals coop/ims':>16}")
    print(f"\n{sh}")
    print("-" * len(sh))
    for s in summary_rows:
        if s is overall_row:
            print("-" * len(sh))
        total = s["n_compared"]
        print(f"{str(s['n']):<9}{fmt(s['avg_rpd_sats_pct'], 2):>17}{fmt(s['avg_rpd_ims_pct'], 2):>16}"
              f"{fmt(s['avg_rpd_coop_pct'], 2):>17}{fmt(s['avg_delta_coop_pct'], 2):>19}"
              f"{fmt_fraction(s['coop_wins'], total):>7}{fmt_fraction(s['coop_ties'], total):>7}"
              f"{fmt_fraction(s['coop_losses'], total):>8}{fmt(s['evals_ratio'], 3):>16}")

    if args.no_save:
        return

    inst_csv = [
        {"n": r["n"], "instance": r["instance"], "bks": r["bks"],
         "rpd_sats_pct": r["sats_rpd_pct"], "rpd_ims_pct": r["ims_rpd_pct"],
         "rpd_coop_pct": r["coop_rpd_pct"], "delta_coop_pct": r["delta_coop_pct"],
         "result": r["coop_result"],
         "evals_paired_runs": r["evals_paired_runs"],
         "coop_evals": r["coop_evals"], "ims_evals": r["ims_evals"],
         "evals_ratio_coop_over_ims": r["evals_ratio"]}
        for r in rows
    ]
    summary_csv = [
        {"n": s["n"], "avg_rpd_sats_pct": s["avg_rpd_sats_pct"],
         "avg_rpd_ims_pct": s["avg_rpd_ims_pct"], "avg_rpd_coop_pct": s["avg_rpd_coop_pct"],
         "avg_delta_coop_pct": s["avg_delta_coop_pct"],
         "coop_wins": fmt_fraction(s["coop_wins"], s["n_compared"]),
         "coop_ties": fmt_fraction(s["coop_ties"], s["n_compared"]),
         "coop_losses": fmt_fraction(s["coop_losses"], s["n_compared"]),
         "evals_ratio_coop_over_ims": s["evals_ratio"]}
        for s in summary_rows
    ]

    saved = []
    if inst_csv:
        p = outputs_root / "instances.csv"
        write_csv(p, inst_csv)
        saved.append(p)
    p = outputs_root / "summary.csv"
    write_csv(p, summary_csv)
    saved.append(p)

    print("\nSaved:")
    for p in saved:
        print(f"  {p}")


if __name__ == "__main__":
    main()
