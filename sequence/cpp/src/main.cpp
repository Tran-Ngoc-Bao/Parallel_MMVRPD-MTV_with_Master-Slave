#include <CLI/CLI.hpp>
#include <iostream>
#include <fstream>
#include <filesystem>
#include <chrono>
#include <iomanip>
#include <nlohmann/json.hpp>

#include "cli.hpp"
#include "config.hpp"
#include "solutions.hpp"
#include "logger.hpp"

int main(int argc, char** argv)
{
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
    run_cmd->add_option("--adaptive-segments",         args.run.adaptive_segments);
    run_cmd->add_flag  ("--adaptive-fixed-segments",   args.run.adaptive_fixed_segments);
    run_cmd->add_option("--ejection-chain-iterations", args.run.ejection_chain_iterations);
    run_cmd->add_option("--destroy-rate",              args.run.destroy_rate);

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
    run_cmd->add_option("--time-limit",           args.run.time_limit,
        "If non-zero, stop after this many seconds instead of after a number "
        "of non-improving segments");
    run_cmd->add_option("--penalty-exponent",     args.run.penalty_exponent);
    run_cmd->add_flag  ("--single-truck-route",   args.run.single_truck_route);
    run_cmd->add_flag  ("--single-drone-route",   args.run.single_drone_route);
    run_cmd->add_flag  ("-v,--verbose",            args.run.verbose);
    run_cmd->add_option("--outputs",               args.run.outputs);
    run_cmd->add_flag  ("--disable-logging",       args.run.disable_logging);
    run_cmd->add_flag  ("--dry-run",               args.run.dry_run);
    run_cmd->add_option("--extra",                 args.run.extra);
    run_cmd->add_flag  ("--compact-output",        args.run.compact_output);
    run_cmd->add_option("--run-id",                args.run.run_id);
    std::optional<uint64_t> seed_cli;
    run_cmd->add_option("--seed", seed_cli);

    // ---- evaluate subcommand ----
    auto* eval_cmd = app.add_subcommand("evaluate", "Evaluate a solution");
    eval_cmd->add_option("solution", args.evaluate.solution, "Solution JSON")->required();
    eval_cmd->add_option("config",   args.evaluate.config,   "Config JSON")  ->required();

    CLI11_PARSE(app, argc, argv);

    if (run_cmd->parsed()) {
        args.cmd = cli::CommandType::Run;
        if (trucks_count_cli) args.run.trucks_count = trucks_count_cli;
        if (drones_count_cli) args.run.drones_count = drones_count_cli;
        if (fix_iter_cli)     args.run.fix_iteration = fix_iter_cli;
        if (seed_cli)         args.run.seed          = seed_cli;
    } else {
        args.cmd = cli::CommandType::Evaluate;
    }

    Solution solution;

    if (args.cmd == cli::CommandType::Run) {
        set_global_config(build_config(args.run));
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
                  << "Timing (mode=sequential-reference, unit=s): init="
                  << init_sec << " search=" << search_sec
                  << " total=" << total_sec << "\n";
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
    return 0;
}
