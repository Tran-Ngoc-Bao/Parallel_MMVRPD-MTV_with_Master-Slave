#!/usr/bin/env python3
"""
Statistics for run2.sh (exp2/d, adaptive_pull_elite_segments comparison).

Compares 3 adaptive_pull_elite_segments values (2, 4, 8) -- the number of
stagnant adaptive segments before a worker pulls an elite -- all run with
elite-pull-strategy=rank, elite-pull-accept-strategy=selective,
elite_pool_factor 0.03, and td-crowding pool replacement. Prints 2
tables:

  1. final_rpd (%), irpd (%), and pull request rate (pull_request_count
     per 1,000,000 total_evaluations), one row per segments value.
     final_rpd and the request rate use the same stratified average:
     computed per run first (each involves a division, so the ratio is
     taken per run, not from runs summed/averaged together) -> mean over
     that instance's runs -> mean over instances (6 by default: the 2
     customer counts x 2 combos grid, plus the 2 extra instances in
     EXTRA_INSTANCES: 200.20.3, 500.30.4). irpd is computed the same way
     as exp2/a: trapezoidal-rule average of R0..R8 (RPD (%) of
     best_solution_cost_by_evaluation_checkpoint at 0/8..8/8 of the
     evaluation budget vs BKS working_time), (1/8) * [(R0+R8)/2 +
     R1+...+R7], with R0..R8 themselves averaged per run first, then over
     instances, same as final_rpd.
  2. pull_request_count per worker, one row per worker, one column per
     segments value. Each cell is a plain count (no division), averaged the
     same stratified way: per-worker count per run -> mean over runs for
     that instance -> mean over instances.

RPD (%) = (value - BKS) / BKS * 100, same as exp1.5/stats.py. final_rpd
uses the final best cost (solution.working_time); irpd uses the tracked
best_solution_cost_by_evaluation_checkpoint values (the true best solution
found so far at each checkpoint, not the elite pool's own best -- see
exp2/a for that one -- the two can diverge under
elite-replace-strategy=quality-only/random-target), which can carry an
infeasibility penalty at early checkpoints if the best-known solution
isn't yet feasible at that point.

N_offer / N_accept are intentionally not tracked here -- see exp2/c for
those (pull-strategy x accept-strategy comparison already reports them).

Looks for run files at
<outputs>/<n>/<n>.<combo>-seg<segments>-<run_id>.json (the naming used by
run2.sh) and BKS files at <bks>/<n>/<n>.<combo>-bks.json.

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

SEGMENTS_LIST = ["2", "4", "8"]
NUM_CHECKPOINTS = 9  # R0..R8, at k/8 of the evaluation budget

# Extra (n, combo) instances run in addition to the customers x combos grid
# below -- run2.sh runs these two on top of the regular grid for their
# matching customer count (see the COMBOS case in run2.sh).
EXTRA_INSTANCES = [("200", "20.3"), ("500", "30.4")]


def load_run(path: Path):
    with path.open() as f:
        return json.load(f)


def load_bks_working_time(path: Path):
    with path.open() as f:
        return json.load(f)["solution"]["working_time"]


def find_run_files(outputs_dir: Path, n: str, instance: str, segments: str):
    """Returns [(run_id, path), ...] sorted by run_id."""
    inst_dir = outputs_dir / n
    if not inst_dir.is_dir():
        return []
    pattern = re.compile(rf"^{re.escape(instance)}-seg{re.escape(segments)}-(\d+)\.json$")
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


def mean_or_none(vals):
    vals = [v for v in vals if v is not None]
    return statistics.mean(vals) if vals else None


def irpd_pct(r):
    """Trapezoidal-rule average of r[0..8] over the 8 intervals."""
    if any(v is None for v in r):
        return None
    return ((r[0] + r[8]) / 2.0 + sum(r[1:8])) / 8.0


def compute_instance(outputs_dir: Path, bks_dir: Path, n: str, combo: str, expected_runs: int):
    instance = f"{n}.{combo}"
    bks_path = bks_dir / n / f"{instance}-bks.json"
    bks_value = load_bks_working_time(bks_path) if bks_path.is_file() else None

    summary_rows = []
    detail_rows = []
    worker_rows = []
    for segments in SEGMENTS_LIST:
        run_files = find_run_files(outputs_dir, n, instance, segments)

        rpds, req_rates, r_per_run, irpds_per_run = [], [], [], []
        worker_requests = {}  # worker -> [request_count per run]
        for run_id, path in run_files:
            data = load_run(path)
            final_cost = data["solution"]["working_time"]
            req = data["pull_request_count"]
            req_rate = data.get("pull_request_rate_per_million_evals")
            cps = data.get("best_solution_cost_by_evaluation_checkpoint")

            run_rpd = rpd_pct(final_cost, bks_value)

            if run_rpd is not None:
                rpds.append(run_rpd)
            if req_rate is not None:
                req_rates.append(req_rate)
            if cps is not None and len(cps) == NUM_CHECKPOINTS:
                run_r = [rpd_pct(v, bks_value) for v in cps]
                r_per_run.append(run_r)
                irpds_per_run.append(irpd_pct(run_r))

            for w in data.get("worker_pull_stats", []):
                worker_requests.setdefault(w["worker"], []).append(w["request_count"])

            detail_rows.append({
                "n": n, "instance": instance, "segments": segments, "run": run_id,
                "final_cost": final_cost, "final_rpd_pct": run_rpd,
                "pull_request_count": req,
                "pull_request_rate_per_million_evals": req_rate,
            })

        # irpd is computed per run first (from that run's own R0..R8), then
        # averaged over runs -- same approach as exp2/a.
        summary_rows.append({
            "n": n, "instance": instance, "segments": segments,
            "bks": bks_value, "runs": len(run_files), "expected_runs": expected_runs,
            "final_rpd_pct": mean_or_none(rpds),
            "irpd_pct": mean_or_none(irpds_per_run),
            "pull_request_rate_per_million_evals": mean_or_none(req_rates),
        })

        for worker, vals in sorted(worker_requests.items()):
            worker_rows.append({
                "n": n, "instance": instance, "segments": segments, "worker": worker,
                "avg_pull_request_count": mean_or_none(vals),
            })
    return summary_rows, detail_rows, worker_rows


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
    ap.add_argument("--outputs", default="../../../outputs/exp2/d",
                     help="Dir containing run2.sh output, relative to this script "
                          "(default: ../../../outputs/exp2/d)")
    ap.add_argument("--bks", default="../../../../bks",
                     help="Dir containing BKS json files, relative to this script "
                          "(default: ../../../../bks)")
    ap.add_argument("--customers", default="200,500",
                     help="Comma-separated customer counts (default: 200,500)")
    ap.add_argument("--combos", default="10.2,40.1",
                     help="Comma-separated instance combos, matches run2.sh "
                          "(default: 10.2,40.1)")
    ap.add_argument("--runs", type=int, default=3,
                     help="Expected number of runs per (instance, segments), used to "
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

    instance_pairs = [(n, combo) for n in customers for combo in combos]
    for extra in EXTRA_INSTANCES:
        if extra not in instance_pairs:
            instance_pairs.append(extra)

    summary_rows = []
    detail_rows = []
    worker_rows = []
    for n, combo in instance_pairs:
        s, d, w = compute_instance(outputs_dir, bks_dir, n, combo, args.runs)
        summary_rows.extend(s)
        detail_rows.extend(d)
        worker_rows.extend(w)

    incomplete = [f"{r['instance']}/seg{r['segments']} ({r['runs']}/{r['expected_runs']})"
                  for r in summary_rows if r["runs"] < r["expected_runs"]]

    def avg_field(rows, f):
        vals = [r[f] for r in rows if r[f] is not None]
        return statistics.mean(vals) if vals else None

    # ---- Table 1: final_rpd (%), irpd (%), and pull request rate per segments value ----
    # Stratified average: per run -> mean over runs (per instance, done in
    # compute_instance) -> mean over instances (here).
    out(f"{'Segments':<10}{'final_RPD(%)':>14}{'irpd(%)':>10}{'Request/1M evals':>18}")
    out("-" * 52)
    segments_rows = []
    for segments in SEGMENTS_LIST:
        rows = [r for r in summary_rows if r["segments"] == segments]
        segments_row = {
            "segments": segments,
            "final_rpd_pct": avg_field(rows, "final_rpd_pct"),
            "irpd_pct": avg_field(rows, "irpd_pct"),
            "pull_request_rate_per_million_evals": avg_field(rows, "pull_request_rate_per_million_evals"),
        }
        segments_rows.append(segments_row)
        out(f"{segments:<10}{fmt(segments_row['final_rpd_pct'], 3):>14}"
            f"{fmt(segments_row['irpd_pct'], 3):>10}"
            f"{fmt(segments_row['pull_request_rate_per_million_evals'], 3):>18}")
    if incomplete:
        out(f"\nNote: fewer runs found than expected for: {', '.join(incomplete)}")

    # ---- Table 2: pull_request_count per worker, columns = segments value ----
    # Stratified average: per-worker count per run -> mean over runs for
    # that instance (done in compute_instance) -> mean over instances.
    workers = sorted({r["worker"] for r in worker_rows})
    worker_summary_rows = []
    if workers:
        out(f"\n{'Worker':<10}" + "".join(f"{'seg=' + s:>14}" for s in SEGMENTS_LIST))
        out("-" * (10 + 14 * len(SEGMENTS_LIST)))
        for worker in workers:
            worker_summary_row = {"worker": worker}
            for segments in SEGMENTS_LIST:
                rows = [r for r in worker_rows if r["worker"] == worker and r["segments"] == segments]
                worker_summary_row[f"seg_{segments}_avg_pull_request_count"] = avg_field(
                    rows, "avg_pull_request_count")
            worker_summary_rows.append(worker_summary_row)
            out(f"{worker:<10}" + "".join(
                f"{fmt(worker_summary_row[f'seg_{s}_avg_pull_request_count'], 3):>14}"
                for s in SEGMENTS_LIST))

        # Total across all workers (whole program), per segments column --
        # sum of the per-worker stratified averages above, not a separate
        # re-average, since each worker's average already went through the
        # same per-run -> per-instance -> per-6-instances stratification.
        out("-" * (10 + 14 * len(SEGMENTS_LIST)))
        total_row = {"worker": "Total"}
        for segments in SEGMENTS_LIST:
            key = f"seg_{segments}_avg_pull_request_count"
            vals = [r[key] for r in worker_summary_rows if r[key] is not None]
            total_row[key] = sum(vals) if vals else None
        worker_summary_rows.append(total_row)
        out(f"{'Total':<10}" + "".join(
            f"{fmt(total_row[f'seg_{s}_avg_pull_request_count'], 3):>14}"
            for s in SEGMENTS_LIST))

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
        detail_path = outputs_dir / "detail_pull.csv"
        write_csv(detail_path, detail_rows)
        saved.append(detail_path)
    if segments_rows:
        segments_path = outputs_dir / "segments_summary.csv"
        write_csv(segments_path, segments_rows)
        saved.append(segments_path)
    if worker_summary_rows:
        worker_summary_path = outputs_dir / "worker_pull_summary.csv"
        write_csv(worker_summary_path, worker_summary_rows)
        saved.append(worker_summary_path)

    print("\nSaved:")
    for p in saved:
        print(f"  {p}")


if __name__ == "__main__":
    main()
