#!/usr/bin/env python3
"""
Statistics for run2.sh (exp2/c, elite-pull-strategy x elite-pull-accept-
strategy comparison).

Compares 4 elite-pull strategies (random, topk, rank, pullcount), each run
with both elite-pull-accept strategies (always, selective), all with
significant-best push strategy, elite_pool_factor 0.03, and
similarity-aware pool replacement. For each pull strategy reports:
  - always RPD (%): RPD of the final best cost (solution.working_time) vs
    BKS working_time, under the "always" accept strategy
  - selective RPD (%): same, under the "selective" accept strategy
  - delta_acc (%): always_rpd - selective_rpd (how much RPD the selective
    accept filter saves/costs vs blindly accepting every pull)
  - selective AR (%): pull_accept_count / pull_offer_count * 100, under the
    "selective" accept strategy (acceptance rate of offered elite pulls)

RPD (%) = (value - BKS) / BKS * 100, same as exp1.5/stats.py.

Aggregation: for each (n, combo) instance and each (pull_strategy,
accept_strategy) pair, RPD is computed per run first (from that run's own
final cost vs BKS), then averaged over runs. Selective AR (%) is instead
pooled across runs: N_accept summed over the instance's runs / N_offer
summed over the instance's runs * 100 (not averaged per-run) -- offer
counts can vary a lot run to run, so pooling weights each pull attempt
equally instead of each run equally. These per-instance numbers are then
averaged over instances (6 by default: 3 customer counts x 2 combos) to get
each pull strategy's final numbers. delta_acc is derived from the final
averaged always/selective RPDs (equivalent to averaging per-instance
deltas, since RPD is linear).

Looks for run files at
<outputs>/<n>/<n>.<combo>-<pull_strategy>-<accept_strategy>-<run_id>.json
(the naming used by run2.sh) and BKS files at
<bks>/<n>/<n>.<combo>-bks.json.

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

PULL_STRATEGIES = ["random", "topk", "rank", "pullcount"]
ACCEPT_STRATEGIES = ["always", "selective"]


def load_run(path: Path):
    with path.open() as f:
        return json.load(f)


def load_bks_working_time(path: Path):
    with path.open() as f:
        return json.load(f)["solution"]["working_time"]


def find_run_files(outputs_dir: Path, n: str, instance: str, pull_strategy: str, accept_strategy: str):
    """Returns [(run_id, path), ...] sorted by run_id."""
    inst_dir = outputs_dir / n
    if not inst_dir.is_dir():
        return []
    pattern = re.compile(
        rf"^{re.escape(instance)}-{re.escape(pull_strategy)}-{re.escape(accept_strategy)}-(\d+)\.json$")
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


def compute_instance(outputs_dir: Path, bks_dir: Path, n: str, combo: str, expected_runs: int):
    instance = f"{n}.{combo}"
    bks_path = bks_dir / n / f"{instance}-bks.json"
    bks_value = load_bks_working_time(bks_path) if bks_path.is_file() else None

    summary_rows = []
    detail_rows = []
    instance_rows = []
    for pull_strategy in PULL_STRATEGIES:
        per_accept = {}
        for accept_strategy in ACCEPT_STRATEGIES:
            run_files = find_run_files(outputs_dir, n, instance, pull_strategy, accept_strategy)

            rpds, accept_counts, offer_counts = [], [], []
            for run_id, path in run_files:
                data = load_run(path)
                final_cost = data["solution"]["working_time"]
                pac = data["pull_accept_count"]
                poc = data["pull_offer_count"]

                run_rpd = rpd_pct(final_cost, bks_value)
                run_ar_pct = pac / poc * 100.0 if poc else None
                if run_rpd is not None:
                    rpds.append(run_rpd)
                accept_counts.append(pac)
                offer_counts.append(poc)

                detail_rows.append({
                    "n": n, "instance": instance, "pull_strategy": pull_strategy,
                    "accept_strategy": accept_strategy, "run": run_id,
                    "final_cost": final_cost, "rpd_pct": run_rpd,
                    "pull_accept_count": pac, "pull_offer_count": poc,
                    "accept_rate_pct": run_ar_pct,
                })

            def mean_or_none(vals):
                vals = [v for v in vals if v is not None]
                return statistics.mean(vals) if vals else None

            rpd = mean_or_none(rpds)
            ar_pct = None
            total_offer = sum(offer_counts)
            if accept_strategy == "selective" and total_offer:
                ar_pct = sum(accept_counts) / total_offer * 100.0

            per_accept[accept_strategy] = {"runs": len(run_files), "rpd": rpd, "ar_pct": ar_pct}
            summary_rows.append({
                "n": n, "instance": instance, "pull_strategy": pull_strategy,
                "accept_strategy": accept_strategy, "bks": bks_value,
                "runs": len(run_files), "expected_runs": expected_runs,
                "rpd_pct": rpd, "accept_rate_pct": ar_pct,
            })

        always = per_accept["always"]
        selective = per_accept["selective"]
        delta_acc = None
        if always["rpd"] is not None and selective["rpd"] is not None:
            delta_acc = always["rpd"] - selective["rpd"]

        instance_rows.append({
            "n": n, "instance": instance, "pull_strategy": pull_strategy,
            "bks": bks_value,
            "always_runs": always["runs"], "selective_runs": selective["runs"],
            "expected_runs": expected_runs,
            "always_rpd_pct": always["rpd"], "selective_rpd_pct": selective["rpd"],
            "delta_acc_pct": delta_acc, "selective_ar_pct": selective["ar_pct"],
        })
    return summary_rows, detail_rows, instance_rows


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
    ap.add_argument("--outputs", default="../../../outputs/exp2/c",
                     help="Dir containing run2.sh output, relative to this script "
                          "(default: ../../../outputs/exp2/c)")
    ap.add_argument("--bks", default="../../../../bks",
                     help="Dir containing BKS json files, relative to this script "
                          "(default: ../../../../bks)")
    ap.add_argument("--customers", default="200,500",
                     help="Comma-separated customer counts (default: 200,500)")
    ap.add_argument("--combos", default="10.2,40.1",
                     help="Comma-separated instance combos, matches run2.sh "
                          "(default: 10.2,40.1)")
    ap.add_argument("--runs", type=int, default=3,
                     help="Expected number of runs per (instance, pull_strategy, "
                          "accept_strategy), used to flag incomplete ones (default: 3)")
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
    instance_rows = []
    for n in customers:
        for combo in combos:
            s, d, i = compute_instance(outputs_dir, bks_dir, n, combo, args.runs)
            summary_rows.extend(s)
            detail_rows.extend(d)
            instance_rows.extend(i)

    # ---- Per-instance-per-(pull,accept) summary ----
    header = (f"{'Instance':<12}{'Pull':<12}{'Accept':<11}{'Runs':>6}"
              f"{'RPD(%)':>11}{'AR(%)':>10}")
    out(header)
    out("-" * len(header))
    incomplete = []
    for r in summary_rows:
        out(f"{r['instance']:<12}{r['pull_strategy']:<12}{r['accept_strategy']:<11}{r['runs']:>6}"
            f"{fmt(r['rpd_pct'], 3):>11}{fmt(r['accept_rate_pct'], 3):>10}")
        if r["runs"] < r["expected_runs"]:
            incomplete.append(
                f"{r['instance']}/{r['pull_strategy']}-{r['accept_strategy']} ({r['runs']}/{r['expected_runs']})")
    if incomplete:
        out(f"\nNote: fewer runs found than expected for: {', '.join(incomplete)}")

    # ---- Per-instance-per-pull_strategy: always vs selective + delta_acc ----
    out(f"\n{'Instance':<12}{'Pull':<12}{'AlwaysRPD(%)':>14}{'SelectRPD(%)':>14}"
        f"{'DeltaAcc(%)':>13}{'SelectAR(%)':>13}")
    out("-" * 78)
    for r in instance_rows:
        out(f"{r['instance']:<12}{r['pull_strategy']:<12}"
            f"{fmt(r['always_rpd_pct'], 3):>14}{fmt(r['selective_rpd_pct'], 3):>14}"
            f"{fmt(r['delta_acc_pct'], 3):>13}{fmt(r['selective_ar_pct'], 3):>13}")

    # ---- Aggregate: instance -> pull_strategy (avg over instances) ----
    out(f"\n{'PullStrategy':<14}{'AlwaysRPD(%)':>14}{'SelectRPD(%)':>14}"
        f"{'DeltaAcc(%)':>13}{'SelectAR(%)':>13}")
    out("-" * 68)
    strategy_rows = []
    for pull_strategy in PULL_STRATEGIES:
        rows = [r for r in instance_rows if r["pull_strategy"] == pull_strategy]

        def avg_field(f, rows=rows):
            vals = [r[f] for r in rows if r[f] is not None]
            return statistics.mean(vals) if vals else None

        always_rpd = avg_field("always_rpd_pct")
        selective_rpd = avg_field("selective_rpd_pct")
        delta_acc = (always_rpd - selective_rpd) if always_rpd is not None and selective_rpd is not None else None
        selective_ar = avg_field("selective_ar_pct")

        strategy_row = {
            "pull_strategy": pull_strategy,
            "always_rpd_pct": always_rpd, "selective_rpd_pct": selective_rpd,
            "delta_acc_pct": delta_acc, "selective_ar_pct": selective_ar,
        }
        strategy_rows.append(strategy_row)
        out(f"{pull_strategy:<14}{fmt(always_rpd, 3):>14}{fmt(selective_rpd, 3):>14}"
            f"{fmt(delta_acc, 3):>13}{fmt(selective_ar, 3):>13}")

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
    if instance_rows:
        instance_path = outputs_dir / "instance_summary.csv"
        write_csv(instance_path, instance_rows)
        saved.append(instance_path)
    if strategy_rows:
        strategy_path = outputs_dir / "strategy_summary.csv"
        write_csv(strategy_path, strategy_rows)
        saved.append(strategy_path)

    print("\nSaved:")
    for p in saved:
        print(f"  {p}")


if __name__ == "__main__":
    main()
