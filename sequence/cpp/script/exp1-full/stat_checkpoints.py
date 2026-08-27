#!/usr/bin/env python3
"""
Emit a long/tidy CSV from exp1-full output for plotting convergence
curves (best cost at each time checkpoint) of the sats and ims pipelines.

BestCost per checkpoint is "best_solution_cost_by_time_checkpoint_all_workers"
(per checkpoint, the best cost any worker had reached by then -- written by
run_ims_full.sh), falling back to "best_solution_cost_by_time_checkpoint"
(single-worker) when the all-workers array is absent (e.g. sats runs).

Source:
    <outputs-dir>/<method>/<customers>/<customers>.<combo>-<run>.json
Each file must carry one of those arrays (a 9-element array, index 0..8 --
written when run with --time-limit). Files with neither are skipped.

BKS is read from <bks>/<customers>/<customers>.<combo>-bks.json
(solution.working_time). Missing BKS -> the BKS column is left blank.

By default 4 instances are excluded: 200.10.2, 200.40.1, 500.10.2,
500.40.1 (see EXCLUDED_INSTANCES / --exclude).

One CSV row per (instance, run, method, checkpoint):
    instance     e.g. 1000.30.2
    customers    e.g. 1000
    run          run index (from the file name)
    method       sats or ims
    checkpoint   0..8
    q            checkpoint as a fraction: 0 -> "0/8", 1 -> "1/8", ...
    BestCost     the checkpoint value (not rounded)
    BKS          BKS working_time (not rounded)

Usage:
    python3 stat_checkpoints.py
    python3 stat_checkpoints.py --methods ims sats --customers 500,1000
    python3 stat_checkpoints.py -o /path/to/checkpoint_curves.csv
"""

import argparse
import csv
import json
import re
import sys
from pathlib import Path

# ten file ket qua: <customers>.<combo>-<run>.json  (vd: 1000.30.2-7.json)
FILENAME_RE = re.compile(r"^(\d+)\.(.+)-(\d+)\.json$")

# preferred field, then fallback
CHECKPOINT_FIELDS = ("best_solution_cost_by_time_checkpoint_all_workers",
                     "best_solution_cost_by_time_checkpoint")

# Cac instance bi loai khoi thong ke (dung "<customers>.<combo>").
EXCLUDED_INSTANCES = {"200.10.2", "200.40.1", "500.10.2", "500.40.1"}


def load_bks(bks_dir: Path, n: str, instance: str):
    path = bks_dir / n / f"{instance}-bks.json"
    if not path.is_file():
        return None
    with path.open() as fh:
        data = json.load(fh)
    return data["solution"]["working_time"]


def discover_customers(method_dir: Path):
    return sorted(
        (p.name for p in method_dir.iterdir() if p.is_dir() and p.name.isdigit()),
        key=int,
    )


