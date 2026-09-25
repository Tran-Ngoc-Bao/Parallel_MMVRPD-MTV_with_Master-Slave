# Parallel MMVRPD-MTV with Master-Slave

A parallel, cooperative **Tabu Search** solver for the min-max Vehicle Routing Problem with Drones and multiple trips (**MMVRPD-MTV**). The objective is to minimise the `working_time` (makespan) of a fleet of trucks and drones serving a set of customers from one depot.

The repository has two solvers:

| Solver | Path | Description |
|---|---|---|
| **master-slave** | `master-slave/` | C++17 + MPI. One master process keeps a shared **elite pool**. Worker processes run tabu search and push/pull elite solutions through the master. |
| **sequence** | `sequence/cpp/`, `sequence/rust/` | Sequential baseline (one tabu search per process), in C++ and Rust. Used for the `sats` (single run) and `ims` (island model: independent runs, best picked afterwards) baselines. |

## Repository layout

```
.
├── data/                 # Benchmark instances: <customers>.<group>.<id>.txt (n = 6 … 1000)
├── bks/                  # Best-known solutions per instance (<n>/<instance>-bks.json)
│   └── capacity-1400_baseline.xlsx   # Source baseline used to generate the BKS files
├── master-slave/
│   ├── src/              # Solver sources (parallel.cpp = MPI master/worker logic)
│   ├── problems/config_parameter/    # Truck & drone parameter files (JSON)
│   ├── script/           # build.sh, script.sh, experiment scripts (exp1-full, exp2/*)
│   └── outputs/          # Experiment results (JSON)
├── sequence/
│   ├── cpp/              # Sequential C++ solver + exp1 / exp1-full scripts, gen_bks.py
│   └── rust/             # Sequential Rust solver
└── install-linux.sh      # Installs build dependencies (Debian/Ubuntu)
```

## Algorithm overview

Every search process runs an **adaptive tabu search** over these neighborhoods: Move (1,0), (1,1), (2,0), (2,1), (2,2), 2-opt and Ejection-chain. It supports several energy models for the drones (`linear`, `non-linear`, `endurance`, `unlimited`).

In the **master-slave** version (`master-slave/src/parallel.cpp`):

- **Rank 0 (master)** keeps an elite pool of solutions. It does not run a search itself.
- **Ranks 1..N-1 (workers)** each build an initial solution and run tabu search with their own seed. They:
  - **push** good solutions to the master (`--elite-push-strategy`: `new-best`, `segment-best`, `significant-best`);
  - **pull** an elite solution from the master every few non-improving segments (`--adaptive-pull-elite-segments`, `--elite-pull-strategy`: `random`, `topk`, `rank`, `pullcount`, `off`);
  - accept a pulled solution either always or only when it is good and diverse enough (`--elite-pull-accept-strategy`, `--elite-pull-quality-tolerance-pct`).
- When the pool is full, the master picks which solution to replace with `--elite-replace-strategy` (`td-crowding`, `edge-crowding`, `quality-only`, `random-target`).
- At the end, the master collects every worker's result and returns the best solution.

With only one MPI process, the binary runs a plain sequential tabu search.

## Requirements

- Linux (Debian/Ubuntu recommended)
- CMake ≥ 3.16 and a C++17 compiler (GCC)
- OpenMPI (`openmpi-bin`, `libopenmpi-dev`)
- Python 3 + `matplotlib` (for the statistics and plotting scripts)
- Rust / Cargo (only for `sequence/rust`)

To install everything on Debian/Ubuntu:

```bash
bash install-linux.sh
```

CLI11, nlohmann/json and mimalloc are downloaded automatically by CMake (`FetchContent`).

## Build

```bash
# Master-slave (MPI)
bash master-slave/script/build.sh          # release build  -> master-slave/build/master_slave
bash master-slave/script/build.sh pgo      # optional two-pass profile-guided build

# Sequential C++
bash sequence/cpp/script/build.sh          # -> sequence/cpp/build/sequence

# Sequential Rust
bash sequence/rust/script/build.sh         # -> sequence/rust/target/release/sequence
```

The binaries are built with `-march=native`, so rebuild them on each machine where you run them.

## Usage

### Quick run

```bash
# Master-slave: 1 master + 6 workers on data/200.10.2.txt
NUM_WORKERS=7 bash master-slave/script/script.sh data/200.10.2.txt

# Sequential
bash sequence/cpp/script/script.sh data/200.10.2.txt
```

