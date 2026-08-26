#!/usr/bin/env python3
"""
Given several run4_ims.sh worker output JSON files for the *same*
instance/run, print the path of the best one on stdout: feasible
solutions are preferred over infeasible ones, and among solutions with
the same feasibility the lowest working_time wins.

Usage: pick_best.py FILE [FILE ...]
"""
import json
import sys


def sort_key(path):
    with open(path) as f:
        sol = json.load(f)["solution"]
    # `not feasible` puts feasible (False) before infeasible (True).
    return (not sol["feasible"], sol["working_time"])


def main():
    files = sys.argv[1:]
    if not files:
        print("usage: pick_best.py FILE [FILE ...]", file=sys.stderr)
        sys.exit(1)
    print(min(files, key=sort_key))


if __name__ == "__main__":
    main()
