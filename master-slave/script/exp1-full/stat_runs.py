#!/usr/bin/env python3
"""
Emit a CSV table of exp1-full at the per-run level: one row per
(instance, run), pairing the ims (sequence/cpp) and coop (master-slave)
results by run index.

Columns:
    instance    e.g. 1000.30.2
    n           customer count (e.g. 1000)
    set         "tuning" if the instance is in {200.10.2, 200.40.1,
                500.10.2, 500.40.1}, otherwise "held-out"
    Tn          the instance's time limit (config.time_limit, taken from
                coop first, else from ims)
    run         run index (from the <instance>-<run>.json file name)
    seed        the coop run's seed (worker_seeds[0].search_seed - 1); if
                there is no coop file, derived from run_full.sh: run*100
    bks         BKS working_time (not rounded)
    ims-final   ims run's final working_time (not rounded)
    coop-final  coop run's final working_time (not rounded)
    ims-evals   ims run's total_evaluations_all_workers (sum over every
                worker; not rounded)
    coop-evals  coop run's total_evaluations (already the island-wide sum)

Source:
    coop : <coop-outputs>/<n>/<n>.<combo>-<run>.json
    ims  : <seq-ims-outputs>/<n>/<n>.<combo>-<run>.json
    bks  : <bks>/<n>/<n>.<combo>-bks.json

Usage:
    python3 stat_runs.py
    python3 stat_runs.py --customers 500,1000 -o /path/to/runs.csv
"""

import argparse
import csv
import json
import re
import sys
from pathlib import Path

TUNING_INSTANCES = {"200.10.2", "200.40.1", "500.10.2", "500.40.1"}

COOP_EVALS_FIELD = "total_evaluations"
IMS_EVALS_FIELD = "total_evaluations_all_workers"

COLUMNS = ["instance", "n", "set", "Tn", "run", "seed", "bks",
           "ims-final", "coop-final", "ims-evals", "coop-evals"]

RUNFILE_RE = re.compile(r"^(\d+)\.(.+)-(\d+)\.json$")


def load_json(path: Path):
    with path.open() as fh:
        return json.load(fh)


def working_time(data):
    return data.get("solution", {}).get("working_time")


def coop_seed_of(data):
    ws = data.get("worker_seeds") or []
    if ws and ws[0].get("search_seed") is not None:
        return ws[0]["search_seed"] - 1
    return None


def index_runs(method_dir: Path, n: str):
    """{(instance, run): Path} for one method + one customer count."""
    out = {}
    cust_dir = method_dir / n
    if not cust_dir.is_dir():
        return out
    for fpath in cust_dir.glob("*.json"):
        m = RUNFILE_RE.match(fpath.name)
        if not m:
            continue
        file_n, combo, run = m.groups()
        if file_n != n:
            continue
        out[(f"{n}.{combo}", int(run))] = fpath
    return out


def collect_rows(coop_dir, ims_dir, bks_dir, wanted_customers, tuning):
    # discover customer counts from both method dirs
    ns = set()
    for d in (coop_dir, ims_dir):
        if d.is_dir():
            ns |= {p.name for p in d.iterdir() if p.is_dir() and p.name.isdigit()}
    if wanted_customers:
        ns &= wanted_customers
    ns = sorted(ns, key=int)

    rows = []
    missing_ims_evals = 0
    for n in ns:
        coop_runs = index_runs(coop_dir, n)
        ims_runs = index_runs(ims_dir, n)
        bks_cache = {}

        for key in sorted(set(coop_runs) | set(ims_runs), key=lambda k: (k[0], k[1])):
            instance, run = key
            combo = instance.split(".", 1)[1]

            coop = load_json(coop_runs[key]) if key in coop_runs else None
            ims = load_json(ims_runs[key]) if key in ims_runs else None

            if instance not in bks_cache:
                bks_path = bks_dir / n / f"{instance}-bks.json"
                bks_cache[instance] = working_time(load_json(bks_path)) if bks_path.is_file() else None

            tn = None
            for d in (coop, ims):
                if d and d.get("config", {}).get("time_limit") is not None:
                    tn = d["config"]["time_limit"]
                    break

            seed = coop_seed_of(coop) if coop else None
            if seed is None:
                seed = run * 100  # run_full.sh formula

            ims_evals = ims.get(IMS_EVALS_FIELD) if ims else None
            if ims is not None and ims_evals is None:
                missing_ims_evals += 1

            rows.append({
                "instance": instance,
                "n": int(n),
                "set": "tuning" if instance in tuning else "held-out",
                "Tn": tn,
                "run": run,
                "seed": seed,
                "bks": bks_cache[instance],
                "ims-final": working_time(ims) if ims else None,
                "coop-final": working_time(coop) if coop else None,
                "ims-evals": ims_evals,
                "coop-evals": coop.get(COOP_EVALS_FIELD) if coop else None,
            })
    return rows, missing_ims_evals