`master-slave/script/script.sh` reads these environment variables: `NUM_WORKERS` (total MPI processes, master included), `SEED`, `TIME_LIMIT`, `MAX_EVALUATIONS`, `OUTPUTS_DIR`, `RUN_ID`, `COMPACT_OUTPUT`, and the `ELITE_*` / `ADAPTIVE_*` parameters.

### Calling the binary directly

Run from inside `master-slave/`, because the default truck/drone config paths are relative:

```bash
cd master-slave
mpirun -np 7 ./build/master_slave run ../data/200.10.2.txt \
    --seed 1 --time-limit 35 \
    --elite-pool-size 4 \
    --elite-pull-strategy rank \
    --elite-pull-accept-strategy selective \
    --elite-push-strategy significant-best \
    --elite-replace-strategy edge-crowding \
    --outputs outputs/
```

Useful options (see `src/main.cpp` and `src/cli.hpp` for the full list):

| Option | Meaning |
|---|---|
| `-c, --config` | Drone energy model: `linear`, `non-linear`, `endurance` (default), `unlimited` |
| `--truck-cfg`, `--drone-cfg` | Truck/drone parameter JSON files |
| `--trucks-count`, `--drones-count` | Override the fleet size given in the instance |
| `--strategy` | Neighborhood selection: `random`, `cyclic`, `vns`, `adaptive` (default) |
| `--time-limit` | Stop after N seconds per worker |
| `--max-evaluations` | Stop after N neighborhood evaluations per worker (takes priority over `--time-limit`) |
| `--elite-pool-size` / `--elite-pool-factor` | Elite pool capacity (a fixed size, or derived from a factor) |
| `--seed` | Random seed |
| `--dry-run` | Only build and print the config, without searching |
| `--compact-output`, `--outputs`, `--run-id` | Output format and location |

To check a saved solution:

```bash
./build/master_slave evaluate <solution.json> <config.json>
```

### Output

Each run writes a JSON file with the final `solution` (`truck_routes`, `drone_routes`, `working_time`, `feasible`, violation terms), the `config` used, the run time, and the cooperation statistics (elite pushes/pulls, pool diversity, evaluation/time checkpoints).

## Instances and best-known solutions

- `data/<n>.<group>.<id>.txt`: the header gives `trucks_count`, `drones_count`, `customers` and the `depot` position, followed by one line per customer: `X Y Dronable Demand`.
- `bks/<n>/<instance>-bks.json`: best-known solutions, used to compute the **RPD** (Relative Percentage Deviation) `(working_time − BKS) / BKS × 100`. To (re)generate them from the Excel baseline:

  ```bash
  python3 sequence/cpp/script/gen_bks.py            # all instances
  python3 sequence/cpp/script/gen_bks.py --customers 6,10
  ```

## Experiments

| Experiment | Script | Purpose |
|---|---|---|
| exp1-full (sequence) | `sequence/cpp/script/exp1-full/run_all.sh` | `ims` and `sats` baselines on every instance (n = 6 … 1000) |
| exp1-full (master-slave) | `master-slave/script/exp1-full/run_all.sh` | Cooperative (`coop`) search on the same instances and time limits |
| exp2/a | `master-slave/script/exp2/a/` | Compare elite push strategies |
| exp2/b | `master-slave/script/exp2/b/` | Compare elite pool sizes |
| exp2/c | `master-slave/script/exp2/c/` | Pull strategies × pull-accept strategies |
| exp2/d | `master-slave/script/exp2/d/` | `adaptive_pull_elite_segments` ∈ {2, 4, 8} |
| exp2/e, e-1 | `master-slave/script/exp2/e*/` | `elite_pull_quality_tolerance_pct` ∈ {0.5, 1, 2} |
| exp2/g | `master-slave/script/exp2/g/` | Elite replace strategies × pool sizes |

Example:

```bash
# exp1-full: RUNS=10, SLEEP_SEC=0, NUM_WORKERS=7
bash master-slave/script/exp1-full/run_all.sh 10 0 7

# Summary statistics (RPD of sats / ims / coop, win/tie/loss)
cd master-slave/script/exp1-full && python3 stats.py   # default paths are relative to this folder
```

The run scripts skip result files that already exist, so an interrupted experiment can be restarted.

## Notes

- **CPU pinning:** `master-slave/src/main.cpp` pins each MPI rank to a core using a layout written for an **Intel i5-14400F** (6 P-cores + 4 E-cores). The sequence scripts use the same core IDs by default (`CPU_CORES=0,2,4,6,8,10`). Change these values on other hardware.
- `script.sh` calls `mpirun --allow-run-as-root`. Remove this flag if you do not run as root.
