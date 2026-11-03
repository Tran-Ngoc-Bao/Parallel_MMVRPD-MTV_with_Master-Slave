#!/usr/bin/env python3
"""
Generate bks-new/<n>/<instance>-bks.json for every instance in data/ from
the runs of the master-slave algorithm in master-slave/outputs/exp1-full/coop
(<n>/<instance>-<run>.json), independently of the old bks/ (which came from
bks/capacity-1400_baseline.xlsx, see sequence/cpp/script/gen_bks.py).

For each instance the best feasible run (lowest solution.working_time; ties:
lowest run number) is taken and, as in gen_bks.py, re-checked with the
sequence binary so that every bks-new file has the same format as bks/:

  1. `sequence run <problem> --dry-run --compact-output` gives a config
     skeleton (customers + default truck/drone parameters) for the instance.
  2. `sequence evaluate <solution.json> <config.json>` with the run's
     truck/drone routes computes working_time/feasibility/violations.

The re-evaluated working_time must match the run's own value (relative
tolerance COST_CHECK_REL_TOL) and be feasible, otherwise the instance fails.
bks-new/summary.csv lists, per instance, the source run, its working_time
and, for reference only, the old bks working_time and the gap to it.

Usage:
    python3 gen_bks_new.py                   # all instances found in data/
    python3 gen_bks_new.py --customers 6,10  # only these customer counts
    python3 gen_bks_new.py --dry-run         # print what would happen, write nothing
"""
import argparse
import csv
import json
import re
import subprocess
import sys
import tempfile
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
PROJECT_ROOT = SCRIPT_DIR.parent.parent
SEQUENCE_DIR = PROJECT_ROOT / "sequence" / "cpp"
BUILD_BIN = SEQUENCE_DIR / "build" / "sequence"
DATA_DIR = PROJECT_ROOT / "data"
COOP_DIR = PROJECT_ROOT / "master-slave" / "outputs" / "exp1-full" / "coop"
OLD_BKS_DIR = PROJECT_ROOT / "bks"
BKS_NEW_DIR = PROJECT_ROOT / "bks-new"

# The run's working_time and the sequence re-evaluation should agree up to
# floating-point noise; anything further off is a logic error.
COST_CHECK_REL_TOL = 1e-6

RUN_RE = re.compile(r"^(?P<problem>\d+\.\d+\.\d+)-(?P<run>\d+)\.json$")


def best_run(problem: str):
    """(path, run, working_time) of the best feasible coop run, the number of
    runs found, and the number of feasible ones."""
    n = problem.split(".")[0]
    best, runs, feasible = None, 0, 0
    for path in sorted((COOP_DIR / n).glob(f"{problem}-*.json")):
        m = RUN_RE.match(path.name)
        if not m or m["problem"] != problem:
            continue
        runs += 1
        with path.open() as f:
            solution = json.load(f)["solution"]
        if not solution["feasible"]:
            continue
        feasible += 1
        key = (solution["working_time"], int(m["run"]))
        if best is None or key < (best[2], best[1]):
            best = (path, int(m["run"]), solution["working_time"], solution)
    return best, runs, feasible


def run_cmd(cmd):
    # The binary loads problems/config_parameter/*.json relative to CWD.
    r = subprocess.run(cmd, capture_output=True, text=True, cwd=str(SEQUENCE_DIR))
    if r.returncode != 0:
        raise RuntimeError(
            f"command failed: {' '.join(cmd)}\nstdout={r.stdout}\nstderr={r.stderr}")
    return r


def build_skeleton_config(problem: str, tmpdir: Path):
    data_file = DATA_DIR / f"{problem}.txt"
    skel_dir = tmpdir / "skel"
    skel_dir.mkdir(exist_ok=True)
    run_cmd([str(BUILD_BIN), "run", str(data_file), "--dry-run",
             "--outputs", str(skel_dir), "--run-id", "sk", "--compact-output"])
    with (skel_dir / f"{problem}-sk.json").open() as f:
        return json.load(f)["config"]


def evaluate(problem: str, config: dict, truck_routes, drone_routes, tmpdir: Path):
    eval_dir = tmpdir / f"eval-{problem}"
    eval_dir.mkdir(exist_ok=True)
    config_path = tmpdir / f"{problem}-config.json"
    solution_path = tmpdir / f"{problem}-solution.json"

    config = dict(config)
    config["outputs"] = str(eval_dir)
    with config_path.open("w") as f:
        json.dump(config, f)
    with solution_path.open("w") as f:
        json.dump({"truck_routes": truck_routes, "drone_routes": drone_routes}, f)

    run_cmd([str(BUILD_BIN), "evaluate", str(solution_path), str(config_path)])

    candidates = [p for p in eval_dir.glob(f"{problem}-*.json")
                  if not p.name.endswith("-solution.json")
                  and not p.name.endswith("-config.json")]
    if len(candidates) != 1:
        raise RuntimeError(f"expected exactly 1 output file for {problem}, got {candidates}")
    with candidates[0].open() as f:
        return json.load(f)


