#!/usr/bin/env python3
"""
Statistics for run2.sh (exp2/e-1, elite_pull_quality_tolerance_pct comparison).

Compares 3 elite_pull_quality_tolerance_pct values (0.5, 1, 2) -- how far
above the requester's personal-best cost (in %) an offered elite may still
be, and still get accepted under Selective if it's more diverse than
average (see pick_for_dispatch in parallel.cpp). All runs use
elite-pull-strategy=rank, elite-pull-accept-strategy=selective (required --
this tolerance has no effect under "always"), significant-best push
strategy, a fixed --elite-pool-size (same as exp2/g's po3: 200->4,
500->5), td-crowding pool replacement, and adaptive_pull_elite_segments
set to the best value found in exp2/d
(ADAPTIVE_PULL_ELITE_SEGMENTS env var in run2.sh, default 4 until exp2/d
has been run).
Reports 3 numbers per tolerance value:
  - final_rpd (%): RPD of the final best cost (solution.working_time) vs BKS
  - irpd (%): trapezoidal-rule average of R0..R8 (RPD (%) of
    best_solution_cost_by_evaluation_checkpoint at 0/8..8/8 of the
    evaluation budget vs BKS working_time), (1/8) * [(R0+R8)/2 + R1+...+R7]
  - AR / accepted_transfer (%): pull_accept_count / pull_offer_count --
    of the offers a worker's pull request could get (offered = at least
    one elite in the pool from a worker other than the requester), how
    many actually get sent back to it (see pick_for_dispatch in
    parallel.cpp; same metric as exp2/c/exp2/g's AR)

RPD (%) = (value - BKS) / BKS * 100, same as exp1.5/stats.py. final_rpd
uses the final best cost (solution.working_time); irpd uses the tracked
best_solution_cost_by_evaluation_checkpoint values (the true best solution
found so far at each checkpoint, not the elite pool's own best -- see
exp2/a for that one -- the two can diverge under
elite-replace-strategy=quality-only/random-target), which can carry an
infeasibility penalty at early checkpoints if the best-known solution
isn't yet feasible at that point.

Aggregation: same style as exp2/c/d for final_rpd and irpd -- computed per
run first (using that run's own solution/checkpoints vs BKS), then averaged
over that instance's runs, then averaged over instances (4 by default: 2
customer counts x 2 combos). AR (%) instead follows exp2/c's pooling:
N_accept summed over the instance's runs / N_offer summed over the
instance's runs * 100 (not averaged per-run), since offer counts can vary
a lot run to run -- pooling weights each pull attempt equally instead of
each run equally. These per-instance numbers are then averaged over
instances to get each tolerance value's final numbers.

Looks for run files at
<outputs>/<n>/<n>.<combo>-tol<tolerance>-<run_id>.json (the naming used by
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

TOLERANCE_LIST = ["0.5", "1", "2"]
NUM_CHECKPOINTS = 9  # R0..R8, at k/8 of the evaluation budget


def load_run(path: Path):
    with path.open() as f:
        return json.load(f)


def load_bks_working_time(path: Path):
    with path.open() as f:
        return json.load(f)["solution"]["working_time"]


def find_run_files(outputs_dir: Path, n: str, instance: str, tolerance: str):
    """Returns [(run_id, path), ...] sorted by run_id."""
    inst_dir = outputs_dir / n
    if not inst_dir.is_dir():
        return []
    pattern = re.compile(rf"^{re.escape(instance)}-tol{re.escape(tolerance)}-(\d+)\.json$")
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
    for tolerance in TOLERANCE_LIST:
        run_files = find_run_files(outputs_dir, n, instance, tolerance)

        rpds, irpds_per_run, offer_counts, accepted_counts = [], [], [], []
        for run_id, path in run_files:
            data = load_run(path)
            final_cost = data["solution"]["working_time"]
            off = data["pull_offer_count"]
            acc = data["pull_accept_count"]
            cps = data.get("best_solution_cost_by_evaluation_checkpoint")

            run_rpd = rpd_pct(final_cost, bks_value)
            run_ar_pct = acc / off * 100.0 if off else None

            if run_rpd is not None:
                rpds.append(run_rpd)
            if cps is not None and len(cps) == NUM_CHECKPOINTS:
                run_r = [rpd_pct(v, bks_value) for v in cps]
                irpds_per_run.append(irpd_pct(run_r))
            offer_counts.append(off)
            accepted_counts.append(acc)

            detail_rows.append({
                "n": n, "instance": instance, "tolerance": tolerance, "run": run_id,
                "final_cost": final_cost, "final_rpd_pct": run_rpd,
                "pull_offer_count": off,
                "pull_accept_count": acc, "ar_pct": run_ar_pct,
            })

        total_offer = sum(offer_counts)
        ar_pct = sum(accepted_counts) / total_offer * 100.0 if total_offer else None

        summary_rows.append({
            "n": n, "instance": instance, "tolerance": tolerance,
            "bks": bks_value, "runs": len(run_files), "expected_runs": expected_runs,
            "final_rpd_pct": mean_or_none(rpds),
            "irpd_pct": mean_or_none(irpds_per_run),
            "ar_pct": ar_pct,
        })
    return summary_rows, detail_rows


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
    ap.add_argument("--outputs", default="../../../outputs/exp2/e-1",
                     help="Dir containing run2.sh output, relative to this script "
                          "(default: ../../../outputs/exp2/e-1)")
    ap.add_argument("--bks", default="../../../../bks",
                     help="Dir containing BKS json files, relative to this script "
                          "(default: ../../../../bks)")
    ap.add_argument("--customers", default="200,500",
                     help="Comma-separated customer counts (default: 200,500)")
    ap.add_argument("--combos", default="10.2,40.1",
                     help="Comma-separated instance combos, matches run2.sh "
                          "(default: 10.2,40.1)")
    ap.add_argument("--runs", type=int, default=3,
                     help="Expected number of runs per (instance, tolerance), used to "
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

    summary_rows = []
    detail_rows = []
    for n in customers:
        for combo in combos:
            s, d = compute_instance(outputs_dir, bks_dir, n, combo, args.runs)
            summary_rows.extend(s)
            detail_rows.extend(d)

    incomplete = [f"{r['instance']}/tol{r['tolerance']} ({r['runs']}/{r['expected_runs']})"
                  for r in summary_rows if r["runs"] < r["expected_runs"]]

    def avg_field(rows, f):
        vals = [r[f] for r in rows if r[f] is not None]
        return statistics.mean(vals) if vals else None

    # ---- Per-instance-per-tolerance detail ----
    detail_header = (f"{'Instance':<12}{'Tolerance(%)':<14}{'Runs':>6}"
                      f"{'final_RPD(%)':>14}{'irpd(%)':>10}{'AR(%)':>10}")
    out(detail_header)
    out("-" * len(detail_header))
    for r in summary_rows:
        out(f"{r['instance']:<12}{r['tolerance']:<14}{r['runs']:>6}"
            f"{fmt(r['final_rpd_pct'], 3):>14}{fmt(r['irpd_pct'], 3):>10}"
            f"{fmt(r['ar_pct'], 3):>10}")
    out()

    # ---- final_rpd(%), irpd(%), and AR(%) per quality-tolerance value ----
    # final_rpd(%) and irpd(%): stratified average, per run -> mean over
    # runs (per instance, done in compute_instance) -> mean over instances
    # (here). AR(%): pooled per instance (done in compute_instance) ->
    # mean over instances (here).
    out(f"{'Tolerance(%)':<14}{'final_RPD(%)':>14}{'irpd(%)':>10}{'AR(%)':>10}")
    out("-" * 48)
    tolerance_rows = []
    for tolerance in TOLERANCE_LIST:
        rows = [r for r in summary_rows if r["tolerance"] == tolerance]
        tolerance_row = {
            "tolerance": tolerance,
            "final_rpd_pct": avg_field(rows, "final_rpd_pct"),
            "irpd_pct": avg_field(rows, "irpd_pct"),
            "ar_pct": avg_field(rows, "ar_pct"),
        }
        tolerance_rows.append(tolerance_row)
        out(f"{tolerance:<14}{fmt(tolerance_row['final_rpd_pct'], 3):>14}"
            f"{fmt(tolerance_row['irpd_pct'], 3):>10}"
            f"{fmt(tolerance_row['ar_pct'], 3):>10}")
    if incomplete:
        out(f"\nNote: fewer runs found than expected for: {', '.join(incomplete)}")

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
    if tolerance_rows:
        tolerance_path = outputs_dir / "tolerance_summary.csv"
        write_csv(tolerance_path, tolerance_rows)
        saved.append(tolerance_path)

    print("\nSaved:")
    for p in saved:
        print(f"  {p}")


if __name__ == "__main__":
    main()