def write_csv(rows, out_path: Path):
    out_path.parent.mkdir(parents=True, exist_ok=True)
    with out_path.open("w", newline="") as fh:
        writer = csv.writer(fh)
        writer.writerow(COLUMNS)
        for r in rows:
            writer.writerow(["" if r[c] is None else r[c] for c in COLUMNS])


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    script_dir = Path(__file__).resolve().parent
    ap.add_argument("--coop-outputs",
                    default=str(script_dir / ".." / ".." / "outputs" / "exp1-full" / "coop"),
                    help="master-slave run_full.sh output dir "
                         "(mac dinh: ../../outputs/exp1-full/coop)")
    ap.add_argument("--seq-ims-outputs",
                    default=str(script_dir / ".." / ".." / ".." / "sequence" / "cpp"
                               / "outputs" / "exp1-full" / "ims"),
                    help="sequence/cpp run_ims_full.sh output dir "
                         "(mac dinh: ../../../sequence/cpp/outputs/exp1-full/ims)")
    ap.add_argument("--bks", default=str(script_dir / ".." / ".." / ".." / "bks"),
                    help="Thu muc BKS (mac dinh: ../../../bks)")
    ap.add_argument("--customers", default=None,
                    help="Loc theo bo customer, ngan cach dau phay (vd: 500,1000)")
    ap.add_argument("--tuning", default=",".join(sorted(TUNING_INSTANCES)),
                    help="Cac instance thuoc set 'tuning', ngan cach dau phay "
                         f"(mac dinh: {','.join(sorted(TUNING_INSTANCES))})")
    ap.add_argument("-o", "--out", default=None,
                    help="File .csv dau ra (mac dinh: <coop-outputs>/../runs.csv)")
    args = ap.parse_args()

    coop_dir = Path(args.coop_outputs).resolve()
    ims_dir = Path(args.seq_ims_outputs).resolve()
    bks_dir = Path(args.bks).resolve()
    wanted = ({c.strip() for c in args.customers.split(",") if c.strip()}
              if args.customers else None)
    tuning = {s.strip() for s in args.tuning.split(",") if s.strip()}
    out_path = (Path(args.out).resolve() if args.out
                else coop_dir.parent / "runs.csv")

    rows, missing_ims_evals = collect_rows(coop_dir, ims_dir, bks_dir, wanted, tuning)
    if not rows:
        print("Khong tim thay run nao (kiem tra lai duong dan).", file=sys.stderr)
        sys.exit(1)

    write_csv(rows, out_path)

    n_inst = len({r["instance"] for r in rows})
    print(f"Da ghi {len(rows)} dong ({n_inst} instance) vao {out_path}")
    both = sum(1 for r in rows if r["ims-final"] is not None and r["coop-final"] is not None)
    print(f"  co ca ims va coop: {both} dong")
    if missing_ims_evals:
        print(f"  {missing_ims_evals} run ims thieu '{IMS_EVALS_FIELD}' "
              f"(cot ims-evals de trong)")


if __name__ == "__main__":
    main()
