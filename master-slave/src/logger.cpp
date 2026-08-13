#include "logger.hpp"
#include "solutions.hpp"
#include "config.hpp"
#include <filesystem>
#include <sstream>
#include <random>
#include <stdexcept>
#include <iostream>
#include <nlohmann/json.hpp>

namespace fs = std::filesystem;

static std::string random_id(size_t len = 8) {
    static const char chars[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    static std::mt19937 rng{std::random_device{}()};
    std::uniform_int_distribution<> dist(0, sizeof(chars)-2);
    std::string s;
    s.reserve(len);
    for (size_t i = 0; i < len; ++i) s += chars[dist(rng)];
    return s;
}

Logger::Logger()
{
    const Config& cfg = global_config();
    _time_offset = std::chrono::steady_clock::now();
    _outputs = cfg.outputs;

    fs::path out(cfg.outputs);
    if (!fs::is_directory(out)) fs::create_directories(out);

    // Extract problem stem
    fs::path p(cfg.problem);
    _problem = p.stem().string();
    if (_problem.empty()) throw std::runtime_error("Cannot determine problem name");

    _id = cfg.run_id.empty() ? random_id(8) : cfg.run_id;

    if (!cfg.disable_logging && !cfg.compact_output) {
        fs::path csv = out / (_problem + "-" + _id + ".csv");
        std::ofstream f(csv);
        if (!f) throw std::runtime_error("Cannot create CSV: " + csv.string());
        std::cerr << "Logging iterations to " << csv << "\n";
        f << "sep=,\n";
        f << "Iteration,Cost,Working time,Feasible,p0,Energy violation,"
             "p1,Capacity violation,p2,Waiting time violation,p3,"
             "Fixed time violation,Truck routes,Drone routes,"
             "Truck routes count,Drone routes count,Neighborhood,Tabu list\n";
        _writer = std::move(f);
    }
}

static std::string wrap(const std::string& s) {
    return "\"" + s + "\"";
}

static std::string routes_repr(
    const std::vector<std::vector<LocalRc<TruckRoute>>>& routes)
{
    std::ostringstream oss;
    oss << "[";
    for (size_t i = 0; i < routes.size(); ++i) {
        if (i) oss << ", ";
        oss << "[";
        for (size_t j = 0; j < routes[i].size(); ++j) {
            if (j) oss << ", ";
            const auto& c = routes[i][j]->data().customers;
            oss << "[";
            for (size_t k = 0; k < c.size(); ++k) {
                if (k) oss << ", ";
                oss << c[k];
            }
            oss << "]";
        }
        oss << "]";
    }
    oss << "]";
    return oss.str();
}

static std::string drone_routes_repr(
    const std::vector<std::vector<LocalRc<DroneRoute>>>& routes)
{
    std::ostringstream oss;
    oss << "[";
    for (size_t i = 0; i < routes.size(); ++i) {
        if (i) oss << ", ";
        oss << "[";
        for (size_t j = 0; j < routes[i].size(); ++j) {
            if (j) oss << ", ";
            const auto& c = routes[i][j]->data().customers;
            oss << "[";
            for (size_t k = 0; k < c.size(); ++k) {
                if (k) oss << ", ";
                oss << c[k];
            }
            oss << "]";
        }
        oss << "]";
    }
    oss << "]";
    return oss.str();
}

static std::string tabu_repr(const std::vector<std::vector<size_t>>& tabu) {
    std::ostringstream oss;
    oss << "[";
    for (size_t i = 0; i < tabu.size(); ++i) {
        if (i) oss << ", ";
        oss << "[";
        for (size_t j = 0; j < tabu[i].size(); ++j) {
            if (j) oss << ", ";
            oss << tabu[i][j];
        }
        oss << "]";
    }
    oss << "]";
    return oss.str();
}

void Logger::log(const Solution& sol,
                 Neighborhood nb,
                 const std::vector<std::vector<size_t>>& tabu_list)
{
    ++_iteration;
    if (!_writer) return;

    size_t tc = 0, dc = 0;
    for (auto& r : sol.truck_routes) tc += r.size();
    for (auto& r : sol.drone_routes) dc += r.size();

    *_writer
        << _iteration << ","
        << sol.cost()              << ","
        << sol.working_time        << ","
        << (sol.feasible ? 1 : 0)  << ","
        << penalty::get(0)         << ","
        << sol.energy_violation    << ","
        << penalty::get(1)         << ","
        << sol.capacity_violation  << ","
        << penalty::get(2)         << ","
        << sol.waiting_time_violation << ","
        << penalty::get(3)         << ","
        << sol.fixed_time_violation<< ","
        << wrap(routes_repr(sol.truck_routes)) << ","
        << wrap(drone_routes_repr(sol.drone_routes)) << ","
        << tc << ","
        << dc << ","
        << wrap(neighborhood_to_str(nb)) << ","
        << wrap(tabu_repr(tabu_list))
        << "\n";
}

void Logger::finalize(const Solution& result,
                      size_t tabu_size,
                      size_t reset_after,
                      size_t actual_adaptive_iterations,
                      size_t total_adaptive_segments,
                      size_t last_improved,
                      double post_optimization,
                      double post_optimization_elapsed,
                      std::size_t total_evaluations,
                      std::size_t total_elite_pushes,
                      std::size_t accepted_elite_pushes,
                      std::size_t evaluation_checkpoint_budget,
                      const std::vector<double>& best_cost_by_evaluation_checkpoint,
                      std::size_t elite_pool_size,
                      double elite_pool_diversity,
                      const std::vector<double>& elite_pool_costs,
                      std::size_t best_solution_evaluations)
{
    this->total_evaluations = total_evaluations;

    const Config& cfg = global_config();
    if (cfg.disable_logging) return;

    auto elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - _time_offset).count();

    auto sj = result.to_json();
    auto cj = config_to_json(cfg);

    nlohmann::json run;
    run["problem"]                    = _problem;
    run["tabu_size"]                  = tabu_size;
    run["reset_after"]                = reset_after;
    run["iterations"]                 = _iteration;
    run["actual_adaptive_iterations"] = actual_adaptive_iterations;
    run["total_adaptive_segments"]    = total_adaptive_segments;
    run["solution"]                   = sj;
    run["config"]                     = cj;
    run["last_improved"]              = last_improved;
    run["elapsed"]                    = elapsed;
    run["post_optimization"]          = post_optimization;
    run["post_optimization_elapsed"]  = post_optimization_elapsed;
    run["total_evaluations"]          = total_evaluations;
    run["total_elite_pushes"]         = total_elite_pushes;
    run["accepted_elite_pushes"]      = accepted_elite_pushes;
    run["elite_push_accept_ratio"]    = total_elite_pushes > 0
        ? static_cast<double>(accepted_elite_pushes) / static_cast<double>(total_elite_pushes)
        : 0.0;
    run["evaluation_checkpoint_budget"]        = evaluation_checkpoint_budget;
    run["best_cost_by_evaluation_checkpoint"]  = best_cost_by_evaluation_checkpoint;
    run["elite_pool_size"]                     = elite_pool_size;
    run["elite_pool_diversity"]                = elite_pool_diversity;
    run["elite_pool_costs"]                    = elite_pool_costs;
    run["best_solution_evaluations"]           = best_solution_evaluations;
    run["best_solution_evaluations_percent"]   = evaluation_checkpoint_budget > 0
        ? 100.0 * static_cast<double>(best_solution_evaluations) / static_cast<double>(evaluation_checkpoint_budget)
        : 0.0;

    fs::path out(_outputs);

    auto write = [&](const fs::path& p, const std::string& content) {
        std::ofstream f(p);
        if (!f) throw std::runtime_error("Cannot write " + p.string());
        f << content;
        std::cout << p << "\n";
    };

    write(out / (_problem + "-" + _id + ".json"), run.dump());
    if (!cfg.compact_output) {
        write(out / (_problem + "-" + _id + "-solution.json"), sj.dump());
        write(out / (_problem + "-" + _id + "-config.json"),   cj.dump());
    }
}