def old_bks_wt(problem: str):
    path = OLD_BKS_DIR / problem.split(".")[0] / f"{problem}-bks.json"
    if not path.is_file():
        return None
    with path.open() as f:
        solution = json.load(f)["solution"]
    return solution["working_time"] if solution["feasible"] else None


def problem_key(problem: str):
    return tuple(int(x) for x in problem.split("."))


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--customers", default=None,
                    help="Comma-separated customer counts to (re)generate "
                         "(default: all n found in data/)")
    ap.add_argument("--dry-run", action="store_true",
                    help="Print what would happen, write nothing")
    args = ap.parse_args()

    if not BUILD_BIN.is_file():
        sys.exit(f"error: {BUILD_BIN} not found -- build it first "
                 f"(sequence/cpp/script/build.sh)")

    problems = sorted((p.stem for p in DATA_DIR.glob("*.txt")), key=problem_key)
    if args.customers:
        wanted_n = {c.strip() for c in args.customers.split(",") if c.strip()}
        problems = [p for p in problems if p.split(".")[0] in wanted_n]

    written, failed, summary = [], [], []
    with tempfile.TemporaryDirectory(prefix="gen_bks_new_") as tmp:
        tmpdir = Path(tmp)
        for problem in problems:
            best, runs, feasible = best_run(problem)
            if best is None:
                print(f"FAIL  {problem}: no feasible run in {COOP_DIR} ({runs} run(s) found)")
                failed.append(problem)
                continue
            path, run, run_wt, solution = best

            try:
                config = build_skeleton_config(problem, tmpdir)
                result = evaluate(problem, config, solution["truck_routes"],
                                  solution["drone_routes"], tmpdir)
            except Exception as e:
                print(f"FAIL  {problem}: {e}")
                failed.append(problem)
                continue

            wt = result["solution"]["working_time"]
            if not result["solution"]["feasible"]:
                print(f"FAIL  {problem}: run {run} is infeasible per sequence evaluate")
                failed.append(problem)
                continue
            if abs(wt - run_wt) > COST_CHECK_REL_TOL * run_wt:
                print(f"FAIL  {problem}: sequence working_time={wt} disagrees with "
                      f"run {run} working_time={run_wt}")
                failed.append(problem)
                continue

            old = old_bks_wt(problem)
            gap = "" if old is None else f"{(wt - old) / old * 100:+.4f}%"
            print(f"WRITE {problem}: working_time={wt:.4f} (run {run}, "
                  f"{feasible}/{runs} feasible){'' if old is None else f', old bks {old:.4f} ({gap})'}")
            written.append(problem)
            summary.append({
                "instance": problem, "runs": runs, "feasible_runs": feasible,
                "source": str(path.relative_to(PROJECT_ROOT)), "working_time": wt,
                "old_bks_working_time": "" if old is None else old, "gap_to_old_pct": gap,
            })

            if not args.dry_run:
                out = BKS_NEW_DIR / problem.split(".")[0] / f"{problem}-bks.json"
                out.parent.mkdir(parents=True, exist_ok=True)
                with out.open("w") as f:
                    json.dump(result, f)

    if summary and not args.dry_run:
        with (BKS_NEW_DIR / "summary.csv").open("w", newline="") as f:
            writer = csv.DictWriter(f, fieldnames=list(summary[0].keys()))
            writer.writeheader()
            writer.writerows(summary)

    gaps = [s["working_time"] - s["old_bks_working_time"]
            for s in summary if s["old_bks_working_time"] != ""]
    better = sum(1 for g in gaps if g < -COST_CHECK_REL_TOL)
    worse = sum(1 for g in gaps if g > COST_CHECK_REL_TOL)
    print(f"\nDone: {len(written)} written, {len(failed)} failed, out of "
          f"{len(problems)} instances. Versus the old bks: {better} better, "
          f"{worse} worse, {len(gaps) - better - worse} equal.")
    if failed:
        print(f"Failed: {failed}")
        sys.exit(1)


if __name__ == "__main__":
    main()
