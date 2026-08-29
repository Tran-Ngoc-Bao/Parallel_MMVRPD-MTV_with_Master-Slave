#!/usr/bin/env python3
"""
Summarise evaluation counts from the result JSON files under
sequence/cpp/outputs/<group>/<customers>/<customers>.<a>.<b>-<run>.json

NOTE: a run_ims_full.sh result file is the JSON of the worker that
produced the best solution in one island, so its "total_evaluations" is
that worker's ALONE. run_ims_full.sh additionally writes:
  - "total_evaluations_all_workers": evaluations summed over ALL workers
    of the island
  - "num_workers": how many workers contributed to that sum
sats / plain sequential runs have only "total_evaluations" (already the
full count). So by default (no --field) each file is measured with the
first field it actually contains, in this order:
    total_evaluations_all_workers  ->  total_evaluations
which lets a single run cover ims and sats together. Pass --field to
force one specific field for every file instead.

This script also computes an avg_per_worker column = <chosen field> /
num_workers, matching master-slave/script/exp1-full/stat_evaluations.py
(the only difference: the worker count comes from "num_workers" instead
of the length of "worker_seeds"). sats rows have no num_workers, so
their avg_per_worker / workers cells stay blank.

Every group directory (e.g. ims, sats) and every available customer set
(100, 200, 500, 1000, ...) is scanned automatically, so new data only
needs a re-run of the script -- no code change.

Usage:
    python3 stat_evaluations.py
    python3 stat_evaluations.py --outputs-dir /other/path
    python3 stat_evaluations.py --groups ims --field total_evaluations_all_workers

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

# With no --field, each file is measured with the first of these it has.
FIELD_PREFERENCE = ("total_evaluations_all_workers", "total_evaluations")


def resolve_field_value(data, field):
    """(field_name, value) for one result dict; (None, None) if none apply."""
    if field is not None:
        return field, data.get(field)
    for name in FIELD_PREFERENCE:
        if data.get(name) is not None:
            return name, data[name]
    return None, None


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
                per_worker_vals = []
                worker_counts = []
                fields_used = set()
                for fpath in sorted(by_instance[inst]):
                    with open(fpath) as fh:
                        data = json.load(fh)
                    fname_used, v = resolve_field_value(data, field)
                    if v is None:
                        continue
                    vals.append(v)
                    fields_used.add(fname_used)
                    num_workers = data.get("num_workers", 0)
                    if num_workers > 0:
                        per_worker_vals.append(v / num_workers)
                        worker_counts.append(num_workers)
                if not vals:
                    continue
                rows.append({
                    "group": group,
                    "customers": customers,
                    "instance": f"{customers}.{inst}",
                    "runs": len(vals),
                    "field": "/".join(sorted(fields_used)),
                    "avg": statistics.mean(vals),
                    "min": min(vals),
                    "max": max(vals),
                    "sum": sum(vals),
                    "avg_per_worker": statistics.mean(per_worker_vals) if per_worker_vals else None,
                    "avg_workers": statistics.mean(worker_counts) if worker_counts else None,
                })
    return rows


def write_csv(rows, out_path):
    with open(out_path, "w", newline="") as fh:
        w = csv.writer(fh)
        w.writerow(["group", "customers", "instance", "runs", "field",
                    "avg_evaluations", "avg_evaluations_per_worker", "avg_workers",
                    "min_evaluations", "max_evaluations", "sum_evaluations"])
        for r in rows:
            avg_per_worker = f"{r['avg_per_worker']:.2f}" if r["avg_per_worker"] is not None else ""
            avg_workers = f"{r['avg_workers']:.2f}" if r["avg_workers"] is not None else ""
            w.writerow([r["group"], r["customers"], r["instance"], r["runs"], r["field"],
                        f"{r['avg']:.2f}", avg_per_worker, avg_workers,
                        r["min"], r["max"], r["sum"]])


def render_table(rows_data, columns):
    """rows_data: list of dict[key->str]. columns: list of (key, label, align)
    where align is '<' or '>'. Column widths are sized to the widest cell
    actually present (label or data), plus a fixed gap -- so a column never
    glues into its neighbour no matter how large the numbers get (unlike a
    hardcoded width, which silently stops padding once content exceeds it)."""
    GAP = 2
    widths = {}
    for key, label, _align in columns:
        widest = max([len(label)] + [len(r[key]) for r in rows_data])
        widths[key] = widest + GAP

    def render_row(r):
        return "".join(f"{r[key]:{align}{widths[key]}}" for key, _label, align in columns)

    header = "".join(f"{label:{align}{widths[key]}}" for key, label, align in columns)
    return header, [render_row(r) for r in rows_data]


def write_txt(rows, out_path, field):
    lines = []
    title_field = (field or "evaluations").upper()
    lines.append(f"{title_field} COUNT BY INSTANCE GROUP")
    lines.append("(each instance group is usually 10 runs; values taken from the result JSON files)")
    lines.append("=" * 94)
    lines.append("")

    columns = [
        ("instance", "Instance", "<"),
        ("runs", "Runs", ">"),
        ("avg", "Avg", ">"),
        ("avg_per_worker", "Avg/worker", ">"),
        ("workers", "Workers", ">"),
        ("min", "Min", ">"),
        ("max", "Max", ">"),
        ("sum", "Sum", ">"),
    ]

    groups = sorted(set(r["group"] for r in rows))
    for group in groups:
        lines.append(f"### Directory: {group}")
        lines.append("")
        customers_list = sorted(set(r["customers"] for r in rows if r["group"] == group), key=int)
        for customers in customers_list:
            sub_rows = [r for r in rows if r["group"] == group and r["customers"] == customers]
            if not sub_rows:
                continue

            all_vals_sum = 0
            all_vals_runs = 0
            all_vals_min = None
            all_vals_max = None
            weighted_avg_num = 0
            weighted_avg_per_worker_num = 0
            per_worker_runs = 0

            table_rows = []
            for r in sub_rows:
                avg_per_worker_str = f"{r['avg_per_worker']:,.2f}" if r["avg_per_worker"] is not None else "-"
                workers_str = f"{r['avg_workers']:,.1f}" if r["avg_workers"] is not None else "-"
                table_rows.append({
                    "instance": r["instance"], "runs": str(r["runs"]),
                    "avg": f"{r['avg']:,.2f}", "avg_per_worker": avg_per_worker_str,
                    "workers": workers_str,
                    "min": f"{r['min']:,}", "max": f"{r['max']:,}", "sum": f"{r['sum']:,}",
                })

                all_vals_sum += r["sum"]
                all_vals_runs += r["runs"]
                weighted_avg_num += r["avg"] * r["runs"]
                if r["avg_per_worker"] is not None:
                    weighted_avg_per_worker_num += r["avg_per_worker"] * r["runs"]
                    per_worker_runs += r["runs"]
                all_vals_min = r["min"] if all_vals_min is None else min(all_vals_min, r["min"])
                all_vals_max = r["max"] if all_vals_max is None else max(all_vals_max, r["max"])

            overall_avg = weighted_avg_num / all_vals_runs if all_vals_runs else 0
            overall_avg_per_worker = (weighted_avg_per_worker_num / per_worker_runs
                                       if per_worker_runs else None)
            overall_avg_per_worker_str = (f"{overall_avg_per_worker:,.2f}"
                                           if overall_avg_per_worker is not None else "-")
            total_row = {
                "instance": "TOTAL/AVG", "runs": str(all_vals_runs),
                "avg": f"{overall_avg:,.2f}", "avg_per_worker": overall_avg_per_worker_str,
                "workers": "",
                "min": f"{all_vals_min:,}", "max": f"{all_vals_max:,}", "sum": f"{all_vals_sum:,}",
            }

            header, rendered_rows = render_table(table_rows + [total_row], columns)

            fields_here = "/".join(sorted({r["field"] for r in sub_rows if r["field"]}))
            lines.append(f"-- Customer set = {customers}  (field: {fields_here}) --")
            lines.append(header)
            lines.append("-" * len(header))
            lines.extend(rendered_rows[:-1])
            lines.append("-" * len(header))
            lines.append(rendered_rows[-1])
            lines.append("")
        lines.append("")

    with open(out_path, "w") as fh:
        fh.write("\n".join(lines))


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    default_outputs_dir = os.path.normpath(
        os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "outputs", "exp1-full")
    )
    parser.add_argument("--outputs-dir", default=default_outputs_dir,
                         help="Path to the outputs directory (default: the directory containing this script)")
    parser.add_argument("--groups", nargs="*", default=None,
                         help="List of group directories to summarise, e.g. ims sats (default: auto-scan all)")
    parser.add_argument("--field", default=None,
                         help="Force one JSON field for every file. Default: per file, "
                              "total_evaluations_all_workers if present else total_evaluations "
                              "(so ims + sats are covered by one run).")
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

    fields_seen = "/".join(sorted({r["field"] for r in rows if r["field"]}))
    print(f"Wrote {len(rows)} summary rows ({', '.join(groups)}; field: {fields_seen}) to:")
    print(f"  - {csv_out}")
    print(f"  - {txt_out}")


if __name__ == "__main__":
    main()
