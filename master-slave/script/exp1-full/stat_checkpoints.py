#!/usr/bin/env python3
"""
Emit a single long/tidy CSV combining exp1-full outputs from the
sequence/cpp pipelines (ims, sats) AND the master-slave pipeline (coop),
for plotting convergence curves (best cost at each time checkpoint) of
all three side by side.

BestCost per checkpoint prefers "best_solution_cost_by_time_checkpoint_all_workers"
(written by sequence/cpp's run_ims_full.sh), falling back to
"best_solution_cost_by_time_checkpoint" (single search / master-slave's
own field). Files with neither are skipped.

Source directories (all scanned by default):
    <sequence-outputs-dir>/ims/<customers>/<customers>.<combo>-<run>.json
    <sequence-outputs-dir>/sats/<customers>/<customers>.<combo>-<run>.json
    <master-slave-outputs-dir>/coop/<customers>/<customers>.<combo>-<run>.json

BKS is read from <bks>/<customers>/<customers>.<combo>-bks.json
(solution.working_time), shared between both pipelines. Missing BKS -> the
BKS column is left blank.

By default 4 instances are excluded: 200.10.2, 200.40.1, 500.10.2,
500.40.1 (see EXCLUDED_INSTANCES / --exclude).

One CSV row per (instance, run, method, checkpoint):
    instance     e.g. 1000.30.2
    customers    e.g. 1000
    run          run index (from the file name)
    method       ims, sats or coop
    checkpoint   0..8
    q            checkpoint as a fraction: 0 -> "0/8", 1 -> "1/8", ...
    BestCost     the checkpoint value (not rounded)
    BKS          BKS working_time (not rounded)

Usage:
    python3 stat_checkpoints.py
    python3 stat_checkpoints.py --methods coop ims
    python3 stat_checkpoints.py --customers 500,1000
    python3 stat_checkpoints.py -o /path/to/checkpoint_curves.csv
"""

import argparse
import csv
import json
import re
import sys
from pathlib import Path

# result file name: <customers>.<combo>-<run>.json  (e.g. 1000.30.2-7.json)
FILENAME_RE = re.compile(r"^(\d+)\.(.+)-(\d+)\.json$")

# preferred field, then fallback
CHECKPOINT_FIELDS = ("best_solution_cost_by_time_checkpoint_all_workers",
                     "best_solution_cost_by_time_checkpoint")

# Instances excluded from the stats (keyed by "<customers>.<combo>").
EXCLUDED_INSTANCES = {"200.10.2", "200.40.1", "500.10.2", "500.40.1"}

SCRIPT_DIR = Path(__file__).resolve().parent
DEFAULT_SEQUENCE_OUTPUTS = SCRIPT_DIR / ".." / ".." / ".." / "sequence" / "cpp" / "outputs" / "exp1-full"
DEFAULT_MASTER_SLAVE_OUTPUTS = SCRIPT_DIR / ".." / ".." / "outputs" / "exp1-full"

# method -> which outputs-dir root its <method>/<customers>/*.json lives under
ALL_METHODS = ("ims", "sats", "coop")


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


def collect_rows(sources, bks_dir: Path, wanted_customers, excluded_instances):
    rows = []
    bks_cache = {}
    skipped_no_field = 0
    skipped_bad_name = 0
    skipped_excluded = 0
    missing_bks = set()

    for method, method_dir in sources:
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
    ap.add_argument("--sequence-outputs-dir",
                    default=str(DEFAULT_SEQUENCE_OUTPUTS),
                    help="Thu muc outputs/exp1-full cua sequence/cpp, chua ims/ va sats/ "
                         f"(mac dinh: {DEFAULT_SEQUENCE_OUTPUTS})")
    ap.add_argument("--master-slave-outputs-dir",
                    default=str(DEFAULT_MASTER_SLAVE_OUTPUTS),
                    help="Thu muc outputs/exp1-full cua master-slave, chua coop/ "
                         f"(mac dinh: {DEFAULT_MASTER_SLAVE_OUTPUTS})")
    ap.add_argument("--bks", default=str(SCRIPT_DIR / ".." / ".." / ".." / "bks"),
                    help="Thu muc chua file BKS, dung chung cho ca hai pipeline "
                         "(mac dinh: ../../../bks)")
    ap.add_argument("--methods", nargs="+", default=None, choices=ALL_METHODS,
                    help=f"Cac method can gom (mac dinh: ca {', '.join(ALL_METHODS)})")
    ap.add_argument("--customers", default=None,
                    help="Loc theo bo customer, ngan cach bang dau phay "
                         "(vd: 500,1000). Mac dinh: tat ca.")
    ap.add_argument("-o", "--out", default=None,
                    help="File CSV dau ra (mac dinh: "
                         "<master-slave-outputs-dir>/checkpoint_curves.csv)")
    ap.add_argument("--exclude", default=",".join(sorted(EXCLUDED_INSTANCES)),
                    help="Cac instance bo qua, ngan cach bang dau phay "
                         f"(mac dinh: {','.join(sorted(EXCLUDED_INSTANCES))}). "
                         "Dat rong de khong bo instance nao.")
    args = ap.parse_args()

    sequence_outputs = Path(args.sequence_outputs_dir).resolve()
    master_slave_outputs = Path(args.master_slave_outputs_dir).resolve()
    roots = {"ims": sequence_outputs, "sats": sequence_outputs, "coop": master_slave_outputs}

    methods = args.methods if args.methods else list(ALL_METHODS)
    sources = [(m, roots[m] / m) for m in methods]

    bks_dir = Path(args.bks).resolve()
    wanted_customers = ({c.strip() for c in args.customers.split(",") if c.strip()}
                        if args.customers else None)
    excluded_instances = {s.strip() for s in args.exclude.split(",") if s.strip()}
    out_path = (Path(args.out).resolve() if args.out
               else master_slave_outputs / "checkpoint_curves.csv")

    rows, stats = collect_rows(sources, bks_dir, wanted_customers, excluded_instances)

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
