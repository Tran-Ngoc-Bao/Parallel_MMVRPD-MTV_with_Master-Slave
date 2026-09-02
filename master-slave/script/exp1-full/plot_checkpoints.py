#!/usr/bin/env python3
"""
Plot per-checkpoint convergence curves from the `checkpoint_curves.csv`
file emitted by `stat_checkpoints.py`
(default: master-slave/outputs/exp1-full/checkpoint_curves.csv).

    - x-axis: the 9 checkpoints 0/8 -> 8/8.
    - y-axis: mean RPD (%) at that checkpoint.
    - one line per method (sats, ims, coop); only methods present in the
      CSV are drawn.

Two images are written (no title / axis labels):
    checkpoint_curves.png       - all 9 checkpoints (includes 0/8)
    checkpoint_curves_no0.png   - drops checkpoint 0/8 so the remaining
                                  points are readable

Per-row RPD = (BestCost - BKS) / BKS * 100. Rows without a BKS (empty
BKS column) are skipped.

Averaging is done in this exact nested order (NOT one flat mean over all
runs), so runs / instances / customers each carry equal weight:

    1. mean over RUNS       -> one value per (method, customers, instance, checkpoint)
    2. mean over INSTANCES  -> one value per (method, customers, checkpoint)
    3. mean over CUSTOMERS  -> one value per (method, checkpoint)  == a point on the curve

Here "customers" is the customer-set size (6, 10, ..., 500), "instance"
is a concrete instance within a size, and "run" is a repeated run of one
instance.

Usage:
    python3 plot_checkpoints.py
    python3 plot_checkpoints.py --in /path/to/checkpoint_curves.csv -o curve.png
    python3 plot_checkpoints.py --methods coop ims --show
    python3 plot_checkpoints.py --customers 200,500
    python3 plot_checkpoints.py --dump-csv checkpoint_curves_agg.csv
"""

import argparse
import csv
import sys
from collections import defaultdict
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
DEFAULT_IN = SCRIPT_DIR / ".." / ".." / "outputs" / "exp1-full" / "checkpoint_curves.csv"

# Fixed draw order / colour per method (a method absent from the CSV is skipped).
METHOD_ORDER = ("sats", "ims", "coop")
METHOD_STYLE = {
    "sats": {"color": "#1f77b4", "marker": "o", "label": "SATS"},
    "ims":  {"color": "#ff7f0e", "marker": "s", "label": "IMS"},
    "coop": {"color": "#2ca02c", "marker": "^", "label": "COOP"},
}


def mean(xs):
    xs = list(xs)
    return sum(xs) / len(xs) if xs else float("nan")


def load_rpd_rows(csv_path, wanted_methods, wanted_customers):
    """Read the CSV -> list of (method, customers, instance, run, checkpoint, rpd_pct)."""
    out = []
    n_total = 0
    n_no_bks = 0
    n_bad_bks = 0
    with csv_path.open(newline="") as fh:
        reader = csv.DictReader(fh)
        needed = {"instance", "customers", "run", "method", "checkpoint", "BestCost", "BKS"}
        missing_cols = needed - set(reader.fieldnames or [])
        if missing_cols:
            sys.exit(f"error: file {csv_path} is missing column(s) {sorted(missing_cols)}")
        for row in reader:
            n_total += 1
            method = row["method"]
            if wanted_methods and method not in wanted_methods:
                continue
            customers = int(row["customers"])
            if wanted_customers and str(customers) not in wanted_customers:
                continue
            bks_raw = (row["BKS"] or "").strip()
            if not bks_raw:
                n_no_bks += 1
                continue
            try:
                bks = float(bks_raw)
                best = float(row["BestCost"])
            except ValueError:
                n_bad_bks += 1
                continue
            if bks <= 0.0:
                n_bad_bks += 1
                continue
            rpd = (best - bks) / bks * 100.0
            out.append((method, customers, row["instance"], row["run"],
                        int(row["checkpoint"]), rpd))
    return out, {"total": n_total, "no_bks": n_no_bks, "bad_bks": n_bad_bks}


