#!/usr/bin/env python3
"""
Cross-comparison for exp1.5: master-slave (coop) vs sequence/cpp (ims).

Reads detail_rpd.csv from both sides (produced by each side's own
stats.py -- run that first on both sides after adding new instances,
e.g. the 1000 set), pairs rows by (instance, run), and computes:

    delta_rpd = rpd_ims - rpd_coop

RPD is "lower is better" (closer to / below BKS), so delta_rpd > 0 means
rpd_ims > rpd_coop, i.e. coop's solution was better for that run -> COOP
win. delta_rpd < 0 -> IMS win. delta_rpd == 0 -> Tie. Per instance, each
COOP win contributes 1 to that instance's win_fraction numerator (out of
n_runs matched).

Only (instance, run) pairs present on BOTH sides are compared; anything
missing on one side (e.g. a customer count not run yet) is skipped and
reported as a warning rather than silently ignored.

All numbers in the printed tables and saved CSVs are rounded to 2 decimal
places.

Usage:
    python3 cross_stat.py
    python3 cross_stat.py --coop ../../outputs/exp1.5/detail_rpd.csv \
                           --ims ../../../sequence/cpp/outputs/exp1.5/detail_rpd.csv \
                           --out ../../outputs/exp1.5
"""
import argparse
import csv
from collections import defaultdict
from pathlib import Path


def load(path: Path):
    rows = {}
    with path.open() as f:
        reader = csv.DictReader(f)
        for r in reader:
            if not r["rpd_pct"]:
                continue  # missing BKS or missing run -> nothing to compare
            rows[(r["instance"], r["run"])] = float(r["rpd_pct"])
    return rows


def winner_of(delta_rpd: float) -> str:
    if delta_rpd > 0:
        return "COOP win"
    if delta_rpd < 0:
        return "IMS win"
    return "Tie"


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    script_dir = Path(__file__).resolve().parent
    ap.add_argument("--coop", default="../../outputs/exp1.5/detail_rpd.csv",
                     help="master-slave detail_rpd.csv, relative to this script")
    ap.add_argument("--ims", default="../../../sequence/cpp/outputs/exp1.5/detail_rpd.csv",
                     help="sequence/cpp detail_rpd.csv, relative to this script")
    ap.add_argument("--out", default="../../outputs/exp1.5",
                     help="Directory to write cross_stat_detail.csv / cross_stat_summary.csv into")
    ap.add_argument("--no-save", action="store_true")
    args = ap.parse_args()

    coop_path = (script_dir / args.coop).resolve()
    ims_path = (script_dir / args.ims).resolve()
    out_dir = (script_dir / args.out).resolve()

    coop = load(coop_path)
    ims = load(ims_path)

    keys = sorted(set(coop) & set(ims), key=lambda k: (k[0], int(k[1])))
    missing_in_coop = sorted(set(ims) - set(coop))
    missing_in_ims = sorted(set(coop) - set(ims))

    detail_rows = []
    win_count = defaultdict(int)
    counts = defaultdict(int)
    sum_coop = defaultdict(float)
    sum_ims = defaultdict(float)

    for instance, run in keys:
        rpd_coop = coop[(instance, run)]
        rpd_ims = ims[(instance, run)]
        winner = winner_of(rpd_ims - rpd_coop)
        counts[instance] += 1
        sum_coop[instance] += rpd_coop
        sum_ims[instance] += rpd_ims
        if winner == "COOP win":
            win_count[instance] += 1
        detail_rows.append({
            "instance": instance, "run": run,
            "rpd_ims": rpd_ims, "rpd_coop": rpd_coop,
            "winner": winner,
        })

    print(f"coop: {coop_path}\nims:  {ims_path}\n")

    header = f"{'Instance':<12}{'Run':>5}{'rpd_ims':>22}{'rpd_coop':>22}{'winner':>11}"
    print(header)
    print("-" * len(header))
    for row in detail_rows:
        print(f"{row['instance']:<12}{row['run']:>5}{str(row['rpd_ims']):>22}"
              f"{str(row['rpd_coop']):>22}{row['winner']:>11}")

    print(f"\n{'Instance':<12}{'avg_rpd_ims':>12}{'avg_rpd_coop':>13}{'delta_rpd':>10}{'win_fraction':>13}")
    print("-" * 60)
    summary_rows = []
    for instance in sorted(counts):
        w = win_count[instance]
        n = counts[instance]
        avg_coop = sum_coop[instance] / n
        avg_ims = sum_ims[instance] / n
        avg_delta = avg_ims - avg_coop
        frac = f"{w}/{n}"
        print(f"{instance:<12}{avg_ims:>12.2f}{avg_coop:>13.2f}{avg_delta:>10.2f}{frac:>13}")
        summary_rows.append({
            "instance": instance,
            "avg_rpd_ims": f"{avg_ims:.2f}",
            "avg_rpd_coop": f"{avg_coop:.2f}",
            "delta_rpd": f"{avg_delta:.2f}",
            "win_fraction": frac,
        })
    if counts:
        total_wins = sum(win_count.values())
        total_runs = sum(counts.values())
        overall_avg_coop = sum(sum_coop.values()) / total_runs
        overall_avg_ims = sum(sum_ims.values()) / total_runs
        overall_avg_delta = overall_avg_ims - overall_avg_coop
        overall_frac = f"{total_wins}/{total_runs}"
        print("-" * 60)
        print(f"{'overall':<12}{overall_avg_ims:>12.2f}{overall_avg_coop:>13.2f}{overall_avg_delta:>10.2f}"
              f"{overall_frac:>13}")
        summary_rows.append({
            "instance": "overall",
            "avg_rpd_ims": f"{overall_avg_ims:.2f}",
            "avg_rpd_coop": f"{overall_avg_coop:.2f}",
            "delta_rpd": f"{overall_avg_delta:.2f}",
            "win_fraction": overall_frac,
        })

    if missing_in_coop:
        print(f"\n[WARN] present in ims but not in coop, skipped: {missing_in_coop}")
    if missing_in_ims:
        print(f"[WARN] present in coop but not in ims, skipped: {missing_in_ims}")

    if args.no_save or not detail_rows:
        return

    out_dir.mkdir(parents=True, exist_ok=True)
    detail_path = out_dir / "cross_stat_detail.csv"
    with detail_path.open("w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=["instance", "run", "rpd_ims", "rpd_coop", "winner"])
        w.writeheader()
        w.writerows(detail_rows)

    summary_path = out_dir / "cross_stat_summary.csv"
    with summary_path.open("w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=[
            "instance", "avg_rpd_ims", "avg_rpd_coop", "delta_rpd", "win_fraction"])
        w.writeheader()
        w.writerows(summary_rows)

    print(f"\nSaved:\n  {detail_path}\n  {summary_path}")


if __name__ == "__main__":
    main()
