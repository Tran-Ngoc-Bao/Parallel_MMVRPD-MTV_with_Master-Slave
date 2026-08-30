#!/usr/bin/env python3
"""
Summarise evaluation counts (total_evaluations) from the result JSON
files under sequence/cpp/outputs/<group>/<customers>/<customers>.<a>.<b>-<run>.json

Every group directory (e.g. ims, sats) and every available customer set
(100, 200, 500, 1000, ...) is scanned automatically, so new customer-1000
data only needs a re-run of the script -- no code change.

Usage:
    python3 stat_evaluations.py
    python3 stat_evaluations.py --outputs-dir /other/path
    python3 stat_evaluations.py --groups ims sats --field total_evaluations

Output:
    evaluations_by_instance.csv   -> data table, opens in Excel/Sheets
    evaluations_by_instance.txt   -> column-aligned table, readable as-is
"""

import argparse
import csv
import glob
import json
import os
import re
import statistics

# file name pattern: <customers>.<a>.<b>-<run>.json  (e.g. 500.30.3-7.json)
FILENAME_RE = re.compile(r"^(\d+)\.(\d+\.\d+)-(\d+)\.json$")


def discover_groups(outputs_dir, requested_groups=None):
    """Return the immediate sub-directories of outputs_dir that are 'group'
    dirs (e.g. ims, sats), skipping unrelated files/dirs."""
    if requested_groups:
        return [g for g in requested_groups if os.path.isdir(os.path.join(outputs_dir, g))]
    groups = []
    for name in sorted(os.listdir(outputs_dir)):
        path = os.path.join(outputs_dir, name)
        if os.path.isdir(path):
            groups.append(name)
    return groups


def discover_customers(group_dir):
    """Return the customer sets (sub-directory names, e.g. '100','200','500','1000'),
    sorted by ascending numeric value."""
    customers = []
    for name in os.listdir(group_dir):
        path = os.path.join(group_dir, name)
        if os.path.isdir(path) and name.isdigit():
            customers.append(name)
    return sorted(customers, key=int)


def collect_stats(outputs_dir, groups, field):
    rows = []
    for group in groups:
        group_dir = os.path.join(outputs_dir, group)
        for customers in discover_customers(group_dir):
            cust_dir = os.path.join(group_dir, customers)
            # group json files by instance (e.g. 10.1 / 20.2 / 30.3 / 40.4)
            by_instance = {}
            for fpath in glob.glob(os.path.join(cust_dir, "*.json")):
                fname = os.path.basename(fpath)
                m = FILENAME_RE.match(fname)
                if not m:
                    continue
                n, inst, _run = m.groups()
                if n != customers:
                    continue
                by_instance.setdefault(inst, []).append(fpath)

            for inst in sorted(by_instance.keys(), key=lambda s: tuple(map(int, s.split(".")))):
                vals = []
                for fpath in sorted(by_instance[inst]):
                    with open(fpath) as fh:
                        data = json.load(fh)
                    v = data.get(field)
                    if v is not None:
                        vals.append(v)
                if not vals:
                    continue
                rows.append({
                    "group": group,
                    "customers": customers,
                    "instance": f"{customers}.{inst}",
                    "runs": len(vals),
                    "avg": statistics.mean(vals),
                    "min": min(vals),
                    "max": max(vals),
                    "sum": sum(vals),
                })
    return rows


def write_csv(rows, out_path):
    with open(out_path, "w", newline="") as fh:
        w = csv.writer(fh)
        w.writerow(["group", "customers", "instance", "runs",
                    "avg_evaluations", "min_evaluations", "max_evaluations", "sum_evaluations"])
        for r in rows:
            w.writerow([r["group"], r["customers"], r["instance"], r["runs"],
                        f"{r['avg']:.2f}", r["min"], r["max"], r["sum"]])


def write_txt(rows, out_path, field):
    lines = []
    lines.append(f"{field.upper()} COUNT BY INSTANCE GROUP")
    lines.append("(each instance group is usually 10 runs; values taken from the result JSON files)")
    lines.append("=" * 94)
    lines.append("")

    groups = sorted(set(r["group"] for r in rows))
    for group in groups:
        lines.append(f"### Directory: {group}")
        lines.append("")
        customers_list = sorted(set(r["customers"] for r in rows if r["group"] == group), key=int)
        for customers in customers_list:
            sub_rows = [r for r in rows if r["group"] == group and r["customers"] == customers]
            header = f"{'Instance':<12}{'Runs':>6}{'Avg':>20}{'Min':>16}{'Max':>16}{'Sum':>20}"
            lines.append(f"-- Customer set = {customers} --")
            lines.append(header)
            lines.append("-" * len(header))
            all_vals_sum = 0
            all_vals_runs = 0
            all_vals_min = None
            all_vals_max = None
            weighted_avg_num = 0
            for r in sub_rows:
                lines.append(
                    f"{r['instance']:<12}{r['runs']:>6}{r['avg']:>20,.2f}"
                    f"{r['min']:>16,}{r['max']:>16,}{r['sum']:>20,}"
                )
                all_vals_sum += r["sum"]
                all_vals_runs += r["runs"]
                weighted_avg_num += r["avg"] * r["runs"]
                all_vals_min = r["min"] if all_vals_min is None else min(all_vals_min, r["min"])
                all_vals_max = r["max"] if all_vals_max is None else max(all_vals_max, r["max"])
            if sub_rows:
                lines.append("-" * len(header))
                overall_avg = weighted_avg_num / all_vals_runs if all_vals_runs else 0
                lines.append(
                    f"{'TOTAL/AVG':<12}{all_vals_runs:>6}{overall_avg:>20,.2f}"
                    f"{all_vals_min:>16,}{all_vals_max:>16,}{all_vals_sum:>20,}"
                )
            lines.append("")
        lines.append("")

    with open(out_path, "w") as fh:
        fh.write("\n".join(lines))


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    default_outputs_dir = os.path.normpath(
        os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "outputs", "exp1")
    )
    parser.add_argument("--outputs-dir", default=default_outputs_dir,
                         help="Path to the outputs directory (default: the directory containing this script)")
    parser.add_argument("--groups", nargs="*", default=None,
                         help="List of group directories to summarise, e.g. ims sats (default: auto-scan all)")
    parser.add_argument("--field", default="total_evaluations",
                         help="JSON field name to summarise (default: total_evaluations)")
    parser.add_argument("--csv-out", default=None, help="Output CSV file name")
    parser.add_argument("--txt-out", default=None, help="Output TXT file name")
    args = parser.parse_args()

    groups = discover_groups(args.outputs_dir, args.groups)
    if not groups:
        raise SystemExit(f"No group directory found in {args.outputs_dir}")

    rows = collect_stats(args.outputs_dir, groups, args.field)
    if not rows:
        raise SystemExit("No evaluations data found (check the field name / path).")

    csv_out = args.csv_out or os.path.join(args.outputs_dir, "evaluations_by_instance.csv")
    txt_out = args.txt_out or os.path.join(args.outputs_dir, "evaluations_by_instance.txt")

    write_csv(rows, csv_out)
    write_txt(rows, txt_out, args.field)

    print(f"Wrote {len(rows)} summary rows to:")
    print(f"  - {csv_out}")
    print(f"  - {txt_out}")


if __name__ == "__main__":
    main()