def aggregate(rpd_rows):
    """Nested averaging: run -> instance -> customers.

    Returns a dict method -> {checkpoint -> rpd_pct} and the sorted checkpoint list.
    """
    # Step 1: collect per-run RPD keyed by (method, customers, instance, checkpoint)
    by_run = defaultdict(list)
    checkpoints = set()
    for method, customers, instance, _run, cp, rpd in rpd_rows:
        by_run[(method, customers, instance, cp)].append(rpd)
        checkpoints.add(cp)
    checkpoints = sorted(checkpoints)

    # Step 1 -> mean over runs; then group by (method, customers, checkpoint)
    by_instance = defaultdict(list)
    for (method, customers, instance, cp), vals in by_run.items():
        by_instance[(method, customers, cp)].append(mean(vals))

    # Step 2 -> mean over instances; then group by (method, checkpoint)
    by_customers = defaultdict(list)
    for (method, customers, cp), vals in by_instance.items():
        by_customers[(method, cp)].append(mean(vals))

    # Step 3 -> mean over customer-set sizes
    curve = defaultdict(dict)
    for (method, cp), vals in by_customers.items():
        curve[method][cp] = mean(vals)
    return curve, checkpoints


def dump_csv(path, curve, checkpoints, denom):
    with Path(path).open("w", newline="") as fh:
        w = csv.writer(fh)
        w.writerow(["method", "checkpoint", "q", "avg_rpd_pct"])
        for method in METHOD_ORDER:
            if method not in curve:
                continue
            for cp in checkpoints:
                w.writerow([method, cp, f"{cp}/{denom}", curve[method].get(cp, "")])
    print(f"Wrote the aggregated table to {path}")


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--in", dest="in_path", default=str(DEFAULT_IN),
                    help=f"Input checkpoint_curves.csv file (default: {DEFAULT_IN})")
    ap.add_argument("-o", "--out", default=None,
                    help="Output image file (default: <dir of --in>/checkpoint_curves.png)")
    ap.add_argument("--methods", nargs="+", default=None, choices=METHOD_ORDER,
                    help="Only plot these methods (default: every method present in the CSV)")
    ap.add_argument("--customers", default=None,
                    help="Filter by customer set, comma-separated (e.g. 200,500). "
                         "Default: all.")
    ap.add_argument("--dump-csv", default=None,
                    help="Also write the mean-RPD table (method x checkpoint) to this CSV file")
    ap.add_argument("--dpi", type=int, default=150, help="DPI of the output image (default: 150)")
    ap.add_argument("--markersize", type=float, default=4.0,
                    help="Marker size for the circle/square/triangle points (default: 4)")
    ap.add_argument("--ylim", default=None,
                    help="y-axis limits 'min,max' to zoom in (e.g. -1,3). "
                         "Checkpoint 0/8 is usually huge and flattens the rest.")
    ap.add_argument("--yticks", type=float, default=None,
                    help="Fixed spacing between y-axis major ticks (e.g. 0.1). "
                         "Default: auto, denser than matplotlib's default.")
    ap.add_argument("--ynbins", type=int, default=16,
                    help="Target number of y-axis major ticks when --yticks is unset "
                         "(default: 16)")
    ap.add_argument("--yminor", type=int, default=5,
                    help="Minor-tick subdivisions between each pair of major y ticks "
                         "(default: 5; set 1 to disable minor ticks/grid)")
    ap.add_argument("--show", action="store_true", help="Open a window to display the plot")
    args = ap.parse_args()

    csv_path = Path(args.in_path).resolve()
    if not csv_path.is_file():
        sys.exit(f"error: not found: {csv_path}")

    wanted_methods = set(args.methods) if args.methods else None
    wanted_customers = ({c.strip() for c in args.customers.split(",") if c.strip()}
                        if args.customers else None)

    rpd_rows, stats = load_rpd_rows(csv_path, wanted_methods, wanted_customers)
    if not rpd_rows:
        sys.exit("error: no valid rows left after filtering (BKS missing everywhere?)")

    curve, checkpoints = aggregate(rpd_rows)
    denom = max(checkpoints) if checkpoints else 8
    methods_present = [m for m in METHOD_ORDER if m in curve]

    # Print a short summary to stdout
    print(f"Read {stats['total']} rows from {csv_path}")
    if stats["no_bks"]:
        print(f"  skipped {stats['no_bks']} rows with no BKS")
    if stats["bad_bks"]:
        print(f"  skipped {stats['bad_bks']} rows with an invalid BKS")
    n_by = defaultdict(lambda: [set(), set(), set()])
    for method, customers, instance, run, _cp, _rpd in rpd_rows:
        s = n_by[method]
        s[0].add(customers); s[1].add((customers, instance)); s[2].add((customers, instance, run))
    for method in methods_present:
        s = n_by[method]
        print(f"  {method}: {len(s[0])} customer sets, {len(s[1])} instances, {len(s[2])} runs")
        print("    " + "  ".join(f"{cp}/{denom}={curve[method][cp]:+.3f}%" for cp in checkpoints))

    if args.dump_csv:
        dump_csv(args.dump_csv, curve, checkpoints, denom)

    try:
        import matplotlib
        if not args.show:
            matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        from matplotlib.ticker import AutoMinorLocator, MaxNLocator, MultipleLocator
    except ImportError:
        sys.exit("error: matplotlib is required to plot -- install with:  python3 -m pip install --user matplotlib\n"
                 "     (you can still use --dump-csv to export the numbers without matplotlib)")

    ylim = None
    if args.ylim:
        try:
            lo, hi = (float(x) for x in args.ylim.split(","))
            ylim = (lo, hi)
        except ValueError:
            sys.exit("error: --ylim must have the form 'min,max' (e.g. -1,3)")

    def render(cps, out_path):
        xs = [cp / denom for cp in cps]
        fig, ax = plt.subplots(figsize=(8, 5))
        for method in methods_present:
            ys = [curve[method][cp] for cp in cps]
            st = METHOD_STYLE[method]
            ax.plot(xs, ys, marker=st["marker"], color=st["color"], label=st["label"],
                    linewidth=1.8, markersize=args.markersize)
        ax.set_xticks(xs)
        ax.set_xticklabels([f"{cp}/{denom}" for cp in cps])
        if ylim:
            ax.set_ylim(*ylim)
        # Denser y-axis: explicit major step via --yticks, else many auto
        # major ticks, plus --yminor minor subdivisions between them.
        if args.yticks:
            ax.yaxis.set_major_locator(MultipleLocator(args.yticks))
        else:
            ax.yaxis.set_major_locator(MaxNLocator(nbins=args.ynbins, steps=[1, 2, 2.5, 5, 10]))
        if args.yminor > 1:
            ax.yaxis.set_minor_locator(AutoMinorLocator(args.yminor))
        ax.grid(True, which="major", linestyle=":", alpha=0.5)
        ax.grid(True, which="minor", linestyle=":", alpha=0.25, linewidth=0.5)
        ax.axhline(0.0, color="0.6", linewidth=0.8)
        ax.legend()
        fig.tight_layout()
        fig.savefig(out_path, dpi=args.dpi)
        print(f"Saved the plot to {out_path}")

    base = (Path(args.out).resolve() if args.out
            else csv_path.with_name("checkpoint_curves.png"))
    render(checkpoints, base)                                    # with checkpoint 0
    rest = [cp for cp in checkpoints if cp != 0]
    if rest:
        render(rest, base.with_name(base.stem + "_no0" + base.suffix))  # without checkpoint 0

    if args.show:
        plt.show()


if __name__ == "__main__":
    main()