def collect_rows(outputs_dir: Path, bks_dir: Path, methods, wanted_customers,
                 excluded_instances):
    rows = []
    bks_cache = {}
    skipped_no_field = 0
    skipped_bad_name = 0
    skipped_excluded = 0
    missing_bks = set()

    for method in methods:
        method_dir = outputs_dir / method
        if not method_dir.is_dir():
            print(f"warning: khong thay thu muc {method_dir}", file=sys.stderr)
            continue

        for n in discover_customers(method_dir):
            if wanted_customers and n not in wanted_customers:
                continue
            cust_dir = method_dir / n

            for fpath in sorted(cust_dir.glob("*.json")):
                m = FILENAME_RE.match(fpath.name)
                if not m:
                    skipped_bad_name += 1
                    continue
                file_n, combo, run = m.groups()
                if file_n != n:
                    continue
                instance = f"{n}.{combo}"
                if instance in excluded_instances:
                    skipped_excluded += 1
                    continue

                with fpath.open() as fh:
                    data = json.load(fh)

                series = next((data[f] for f in CHECKPOINT_FIELDS if data.get(f)), [])
                if not series:
                    skipped_no_field += 1
                    continue

                key = (n, instance)
                if key not in bks_cache:
                    bks_cache[key] = load_bks(bks_dir, n, instance)
                    if bks_cache[key] is None:
                        missing_bks.add(instance)
                bks_value = bks_cache[key]

                denom = len(series) - 1 if len(series) > 1 else 1
                for checkpoint, best_cost in enumerate(series):
                    rows.append({
                        "instance": instance,
                        "customers": int(n),
                        "run": int(run),
                        "method": method,
                        "checkpoint": checkpoint,
                        "q": f"{checkpoint}/{denom}",
                        "BestCost": best_cost,
                        "BKS": "" if bks_value is None else bks_value,
                    })

    rows.sort(key=lambda r: (r["customers"], r["instance"], r["method"],
                             r["run"], r["checkpoint"]))
    return rows, {
        "skipped_no_field": skipped_no_field,
        "skipped_bad_name": skipped_bad_name,
        "skipped_excluded": skipped_excluded,
        "missing_bks": sorted(missing_bks),
    }


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    script_dir = Path(__file__).resolve().parent
    ap.add_argument("--outputs-dir",
                    default=str(script_dir / ".." / ".." / "outputs" / "exp1-full"),
                    help="Thu muc chua <method>/<customers>/*.json "
                         "(mac dinh: ../../outputs/exp1-full)")
    ap.add_argument("--bks", default=str(script_dir / ".." / ".." / ".." / ".." / "bks"),
                    help="Thu muc chua file BKS (mac dinh: ../../../../bks)")
    ap.add_argument("--methods", nargs="+", default=["ims", "sats"],
                    help="Cac method can gom (mac dinh: ims sats)")
    ap.add_argument("--customers", default=None,
                    help="Loc theo bo customer, ngan cach bang dau phay "
                         "(vd: 500,1000). Mac dinh: tat ca.")
    ap.add_argument("-o", "--out", default=None,
                    help="File CSV dau ra (mac dinh: "
                         "<outputs-dir>/checkpoint_curves.csv)")
    ap.add_argument("--exclude", default=",".join(sorted(EXCLUDED_INSTANCES)),
                    help="Cac instance bo qua, ngan cach bang dau phay "
                         f"(mac dinh: {','.join(sorted(EXCLUDED_INSTANCES))}). "
                         "Dat rong de khong bo instance nao.")
    args = ap.parse_args()

    outputs_dir = Path(args.outputs_dir).resolve()
    bks_dir = Path(args.bks).resolve()
    wanted_customers = ({c.strip() for c in args.customers.split(",") if c.strip()}
                        if args.customers else None)
    excluded_instances = {s.strip() for s in args.exclude.split(",") if s.strip()}
    out_path = Path(args.out).resolve() if args.out else outputs_dir / "checkpoint_curves.csv"

    rows, stats = collect_rows(outputs_dir, bks_dir, args.methods, wanted_customers,
                               excluded_instances)

    fieldnames = ["instance", "customers", "run", "method",
                  "checkpoint", "q", "BestCost", "BKS"]
    out_path.parent.mkdir(parents=True, exist_ok=True)
    with out_path.open("w", newline="") as fh:
        writer = csv.DictWriter(fh, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)

    n_runs = len({(r["method"], r["customers"], r["instance"], r["run"]) for r in rows})
    print(f"Da ghi {len(rows)} dong ({n_runs} run) vao {out_path}")
    by_method = {}
    for r in rows:
        by_method[r["method"]] = by_method.get(r["method"], 0) + 1
    for method, cnt in sorted(by_method.items()):
        print(f"  {method}: {cnt} dong")
    if stats["skipped_no_field"]:
        print(f"  bo qua {stats['skipped_no_field']} file khong co chuoi checkpoint nao "
              f"(chay khong co --time-limit?)")
    if stats["skipped_bad_name"]:
        print(f"  bo qua {stats['skipped_bad_name']} file sai dinh dang ten")
    if stats["skipped_excluded"]:
        print(f"  bo qua {stats['skipped_excluded']} file thuoc instance bi loai "
              f"({', '.join(sorted(excluded_instances))})")
    if stats["missing_bks"]:
        print(f"  thieu BKS cho: {', '.join(stats['missing_bks'])}")


if __name__ == "__main__":
    main()
