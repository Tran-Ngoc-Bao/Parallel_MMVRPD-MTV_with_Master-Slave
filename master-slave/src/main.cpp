#include <CLI/CLI.hpp>
#include <iostream>
#include <fstream>
#include <filesystem>
#include <chrono>
#include <iomanip>
#include <nlohmann/json.hpp>
#include <mpi.h>
#include <sched.h>

#include "cli.hpp"
#include "config.hpp"
#include "parallel.hpp"
#include "solutions.hpp"
#include "logger.hpp"

// i5-14400F: 6 P-cores (SMT pairs) + 4 E-cores (no SMT).
static constexpr int P_CORE_FIRST[] = {0, 2, 4, 6, 8, 10};
static constexpr int P_CORE_COUNT   = 6;
static constexpr int E_CORE[]       = {12, 13, 14, 15};
static constexpr int E_CORE_COUNT   = 4;

static void pin_process(int cpu)
{
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    if (sched_setaffinity(0, sizeof(set), &set) != 0) {
        std::cerr << "Warning: failed to pin process to CPU " << cpu << "\n";
    }
}

// Workers get a dedicated non-SMT P-core each; master takes a leftover
// P-core, or an E-core once workers have claimed all of them.
static void pin_to_cores(int rank, int world_size)
{
    int num_workers = world_size - 1;
    if (rank == 0) {
        if (num_workers < P_CORE_COUNT) pin_process(P_CORE_FIRST[num_workers]);
        else                            pin_process(E_CORE[0]);
        return;
    }
    int worker_idx = rank - 1;
    if (worker_idx < P_CORE_COUNT) {
        pin_process(P_CORE_FIRST[worker_idx]);
    } else if (worker_idx < P_CORE_COUNT * 2) {
        pin_process(P_CORE_FIRST[worker_idx - P_CORE_COUNT] + 1);
    } else {
        pin_process(E_CORE[(worker_idx - P_CORE_COUNT * 2) % E_CORE_COUNT]);
    }
}

