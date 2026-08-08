#!/usr/bin/env python3
"""
Plot convergence chart from a *-convergence.csv file produced by the master-slave solver.

By default, the PNG is auto-saved to a 'convergence/' directory at the same level as
the CSV's parent directory, with the same filename:
    outputs/CMT1-XYZ-convergence.csv  →  convergence/CMT1-XYZ-convergence.png

Usage:
    python3 plot_convergence.py <convergence.csv> [--output <output.png>]
"""

import argparse
import csv
import sys
from pathlib import Path


def load_csv(path: str) -> tuple[list[float], list[float]]:
    times, costs = [], []
    with open(path, newline="", encoding="utf-8") as f:
        first = f.readline().strip()
        if first.lower().startswith("sep="):
            reader = csv.DictReader(f)
        else:
            f.seek(0)
            reader = csv.DictReader(f)
        for row in reader:
            times.append(float(row["time_ms"]))
            costs.append(float(row["cost_min"]))
    return times, costs


def default_output_path(csv_path: str) -> Path:
    p = Path(csv_path).resolve()
    convergence_dir = p.parent.parent / "convergence"
    convergence_dir.mkdir(parents=True, exist_ok=True)
    return convergence_dir / (p.stem + ".png")


def plot(times: list[float], costs: list[float], csv_path: str, output: str | None):
    try:
        import matplotlib.pyplot as plt  # type: ignore
    except ImportError:
        print("matplotlib is required. Install with: pip install matplotlib", file=sys.stderr)
        sys.exit(1)

    out_path = Path(output) if output else default_output_path(csv_path)

    fig, ax = plt.subplots(figsize=(10, 5))
    ax.step(times, costs, where="post", color="steelblue", linewidth=0.6)
    ax.scatter(times, costs, color="steelblue", s=1, zorder=3)
    ax.set_xlabel("Time (ms)")
    ax.set_ylabel("Cost (min)")
    margin = (max(costs) - min(costs)) * 0.1 or max(costs) * 0.01
    ax.set_ylim(min(costs) - margin, max(costs) + margin)
    ax.xaxis.set_major_locator(plt.MaxNLocator(nbins=15))
    ax.yaxis.set_major_locator(plt.MaxNLocator(nbins=15))
    ax.grid(True, linestyle="--", alpha=0.5)
    ax.annotate(
        f"{costs[-1]:.6f} min",
        xy=(times[-1], costs[-1]),
        xytext=(8, 8),
        textcoords="offset points",
        fontsize=8,
        color="steelblue",
    )
    fig.tight_layout()
    fig.savefig(out_path, dpi=150)
    print(f"Chart saved to: {out_path}")


def plot_elites_per_second(times: list[float], csv_path: str, output: str | None):
    try:
        import matplotlib.pyplot as plt  # type: ignore
    except ImportError:
        print("matplotlib is required. Install with: pip install matplotlib", file=sys.stderr)
        sys.exit(1)

    # bin each elite event into its 1-second interval
    bins: dict[int, int] = {}
    for t in times:
        sec = int(t / 1000)
        bins[sec] = bins.get(sec, 0) + 1

    max_sec = max(bins)
    xs = list(range(max_sec + 1))
    ys = [bins.get(s, 0) for s in xs]

    if output:
        out_path = Path(output).with_stem(Path(output).stem + "_elites_per_sec")
    else:
        base = default_output_path(csv_path)
        out_path = base.with_stem(base.stem + "_elites_per_sec")

    fig, ax = plt.subplots(figsize=(10, 5))
    bars = ax.bar(xs, ys, color="steelblue", edgecolor="white", linewidth=0.4)

    # label each bar if count > 0
    for bar, count in zip(bars, ys):
        if count > 0:
            ax.text(
                bar.get_x() + bar.get_width() / 2,
                bar.get_height() + max(ys) * 0.01,
                str(count),
                ha="center", va="bottom", fontsize=7.5, color="#333333",
            )

    ax.set_xlabel("Time (s)")
    ax.set_ylabel("Elites found")
    ax.set_xticks(xs)
    ax.set_xticklabels([str(s) for s in xs], fontsize=8)
    ax.grid(True, axis="y", linestyle="--", alpha=0.4)
    fig.tight_layout()
    fig.savefig(out_path, dpi=150)
    print(f"Bar chart saved to: {out_path}")


def main():
    parser = argparse.ArgumentParser(description="Plot convergence from CSV file")
    parser.add_argument("csv", help="Path to *-convergence.csv file")
    parser.add_argument("--output", "-o", default=None,
                        help="Override output path (png/pdf/svg...). "
                             "Default: convergence/<csv_name>.png next to the outputs/ directory.")
    args = parser.parse_args()

    times, costs = load_csv(args.csv)
    if not times:
        print("CSV file is empty or has no data.", file=sys.stderr)
        sys.exit(1)

    plot(times, costs, args.csv, args.output)
    plot_elites_per_second(times, args.csv, args.output)


if __name__ == "__main__":
    main()