int main(int argc, char** argv)
{
    MPI_Init(&argc, &argv);

    CLI::App app{"Tabu Search for VRP with Drones"};
    app.require_subcommand(1);

    cli::Arguments args;

    // ---- run subcommand ----
    auto* run_cmd = app.add_subcommand("run", "Run tabu search");
    run_cmd->add_option("problem", args.run.problem, "Problem file path")->required();
    run_cmd->add_option("--truck-cfg",  args.run.truck_cfg);
    run_cmd->add_option("--drone-cfg",  args.run.drone_cfg);

    // EnergyModel
    std::map<std::string, cli::EnergyModel> em_map{
        {"linear", cli::EnergyModel::Linear},
        {"non-linear", cli::EnergyModel::NonLinear},
        {"endurance", cli::EnergyModel::Endurance},
        {"unlimited", cli::EnergyModel::Unlimited}
    };
    run_cmd->add_option("-c,--config", args.run.config)->transform(
        CLI::CheckedTransformer(em_map, CLI::ignore_case));

    run_cmd->add_option("--tabu-size-factor",          args.run.tabu_size_factor);
    run_cmd->add_option("--adaptive-iterations",       args.run.adaptive_iterations);
    run_cmd->add_flag  ("--adaptive-fixed-iterations", args.run.adaptive_fixed_iterations);
    run_cmd->add_option("--adaptive-pull-elite-segments", args.run.adaptive_pull_elite_segments);
    run_cmd->add_option("--adaptive-segments",         args.run.adaptive_segments,
        "Non-improving segments before a local do_reset, used only when --elite-pull-strategy=off");
    run_cmd->add_flag  ("--adaptive-fixed-segments",   args.run.adaptive_fixed_segments);
    run_cmd->add_option("--ejection-chain-iterations", args.run.ejection_chain_iterations);
    run_cmd->add_option("--destroy-rate",              args.run.destroy_rate);
    run_cmd->add_option("--elite-pool-factor",         args.run.elite_pool_factor);
    run_cmd->add_option("--elite-pool-size",           args.run.elite_pool_size,
        "If non-zero, used directly as the elite pool capacity, bypassing --elite-pool-factor");
    run_cmd->add_option("--gamma-1",                   args.run.gamma_1);
    run_cmd->add_option("--gamma-2",                   args.run.gamma_2);
    run_cmd->add_option("--gamma-3",                   args.run.gamma_3);
    run_cmd->add_option("--gamma-4",                   args.run.gamma_4);
    run_cmd->add_flag  ("--randomize-worker-adaptive-hyperparams", args.run.randomize_worker_adaptive_hyperparams);
    run_cmd->add_flag  ("--compact-output",            args.run.compact_output);
    run_cmd->add_option("--run-id",                    args.run.run_id);

    std::map<std::string, cli::ElitePullStrategy> elite_pull_map{
        {"random",    cli::ElitePullStrategy::Random},
        {"topk",      cli::ElitePullStrategy::TopK},
        {"rank",      cli::ElitePullStrategy::Rank},
        {"pullcount", cli::ElitePullStrategy::PullCount},
        {"off",       cli::ElitePullStrategy::Off}
    };
    run_cmd->add_option("--elite-pull-strategy", args.run.elite_pull_strategy)->transform(
        CLI::CheckedTransformer(elite_pull_map, CLI::ignore_case));

    std::map<std::string, cli::ElitePullAcceptStrategy> elite_pull_accept_map{
        {"always",    cli::ElitePullAcceptStrategy::Always},
        {"selective", cli::ElitePullAcceptStrategy::Selective}
    };
    run_cmd->add_option("--elite-pull-accept-strategy", args.run.elite_pull_accept_strategy)->transform(
        CLI::CheckedTransformer(elite_pull_accept_map, CLI::ignore_case));

    std::map<std::string, cli::ElitePushStrategy> elite_push_map{
        {"new-best",         cli::ElitePushStrategy::NewBest},
        {"segment-best",     cli::ElitePushStrategy::SegmentBest},
        {"significant-best", cli::ElitePushStrategy::SignificantBest}
    };
    run_cmd->add_option("--elite-push-strategy", args.run.elite_push_strategy)->transform(
        CLI::CheckedTransformer(elite_push_map, CLI::ignore_case));

    run_cmd->add_option("--elite-pull-quality-tolerance-pct", args.run.elite_pull_quality_tolerance_pct,
        "Selective accept: an offered elite within this % of the requester's "
        "personal-best cost can still be accepted if more diverse than average "
        "(e.g. 1.0 = 1%, 0.5 = 0.5%, 2.0 = 2%)");

    std::map<std::string, cli::EliteReplaceStrategy> elite_replace_map{
        {"td-crowding",   cli::EliteReplaceStrategy::TDCrowding},
        {"edge-crowding", cli::EliteReplaceStrategy::EdgeCrowding},
        {"quality-only",  cli::EliteReplaceStrategy::QualityOnly},
        {"random-target", cli::EliteReplaceStrategy::RandomTarget}
    };
    run_cmd->add_option("--elite-replace-strategy", args.run.elite_replace_strategy)->transform(
        CLI::CheckedTransformer(elite_replace_map, CLI::ignore_case));

    std::map<std::string, cli::ConfigType> ct_map{
        {"low", cli::ConfigType::Low}, {"high", cli::ConfigType::High}
    };
    run_cmd->add_option("--speed-type", args.run.speed_type)->transform(
        CLI::CheckedTransformer(ct_map, CLI::ignore_case));
    run_cmd->add_option("--range-type", args.run.range_type)->transform(
        CLI::CheckedTransformer(ct_map, CLI::ignore_case));

    std::map<std::string, cli::DistanceType> dt_map{
        {"manhattan", cli::DistanceType::Manhattan},
        {"euclidean", cli::DistanceType::Euclidean}
    };
    run_cmd->add_option("--truck-distance", args.run.truck_distance)->transform(
        CLI::CheckedTransformer(dt_map, CLI::ignore_case));
    run_cmd->add_option("--drone-distance", args.run.drone_distance)->transform(
        CLI::CheckedTransformer(dt_map, CLI::ignore_case));

    std::optional<size_t> trucks_count_cli, drones_count_cli;
    run_cmd->add_option("--trucks-count", trucks_count_cli);
    run_cmd->add_option("--drones-count", drones_count_cli);

    run_cmd->add_option("--waiting-time-limit",  args.run.waiting_time_limit);

    std::map<std::string, cli::Strategy> strat_map{
        {"random", cli::Strategy::Random},
        {"cyclic", cli::Strategy::Cyclic},
        {"vns",    cli::Strategy::Vns},
        {"adaptive", cli::Strategy::Adaptive}
    };
    run_cmd->add_option("--strategy", args.run.strategy)->transform(
        CLI::CheckedTransformer(strat_map, CLI::ignore_case));

    std::optional<size_t> fix_iter_cli;
    run_cmd->add_option("--fix-iteration", fix_iter_cli);

    run_cmd->add_option("--reset-after-factor",  args.run.reset_after_factor);
    run_cmd->add_option("--max-elite-size",       args.run.max_elite_size);
    run_cmd->add_option("--max-evaluations",      args.run.max_evaluations,
        "If non-zero, stop after this many total neighborhood evaluations "
        "(per worker). Stop-condition priority: max-evaluations, then "
        "non-improving segments");
    run_cmd->add_option("--penalty-exponent",     args.run.penalty_exponent);
    run_cmd->add_flag  ("--single-truck-route",   args.run.single_truck_route);
    run_cmd->add_flag  ("--single-drone-route",   args.run.single_drone_route);
    run_cmd->add_flag  ("-v,--verbose",            args.run.verbose);
    run_cmd->add_option("--outputs",               args.run.outputs);
    run_cmd->add_flag  ("--disable-logging",       args.run.disable_logging);
    run_cmd->add_flag  ("--dry-run",               args.run.dry_run);
    run_cmd->add_option("--extra",                 args.run.extra);
    std::optional<uint64_t> seed_cli;
    run_cmd->add_option("--seed", seed_cli);

    // ---- evaluate subcommand ----
    auto* eval_cmd = app.add_subcommand("evaluate", "Evaluate a solution");
    eval_cmd->add_option("solution", args.evaluate.solution, "Solution JSON")->required();
    eval_cmd->add_option("config",   args.evaluate.config,   "Config JSON")  ->required();

    CLI11_PARSE(app, argc, argv);

    int rank = 0;
    int world_size = 1;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &world_size);
    pin_to_cores(rank, world_size);

    if (run_cmd->parsed()) {
        args.cmd = cli::CommandType::Run;
        if (trucks_count_cli) args.run.trucks_count = trucks_count_cli;
        if (drones_count_cli) args.run.drones_count = drones_count_cli;
        if (fix_iter_cli)     args.run.fix_iteration = fix_iter_cli;
        if (seed_cli)         args.run.seed          = seed_cli;
    } else {
        args.cmd = cli::CommandType::Evaluate;
    }

    if (args.cmd == cli::CommandType::Evaluate && rank != 0) {
        MPI_Finalize();
        return 0;
    }

    Solution solution;

    if (args.cmd == cli::CommandType::Run) {
        set_global_config(build_config(args.run));
        if (world_size > 1) {
            if (rank == 0) {
                solution = parallel::run_master(world_size);
            } else {
                parallel::run_worker(rank);
                MPI_Finalize();
                return 0;
            }
        } else {
            Logger logger;
            auto t0 = std::chrono::steady_clock::now();
            Solution root = Solution::initialize();
            auto t1 = std::chrono::steady_clock::now();
            solution = Solution::tabu_search(root, logger);
            auto t2 = std::chrono::steady_clock::now();
            double init_sec = std::chrono::duration<double>(t1 - t0).count();
            double search_sec = std::chrono::duration<double>(t2 - t1).count();
            double total_sec = std::chrono::duration<double>(t2 - t0).count();
            std::cerr << std::fixed << std::setprecision(6)
                      << "Timing (mode=sequential-in-version-2, unit=s): init="
                      << init_sec << " search=" << search_sec
                      << " total=" << total_sec << "\n";
        }
    } else {
        // Evaluate
        set_global_config(build_config_from_json(args.evaluate.config));
        std::ifstream f(args.evaluate.solution);
        if (!f) { std::cerr << "Cannot open " << args.evaluate.solution << "\n"; return 1; }
        nlohmann::json j; f >> j;
        solution = Solution::from_json(j);
        Logger logger;
        logger.finalize(solution, 0, 0, 0, 0, 0, 0.0, 0.0);
    }

    std::cerr << "Result = " << solution.working_time << "\n";
    solution.verify();
    MPI_Finalize();
    return 0;
}
